#include "console_output.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "app_error.h"
#include "app_uart.h"
#include "boards.h"
#include "common_include.h"

#define UART_TX_BUF_SIZE 512
#define UART_RX_BUF_SIZE 8

#define LENGTH_DATA_BUFFER 401
#define LENGTH_UART_BUFFER ((LENGTH_DATA_BUFFER >> 2) + 20)
#define NUS_MAX_PACKET_LEN 20
#define PACKET_LEN_INDEX NUS_MAX_PACKET_LEN
#define PACKET_ID_INDEX (NUS_MAX_PACKET_LEN + 1)
#define NUS_PACKET_LEN_TIME 5
#define NUS_PACKET_LEN_VOLTAGE 2
#define NUS_PACKET_LEN_CONTROL 9
#define CONTACT_ENTRY_SIZE 5
#define DATA_FLAG_SIZE 1
#define UART_LINE_BUFFER_LEN 96

static uint8_t data_array[LENGTH_UART_BUFFER][NUS_MAX_PACKET_LEN + 2];
static uint16_t idx_read = 0;
static uint16_t idx_write = 0;
static uint8_t uart_active = 0;
static uint8_t disconnect_after_uart = 0;

static void console_uart_put(uint8_t byte)
{
    while (app_uart_put(byte) != NRF_SUCCESS)
    {
        /* Wait for FIFO space. Debug output is intentionally lossless. */
    }
}

static void console_uart_write(const char *p_text)
{
    while (*p_text != '\0')
    {
        console_uart_put((uint8_t)*p_text);
        p_text++;
    }
}

static void console_uart_printf(const char *p_format, ...)
{
    char line[UART_LINE_BUFFER_LEN];
    va_list args;

    va_start(args, p_format);
    (void)vsnprintf(line, sizeof(line), p_format, args);
    va_end(args);

    console_uart_write(line);
}

static bool control_packet_is_finished(const uint8_t *p_data, uint8_t len)
{
    static const uint8_t finished[] = {'f', 'i', 'n', 'i', 's', 'h', 'e', 'd'};

    return (len == sizeof(finished)) &&
           (memcmp(p_data, finished, sizeof(finished)) == 0);
}

static void console_output_send_next(void)
{
    uint32_t time_current;
    uint32_t time_start;
    uint8_t distance;
    uint8_t data_len;
    uint8_t *p_data = data_array[idx_read];
    uint8_t current_id = p_data[PACKET_ID_INDEX];
    uint8_t i;

    data_len = p_data[PACKET_LEN_INDEX];

    if (p_data[0] == DSA_NUS_FLAG_TIME)
    {
        if (data_len != NUS_PACKET_LEN_TIME)
        {
            console_uart_printf("ID:%3u Invalid time packet length:%u\r\n", current_id, data_len);
        }
        else
        {
            time_current = ((uint32_t)p_data[1]) << 24;
            time_current |= ((uint32_t)p_data[2]) << 16;
            time_current |= ((uint32_t)p_data[3]) << 8;
            time_current |= (uint32_t)p_data[4];
            console_uart_printf("ID:%3u Current Timer:%8lu\r\n", current_id, time_current);
        }
    }
    else if (p_data[0] == DSA_NUS_FLAG_DATA)
    {
        if ((data_len <= DATA_FLAG_SIZE) ||
            (((data_len - DATA_FLAG_SIZE) % CONTACT_ENTRY_SIZE) != 0))
        {
            console_uart_printf("ID:%3u Invalid data packet length:%u\r\n", current_id, data_len);
        }
        else
        {
            for (i = 0; CONTACT_ENTRY_SIZE * i < (data_len - DATA_FLAG_SIZE); i++)
            {
                uint8_t base = DATA_FLAG_SIZE + i * CONTACT_ENTRY_SIZE;

                time_start = ((uint32_t)p_data[base + 1]) << 16;
                time_start |= ((uint32_t)p_data[base + 2]) << 8;
                time_start |= (uint32_t)p_data[base + 3];
                distance = p_data[base + 4];

                console_uart_printf("ID:%3u Contact-ID:%3u, Timer:%8lu, RSSI:%3u\r\n",
                                    current_id,
                                    p_data[base],
                                    time_start,
                                    distance);
            }
        }
    }
    else if (p_data[0] == DSA_NUS_FLAG_VOLTAGE)
    {
        if (data_len != NUS_PACKET_LEN_VOLTAGE)
        {
            console_uart_printf("ID:%3u Invalid voltage packet length:%u\r\n", current_id, data_len);
        }
        else
        {
            console_uart_printf("ID:%3u Voltage:%3u\r\n", current_id, p_data[1]);
        }
    }
    else if (p_data[0] == DSA_NUS_FLAG_CONTROL)
    {
        if (data_len != NUS_PACKET_LEN_CONTROL)
        {
            console_uart_printf("ID:%3u Invalid control packet length:%u\r\n", current_id, data_len);
        }
        else
        {
            console_uart_printf("ID:%3u Control:", current_id);
            for (i = 1; i < NUS_PACKET_LEN_CONTROL; i++)
            {
                console_uart_printf(" %02x", p_data[i]);
            }
            console_uart_write("\r\n");

            if (control_packet_is_finished(&p_data[1], NUS_PACKET_LEN_CONTROL - 1))
            {
                console_uart_printf("ID:%3u Finished received\r\n", current_id);
                disconnect_after_uart = 1;
            }
        }
    }
    else
    {
        console_uart_printf("ID:%3u Raw:", current_id);
        for (i = 0; i < data_len; i++)
        {
            console_uart_printf(" %02x", p_data[i]);
        }
        console_uart_write("\r\n");
    }

    idx_read++;
}

void console_output_process(void)
{
    if ((uart_active != 0) || (idx_read == idx_write))
    {
        return;
    }

    uart_active = 1;

    while (idx_read != idx_write)
    {
        console_output_send_next();
    }

    idx_read = 0;
    idx_write = 0;
    uart_active = 0;

    if (disconnect_after_uart)
    {
        disconnect_after_uart = 0;
        app_transfer_finished();
    }
}

static void uart_event_handle(app_uart_evt_t *p_event)
{
    switch (p_event->evt_type)
    {
        case APP_UART_DATA_READY:
            break;

        case APP_UART_COMMUNICATION_ERROR:
            APP_ERROR_HANDLER(p_event->data.error_communication);
            break;

        case APP_UART_FIFO_ERROR:
            APP_ERROR_HANDLER(p_event->data.error_code);
            break;

        case APP_UART_TX_EMPTY:
            break;

        default:
            break;
    }
}

void console_output_init(void)
{
    uint32_t err_code;

    const app_uart_comm_params_t comm_params =
    {
        .rx_pin_no = RX_PIN_NUMBER,
        .tx_pin_no = TX_PIN_NUMBER,
        .rts_pin_no = RTS_PIN_NUMBER,
        .cts_pin_no = CTS_PIN_NUMBER,
        .flow_control = APP_UART_FLOW_CONTROL_DISABLED,
        .use_parity = false,
        .baud_rate = UART_BAUDRATE_BAUDRATE_Baud115200
    };

    APP_UART_FIFO_INIT(&comm_params,
                       UART_RX_BUF_SIZE,
                       UART_TX_BUF_SIZE,
                       uart_event_handle,
                       APP_IRQ_PRIORITY_LOWEST,
                       err_code);

    APP_ERROR_CHECK(err_code);
}

void console_output_packet(uint8_t current_id, const uint8_t *p_data, uint8_t data_len)
{
    if (data_len > NUS_MAX_PACKET_LEN)
    {
        printf("ID:%3u NUS packet too long:%u\r\n", current_id, data_len);
        return;
    }

    if (idx_write >= LENGTH_UART_BUFFER)
    {
        printf("ID:%3u UART queue overflow\r\n", current_id);
        return;
    }

    data_array[idx_write][PACKET_LEN_INDEX] = data_len;
    data_array[idx_write][PACKET_ID_INDEX] = current_id;
    memcpy(data_array[idx_write], p_data, data_len);
    idx_write++;
}

bool console_output_busy(void)
{
    return (uart_active != 0) || (idx_read != idx_write);
}
