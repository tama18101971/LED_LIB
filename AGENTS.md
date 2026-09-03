# AGENTS.md

## Project

Bare-metal LED control library for **1–8** independent LEDs on MCU (no RTOS).
Pure C99, ~100 bytes RAM for library state (LED_COUNT = 4), no malloc/free,
no libc dependency. Version 2.0 (result of a full audit; see CHANGELOG.md).

- **Library**: `include/led.h` + `src/led.c` (+ `library.json`) — portable,
  MCU-agnostic. Internal state (`led_state_t`) is defined in `led.c`, not in
  the public header.
- **Demo project**: `examples/test_ch32v003/` — CH32V003F4P6 (RISC-V 48MHz,
  2KB RAM, 16KB Flash) via PlatformIO + WCH NoneOS SDK. Sources only: no
  library copies, no own `platformio.ini`.

## Build

```bash
pio run                     # build the demo (from repo ROOT)
pio run -t upload           # flash via WCH-Link
pio device monitor          # serial output (115200 baud)
```

## Tests (no hardware required)

```bash
pio test -e native          # host tests; needs gcc/clang/mingw in PATH
# or directly:
gcc -std=c99 -Iinclude test/test_led_lib/test_led_lib.c src/led.c -o t && ./t
```

The suite adapts to the build config; CI (`.github/workflows/ci.yml`) runs it
across the LED_COUNT 1..8 × LED_USE_* matrix with `-Werror`, plus a
`-pedantic-errors` check and the CH32V003 firmware build.

## Layout rules

- The root `platformio.ini` builds the demo: `src_dir =
  examples/test_ch32v003/src`, library attached via `lib_deps = symlink://.`;
  the demo must always compile the root library. No `src_filter` is needed —
  the library uses the standard PlatformIO layout (`src/` is its source dir,
  so `examples/` is never scanned).
- Package contents are controlled by `export.exclude` in `library.json`
  (PlatformIO has no `.pioignore` mechanism).
- Never copy `led.c`/`led.h` into `examples/` — the demo must always
  compile the root library.
- `include/` is the public API surface; internal types (e.g. `led_state_t`)
  live in `src/led.c`.

## Hardware pins

LEDs on **PC4–PC7** (GPIOC), active HIGH (1 = on). LED0=PC4, LED1=PC5,
LED2=PC6, LED3=PC7.

## Clock quirk (CH32V003)

CH32V003 SysTick uses HCLK/8 by default (6MHz at 48MHz SYSCLK). Do NOT
assume SysTick runs at SYSCLK. Use `Delay_Ms()` from WCH SDK for timing —
it's calibrated.

## Tick model

`led_process()` must be called at a constant rate (60×/sec ⇒ 1 tick =
1/60 s). The demo uses a blocking main loop: `led_process(); Delay_Ms(16);`
(≈62 Hz, 1 tick ≈ 16 ms). No interrupts in the current demo. For the
ISR pattern define `LED_ENTER_CRITICAL()/LED_EXIT_CRITICAL()` — mutators
are wrapped in them; `led_process()` is intentionally NOT wrapped.

## Key conventions

- `led_set_fn` callback is the only GPIO interface — never write GPIO
  directly from led.c. Callback must not call led_*() (no reentrancy).
- `led_start_effect()` is **idempotent**: calling it with the already-active
  effect does nothing. Use `led_restart_effect_for()` to force a restart.
- The repeat timer (`led_start_effect_for`) ticks even when every LED was
  detached from the effect; `led_effect_remaining()` returns the tick count
  (`LED_EFFECT_FOREVER` = infinite).
- `led_toggle()` switches on the *actual* pin state (via the GPIO cache).
- `led_init()` keeps the callback and invalidates the GPIO cache; the PRNG
  is reset (RANDOM_FLASH is reproducible after re-init).
- Effects parametrized by `LED_COUNT`; effect masks are derived
  (`LED_MASK_ALL/EVEN/ODD` in led.c). Never hardcode 0x0F/0x05/0x0A.
- Feature gates: `LED_USE_SEQUENCES`, `LED_USE_EFFECTS`, and per-group
  `LED_USE_*_EFFECT`/`LED_USE_*_EFFECTS` — entries of `eff_table[]`/`eff_period[]`
  and the effect functions are compiled conditionally; enum values never change.
- Sequence steps with `ticks == 0` last 1 tick (loader clamps them).
- New effects: add function + enum value + table entries in `eff_table[]`
  and `eff_period[]` (both inside the matching `#if LED_USE_*` block) —
  no led_process() changes needed.
- New API must be mirrored in: `led.h` docs, `docs/LED_LIBRARY.md`,
  `CHANGELOG.md`, and host tests (`test/test_led_lib`).
- C99 only, no GCC extensions (`-pedantic-errors` must stay clean).

## Resource budget (LED_COUNT = 4, everything enabled)

RAM ≈ 100 B, flash ≈ 2.4 KB (`-Os`, RV32EC). Keep these numbers in check
when touching the core; measure with `pio run` + `riscv-wch-elf-nm`.
