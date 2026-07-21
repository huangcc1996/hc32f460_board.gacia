/**
 * @file flash_param.c
 * @brief FlashDB 参数管理器实现
 *
 * 实现要点：
 * - 所有公共 API 在访问 RAM + FlashDB 前后统一加锁（如用户提供 lock/unlock 回调）。
 * - 所有写路径采用"DB 先写成功，再更新 RAM"，避免掉电/失败导致 RAM 与 DB 不一致。
 * - BLOB 写入根据 copy_sz / val_len 控制长度，避免将未初始化 RAM 数据落盘。
 * - 解析 INT/FLOAT 时使用 strtol / strtof 并检查 errno / endptr，避免 atoi / atof 静默失败。
 */

#include "flash_param.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>

#define FDB_LOG_TAG "[param]"

/**
 * @brief 按键名查找参数项
 * @param mgr 参数管理器
 * @param key 参数键名
 * @return 匹配项指针，未找到返回 NULL
 */
static param_item_t *_find_item(param_mgr_t *mgr, const char *key)
{
    size_t i;
    for (i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->items[i].key, key) == 0) {
            return &mgr->items[i];
        }
    }
    return NULL;
}

/* ---------- type helpers ---------- */

static int _int_to_str(const void *val, char *buf, size_t buf_sz)
{
    return snprintf(buf, buf_sz, "%d", *(const int *)val);
}

/**
 * @brief 将字符串解析为 int，失败返回 -1
 *
 * 使用 strtol 并检查 errno / endptr，避免 atoi 静默返回 0。
 */
static int _int_from_str(const char *str, void *val)
{
    char *end;
    long v;

    if (!str) return -1;
    errno = 0;
    v = strtol(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0') return -1;
    *(int *)val = (int)v;
    return 0;
}

static int _float_to_str(const void *val, char *buf, size_t buf_sz)
{
    return snprintf(buf, buf_sz, "%.9g", *(const float *)val);
}

/**
 * @brief 将字符串解析为 float，失败返回 -1
 *
 * 使用 strtof 并检查 errno / endptr，避免 atof 静默返回 0。
 */
static int _float_from_str(const char *str, void *val)
{
    char *end;
    float v;

    if (!str) return -1;
    errno = 0;
    v = strtof(str, &end);
    if (errno != 0 || end == str || *end != '\0') return -1;
    *(float *)val = v;
    return 0;
}

static fdb_err_t _int_db_write(fdb_kvdb_t db, const char *key, const void *val)
{
    char buf[32];
    _int_to_str(val, buf, sizeof(buf));
    return fdb_kv_set(db, key, buf);
}

static int _int_db_read(fdb_kvdb_t db, const char *key, void *val)
{
    char *s = fdb_kv_get(db, key);
    if (!s) return -1;
    return _int_from_str(s, val);
}

static fdb_err_t _float_db_write(fdb_kvdb_t db, const char *key, const void *val)
{
    char buf[32];
    _float_to_str(val, buf, sizeof(buf));
    return fdb_kv_set(db, key, buf);
}

static int _float_db_read(fdb_kvdb_t db, const char *key, void *val)
{
    char *s = fdb_kv_get(db, key);
    if (!s) return -1;
    return _float_from_str(s, val);
}

static fdb_err_t _str_db_write(fdb_kvdb_t db, const char *key, const void *val)
{
    return fdb_kv_set(db, key, (const char *)val);
}

static int _str_db_read(fdb_kvdb_t db, const char *key, void *val, size_t size)
{
    char *s = fdb_kv_get(db, key);
    if (!s) return -1;
    strncpy((char *)val, s, size - 1);
    ((char *)val)[size - 1] = '\0';
    return 0;
}

static fdb_err_t _blob_db_write(fdb_kvdb_t db, const char *key, const void *val, size_t size)
{
    struct fdb_blob blob;
    return fdb_kv_set_blob(db, key, fdb_blob_make(&blob, val, size));
}

/**
 * @brief 从 KVDB 读取 BLOB
 * @return 0 成功（blob.saved.len > 0），-1 失败或长度为 0
 */
static int _blob_db_read(fdb_kvdb_t db, const char *key, void *val, size_t size)
{
    struct fdb_blob blob;
    fdb_kv_get_blob(db, key, fdb_blob_make(&blob, val, size));
    return (blob.saved.len > 0) ? 0 : -1;
}

/**
 * @brief 将单个参数从 FlashDB 同步到 RAM
 *
 * 读取顺序：
 * 1. 尝试从 DB 读取；
 * 2. 若读取失败，将默认值写入 DB；
 * 3. 仅当 DB 写入成功后，才更新 RAM。
 *
 * @param mgr  参数管理器
 * @param item 参数项
 * @return 0 成功，-1 失败
 */
static int _sync_one(param_mgr_t *mgr, param_item_t *item)
{
    fdb_kvdb_t db = &mgr->kvdb;

    switch (item->type) {
    case PARAM_TYPE_INT:
        if (_int_db_read(db, item->key, item->ram_ptr) < 0) {
            if (_int_db_write(db, item->key, item->def_val) != FDB_NO_ERR) return -1;
            memcpy(item->ram_ptr, item->def_val, sizeof(int));
        }
        break;
    case PARAM_TYPE_FLOAT:
        if (_float_db_read(db, item->key, item->ram_ptr) < 0) {
            if (_float_db_write(db, item->key, item->def_val) != FDB_NO_ERR) return -1;
            memcpy(item->ram_ptr, item->def_val, sizeof(float));
        }
        break;
    case PARAM_TYPE_STR:
        if (_str_db_read(db, item->key, item->ram_ptr, item->ram_size) < 0) {
            if (_str_db_write(db, item->key, item->def_val) != FDB_NO_ERR) return -1;
            strncpy((char *)item->ram_ptr, (const char *)item->def_val, item->ram_size - 1);
            ((char *)item->ram_ptr)[item->ram_size - 1] = '\0';
        }
        break;
    case PARAM_TYPE_BLOB:
        if (_blob_db_read(db, item->key, item->ram_ptr, item->ram_size) < 0) {
            if (_blob_db_write(db, item->key, item->def_val, item->def_size) != FDB_NO_ERR) return -1;
            memset(item->ram_ptr, 0, item->ram_size);
            memcpy(item->ram_ptr, item->def_val,
                   item->def_size < item->ram_size ? item->def_size : item->ram_size);
        }
        break;
    default:
        return -1;
    }
    return 0;
}

/* ---------- public API ---------- */

/**
 * @brief 初始化参数管理器
 *
 * 校验内容：
 * - lock / unlock 成对出现；
 * - key / ram_ptr 非空；
 * - type 在合法范围内；
 * - ram_size 满足各类型最小要求；
 * - STR / BLOB 的 def_val 非空；
 * - key 全局唯一。
 *
 * def_nodes / def_kv 注册给 FlashDB 后需保持有效至 deinit。
 *
 * @param mgr       参数管理器指针
 * @param name      KVDB 名称
 * @param path      存储路径
 * @param sec_size  扇区大小，0 使用默认值
 * @param max_size  最大容量，0 使用默认值
 * @param items     参数表数组
 * @param count     参数表项数
 * @param lock      可选加锁回调
 * @param unlock    可选解锁回调，必须与 lock 成对
 * @return 0 成功，-1 失败
 */
int flash_param_init(param_mgr_t *mgr, const char *name, const char *path,
                     uint32_t sec_size, uint32_t max_size,
                     param_item_t *items, size_t count,
                     void (*lock)(fdb_db_t), void (*unlock)(fdb_db_t))
{
    fdb_err_t err;
    size_t i, j;

    if (!mgr || !items || count == 0) return -1;

    /* lock/unlock must be both provided or both NULL */
    if ((lock != NULL) != (unlock != NULL)) return -1;

    /* validate each item */
    for (i = 0; i < count; i++) {
        if (!items[i].key || !items[i].ram_ptr) return -1;
        if (items[i].type < PARAM_TYPE_INT || items[i].type > PARAM_TYPE_BLOB) return -1;
        switch (items[i].type) {
        case PARAM_TYPE_INT:
            if (items[i].ram_size < sizeof(int)) return -1;
            break;
        case PARAM_TYPE_FLOAT:
            if (items[i].ram_size < sizeof(float)) return -1;
            break;
        case PARAM_TYPE_STR:
        case PARAM_TYPE_BLOB:
            if (items[i].ram_size == 0) return -1;
            if (!items[i].def_val) return -1;
            break;
        }
    }

    /* check for duplicate keys */
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (strcmp(items[i].key, items[j].key) == 0) return -1;
        }
    }

    memset(mgr, 0, sizeof(*mgr));
    mgr->items = items;
    mgr->count = count;

    mgr->def_nodes = (struct fdb_default_kv_node *)PARAM_MALLOC(
        count * sizeof(struct fdb_default_kv_node));
    if (!mgr->def_nodes) return -1;

    for (i = 0; i < count; i++) {
        mgr->def_nodes[i].key       = (char *)items[i].key;
        mgr->def_nodes[i].value     = (void *)items[i].def_val;
        mgr->def_nodes[i].value_len = items[i].def_size;
    }
    mgr->def_kv.kvs = mgr->def_nodes;
    mgr->def_kv.num = count;

#ifdef FDB_USING_FILE_MODE
    {
        bool file_mode = true;
        fdb_kvdb_control(&mgr->kvdb, FDB_KVDB_CTRL_SET_FILE_MODE, &file_mode);
    }
#endif
    if (sec_size > 0) {
        fdb_kvdb_control(&mgr->kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec_size);
    }
    if (max_size > 0) {
        fdb_kvdb_control(&mgr->kvdb, FDB_KVDB_CTRL_SET_MAX_SIZE, &max_size);
    }

    err = fdb_kvdb_init(&mgr->kvdb, name, path, &mgr->def_kv, NULL);
    if (err != FDB_NO_ERR) {
        PARAM_FREE(mgr->def_nodes);
        mgr->def_nodes = NULL;
        return -1;
    }

    mgr->lock = lock;
    mgr->unlock = unlock;

    for (i = 0; i < count; i++) {
        if (_sync_one(mgr, &items[i]) != 0) {
            fdb_kvdb_deinit(&mgr->kvdb);
            PARAM_FREE(mgr->def_nodes);
            mgr->def_nodes = NULL;
            return -1;
        }
    }

    mgr->init_ok = 1;
    return 0;
}

/**
 * @brief 反初始化参数管理器
 * @param mgr 参数管理器指针
 * @return 0 成功，-1 失败
 */
int flash_param_deinit(param_mgr_t *mgr)
{
    if (!mgr || !mgr->init_ok) return -1;

    fdb_kvdb_deinit(&mgr->kvdb);
    if (mgr->def_nodes) {
        PARAM_FREE(mgr->def_nodes);
        mgr->def_nodes = NULL;
    }
    mgr->init_ok = 0;
    return 0;
}

/**
 * @brief 设置参数值
 *
 * 写顺序：先写 FlashDB，成功后再更新 RAM。
 * 对 BLOB 使用 min(ram_size, def_size) 作为拷贝/写入长度。
 *
 * @param mgr 参数管理器
 * @param key 参数键名
 * @param val 待写入值指针
 * @return 0 成功，-1 失败
 */
int flash_param_set(param_mgr_t *mgr, const char *key, const void *val)
{
    param_item_t *item;
    fdb_kvdb_t db;
    int ret = -1;

    if (!mgr || !mgr->init_ok || !key || !val) return -1;
    item = _find_item(mgr, key);
    if (!item) return -1;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    db = &mgr->kvdb;

    switch (item->type) {
    case PARAM_TYPE_INT:
        ret = (_int_db_write(db, key, val) == FDB_NO_ERR) ? 0 : -1;
        if (ret == 0) *(int *)item->ram_ptr = *(const int *)val;
        break;
    case PARAM_TYPE_FLOAT:
        ret = (_float_db_write(db, key, val) == FDB_NO_ERR) ? 0 : -1;
        if (ret == 0) *(float *)item->ram_ptr = *(const float *)val;
        break;
    case PARAM_TYPE_STR:
        ret = (_str_db_write(db, key, val) == FDB_NO_ERR) ? 0 : -1;
        if (ret == 0) {
            strncpy((char *)item->ram_ptr, (const char *)val, item->ram_size - 1);
            ((char *)item->ram_ptr)[item->ram_size - 1] = '\0';
        }
        break;
    case PARAM_TYPE_BLOB:
    {
        size_t copy_sz = item->ram_size < item->def_size ? item->ram_size : item->def_size;
        ret = (_blob_db_write(db, key, val, copy_sz) == FDB_NO_ERR) ? 0 : -1;
        if (ret == 0) {
            memset(item->ram_ptr, 0, item->ram_size);
            memcpy(item->ram_ptr, val, copy_sz);
        }
        break;
    }
    default:
        ret = -1;
        break;
    }

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
    return ret;
}

/**
 * @brief 设置 BLOB 参数（支持可变长度）
 *
 * 允许写入长度最大为 ram_size 的任意 BLOB。
 *
 * @param mgr     参数管理器
 * @param key     参数键名
 * @param val     待写入数据指针
 * @param val_len 待写入数据长度（> 0 且 <= ram_size）
 * @return 0 成功，-1 失败
 *
 * @warning val 不应与 item->ram_ptr 相同，否则自覆盖导致数据全零。
 */
int flash_param_set_blob(param_mgr_t *mgr, const char *key,
                          const void *val, size_t val_len)
{
    param_item_t *item;
    fdb_kvdb_t db;
    int ret = -1;

    if (!mgr || !mgr->init_ok || !key || !val || val_len == 0) return -1;
    item = _find_item(mgr, key);
    if (!item || item->type != PARAM_TYPE_BLOB) return -1;
    if (val_len > item->ram_size) return -1;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    db = &mgr->kvdb;

    ret = (_blob_db_write(db, key, val, val_len) == FDB_NO_ERR) ? 0 : -1;
    if (ret == 0) {
        memcpy(item->ram_ptr, val, val_len);
        if (val_len < item->ram_size) {
            memset((uint8_t *)item->ram_ptr + val_len, 0, item->ram_size - val_len);
        }
    }

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
    return ret;
}

/**
 * @brief 获取参数当前 RAM 值
 *
 * 若 *size < ram_size，设置 *size = ram_size 后返回 -1。
 *
 * @param mgr  参数管理器
 * @param key  参数键名
 * @param buf  输出缓冲区
 * @param size [in/out] 输入为缓冲区大小，输出为实际数据大小
 * @return 0 成功，-1 失败
 */
int flash_param_get(param_mgr_t *mgr, const char *key, void *buf, size_t *size)
{
    param_item_t *item;
    int ret = -1;

    if (!mgr || !mgr->init_ok || !key || !buf || !size) return -1;
    item = _find_item(mgr, key);
    if (!item) return -1;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    if (*size < item->ram_size) {
        *size = item->ram_size;
        ret = -1;
    } else {
        memcpy(buf, item->ram_ptr, item->ram_size);
        *size = item->ram_size;
        ret = 0;
    }

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
    return ret;
}

/**
 * @brief 内部重置单个参数到默认值（调用者必须已持有 mgr->lock）
 *
 * 写顺序：先写 FlashDB，成功后再更新 RAM。
 *
 * @param mgr  参数管理器
 * @param item 参数项
 * @return 0 成功，-1 失败
 */
static int _reset_one_locked(param_mgr_t *mgr, param_item_t *item)
{
    fdb_kvdb_t db = &mgr->kvdb;

    switch (item->type) {
    case PARAM_TYPE_INT:
        if (_int_db_write(db, item->key, item->def_val) != FDB_NO_ERR) return -1;
        memcpy(item->ram_ptr, item->def_val, sizeof(int));
        return 0;
    case PARAM_TYPE_FLOAT:
        if (_float_db_write(db, item->key, item->def_val) != FDB_NO_ERR) return -1;
        memcpy(item->ram_ptr, item->def_val, sizeof(float));
        return 0;
    case PARAM_TYPE_STR:
        if (_str_db_write(db, item->key, item->def_val) != FDB_NO_ERR) return -1;
        strncpy((char *)item->ram_ptr, (const char *)item->def_val, item->ram_size - 1);
        ((char *)item->ram_ptr)[item->ram_size - 1] = '\0';
        return 0;
    case PARAM_TYPE_BLOB:
        if (_blob_db_write(db, item->key, item->def_val, item->def_size) != FDB_NO_ERR) return -1;
        memset(item->ram_ptr, 0, item->ram_size);
        memcpy(item->ram_ptr, item->def_val,
               item->def_size < item->ram_size ? item->def_size : item->ram_size);
        return 0;
    default:
        return -1;
    }
}

/**
 * @brief 重置单个参数到默认值
 * @param mgr 参数管理器
 * @param key 参数键名
 * @return 0 成功，-1 失败
 */
int flash_param_reset(param_mgr_t *mgr, const char *key)
{
    param_item_t *item;
    int ret = -1;

    if (!mgr || !mgr->init_ok || !key) return -1;
    item = _find_item(mgr, key);
    if (!item) return -1;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);
    ret = _reset_one_locked(mgr, item);
    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
    return ret;
}

/**
 * @brief 重置指定分组所有参数到默认值
 * @param mgr   参数管理器
 * @param group 分组
 * @return 0 全部成功，-1 任意一项失败
 */
int flash_param_group_reset(param_mgr_t *mgr, param_group_t group)
{
    size_t i;
    int ret = 0;

    if (!mgr || !mgr->init_ok) return -1;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    for (i = 0; i < mgr->count; i++) {
        if (mgr->items[i].group == group) {
            if (_reset_one_locked(mgr, &mgr->items[i]) != 0) ret = -1;
        }
    }

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
    return ret;
}

/**
 * @brief 重置所有参数到默认值
 *
 * 依赖 fdb_kv_set_default 将 KVDB 恢复为默认表，再通过 _sync_one 同步到 RAM。
 *
 * @param mgr 参数管理器
 * @return 0 全部成功，-1 任意一项失败
 *
 * @note 若 fdb_kv_set_default 未覆盖某 key，_sync_one 可能读回旧值而非默认值。
 */
int flash_param_reset_all(param_mgr_t *mgr)
{
    size_t i;
    int ret = 0;

    if (!mgr || !mgr->init_ok) return -1;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    fdb_kv_set_default(&mgr->kvdb);

    for (i = 0; i < mgr->count; i++) {
        if (_sync_one(mgr, &mgr->items[i]) != 0) ret = -1;
    }

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
    return ret;
}

/**
 * @brief 遍历指定分组的所有参数
 *
 * @param mgr   参数管理器
 * @param group 分组
 * @param cb    回调函数
 * @param arg   回调用户参数
 *
 * @warning 回调在 mgr->lock 保护下执行，禁止在回调中调用其它 flash_param_* API（自死锁）。
 */
void flash_param_foreach(param_mgr_t *mgr, param_group_t group,
                         void (*cb)(param_item_t *item, void *arg), void *arg)
{
    size_t i;

    if (!mgr || !cb) return;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    for (i = 0; i < mgr->count; i++) {
        if (mgr->items[i].group == group) {
            cb(&mgr->items[i], arg);
        }
    }

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
}

/**
 * @brief 打印所有参数信息
 *
 * 同时调用 fdb_kv_print 打印底层 KVDB 信息。
 * value-to-string 逻辑与 flash_param_print_group 重复，后续可抽取为 _format_item_value。
 *
 * @param mgr 参数管理器
 */
void flash_param_print(param_mgr_t *mgr)
{
    size_t i;
    char buf[128];

    if (!mgr || !mgr->init_ok) return;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    FDB_INFO("============= All Parameters (%zu) =============\n", mgr->count);
    for (i = 0; i < mgr->count; i++) {
        const param_item_t *item = &mgr->items[i];
        switch (item->type) {
        case PARAM_TYPE_INT:
            snprintf(buf, sizeof(buf), "%d", *(const int *)item->ram_ptr);
            break;
        case PARAM_TYPE_FLOAT:
            snprintf(buf, sizeof(buf), "%.9g", *(const float *)item->ram_ptr);
            break;
        case PARAM_TYPE_STR:
            strncpy(buf, (const char *)item->ram_ptr, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            break;
        case PARAM_TYPE_BLOB:
            snprintf(buf, sizeof(buf), "<blob:%zu bytes>", item->ram_size);
            break;
        default:
            snprintf(buf, sizeof(buf), "<unknown>");
            break;
        }
        FDB_INFO("[%d] %-24s = %-32s | group:%d %s\n",
                 (int)i, item->key, buf, (int)item->group,
                 item->desc ? item->desc : "");
    }
    FDB_INFO("=================================================\n");

    fdb_kv_print(&mgr->kvdb);

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
}

/**
 * @brief 打印指定分组参数信息
 * @param mgr   参数管理器
 * @param group 分组
 */
void flash_param_print_group(param_mgr_t *mgr, param_group_t group)
{
    size_t i;
    char buf[128];

    if (!mgr || !mgr->init_ok) return;

    if (mgr->lock) mgr->lock((fdb_db_t)&mgr->kvdb);

    FDB_INFO("========== Group %d Parameters ==========\n", (int)group);
    for (i = 0; i < mgr->count; i++) {
        const param_item_t *item = &mgr->items[i];
        if (item->group != group) continue;

        switch (item->type) {
        case PARAM_TYPE_INT:
            snprintf(buf, sizeof(buf), "%d", *(const int *)item->ram_ptr);
            break;
        case PARAM_TYPE_FLOAT:
            snprintf(buf, sizeof(buf), "%.9g", *(const float *)item->ram_ptr);
            break;
        case PARAM_TYPE_STR:
            strncpy(buf, (const char *)item->ram_ptr, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            break;
        case PARAM_TYPE_BLOB:
            snprintf(buf, sizeof(buf), "<blob:%zu bytes>", item->ram_size);
            break;
        default:
            snprintf(buf, sizeof(buf), "<unknown>");
            break;
        }
        FDB_INFO("  %-24s = %-32s | %s\n", item->key, buf,
                 item->desc ? item->desc : "");
    }

    if (mgr->unlock) mgr->unlock((fdb_db_t)&mgr->kvdb);
}
