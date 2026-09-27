/**
 * @file gpio_setup.h
 * @brief CH32V003 GPIO setup for LEDs on PC4-PC7.
 */

#ifndef GPIO_SETUP_H
#define GPIO_SETUP_H

#include <stdint.h>
#include "led.h"

void gpio_init_leds(void);

/**
 * Callback вывода LED_LIB: 0 = погасить, 1..LED_LEVEL_MAX = «горит»
 * (для GPIO-включателя любой ненулевой уровень = 1).
 */
void led_hw_set(uint8_t id, uint8_t level);

#endif /* GPIO_SETUP_H */
