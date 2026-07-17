# AGENTS.md

## Project

Bare-metal LED control library for 4 independent LEDs on MCU (no RTOS). Pure C99, ~90 bytes RAM, no malloc/free.

- **Library**: `led.h` + `led.c` — portable, MCU-agnostic
- **Demo project**: `test_ch32v003/` — CH32V003F4P6 (RISC-V 48MHz, 2KB RAM, 16KB Flash) via PlatformIO + WCH NoneOS SDK

## Build

```bash
cd test_ch32v003
pio run                     # build
pio run -t upload           # flash via WCH-Link
pio device monitor          # serial output (115200 baud)
```

## Hardware pins

LEDs on **PC4–PC7** (GPIOC), active HIGH (1 = on). LED0=PC4, LED1=PC5, LED2=PC6, LED3=PC7.

## Clock quirk (CH32V003)

CH32V003 SysTick uses HCLK/8 by default (6MHz at 48MHz SYSCLK). Do NOT assume SysTick runs at SYSCLK. Use `Delay_Ms()` from WCH SDK for timing — it's calibrated.

## Tick model

`led_process()` must be called 60×/sec. The demo uses a blocking main loop: `led_process(); Delay_Ms(16);`. No interrupts in the current demo.

## Key conventions

- `led_set_fn` callback is the only GPIO interface — never write GPIO directly from led.c
- Per-LED effects override global effects (`led_on()` during `led_start_effect()` "detaches" that LED)
- Sequence macros: `LED_STEP_ON(ticks)`, `LED_STEP_OFF(ticks)` — compile to static arrays
- New effects: add function + enum value + table entry in `eff_table[]` — no led_process() changes needed
- C99 only, no GCC extensions, no `<stdbool.h>` workarounds needed
