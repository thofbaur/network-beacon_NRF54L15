#ifndef SHIM_ZEPHYR_SYS_PRINTK_H
#define SHIM_ZEPHYR_SYS_PRINTK_H

#include <stdarg.h>
#include <stdio.h>

#include "shim_common.h"

/* printk() output is captured in a buffer that tests can search
 * (shim_log_contains()); set SHIM_VERBOSE=1 to also echo it to stdout.
 */
void printk(const char *fmt, ...);
#define snprintk snprintf
#define vsnprintk vsnprintf

void shim_log_clear(void);
bool shim_log_contains(const char *needle);
const char *shim_log_text(void);

#endif /* SHIM_ZEPHYR_SYS_PRINTK_H */
