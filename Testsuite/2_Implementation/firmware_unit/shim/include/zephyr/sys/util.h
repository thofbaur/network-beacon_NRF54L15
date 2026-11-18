#ifndef SHIM_ZEPHYR_SYS_UTIL_H
#define SHIM_ZEPHYR_SYS_UTIL_H

#include "shim_common.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#define BIT(n) (1UL << (n))
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define CONTAINER_OF(ptr, type, field) \
	((type *)(((char *)(ptr)) - offsetof(type, field)))

#define USEC_PER_MSEC 1000U
#define MSEC_PER_SEC 1000U

/* Zephyr's IS_ENABLED(): 1 when the macro is defined to 1, else 0. */
#define Z_IS_ENABLED_XXXX1 Z_IS_ENABLED_YYYY,
#define IS_ENABLED(config_macro) Z_IS_ENABLED1(config_macro)
#define Z_IS_ENABLED1(config_macro) Z_IS_ENABLED2(Z_IS_ENABLED_XXXX##config_macro)
#define Z_IS_ENABLED2(one_or_two_args) Z_IS_ENABLED3(one_or_two_args 1, 0)
#define Z_IS_ENABLED3(ignore_this, val, ...) val

#endif /* SHIM_ZEPHYR_SYS_UTIL_H */
