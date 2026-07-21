#ifndef __FLASH_PARAM_H__
#define __FLASH_PARAM_H__

/**
 * @file flash_param.h
 * @brief FlashDB 参数管理器公共头文件
 *
 * 提供基于 FlashDB KVDB 的参数持久化能力，支持 INT / FLOAT / STRING / BLOB 四种类型。
 * 参数表通过 PARAM_INT / PARAM_FLOAT / PARAM_STR / PARAM_BLOB 宏定义，并应在文件作用域
 * 或静态存储期定义，以保证复合字面量 def_val 的生命周期。
 */

#include <flashdb.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 参数数据类型 */
typedef enum {
    PARAM_TYPE_INT   = 0,
    PARAM_TYPE_FLOAT = 1,
    PARAM_TYPE_STR   = 2,
    PARAM_TYPE_BLOB  = 3,
} param_type_t;

/** 参数分组 */
typedef enum {
    PARAM_GROUP_SYSTEM  = 0,
    PARAM_GROUP_COMM    = 1,
    PARAM_GROUP_DI      = 2,
    PARAM_GROUP_DO      = 3,
    PARAM_GROUP_DO_TIME = 4,
    PARAM_GROUP_MAX     = 5,
} param_group_t;

/**
 * @brief 单个参数配置项
 *
 * @note 该结构体数组必须在文件作用域或以静态存储期定义，确保 def_val 中复合字面量
 *       （PARAM_INT / PARAM_FLOAT 生成的 &(int){val} / &(float){val}）不会失效。
 * @note items 数组及其 def_val 的生命周期必须 >= 管理器生命周期。
 */
typedef struct param_item {
    const char   *key;       /**< 参数键名，必须在同一管理器内全局唯一 */
    const void   *def_val;   /**< 默认值指针。INT/FLOAT 由宏生成复合字面量；STR/BLOB 由用户传入 */
    size_t        def_size;  /**< 默认值字节数 */

    param_type_t  type;      /**< 参数类型 */
    void         *ram_ptr;   /**< 运行时 RAM 变量/缓冲区指针 */
    size_t        ram_size;  /**< RAM 缓冲区大小（字节） */

    param_group_t group;     /**< 所属分组 */
    const char   *desc;      /**< 参数描述，可为 NULL */
} param_item_t;

/**
 * @brief 参数管理器上下文
 */
typedef struct param_mgr {
    struct fdb_kvdb            kvdb;       /**< FlashDB KVDB 实例 */
    param_item_t              *items;      /**< 参数表数组指针，生命周期必须 >= 管理器 */
    size_t                     count;      /**< 参数表项数 */
    struct fdb_default_kv      def_kv;     /**< 传递给 FlashDB 的默认 KV 表头 */
    struct fdb_default_kv_node *def_nodes; /**< 默认 KV 节点数组，由 init 动态分配 */
    uint8_t                    init_ok;    /**< 初始化成功标志 */
    void                     (*lock)(fdb_db_t db);   /**< 可选：用户提供的加锁函数 */
    void                     (*unlock)(fdb_db_t db); /**< 可选：用户提供的解锁函数，必须与 lock 成对 */
} param_mgr_t;

/**
 * @defgroup PARAM_MACROS 参数表定义宏
 * @{
 *
 * @note
 * - param_item_t 数组必须在文件作用域或以静态存储期定义。
 * - PARAM_INT / PARAM_FLOAT 使用复合字面量（如 &(int){def_val}），块作用域内会失效。
 * - PARAM_STR 的 def_str 必须是字符串字面量或数组，不能是动态计算出的 char*。
 * - 所有参数键名在同一管理器内必须唯一。
 */

/**
 * @brief 定义一个 INT 型参数
 * @param key     参数键名
 * @param def_val 默认值（值语义）
 * @param ram_var 运行时 RAM 变量（int 类型）
 * @param group   所属分组
 * @param ...     可选描述字符串
 */
#define PARAM_INT(key, def_val, ram_var, group, ...)     \
    { #key, &(int){def_val}, sizeof(int), PARAM_TYPE_INT, \
      &(ram_var), sizeof(ram_var), group, ##__VA_ARGS__ }

/**
 * @brief 定义一个 FLOAT 型参数
 * @param key     参数键名
 * @param def_val 默认值（值语义）
 * @param ram_var 运行时 RAM 变量（float 类型）
 * @param group   所属分组
 * @param ...     可选描述字符串
 */
#define PARAM_FLOAT(key, def_val, ram_var, group, ...)    \
    { #key, &(float){def_val}, sizeof(float), PARAM_TYPE_FLOAT, \
      &(ram_var), sizeof(ram_var), group, ##__VA_ARGS__ }

/**
 * @brief 定义一个 STRING 型参数
 * @note def_str 必须是字符串字面量或数组，不能是指向动态计算字符串的 char*。
 * @param key      参数键名
 * @param def_str  默认字符串（字面量或数组）
 * @param ram_buf  运行时 RAM 缓冲区
 * @param max_len  缓冲区最大容量（字节）
 * @param group    所属分组
 * @param ...      可选描述字符串
 */
#define PARAM_STR(key, def_str, ram_buf, max_len, group, ...) \
    { #key, def_str, (sizeof(def_str) > 1 ? sizeof(def_str) - 1 : 0), \
      PARAM_TYPE_STR, (ram_buf), (max_len), group, ##__VA_ARGS__ }

/**
 * @brief 定义一个 BLOB 型参数
 * @param key     参数键名
 * @param def_val 默认值数据指针
 * @param def_sz  默认值字节数
 * @param ram_var 运行时 RAM 变量
 * @param ram_sz  运行时 RAM 缓冲区大小（字节）
 * @param group   所属分组
 * @param ...     可选描述字符串
 */
#define PARAM_BLOB(key, def_val, def_sz, ram_var, ram_sz, group, ...) \
    { #key, def_val, def_sz, PARAM_TYPE_BLOB,                 \
      &(ram_var), (ram_sz), group, ##__VA_ARGS__ }

/** @} */

/* 内存分配/释放钩子，可按需替换为 rt_malloc/rt_free */
#ifndef PARAM_MALLOC
#define PARAM_MALLOC(sz)  rt_malloc(sz)
#endif
#ifndef PARAM_FREE
#define PARAM_FREE(p)     rt_free(p)
#endif

/* API */

/**
 * @brief 初始化参数管理器
 *
 * 完成参数表校验、FlashDB KVDB 初始化，并将 DB 中已有参数同步到 RAM。
 *
 * 校验内容：
 * - lock / unlock 必须同时提供或同时为空；
 * - key / ram_ptr 非空；
 * - type 在合法范围内；
 * - ram_size 满足各类型最小要求；
 * - STR / BLOB 的 def_val 非空；
 * - key 全局唯一。
 *
 * @param mgr       参数管理器指针
 * @param name      KVDB 名称
 * @param path      存储路径（文件路径或 Flash 分区名）
 * @param sec_size  扇区大小，0 表示使用默认值
 * @param max_size  最大容量，0 表示使用默认值
 * @param items     参数表数组
 * @param count     参数表项数
 * @param lock      可选的加锁回调
 * @param unlock    可选的解锁回调，必须与 lock 同时提供或同时为空
 *
 * @return 0 成功，-1 失败
 *
 * @note items 数组及其 def_val 的生命周期必须 >= 管理器的生命周期。
 * @warning lock 与 unlock 必须成对，否则会导致线程安全状态不一致。
 */
int  flash_param_init(param_mgr_t *mgr, const char *name, const char *path,
                      uint32_t sec_size, uint32_t max_size,
                      param_item_t *items, size_t count,
                      void (*lock)(fdb_db_t), void (*unlock)(fdb_db_t));

/**
 * @brief 反初始化参数管理器
 * @param mgr 参数管理器指针
 * @return 0 成功，-1 失败（未初始化或参数无效）
 */
int  flash_param_deinit(param_mgr_t *mgr);

/**
 * @brief 设置参数值
 *
 * 写顺序：先写 FlashDB，成功后再更新 RAM。
 * 对 BLOB 类型使用 min(ram_size, def_size) 作为拷贝/写入长度；
 * 如需写入超过默认值长度的 BLOB，请使用 flash_param_set_blob。
 *
 * @param mgr 参数管理器
 * @param key 参数键名
 * @param val 待写入值指针
 * @return 0 成功，-1 失败
 */
int  flash_param_set(param_mgr_t *mgr, const char *key, const void *val);

/**
 * @brief 设置 BLOB 类型参数（支持可变长度）
 *
 * 允许写入长度最大为 ram_size 的任意 BLOB。
 *
 * @param mgr     参数管理器
 * @param key     参数键名
 * @param val     待写入数据指针
 * @param val_len 待写入数据长度（必须 > 0 且 <= ram_size）
 *
 * @return 0 成功，-1 失败
 *
 * @warning val 不应与 item->ram_ptr 指向同一块内存，否则会产生自覆盖导致数据全零。
 */
int  flash_param_set_blob(param_mgr_t *mgr, const char *key,
                           const void *val, size_t val_len);

/**
 * @brief 获取参数当前 RAM 值
 *
 * 若 *size < ram_size，设置 *size = ram_size 后返回 -1，方便调用者重新分配缓冲区。
 *
 * @param mgr  参数管理器
 * @param key  参数键名
 * @param buf  输出缓冲区
 * @param size [in/out] 输入时为缓冲区大小，输出时为实际数据大小
 * @return 0 成功，-1 失败
 */
int  flash_param_get(param_mgr_t *mgr, const char *key, void *buf, size_t *size);

/**
 * @brief 重置单个参数到默认值
 * @param mgr 参数管理器
 * @param key 参数键名
 * @return 0 成功，-1 失败
 */
int  flash_param_reset(param_mgr_t *mgr, const char *key);

/**
 * @brief 重置指定分组所有参数到默认值
 * @param mgr   参数管理器
 * @param group 分组
 * @return 0 全部成功，-1 任意一项失败
 */
int  flash_param_group_reset(param_mgr_t *mgr, param_group_t group);

/**
 * @brief 重置所有参数到默认值
 *
 * 依赖 fdb_kv_set_default 将 KVDB 恢复为默认表，再通过 _sync_one 同步到 RAM。
 *
 * @param mgr 参数管理器
 * @return 0 全部成功，-1 任意一项失败
 *
 * @note 若 fdb_kv_set_default 未覆盖某 key（FlashDB 内部行为），_sync_one 可能读回旧值。
 */
int  flash_param_reset_all(param_mgr_t *mgr);

/**
 * @brief 遍历指定分组的所有参数
 *
 * @param mgr   参数管理器
 * @param group 分组
 * @param cb    回调函数
 * @param arg   回调用户参数
 *
 * @warning 回调在 mgr->lock 保护下执行。回调内部禁止调用其它会获取同一把锁的
 *          flash_param_* API（如 set / get / reset / print），否则产生自死锁。
 */
void flash_param_foreach(param_mgr_t *mgr, param_group_t group,
                         void (*cb)(param_item_t *item, void *arg), void *arg);

/**
 * @brief 打印所有参数信息
 *
 * 同时调用 fdb_kv_print 打印底层 KVDB 信息。
 * value-to-string 逻辑与 flash_param_print_group 重复，后续可考虑抽取为 _format_item_value。
 *
 * @param mgr 参数管理器
 */
void flash_param_print(param_mgr_t *mgr);

/**
 * @brief 打印指定分组参数信息
 * @param mgr   参数管理器
 * @param group 分组
 */
void flash_param_print_group(param_mgr_t *mgr, param_group_t group);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_PARAM_H__ */
