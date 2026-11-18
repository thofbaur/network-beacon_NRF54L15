#ifndef SHIM_ZEPHYR_LOGGING_LOG_H
#define SHIM_ZEPHYR_LOGGING_LOG_H
#include <zephyr/sys/printk.h>
/* LOG_* messages end up in the same capture buffer as printk(). */
#define LOG_MODULE_DECLARE(...) extern int shim_log_module_unused
#define LOG_MODULE_REGISTER(...) extern int shim_log_module_unused
#define LOG_ERR(fmt, ...) printk("<err> " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) printk("<wrn> " fmt "\n", ##__VA_ARGS__)
#define LOG_INF(fmt, ...) printk("<inf> " fmt "\n", ##__VA_ARGS__)
#define LOG_DBG(fmt, ...) printk("<dbg> " fmt "\n", ##__VA_ARGS__)
#endif
