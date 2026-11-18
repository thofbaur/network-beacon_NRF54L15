/*
 * Common toolchain macros of the Zephyr host shim.
 *
 * The shim lets the unmodified firmware sources (Network_Beacon_nrf54/src,
 * Network_Base_nrf54/src) compile and run as plain host executables. It only
 * provides the subset of the Zephyr API those sources use, with semantics
 * that mirror Zephyr where the firmware depends on them (documented per
 * function in the shim sources).
 */
#ifndef SHIM_COMMON_H
#define SHIM_COMMON_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#ifndef ENOTSUP
#define ENOTSUP 134
#endif
#ifndef EBADMSG
#define EBADMSG 77
#endif
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef ENOMSG
#define ENOMSG 42
#endif
#ifndef EALREADY
#define EALREADY 120
#endif
#ifndef ECONNRESET
#define ECONNRESET 108
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT 138
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 115
#endif
#ifndef ENOTCONN
#define ENOTCONN 126
#endif
#ifndef EDEADLK
#define EDEADLK 36
#endif

#define BUILD_ASSERT(EXPR, ...) _Static_assert((EXPR), "" __VA_ARGS__)

#define ARG_UNUSED(x) (void)(x)

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#ifndef __maybe_unused
#define __maybe_unused __attribute__((__unused__))
#endif
#ifndef __used
#define __used __attribute__((__used__))
#endif

#define __ASSERT(cond, ...) ((void)0)
#define __ASSERT_NO_MSG(cond) ((void)0)
#define __ASSERT_MSG_INFO(...) ((void)0)

#endif /* SHIM_COMMON_H */
