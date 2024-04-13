#include "slow_sql_recorder.h"
#include "tcop/utility.h"
PG_MODULE_MAGIC;

/* GUC variables */
// static int    auto_explain_log_min_duration = -1; /* msec or -1 */

// static const struct config_enum_entry format_options[] = {
//     {"text", EXPLAIN_FORMAT_TEXT, false},
//     {"xml", EXPLAIN_FORMAT_XML, false},
//     {"json", EXPLAIN_FORMAT_JSON, false},
//     {"yaml", EXPLAIN_FORMAT_YAML, false},
//     {NULL, 0, false}
// };

/* Current nesting depth of ExecutorRun calls */
static int    nesting_level = 0;
static int min_query_duration = -1;
static bool ssl_switch = false;

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;

/* Is the current query sampled, per backend */
// static bool current_query_sampled = true;

#define slow_log_record_enabled(level) \
  (min_query_duration >= 0 && \
	(ssl_switch &&((level) == 0)))


typedef struct sslSharedState
{
    LWLock *lock;
    slock_t mutex; 
};

void        _PG_init(void);
void        _PG_fini(void);


static void ssl_ExecutorEnd(QueryDesc *queryDesc);
static void ssl_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
                    uint64 count, bool execute_once);
static void ssl_ExecutorFinish(QueryDesc *queryDesc);
static void ssl_ExecutorStart(QueryDesc *queryDesc, int eflags);

PG_FUNCTION_INFO_V1(record);
Datum
record(PG_FUNCTION_ARGS){
    int32 times;
    Assert(fcinfo->nargs == 1);
    times = 1000;
    ssl_switch = true;
    PG_RETURN_INT32(times);
}

PG_FUNCTION_INFO_V1(report);
Datum
report(PG_FUNCTION_ARGS){
    int32 times;
    Assert(fcinfo->nargs == 1);
    times = 0;
    PG_RETURN_INT32(times);
}
/*
 * Module load callback
 */
void
_PG_init(void)
{
    /* Define custom GUC variables. */
    // DefineCustomRealVariable("auto_explain.sample_rate",
    //                          "Fraction of queries to process.",
    //                          NULL,
    //                          &auto_explain_sample_rate,
    //                          1.0,
    //                          0.0,
    //                          1.0,
    //                          PGC_SUSET,
    //                          0,
    //                          NULL,
    //                          NULL,
    //                          NULL);

    // EmitWarningsOnPlaceholders("auto_explain");
    elog(LOG, "pg_init: nesting_leve:%d\n", nesting_level);

    DefineCustomIntVariable("slow_sql_recorder.min_query_duration",
                        "Sets the minimum execution time above which plans will be logged.",
                        "Zero prints all plans. -1 turns this feature off.",
                        &min_query_duration,
                        -1,
                        -1, INT_MAX,
                        PGC_SUSET,
                        GUC_UNIT_MS,
                        NULL,
                        NULL,
                        NULL);

    DefineCustomBoolVariable("slow_sql_recorder.ssl_switch",
                            "slow sql record switch.",
                            NULL,
                            &ssl_switch,
                            false,
                            PGC_SUSET,
                            0,
                            NULL,
                            NULL,
                            NULL);

    /* Install hooks. */
    prev_ExecutorStart_hook = ExecutorStart_hook;
    ExecutorStart_hook = ssl_ExecutorStart;

    prev_ExecutorRun_hook = ExecutorRun_hook;
    ExecutorRun_hook = ssl_ExecutorRun;

    prev_ExecutorFinish_hook = ExecutorFinish_hook;
    ExecutorFinish_hook = ssl_ExecutorFinish;

    prev_ExecutorEnd_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = ssl_ExecutorEnd;

    // prev_ExecutorEnd = ExecutorEnd_hook;
    // ExecutorEnd_hook = explain_ExecutorEnd;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
    /* Uninstall hooks. */
    ExecutorStart_hook = prev_ExecutorStart_hook;
    ExecutorRun_hook = prev_ExecutorRun_hook;
    ExecutorFinish_hook = prev_ExecutorFinish_hook;
    ExecutorEnd_hook = prev_ExecutorEnd_hook;
}

static void 
ssl_ExecutorStart(QueryDesc *queryDesc, int eflags)
{

    if (prev_ExecutorStart_hook)
		prev_ExecutorStart_hook(queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);

    /*
	 * If query has queryId zero, don't track it.  This prevents double
	 * counting of optimizable statements that are directly contained in
	 * utility statements.
	 */
    elog(LOG, "begin: slow_log_record_enabled:%d\n", slow_log_record_enabled(nesting_level));
    elog(LOG, "begin: ssl_switch %d, nesting_level:%d\n", ssl_switch , nesting_level);

	if (slow_log_record_enabled(nesting_level))
	{
		/*
		 * Set up to track total elapsed time in ExecutorRun.  Make sure the
		 * space is allocated in the per-query context so it will go away at
		 * ExecutorEnd.
		 */
		if (queryDesc->totaltime == NULL)
		{
			MemoryContext oldcxt;
			oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
			queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
			MemoryContextSwitchTo(oldcxt);
		}
	}
}


/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
ssl_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
                    uint64 count, bool execute_once)
{
    nesting_level++;
    PG_TRY();
    {
        if (prev_ExecutorRun_hook)
            prev_ExecutorRun_hook(queryDesc, direction, count, execute_once);
        else
            standard_ExecutorRun(queryDesc, direction, count, execute_once);
        nesting_level--;
    }
    PG_CATCH();
    {
        nesting_level--;
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
ssl_ExecutorFinish(QueryDesc *queryDesc)
{
    nesting_level++;
    PG_TRY();
    {
        if (prev_ExecutorFinish_hook)
            prev_ExecutorFinish_hook(queryDesc);
        else
            standard_ExecutorFinish(queryDesc);
        nesting_level--;
    }
    PG_CATCH();
    {
        nesting_level--;
        PG_RE_THROW();
    }
    PG_END_TRY();
}


static void
ssl_ExecutorEnd(QueryDesc *queryDesc){
    elog(LOG, "end: nesting_leve:%d\n", nesting_level);
    elog(LOG, "end: queryDesc->totaltime->:%d\n", queryDesc->totaltime == NULL);
    elog(LOG, "[slow_sql_recorder] nesting_leve:%d\n", nesting_level);

    if (queryDesc->totaltime!= NULL && slow_log_record_enabled(nesting_level))
    {
        double        msec;

        /*
         * Make sure stats accumulation is done.  (Note: it's okay if several
         * levels of hook all do this.)
         */
        InstrEndLoop(queryDesc->totaltime);

        /* Log plan if duration is exceeded. */
        msec = queryDesc->totaltime->total * 1000.0;
        elog(LOG, "[slow_sql_recorder] execution duration: %.3f, ms:\n",  msec);
        ereport(LOG,(errmsg("[slow_sql_recorder] execution duration: %.3f, ms:\n",  msec)));
        if (msec >= min_query_duration)
        {
            ExplainState *es = NewExplainState();

            es->analyze = queryDesc->instrument_options;
            es->verbose = false;
            es->buffers = es->analyze;
            es->timing = es->analyze;
            es->summary = es->analyze;
            es->format = EXPLAIN_FORMAT_TEXT;

            ExplainBeginOutput(es);
            ExplainQueryText(es, queryDesc);
            ExplainPrintPlan(es, queryDesc);
            if (es->analyze)
                ExplainPrintTriggers(es, queryDesc);
            ExplainEndOutput(es);

            /* Remove last line break */
            if (es->str->len > 0 && es->str->data[es->str->len - 1] == '\n')
                es->str->data[--es->str->len] = '\0';

            /*
             * Note: we rely on the existing logging of context or
             * debug_query_string to identify just which statement is being
             * reported.  This isn't ideal but trying to do it here would
             * often result in duplication.
             */
            ereport(LOG,
                    (errmsg("[slow_sql_recorder] execution duration: %.3f, ms:\n%s", 
                        msec, es->str->data),
                     errhidestmt(true)));

            pfree(es->str->data);
        }
    }

    if (prev_ExecutorEnd_hook)
        prev_ExecutorEnd_hook(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);

}