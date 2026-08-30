# AGENTS.md

## Project

Bare-metal LED control library for 4 independent LEDs on MCU (no RTOS). Pure C99, ~97 bytes RAM for library state, no malloc/free.

- **Library**: `include/led.h` + `src/led.c` (+ `library.json`) — portable, MCU-agnostic. Internal state (`led_state_t`) is defined in `led.c`, not in the public header.
- **Demo project**: `examples/test_ch32v003/` — CH32V003F4P6 (RISC-V 48MHz, 2KB RAM, 16KB Flash) via PlatformIO + WCH NoneOS SDK. Sources only: no library copies, no own `platformio.ini`.

## Build

```bash
pio run                     # build the demo (from repo ROOT)
pio run -t upload           # flash via WCH-Link
pio device monitor          # serial output (115200 baud)
```

## Layout rules

- The root `platformio.ini` builds the demo: `src_dir = examples/test_ch32v003/src`, library attached via `lib_deps = symlink://.`; `library.json` has `src_filter: "+<src/>"` to prevent recursive scanning of the self-symlinked dependency.
- Never copy `led.c`/`led.h` into `examples/` — the demo must always compile the root library.
- `include/` is the public API surface; internal types (e.g. `led_state_t`) live in `src/led.c`.

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
