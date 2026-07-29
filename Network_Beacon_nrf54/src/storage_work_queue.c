#include <stdbool.h>

#include <zephyr/kernel.h>

#include "storage_work_queue.h"

#define STORAGE_WORK_STACK_SIZE 1536
#define STORAGE_WORK_PRIORITY 10

K_THREAD_STACK_DEFINE(storage_work_stack, STORAGE_WORK_STACK_SIZE);
static struct k_work_q storage_work_q;
static bool initialized;
static K_MUTEX_DEFINE(init_lock);

int storage_work_queue_init(void)
{
	k_mutex_lock(&init_lock, K_FOREVER);
	if (!initialized) {
		k_work_queue_start(&storage_work_q, storage_work_stack,
				   K_THREAD_STACK_SIZEOF(storage_work_stack),
				   STORAGE_WORK_PRIORITY, NULL);
		initialized = true;
	}
	k_mutex_unlock(&init_lock);
	return 0;
}

int storage_work_submit(struct k_work *work)
{
	storage_work_queue_init();
	return k_work_submit_to_queue(&storage_work_q, work);
}

int storage_work_reschedule(struct k_work_delayable *work, k_timeout_t delay)
{
	storage_work_queue_init();
	return k_work_reschedule_for_queue(&storage_work_q, work, delay);
}
