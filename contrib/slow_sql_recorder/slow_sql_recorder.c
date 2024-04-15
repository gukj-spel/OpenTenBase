#include "slow_sql_recorder.h"
#include "tcop/utility.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "miscadmin.h"
#include "port.h"
#include "pgstat.h"
#include "executor/spi.h"

PG_MODULE_MAGIC;
#define SSL_LOG_FILE	"pg_log/slow_sql_log.csv"
#define SSL_MAX_LOG_SQL 2048
#define SSL_LOG_TABLE_NAME "ssl_recoder"

// static bool current_query_sampled = true;

#define slow_log_record_enabled(level) \
  (min_query_duration >= 0 && \
	(ssl_switch &&((level) == 0)))


typedef struct sslSharedState
{
    LWLock *lock;
    slock_t mutex; 
}sslSharedState;

/* Current nesting depth of ExecutorRun calls */
static int    nesting_level = 0;
static int min_query_duration = -1;
static bool ssl_switch = false;

// static sslSharedState* ssl_shared_state = NULL;

/* Saved hook values in case of unload */
// static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;


void        _PG_init(void);
void        _PG_fini(void);


static void ssl_ExecutorEnd(QueryDesc *queryDesc);
static void ssl_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
                    uint64 count, bool execute_once);
static void ssl_ExecutorFinish(QueryDesc *queryDesc);
static void ssl_ExecutorStart(QueryDesc *queryDesc, int eflags);
// static void ssl_shmem_startup(void);
// static void ssl_shmem_shutdown(int code, Datum arg);
static bool ssl_store(const char* query, double total_time); //write the slow sql to the log file
static bool ssl_store_log_to_table(const char* query, double total_time);
static bool ssl_connect_spi(void);
static bool ssl_close_spi(void);


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
                            "slow sql record switch. TRUE truns on this feature, FALSE turns off",
                            NULL,
                            &ssl_switch,
                            false,
                            PGC_SUSET,
                            0,
                            NULL,
                            NULL,
                            NULL);
    

    // prev_shmem_startup_hook = shmem_startup_hook;
    // shmem_startup_hook = ssl_shmem_startup;
    /* Install hooks. */
    prev_ExecutorStart_hook = ExecutorStart_hook;
    ExecutorStart_hook = ssl_ExecutorStart;

    prev_ExecutorRun_hook = ExecutorRun_hook;
    ExecutorRun_hook = ssl_ExecutorRun;

    prev_ExecutorFinish_hook = ExecutorFinish_hook;
    ExecutorFinish_hook = ssl_ExecutorFinish;

    prev_ExecutorEnd_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = ssl_ExecutorEnd;
    // if(!ssl_connect_spi()){
    //     elog(ERROR, "Failed to connect to SPI");
    // }

    // prev_ExecutorEnd = ExecutorEnd_hook;
    // ExecutorEnd_hook = explain_ExecutorEnd;
    RequestAddinShmemSpace(MAXALIGN(sizeof(sslSharedState)));
    RequestNamedLWLockTranche("slow_sql_recorder", 1);
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
    // if(ssl_close_spi()){
    //     elog(ERROR, "failed to finish spi");
    // }
    /* Uninstall hooks. */
    ExecutorStart_hook = prev_ExecutorStart_hook;
    ExecutorRun_hook = prev_ExecutorRun_hook;
    ExecutorFinish_hook = prev_ExecutorFinish_hook;
    ExecutorEnd_hook = prev_ExecutorEnd_hook;
    // shmem_startup_hook = prev_shmem_startup_hook;
    
}

static bool 
ssl_store_log_to_table(const char* query, double total_time){
    char * insert_sql;
    int ret;
    elog(LOG, "[ssl_log_to_table] query:%s", query);
    PG_TRY();
    {
        ssl_connect_spi();
        insert_sql = psprintf("INSERT INTO %s VALUES ('%s', %f)",SSL_LOG_TABLE_NAME, query, total_time);
        elog(LOG, "[ssl_log_to_table] insert_sql:%s", insert_sql);
        ret = SPI_exec(insert_sql, 0);
        if(ret < 0){
            elog(ERROR, "Failed to execute insert query: %d", ret);
            return false;
        }
        pfree(insert_sql);
        ssl_close_spi();
        return true;
    }
    PG_CATCH();
    {
        elog(ERROR, "Failed to execute insert query");
        return false;
    }
    PG_END_TRY();
}

static bool ssl_connect_spi(void){
    if(SPI_connect() != SPI_OK_CONNECT){
        return false;
    }
    return true;
}
static bool ssl_close_spi(void){
    int ret = SPI_finish();
    if(SPI_OK_FINISH != ret){
        return false;
    }
    return true;

}

static bool 
ssl_store(const char* query, double total_time){
    FILE* file;
    LWLockAcquire(ssl_shared_state->lock, LW_EXCLUSIVE);
    elog(LOG, "[ssl store], log_file:%s\n", SSL_LOG_FILE);
    file = AllocateFile(SSL_LOG_FILE, "a");

    if(file == NULL){
        elog(LOG, "file cannot open:%s\n", SSL_LOG_FILE);
        goto error;
    }
    if(fprintf(file, "%s,%.2f\n", query, total_time) <= 0 ||fflush(file) != 0){
        goto error;
    }
    if(!ssl_store_log_to_table(query, total_time)){
        goto error;
    }
    FreeFile(file);
    LWLockRelease(ssl_shared_state->lock);
    return true;

error:
	ereport(LOG,
			(errcode_for_file_access(),
			 errmsg("could not write file \"%s\": %m",
					SSL_LOG_FILE)));
    if(file != NULL){
        FreeFile(file);
    }
    LWLockRelease(ssl_shared_state->lock);
	/* Mark our write complete */
	return false;
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
                    (errmsg("[slow_sql_recorder] exec_time: %.3f, %s\n", 
                        msec, es->str->data),
                     errhidestmt(true)));

            pfree(es->str->data);
        }
            ssl_store(queryDesc->sourceText, msec);
    }

    if (prev_ExecutorEnd_hook)
        prev_ExecutorEnd_hook(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);

}

static void 
ssl_shmem_startup(void){
    /*
     * Create or attach to the shared memory state, including hash table
     */
    bool        found;
    elog(LOG, "ssl shmem startup\n");

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    ssl_shared_state = (sslSharedState*)ShmemInitStruct("slow_sql_recorder", sizeof(sslSharedState), &found);
    if (!ssl_shared_state)
        elog(ERROR, "out of shared memory");
    elog(LOG, "get_ssl_shared_state");

    if (!found){
        ssl_shared_state->lock = &(GetNamedLWLockTranche("slow_sql_recorder"))->lock;
        SpinLockInit(&ssl_shared_state->mutex);
    }
    LWLockRelease(AddinShmemInitLock);

    /*
     * If we're in the postmaster (or a standalone backend...), set up a shmem
     * exit hook to dump the statistics to disk.
     */
    if (!IsUnderPostmaster)
        on_shmem_exit(ssl_shmem_shutdown, (Datum) 0);

}

static void 
ssl_shmem_shutdown(int code, Datum arg){

}

