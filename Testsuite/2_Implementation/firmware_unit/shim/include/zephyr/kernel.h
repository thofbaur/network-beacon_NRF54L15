/*
 * Host shim of the Zephyr kernel API used by the DSA firmware.
 *
 * Execution model (deterministic, single-threaded):
 *   - Time only advances when a test calls shim_advance_ms().
 *   - Submitted work items and delayable work items whose delay has expired
 *     run only when the test calls shim_run_ready_work() (or while
 *     shim_advance_ms() moves time forward). Nothing runs "behind the back"
 *     of a test, which makes flush/export interleavings reproducible.
 *   - Mutexes are no-ops; races between threads cannot be reproduced here
 *     (concurrency is covered by hardware test H8).
 */
#ifndef SHIM_ZEPHYR_KERNEL_H
#define SHIM_ZEPHYR_KERNEL_H

#include "shim_common.h"
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* ---- timeouts ---------------------------------------------------------- */
typedef struct {
	int64_t ms; /* -1 == K_FOREVER */
} k_timeout_t;

#define K_NO_WAIT ((k_timeout_t){ .ms = 0 })
#define K_FOREVER ((k_timeout_t){ .ms = -1 })
#define K_MSEC(x) ((k_timeout_t){ .ms = (int64_t)(x) })
#define K_SECONDS(x) ((k_timeout_t){ .ms = (int64_t)(x) * 1000LL })

/* ---- time -------------------------------------------------------------- */
int64_t k_uptime_get(void);
int64_t k_uptime_seconds(void);
int32_t k_sleep(k_timeout_t timeout);

/* Test control: set/advance the fake clock. Advancing runs every work item
 * that becomes due on the way, in due-time order.
 */
void shim_set_uptime_ms(int64_t ms);
void shim_advance_ms(int64_t ms);

/* ---- mutex ------------------------------------------------------------- */
struct k_mutex {
	int lock_count;
};

#define K_MUTEX_DEFINE(name) struct k_mutex name = { 0 }

static inline int k_mutex_init(struct k_mutex *mutex)
{
	mutex->lock_count = 0;
	return 0;
}

static inline int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	mutex->lock_count++;
	return 0;
}

static inline int k_mutex_unlock(struct k_mutex *mutex)
{
	mutex->lock_count--;
	return 0;
}

/* ---- semaphore --------------------------------------------------------- */
struct k_sem {
	unsigned int count;
	unsigned int limit;
};

#define K_SEM_DEFINE(name, initial, max) \
	struct k_sem name = { .count = (initial), .limit = (max) }

int k_sem_take(struct k_sem *sem, k_timeout_t timeout);
void k_sem_give(struct k_sem *sem);
void k_sem_reset(struct k_sem *sem);

/* ---- work items -------------------------------------------------------- */
struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work {
	k_work_handler_t handler;
	bool queued;
};

struct k_work_delayable {
	struct k_work work;
	int64_t due_ms;
	bool scheduled;
};

struct k_work_q {
	int unused;
};

#define K_WORK_DEFINE(name, h) struct k_work name = { .handler = (h) }
#define K_WORK_DELAYABLE_DEFINE(name, h) \
	struct k_work_delayable name = { .work = { .handler = (h) } }

#define K_THREAD_STACK_DEFINE(name, size) static char name[(size)]
#define K_THREAD_STACK_SIZEOF(sym) sizeof(sym)

void k_work_init(struct k_work *work, k_work_handler_t handler);
void k_work_init_delayable(struct k_work_delayable *dwork,
			   k_work_handler_t handler);
struct k_work_delayable *k_work_delayable_from_work(struct k_work *work);
int k_work_submit(struct k_work *work);
int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work);
int k_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay);
int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay);
int k_work_reschedule_for_queue(struct k_work_q *queue,
				struct k_work_delayable *dwork,
				k_timeout_t delay);
int k_work_cancel_delayable(struct k_work_delayable *dwork);
bool k_work_delayable_is_pending(const struct k_work_delayable *dwork);
void k_work_queue_start(struct k_work_q *queue, void *stack, size_t size,
			int prio, const void *cfg);

/* Test control: run submitted work and expired delayable work until idle.
 * Returns the number of handlers that ran.
 */
int shim_run_ready_work(void);
/* Test control: forget all pending work (used between simulated reboots). */
void shim_work_reset(void);
/* Number of currently scheduled delayable items with this handler. */
bool shim_work_is_scheduled(const struct k_work_delayable *dwork);
int64_t shim_work_due_ms(const struct k_work_delayable *dwork);

/* ---- message queue ----------------------------------------------------- */
struct k_msgq {
	char *buffer;
	size_t msg_size;
	uint32_t max_msgs;
	uint32_t read_index;
	uint32_t used;
};

#define K_MSGQ_DEFINE(name, size, max, align)                                \
	static char name##_shim_buf[(size) * (max)];                         \
	struct k_msgq name = { .buffer = name##_shim_buf,                    \
			       .msg_size = (size),                           \
			       .max_msgs = (max) }

int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout);
int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout);
uint32_t k_msgq_num_used_get(struct k_msgq *q);

/* ---- misc -------------------------------------------------------------- */
#define k_is_in_isr() false

#endif /* SHIM_ZEPHYR_KERNEL_H */
