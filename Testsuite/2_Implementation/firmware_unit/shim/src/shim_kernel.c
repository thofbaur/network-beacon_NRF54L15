#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

/* ---- printk capture ---------------------------------------------------- */
#define SHIM_LOG_CAPACITY (256 * 1024)
static char log_buffer[SHIM_LOG_CAPACITY];
static size_t log_len;

void printk(const char *fmt, ...)
{
	char line[1024];
	va_list args;
	int written;

	va_start(args, fmt);
	written = vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	if (written < 0) {
		return;
	}
	if ((size_t)written >= sizeof(line)) {
		written = (int)sizeof(line) - 1;
	}

	if (getenv("SHIM_VERBOSE")) {
		fputs(line, stdout);
	}

	if (log_len + (size_t)written + 1 >= SHIM_LOG_CAPACITY) {
		/* Drop the older half so recent output stays searchable. */
		size_t keep = log_len / 2;

		memmove(log_buffer, log_buffer + (log_len - keep), keep);
		log_len = keep;
	}
	memcpy(log_buffer + log_len, line, (size_t)written);
	log_len += (size_t)written;
	log_buffer[log_len] = '\0';
}

void shim_log_clear(void)
{
	log_len = 0;
	log_buffer[0] = '\0';
}

bool shim_log_contains(const char *needle)
{
	return strstr(log_buffer, needle) != NULL;
}

const char *shim_log_text(void)
{
	return log_buffer;
}

/* ---- random (deterministic) -------------------------------------------- */
static uint32_t rand_state = 0x12345678U;

uint32_t sys_rand32_get(void)
{
	rand_state = rand_state * 1664525U + 1013904223U;
	return rand_state;
}

uint8_t sys_rand8_get(void)
{
	return (uint8_t)(sys_rand32_get() >> 24);
}

void shim_rand_seed(uint32_t seed)
{
	rand_state = seed;
}

/* ---- time -------------------------------------------------------------- */
static int64_t uptime_ms;

int64_t k_uptime_get(void)
{
	return uptime_ms;
}

int64_t k_uptime_seconds(void)
{
	return uptime_ms / 1000;
}

int32_t k_sleep(k_timeout_t timeout)
{
	/* Only moves the clock; never runs work re-entrantly. */
	if (timeout.ms > 0) {
		uptime_ms += timeout.ms;
	}
	return 0;
}

void shim_set_uptime_ms(int64_t ms)
{
	uptime_ms = ms;
}

/* ---- work items -------------------------------------------------------- */
#define SHIM_MAX_WORK 256

static struct k_work *work_queue[SHIM_MAX_WORK];
static size_t work_queue_len;
static struct k_work_delayable *delayed[SHIM_MAX_WORK];
static size_t delayed_len;

static void delayed_register(struct k_work_delayable *dwork)
{
	for (size_t i = 0; i < delayed_len; i++) {
		if (delayed[i] == dwork) {
			return;
		}
	}
	if (delayed_len >= SHIM_MAX_WORK) {
		fprintf(stderr, "shim: too many delayable work items\n");
		abort();
	}
	delayed[delayed_len++] = dwork;
}

void k_work_init(struct k_work *work, k_work_handler_t handler)
{
	work->handler = handler;
	work->queued = false;
}

void k_work_init_delayable(struct k_work_delayable *dwork,
			   k_work_handler_t handler)
{
	dwork->work.handler = handler;
	dwork->work.queued = false;
	dwork->scheduled = false;
	dwork->due_ms = 0;
}

struct k_work_delayable *k_work_delayable_from_work(struct k_work *work)
{
	return CONTAINER_OF(work, struct k_work_delayable, work);
}

int k_work_submit(struct k_work *work)
{
	if (work->queued) {
		return 0;
	}
	if (work_queue_len >= SHIM_MAX_WORK) {
		fprintf(stderr, "shim: work queue overflow\n");
		abort();
	}
	work->queued = true;
	work_queue[work_queue_len++] = work;
	return 1;
}

int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work)
{
	ARG_UNUSED(queue);
	return k_work_submit(work);
}

int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	int64_t delay_ms = delay.ms < 0 ? 0 : delay.ms;

	dwork->due_ms = uptime_ms + delay_ms;
	dwork->scheduled = true;
	delayed_register(dwork);
	return 1;
}

int k_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	/* Zephyr: no effect when the item is already scheduled. */
	if (dwork->scheduled) {
		return 0;
	}
	return k_work_reschedule(dwork, delay);
}

int k_work_reschedule_for_queue(struct k_work_q *queue,
				struct k_work_delayable *dwork,
				k_timeout_t delay)
{
	ARG_UNUSED(queue);
	return k_work_reschedule(dwork, delay);
}

int k_work_cancel_delayable(struct k_work_delayable *dwork)
{
	dwork->scheduled = false;
	return 0;
}

bool k_work_delayable_is_pending(const struct k_work_delayable *dwork)
{
	return dwork->scheduled;
}

bool shim_work_is_scheduled(const struct k_work_delayable *dwork)
{
	return dwork->scheduled;
}

int64_t shim_work_due_ms(const struct k_work_delayable *dwork)
{
	return dwork->due_ms;
}

void k_work_queue_start(struct k_work_q *queue, void *stack, size_t size,
			int prio, const void *cfg)
{
	ARG_UNUSED(queue);
	ARG_UNUSED(stack);
	ARG_UNUSED(size);
	ARG_UNUSED(prio);
	ARG_UNUSED(cfg);
}

static int run_one_round(void)
{
	int ran = 0;

	/* Immediate work, FIFO. Handlers may submit more work. */
	while (work_queue_len > 0) {
		struct k_work *work = work_queue[0];

		memmove(&work_queue[0], &work_queue[1],
			(work_queue_len - 1) * sizeof(work_queue[0]));
		work_queue_len--;
		work->queued = false;
		work->handler(work);
		ran++;
	}

	/* Expired delayable work, earliest due first. */
	for (;;) {
		struct k_work_delayable *next = NULL;

		for (size_t i = 0; i < delayed_len; i++) {
			struct k_work_delayable *d = delayed[i];

			if (d->scheduled && d->due_ms <= uptime_ms &&
			    (!next || d->due_ms < next->due_ms)) {
				next = d;
			}
		}
		if (!next) {
			break;
		}
		next->scheduled = false;
		next->work.handler(&next->work);
		ran++;
		if (work_queue_len > 0) {
			break; /* let newly submitted work run in order */
		}
	}

	return ran;
}

int shim_run_ready_work(void)
{
	int total = 0;
	int rounds = 0;

	for (;;) {
		int ran = run_one_round();

		total += ran;
		if (ran == 0) {
			break;
		}
		if (++rounds > 1000000) {
			fprintf(stderr, "shim: work did not settle\n");
			abort();
		}
	}
	return total;
}

void shim_advance_ms(int64_t ms)
{
	int64_t target = uptime_ms + ms;

	shim_run_ready_work();
	for (;;) {
		struct k_work_delayable *next = NULL;

		for (size_t i = 0; i < delayed_len; i++) {
			struct k_work_delayable *d = delayed[i];

			if (d->scheduled && d->due_ms <= target &&
			    (!next || d->due_ms < next->due_ms)) {
				next = d;
			}
		}
		if (!next) {
			break;
		}
		if (next->due_ms > uptime_ms) {
			uptime_ms = next->due_ms;
		}
		shim_run_ready_work();
	}
	uptime_ms = target;
	shim_run_ready_work();
}

void shim_work_reset(void)
{
	for (size_t i = 0; i < work_queue_len; i++) {
		work_queue[i]->queued = false;
	}
	work_queue_len = 0;
	for (size_t i = 0; i < delayed_len; i++) {
		delayed[i]->scheduled = false;
	}
	delayed_len = 0;
}

/* ---- semaphore --------------------------------------------------------- */
int k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	if (sem->count == 0) {
		return -EAGAIN;
	}
	sem->count--;
	return 0;
}

void k_sem_give(struct k_sem *sem)
{
	if (sem->count < sem->limit) {
		sem->count++;
	}
}

void k_sem_reset(struct k_sem *sem)
{
	sem->count = 0;
}

/* ---- message queue ----------------------------------------------------- */
int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout)
{
	uint32_t slot;

	ARG_UNUSED(timeout);
	if (q->used >= q->max_msgs) {
		return -ENOMSG; /* Zephyr: full queue with K_NO_WAIT */
	}
	slot = (q->read_index + q->used) % q->max_msgs;
	memcpy(q->buffer + (size_t)slot * q->msg_size, data, q->msg_size);
	q->used++;
	return 0;
}

int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	if (q->used == 0) {
		return -ENOMSG;
	}
	memcpy(data, q->buffer + (size_t)q->read_index * q->msg_size,
	       q->msg_size);
	q->read_index = (q->read_index + 1) % q->max_msgs;
	q->used--;
	return 0;
}

uint32_t k_msgq_num_used_get(struct k_msgq *q)
{
	return q->used;
}
