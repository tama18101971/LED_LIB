/**
 * @file gpio_setup.h
 * @brief CH32V003 GPIO setup for LEDs on PC4-PC7.
 */

#ifndef GPIO_SETUP_H
#define GPIO_SETUP_H

#include <stdint.h>
#include <stdbool.h>
#include "led.h"

void gpio_init_leds(void);
void led_hw_set(uint8_t id, bool state);

#endif /* GPIO_SETUP_H */
