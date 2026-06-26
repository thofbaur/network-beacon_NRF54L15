#include "app.h"

#include <stdio.h>
#include "app_error.h"
#include "app_timer.h"
#include "button.h"
#include "console_output.h"
#include "led.h"
#include "nus.h"
#include "radio.h"
#include "softdevice_handler.h"

#define APP_TIMER_PRESCALER_VALUE 0
#define APP_TIMER_OP_QUEUE_SIZE_VALUE 8

void app_ble_evt_dispatch(ble_evt_t *p_ble_evt)
{
    radio_on_ble_evt(p_ble_evt);
    button_on_ble_evt(p_ble_evt);
    nus_on_ble_evt(p_ble_evt);
}

void app_transfer_finished(void)
{
    radio_disconnect_current();
}

void app_init(void)
{
    APP_TIMER_INIT(APP_TIMER_PRESCALER_VALUE, APP_TIMER_OP_QUEUE_SIZE_VALUE, NULL);

    console_output_init();
    printf("Network base started\r\n");
    led_init();
    button_init();
    radio_init();
    nus_init();

    radio_scan_start();
}

void app_run(void)
{
    for (;;)
    {
        console_output_process();

        uint32_t err_code = sd_app_evt_wait();
        APP_ERROR_CHECK(err_code);
    }
}
