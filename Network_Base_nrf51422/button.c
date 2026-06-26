#include "button.h"

#include <stdio.h>
#include "app_error.h"
#include "app_timer.h"
#include "bsp.h"
#include "bsp_btn_ble.h"
#include "radio.h"

#define BUTTON_APP_TIMER_PRESCALER 0

static void button_event_handler(bsp_event_t event)
{
    switch (event)
    {
        case BSP_EVENT_DISCONNECT:
            radio_disconnect_current();
            break;

        case BSP_EVENT_KEY_0:
            printf("Connecting mode enabled\r\n");
            radio_connecting_set(true);
            break;

        case BSP_EVENT_KEY_1:
            printf("Connecting mode disabled after current transfer\r\n");
            radio_connecting_set(false);
            break;

        default:
            break;
    }
}

void button_init(void)
{
    uint32_t err_code;
    bsp_event_t startup_event;

    err_code = bsp_init(BSP_INIT_BUTTONS,
                        APP_TIMER_TICKS(100, BUTTON_APP_TIMER_PRESCALER),
                        button_event_handler);
    APP_ERROR_CHECK(err_code);

    err_code = bsp_btn_ble_init(NULL, &startup_event);
    APP_ERROR_CHECK(err_code);

    err_code = bsp_event_to_button_action_assign(0, BSP_BUTTON_ACTION_PUSH, BSP_EVENT_KEY_0);
    APP_ERROR_CHECK(err_code);

    err_code = bsp_event_to_button_action_assign(1, BSP_BUTTON_ACTION_PUSH, BSP_EVENT_KEY_1);
    APP_ERROR_CHECK(err_code);
}

void button_on_ble_evt(ble_evt_t *p_ble_evt)
{
    bsp_btn_ble_on_ble_evt(p_ble_evt);
}
