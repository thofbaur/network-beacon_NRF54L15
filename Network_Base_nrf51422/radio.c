#include "radio.h"

#include <stdio.h>
#include <string.h>
#include "app.h"
#include "app_error.h"
#include "app_util.h"
#include "ble_advdata.h"
#include "ble_gap.h"
#include "ble_hci.h"
#include "common_include.h"
#include "console_output.h"
#include "led.h"
#include "nordic_common.h"
#include "nus.h"
#include "softdevice_handler.h"

#define PERIPHERAL_DEVICE_NAME "DSA"
#define LENGTH_PERIPHERAL_DEVICE_NAME 3
#define DSA_MANUFACTURER_PAYLOAD_LEN 3

#define CENTRAL_LINK_COUNT 1
#define PERIPHERAL_LINK_COUNT 0

#if (NRF_SD_BLE_API_VERSION == 3)
#define NRF_BLE_MAX_MTU_SIZE GATT_MTU_SIZE_DEFAULT
#endif

#define SCAN_INTERVAL 0x00A0
#define SCAN_WINDOW SCAN_INTERVAL
#define SCAN_TIMEOUT 0x0000

#define MIN_CONNECTION_INTERVAL MSEC_TO_UNITS(8, UNIT_1_25_MS)
#define MAX_CONNECTION_INTERVAL MSEC_TO_UNITS(75, UNIT_1_25_MS)
#define SLAVE_LATENCY 0
#define SUPERVISION_TIMEOUT MSEC_TO_UNITS(4000, UNIT_10_MS)

#ifndef NRF_CLOCK_LFCLKSRC
#define NRF_CLOCK_LFCLKSRC      {.source        = NRF_CLOCK_LF_SRC_XTAL, \
                                 .rc_ctiv       = 0,                     \
                                 .rc_temp_ctiv  = 0,                     \
                                 .xtal_accuracy = NRF_CLOCK_LF_XTAL_ACCURACY_20_PPM}
#endif

static uint8_t identifier[3] = PERIPHERAL_DEVICE_NAME;
static uint8_t current_id;
static uint8_t current_radio_status;
static uint8_t current_network_status;
static uint16_t m_conn_handle = BLE_CONN_HANDLE_INVALID;
static uint8_t connect_enabled = 0;
static uint8_t scan_active = 0;

static const ble_gap_conn_params_t m_connection_param =
{
    (uint16_t)MIN_CONNECTION_INTERVAL,
    (uint16_t)MAX_CONNECTION_INTERVAL,
    (uint16_t)SLAVE_LATENCY,
    (uint16_t)SUPERVISION_TIMEOUT
};

static const ble_gap_scan_params_t m_scan_params =
{
    .active = 1,
    .interval = SCAN_INTERVAL,
    .window = SCAN_WINDOW,
    .timeout = SCAN_TIMEOUT,
#if (NRF_SD_BLE_API_VERSION == 2)
    .selective = 0,
    .p_whitelist = NULL,
#endif
#if (NRF_SD_BLE_API_VERSION == 3)
    .use_whitelist = 0,
#endif
};

void assert_nrf_callback(uint16_t line_num, const uint8_t *p_file_name)
{
    app_error_handler(0xDEADBEEF, line_num, p_file_name);
}

static void radio_leds_update(void)
{
    led_set_connection_available(connect_enabled != 0);
    led_set_connected(m_conn_handle != BLE_CONN_HANDLE_INVALID);
}

static bool adv_find_type(const uint8_t *p_data,
                          uint8_t data_len,
                          uint8_t type,
                          const uint8_t **pp_field_data,
                          uint8_t *p_field_len)
{
    uint8_t offset = 0;

    while (offset < data_len)
    {
        uint8_t field_len = p_data[offset];

        if (field_len == 0)
        {
            break;
        }

        if (((uint16_t)offset + 1 + field_len) > data_len)
        {
            break;
        }

        if ((field_len >= 1) && (p_data[offset + 1] == type))
        {
            *pp_field_data = &p_data[offset + 2];
            *p_field_len = field_len - 1;
            return true;
        }

        offset += field_len + 1;
    }

    return false;
}

static bool adv_name_matches_identifier(const ble_gap_evt_adv_report_t *p_adv_report)
{
    const uint8_t *p_name;
    uint8_t name_len;

    if (adv_find_type(p_adv_report->data,
                      p_adv_report->dlen,
                      BLE_GAP_AD_TYPE_COMPLETE_LOCAL_NAME,
                      &p_name,
                      &name_len) ||
        adv_find_type(p_adv_report->data,
                      p_adv_report->dlen,
                      BLE_GAP_AD_TYPE_SHORT_LOCAL_NAME,
                      &p_name,
                      &name_len))
    {
        return (name_len == LENGTH_PERIPHERAL_DEVICE_NAME) &&
               (memcmp(p_name, identifier, LENGTH_PERIPHERAL_DEVICE_NAME) == 0);
    }

    return false;
}

static bool adv_get_manuf_payload(const ble_gap_evt_adv_report_t *p_adv_report,
                                  const uint8_t **pp_payload,
                                  uint8_t *p_payload_len)
{
    return adv_find_type(p_adv_report->data,
                         p_adv_report->dlen,
                         BLE_GAP_AD_TYPE_MANUFACTURER_SPECIFIC_DATA,
                         pp_payload,
                         p_payload_len);
}

static bool is_valid_connector(const ble_gap_evt_adv_report_t *p_adv_report)
{
    const uint8_t *p_manuf_payload;
    uint8_t manuf_payload_len;
    uint8_t readout_level;

    if (p_adv_report->rssi <= -100)
    {
        return false;
    }

    if (!adv_name_matches_identifier(p_adv_report))
    {
        return false;
    }

    if (!adv_get_manuf_payload(p_adv_report, &p_manuf_payload, &manuf_payload_len))
    {
        return false;
    }

    if (manuf_payload_len != DSA_MANUFACTURER_PAYLOAD_LEN)
    {
        return false;
    }

    current_id = p_manuf_payload[ADV_POS_ID];
    current_radio_status = p_manuf_payload[ADV_POS_RADIO_STATUS];
    current_network_status = p_manuf_payload[ADV_POS_NETWORK_STATUS];
    readout_level = (current_network_status & DATA_LEVEL_MASK) >> P_SHIFT_STATUS_DATA;

    return readout_level >= READOUT_LEVEL;
}

void radio_init(void)
{
    uint32_t err_code;
    nrf_clock_lf_cfg_t clock_lf_cfg = NRF_CLOCK_LFCLKSRC;
    ble_enable_params_t ble_enable_params;

    SOFTDEVICE_HANDLER_INIT(&clock_lf_cfg, NULL);

    err_code = softdevice_enable_get_default_config(CENTRAL_LINK_COUNT,
                                                    PERIPHERAL_LINK_COUNT,
                                                    &ble_enable_params);
    APP_ERROR_CHECK(err_code);

    CHECK_RAM_START_ADDR(CENTRAL_LINK_COUNT, PERIPHERAL_LINK_COUNT);

#if (NRF_SD_BLE_API_VERSION == 3)
    ble_enable_params.gatt_enable_params.att_mtu = NRF_BLE_MAX_MTU_SIZE;
#endif
    err_code = softdevice_enable(&ble_enable_params);
    APP_ERROR_CHECK(err_code);

    err_code = softdevice_ble_evt_handler_set(app_ble_evt_dispatch);
    APP_ERROR_CHECK(err_code);
}

void radio_scan_start(void)
{
    ret_code_t ret;

    if (scan_active)
    {
        return;
    }

    ret = sd_ble_gap_scan_start(&m_scan_params);
    APP_ERROR_CHECK(ret);
    scan_active = 1;
    radio_leds_update();
}

void radio_connecting_set(bool enabled)
{
    connect_enabled = enabled ? 1 : 0;
    radio_leds_update();

    if ((m_conn_handle == BLE_CONN_HANDLE_INVALID) && !scan_active)
    {
        radio_scan_start();
    }
}

void radio_disconnect_current(void)
{
    uint32_t err_code;

    if (m_conn_handle == BLE_CONN_HANDLE_INVALID)
    {
        return;
    }

    err_code = sd_ble_gap_disconnect(m_conn_handle,
                                     BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
    if (err_code != NRF_ERROR_INVALID_STATE)
    {
        APP_ERROR_CHECK(err_code);
    }
}

void radio_on_ble_evt(ble_evt_t *p_ble_evt)
{
    uint32_t err_code;
    const ble_gap_evt_t *p_gap_evt = &p_ble_evt->evt.gap_evt;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_ADV_REPORT:
        {
            const ble_gap_evt_adv_report_t *p_adv_report = &p_gap_evt->params.adv_report;

            if ((!console_output_busy()) && connect_enabled && is_valid_connector(p_adv_report))
            {
                err_code = sd_ble_gap_connect(&p_adv_report->peer_addr,
                                              &m_scan_params,
                                              &m_connection_param);
                if (err_code == NRF_SUCCESS)
                {
                    scan_active = 0;
                    radio_leds_update();
                    printf("Connecting to target %02x%02x%02x%02x%02x%02x\r\n",
                           p_adv_report->peer_addr.addr[0],
                           p_adv_report->peer_addr.addr[1],
                           p_adv_report->peer_addr.addr[2],
                           p_adv_report->peer_addr.addr[3],
                           p_adv_report->peer_addr.addr[4],
                           p_adv_report->peer_addr.addr[5]);
                    printf("Device ID and RSSI: %3u, %3d\r\n", current_id, p_adv_report->rssi);
                    printf("Advertised status: radio=0x%02x network=0x%02x\r\n",
                           current_radio_status,
                           current_network_status);
                }
            }
        } break;

        case BLE_GAP_EVT_CONNECTED:
            m_conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            printf("Connected.\r\n");
            radio_leds_update();
            nus_start_discovery(p_ble_evt->evt.gap_evt.conn_handle);
            break;

        case BLE_GAP_EVT_TIMEOUT:
            if (p_gap_evt->params.timeout.src == BLE_GAP_TIMEOUT_SRC_SCAN)
            {
                printf("\r\nScan timed out\r\n");
                scan_active = 0;
                radio_scan_start();
            }
            else if (p_gap_evt->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN)
            {
                printf("Connection Request timed out.\r\n");
                radio_scan_start();
            }
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            printf("\r\nGAP Disconnected\r\n");
            m_conn_handle = BLE_CONN_HANDLE_INVALID;
            radio_leds_update();
            radio_scan_start();
            break;

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            err_code = sd_ble_gap_sec_params_reply(p_ble_evt->evt.gap_evt.conn_handle,
                                                   BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP,
                                                   NULL,
                                                   NULL);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
            err_code = sd_ble_gap_conn_param_update(
                p_gap_evt->conn_handle,
                &p_gap_evt->params.conn_param_update_request.conn_params);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTC_EVT_TIMEOUT:
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

#if (NRF_SD_BLE_API_VERSION == 3)
        case BLE_GATTS_EVT_EXCHANGE_MTU_REQUEST:
            err_code = sd_ble_gatts_exchange_mtu_reply(p_ble_evt->evt.gatts_evt.conn_handle,
                                                       NRF_BLE_MAX_MTU_SIZE);
            APP_ERROR_CHECK(err_code);
            break;
#endif

        default:
            break;
    }
}

uint8_t radio_current_id(void)
{
    return current_id;
}

bool radio_is_connected(void)
{
    return m_conn_handle != BLE_CONN_HANDLE_INVALID;
}

bool radio_is_scanning(void)
{
    return scan_active != 0;
}

bool radio_connecting_enabled(void)
{
    return connect_enabled != 0;
}
