#ifndef STORAGE_WORK_QUEUE_H
#define STORAGE_WORK_QUEUE_H

#include <zephyr/kernel.h>

int storage_work_queue_init(void);
int storage_work_submit(struct k_work *work);
int storage_work_reschedule(struct k_work_delayable *work, k_timeout_t delay);

#endif /* STORAGE_WORK_QUEUE_H */
