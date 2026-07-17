/**
 * @file gpio_setup.c
 * @brief CH32V003 GPIO setup for LEDs on PC4-PC7 using WCH NoneOS SDK.
 *
 * CH32V003F4P6 pins:
 *   PC4 = Pin 7   (LED0)
 *   PC5 = Pin 8   (LED1)
 *   PC6 = Pin 9   (LED2)
 *   PC7 = Pin 10  (LED3)
 *   Active level: HIGH (1)
 */

#include "gpio_setup.h"
#include "ch32v00x.h"
#include "ch32v00x_gpio.h"
#include "ch32v00x_rcc.h"

void gpio_init_leds(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    /* Enable GPIOC clock */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);

    /* PC4-PC7: 2MHz push-pull output */
    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_4 | GPIO_Pin_5 |
                                    GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    /* All LEDs off initially */
    GPIO_ResetBits(GPIOC, GPIO_Pin_4 | GPIO_Pin_5 |
                           GPIO_Pin_6 | GPIO_Pin_7);
}

void led_hw_set(uint8_t id, bool state)
{
    if (id >= LED_COUNT) return;

    if (state) {
        GPIO_SetBits(GPIOC, (uint16_t)(1UL << (4 + id)));
    } else {
        GPIO_ResetBits(GPIOC, (uint16_t)(1UL << (4 + id)));
    }
}
