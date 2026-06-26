#include "nus.h"

#include <stdio.h>
#include <string.h>
#include "app_error.h"
#include "app_timer.h"
#include "ble_db_discovery.h"
#include "ble_nus_c.h"
#include "console_output.h"
#include "radio.h"

#define NUS_INACTIVITY_TIMEOUT_MS 10000
#define NUS_TIMER_PRESCALER 0

static ble_nus_c_t m_ble_nus_c;
static ble_db_discovery_t m_ble_db_discovery;
APP_TIMER_DEF(m_nus_inactivity_timer_id);

static void nus_inactivity_timeout_handler(void *p_context)
{
    UNUSED_PARAMETER(p_context);

    printf("NUS inactivity timeout\r\n");
    radio_disconnect_current();
}

static void nus_inactivity_timer_restart(void)
{
    uint32_t err_code;

    err_code = app_timer_stop(m_nus_inactivity_timer_id);
    if ((err_code != NRF_ERROR_INVALID_STATE) && (err_code != NRF_ERROR_NO_MEM))
    {
        APP_ERROR_CHECK(err_code);
    }

    err_code = app_timer_start(m_nus_inactivity_timer_id,
                               APP_TIMER_TICKS(NUS_INACTIVITY_TIMEOUT_MS, NUS_TIMER_PRESCALER),
                               NULL);
    if (err_code != NRF_ERROR_NO_MEM)
    {
        APP_ERROR_CHECK(err_code);
    }
}

static void nus_inactivity_timer_stop(void)
{
    uint32_t err_code;

    err_code = app_timer_stop(m_nus_inactivity_timer_id);
    if ((err_code != NRF_ERROR_INVALID_STATE) && (err_code != NRF_ERROR_NO_MEM))
    {
        APP_ERROR_CHECK(err_code);
    }
}

static void db_disc_handler(ble_db_discovery_evt_t *p_evt)
{
    ble_nus_c_on_db_disc_evt(&m_ble_nus_c, p_evt);
}

static void ble_nus_c_evt_handler(ble_nus_c_t *p_ble_nus_c,
                                  const ble_nus_c_evt_t *p_ble_nus_evt)
{
    uint32_t err_code;
    uint8_t init_msg[2] = {'s', 't'};

    switch (p_ble_nus_evt->evt_type)
    {
        case BLE_NUS_C_EVT_DISCOVERY_COMPLETE:
            err_code = ble_nus_c_handles_assign(p_ble_nus_c,
                                                p_ble_nus_evt->conn_handle,
                                                &p_ble_nus_evt->handles);
            APP_ERROR_CHECK(err_code);

            err_code = ble_nus_c_rx_notif_enable(p_ble_nus_c);
            APP_ERROR_CHECK(err_code);

            err_code = ble_nus_c_string_send(&m_ble_nus_c, init_msg, sizeof(init_msg));
            APP_ERROR_CHECK(err_code);
            nus_inactivity_timer_restart();
            printf("Init Message Sent.\r\n");
            break;

        case BLE_NUS_C_EVT_NUS_RX_EVT:
            nus_inactivity_timer_restart();
            console_output_packet(radio_current_id(),
                                  p_ble_nus_evt->p_data,
                                  p_ble_nus_evt->data_len);
            break;

        case BLE_NUS_C_EVT_DISCONNECTED:
            nus_inactivity_timer_stop();
            printf("\r\nNUS Disconnected\r\n");
            break;

        default:
            break;
    }
}

void nus_init(void)
{
    uint32_t err_code;
    ble_nus_c_init_t nus_c_init;

    err_code = ble_db_discovery_init(db_disc_handler);
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&m_nus_inactivity_timer_id,
                                APP_TIMER_MODE_SINGLE_SHOT,
                                nus_inactivity_timeout_handler);
    APP_ERROR_CHECK(err_code);

    memset(&nus_c_init, 0, sizeof(nus_c_init));
    nus_c_init.evt_handler = ble_nus_c_evt_handler;

    err_code = ble_nus_c_init(&m_ble_nus_c, &nus_c_init);
    APP_ERROR_CHECK(err_code);
}

void nus_start_discovery(uint16_t conn_handle)
{
    uint32_t err_code;

    err_code = ble_db_discovery_start(&m_ble_db_discovery, conn_handle);
    printf("Service Discovery started with err code %4i.\r\n", (int)err_code);
    APP_ERROR_CHECK(err_code);
}

void nus_on_ble_evt(ble_evt_t *p_ble_evt)
{
    ble_db_discovery_on_ble_evt(&m_ble_db_discovery, p_ble_evt);
    ble_nus_c_on_ble_evt(&m_ble_nus_c, p_ble_evt);
}
