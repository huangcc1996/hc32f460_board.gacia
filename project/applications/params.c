#include "flash_param.h"

#define LOG_TAG "params"      // 该模块对应的标签。不定义时，默认：NO_TAG
#define LOG_LVL LOG_LVL_DBG   // 该模块对应的日志输出级别。不定义时，默认：调试级别
#include <ulog.h>             // 必须在 LOG_TAG 与 LOG_LVL 下面


param_mgr_t mgr;
static int g_baud_1 = 0;
static int g_baud_2 = 0;
static char g_sn[32] = {0};
static char g_sn2[32] = {0};
static int g_di1_trigger = 0;
static int g_do1_level = 0;
static int g_do1_time_type = 0;

static param_item_t g_items[] = {
    PARAM_STR(SN, "01234567890123456789", g_sn, sizeof(g_sn), PARAM_GROUP_SYSTEM, "serial number"),
    PARAM_STR(SN2, "01234567890123456789", g_sn2, sizeof(g_sn2), PARAM_GROUP_SYSTEM, "serial2 number"),
    PARAM_INT(rs485_1_baud, 115200, g_baud_1, PARAM_GROUP_COMM, "RS485-1 baudrate"),
    PARAM_INT(rs485_2_baud, 115200, g_baud_2, PARAM_GROUP_COMM, "RS485-2 baudrate"),

    PARAM_INT(di1_trigger_level, 0, g_di1_trigger, PARAM_GROUP_DI, "DI1 trigger level"),

    PARAM_INT(do1_level, 2, g_do1_level, PARAM_GROUP_DO, "DO1 output level"),

    PARAM_BLOB(do_time1, &g_do1_time_type, sizeof(g_do1_time_type), g_do1_time_type, sizeof(g_do1_time_type), PARAM_GROUP_DO_TIME, "DO1 timer config (type)"),
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static rt_mutex_t mgr_mutex = RT_NULL;
static void mt_lock_fn(fdb_db_t db)
{
    (void)db;
    if (mgr_mutex) {
        rt_mutex_take(mgr_mutex, RT_WAITING_FOREVER);
    }
}

static void mt_unlock_fn(fdb_db_t db)
{
    (void)db;
    if (mgr_mutex) {
        rt_mutex_release(mgr_mutex);
    }
}

int fdb_kvdc_params(void)
{

    int ret;
    mgr_mutex = rt_mutex_create("mgr_mutex", RT_IPC_FLAG_FIFO);
    if (mgr_mutex == RT_NULL) {
        return -RT_ERROR;
    }
    ret = flash_param_init(&mgr, "env", "fdb_kvdb1", 4096, 4096 * 4, g_items, ARRAY_SIZE(g_items), mt_lock_fn, mt_unlock_fn);
    if (ret != 0) {
        LOG_D("init failed (%d)\n", ret);
        rt_mutex_delete(mgr_mutex);
        return -RT_ERROR;
    }

    flash_param_print(&mgr);

    return RT_EOK;
}

static void perf_test(void){
    static int baud = 1;
    char sn[32] = {0};
    LOG_D("perf_test[%d-%d]",baud,baud+10);
    for(int i=0;i<10;i++){
        baud++;
        flash_param_set(&mgr, "rs485_1_baud", &baud);
        flash_param_set(&mgr, "rs485_2_baud", &baud);
        rt_snprintf(sn,32,"TEST-%04d",baud);
        flash_param_set(&mgr, "SN", sn);
    }
    flash_param_print(&mgr);
}
MSH_CMD_EXPORT(perf_test,perf_test);
