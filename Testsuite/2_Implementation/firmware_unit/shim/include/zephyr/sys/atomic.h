#ifndef SHIM_ZEPHYR_SYS_ATOMIC_H
#define SHIM_ZEPHYR_SYS_ATOMIC_H

#include "shim_common.h"

/* Single-threaded host shim: plain reads/writes are sufficient. */
typedef long atomic_t;
typedef long atomic_val_t;

#define ATOMIC_INIT(i) (i)

static inline atomic_val_t atomic_get(const atomic_t *target)
{
	return *target;
}

static inline atomic_val_t atomic_set(atomic_t *target, atomic_val_t value)
{
	atomic_val_t old = *target;

	*target = value;
	return old;
}

static inline atomic_val_t atomic_or(atomic_t *target, atomic_val_t value)
{
	atomic_val_t old = *target;

	*target = old | value;
	return old;
}

static inline atomic_val_t atomic_and(atomic_t *target, atomic_val_t value)
{
	atomic_val_t old = *target;

	*target = old & value;
	return old;
}

static inline bool atomic_cas(atomic_t *target, atomic_val_t old_value,
			      atomic_val_t new_value)
{
	if (*target != old_value) {
		return false;
	}
	*target = new_value;
	return true;
}

#endif /* SHIM_ZEPHYR_SYS_ATOMIC_H */
