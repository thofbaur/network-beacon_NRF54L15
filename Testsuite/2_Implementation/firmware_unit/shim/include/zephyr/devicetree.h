/*
 * Minimal devicetree shim. The host "board" mirrors the nrf54l15tag board:
 * three LED colours (led1_green/red/blue) and button0 exist; plain DK labels
 * (led0, led1) do not. No motion_detector alias, so motion.c builds its
 * "no sensor present" variant.
 */
#ifndef SHIM_ZEPHYR_DEVICETREE_H
#define SHIM_ZEPHYR_DEVICETREE_H

#define DT_NODELABEL(label) label
#define DT_ALIAS(alias) alias
#define DT_HAS_ALIAS(alias) 0

#define DT_NODE_HAS_STATUS(node, status) Z_SHIM_DT_HAS(node)
#define Z_SHIM_DT_HAS(node) SHIM_DT_HAS_##node
#define DT_SAME_NODE(a, b) (Z_SHIM_DT_PIN(a) == Z_SHIM_DT_PIN(b))
#define Z_SHIM_DT_PIN(node) SHIM_DT_PIN_##node

#define SHIM_DT_HAS_led1_green 1
#define SHIM_DT_HAS_led1_red 1
#define SHIM_DT_HAS_led1_blue 1
#define SHIM_DT_HAS_button0 1
#define SHIM_DT_HAS_adc 1

#define SHIM_DT_PIN_led1_green 10
#define SHIM_DT_PIN_led1_red 11
#define SHIM_DT_PIN_led1_blue 12
#define SHIM_DT_PIN_button0 20
#define SHIM_DT_PIN_led0 30
#define SHIM_DT_PIN_led1 31

#endif /* SHIM_ZEPHYR_DEVICETREE_H */
