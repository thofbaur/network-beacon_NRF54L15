#include "led.h"

#include "boards.h"

void led_init(void)
{
    bsp_board_leds_init();
    bsp_board_leds_off();
    bsp_board_led_on(BSP_BOARD_LED_0);
}

void led_set_connection_available(bool on)
{
    if (on)
    {
        bsp_board_led_on(BSP_BOARD_LED_1);
    }
    else
    {
        bsp_board_led_off(BSP_BOARD_LED_1);
    }
}

void led_set_connected(bool on)
{
    if (on)
    {
        bsp_board_led_on(BSP_BOARD_LED_2);
    }
    else
    {
        bsp_board_led_off(BSP_BOARD_LED_2);
    }
}
