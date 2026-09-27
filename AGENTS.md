# AGENTS.md

## Project

Bare-metal LED control library for **1–8** independent LEDs on MCU (no RTOS).
Pure C99, no malloc/free, no libc dependency. Version 3.0: event-driven time
model (sleeping hosts), dimmable output levels, indicator ownership, 32-bit
packed commands (see CHANGELOG.md).

- **Library**: `include/led.h` + `src/led.c` (+ `library.json`) — portable,
  MCU-agnostic. Internal state (`led_state_t`) is defined in `led.c`, not in
  the public header.
- **Demo project**: `examples/test_ch32v003/` — CH32V003F4P6 (RISC-V 48MHz,
  2KB RAM, 16KB Flash) via PlatformIO + WCH NoneOS SDK. Sources only: no
  library copies, no own `platformio.ini`.
- **Minimal example**: `examples/rates_ch32v003/` — four LEDs with different
  blink rates at 50 % duty. Sources only, like the demo.

## Build

```bash
pio run                     # build all examples (from repo ROOT)
pio run -e ch32v003         # demo only
pio run -e ch32v003-rates   # four-rates example only
pio run -t upload           # flash the selected environment via WCH-Link
pio device monitor          # serial output (115200 baud)
```

## Tests (no hardware required)

```bash
pio test -e native          # host tests; needs gcc/clang/mingw in PATH
# or directly:
gcc -std=c99 -Iinclude test/test_led_lib/test_led_lib.c src/led.c -o t && ./t
```

The suite adapts to the build config; CI (`.github/workflows/ci.yml`) runs it
across the LED_COUNT 1..8 × LED_USE_* × LED_LEVEL_MAX matrix with `-Werror`,
plus a `-pedantic-errors` check and the CH32V003 firmware build.

## Layout rules

- The root `platformio.ini` builds **all** examples: `src_dir = examples`
  plus a per-env `build_src_filter` that picks one example
  (`ch32v003` → `test_ch32v003/src`, `ch32v003-rates` →
  `rates_ch32v003/src`). `src_dir` is a project-level option and cannot be
  set per env, hence the filter. The library is attached once via
  `lib_deps = symlink://.`; every example must always compile the root
  library. No `build.src_filter` is needed for the library itself — it uses
  the standard PlatformIO layout (`src/` is its source dir, so
  `examples/` is never scanned as library sources).
- A new example = a new `examples/<name>/src/main.c` + a new env in the root
  `platformio.ini` with a `build_src_filter` that includes it. Examples do
  NOT get their own `platformio.ini` (that would be a second source of
  truth and would drift).
- Package contents are controlled by `export.exclude` in `library.json`
  (PlatformIO has no `.pioignore` mechanism).
- Never copy `led.c`/`led.h` into `examples/` — examples must always
  compile the root library.
- `include/` is the public API surface; internal types (e.g. `led_state_t`)
  live in `src/led.c`.

## Time model (v3 core invariant)

- **The host owns time.** The library has no internal time source: only
  `led_advance(ticks)` moves anything forward, `led_process()` ==
  `led_advance(1)`.
- **INVARIANT: `led_advance(1)` × N ≡ `led_advance(N)`** — identical callback
  call sequence and identical final state. `idle_ticks()` may fast-forward only
  while `next_change_ticks() > n`, so no transition is ever skipped, and it
  must re-apply the channel level (the BLINK phase switch is deferred to the
  next tick, otherwise the batched advance would show a stale level).
- `led_ticks_until_next()` never returns 0; `LED_TICKS_FOREVER` = no timers.
  Both are public contract, guarded by defensive clamps in `led_advance()`.
  `patterns_active()` is *defined as* `next_change_ticks() != LED_TICKS_FOREVER`
  — do not re-introduce a second traversal, or the two answers can diverge.
- `sync_outputs()` must stay at the **start** of `led_advance()`: the first
  full drive after `led_init()/led_set_callback()/led_refresh()` has to occupy
  the same position in the callback stream for any advance splitting.
- Active effects tick one tick per `led_advance(1)` (`next_change_ticks()`
  returns 1) — their modular counters are not fast-forwarded on purpose
  (would require division in the hot path).
- Per-LED timers are `uint16_t`, so a single fast-forward is clamped to
  `0xFFFE` ticks.

## Economy rules (v3, flash is the scarcest resource)

- **One timer per channel.** `leds[i].timer` is "ticks to the next output
  change"; `0` means "no timed mode". `next_change_ticks()`, `idle_ticks()`
  and `tick_one()` therefore need no switch on the mode — a mode switch in
  these hot paths costs ~100 B of flash. Keep it that way.
- **Index `leds[]` only in the public API; use pointers inside.** A per-channel
  struct is 20 B, so `leds[id]` costs `slli/add/slli` and a reload of the
  array base after every call that may clobber registers. Mutators and scan
  loops take `led_state_t *s` instead. Measured: removing the base spill
  inside the scan loops was worth ~10 B each.
- **One shared "switch channels off by mode" loop.** `packed_reset()`,
  `stop_effect_inner()` and `stop_all_patterns()` all walk `leds[]` calling
  `set_mode_off()`. They use `off_by_mode(mode, or_higher)`; do not
  re-introduce a private copy of that loop.
- **Ownership is dropped in two different shapes on purpose.** On the
  `LED_TICKS_FOREVER` exit `led_advance()` stores `LED_OWNER_NONE`
  unconditionally (one store, no rescan of the channels); on the
  ticks-exhausted exit it must call `patterns_active()`. Folding both into
  one helper with a flag makes GCC expand the check three times and grows
  `led_advance()`.
- `eff_period[]` is `uint8_t`, not `uint16_t` — the largest entry is 145 at
  `LED_COUNT = 8`. The `LED_SA(...)` guards next to it must stay: they turn a
  future period overflow into a build error instead of a silent wrap.
- `LED_M_PHASE` (BLINK phase) and `LED_M_LOOP` (SEQUENCE) live in the spare
  bits of `mode`; there is no separate `flags` field. `led_get_mode()` must
  mask them off with `LED_M_MODE`.
- `led_init()` clears only `timer`/`level`/`mode` per channel: the rest are
  mode-scoped parameters written by the mutator that selects the mode.
- The packed FSM reads the executing word through `pc_cmd` and takes the
  channels of the FIRST command's device mask once, in `packed_begin()` —
  a per-LED call then detaches a channel for the rest of the pattern.
- `led_packed_once()` is `led_packed_run()` over a one-word table
  (`pc_one_word`); do not add a second code path.
- The packed decoder uses shifts only: `pc_level()`/`pc_time()` decode both
  phases from one expression, `duration = win << (2 * shift)`.
- The library must stay clean under `-Wall -Wextra -Wconversion -Wpedantic
  -Werror`; watch the `uint8_t -> int` promotion in mask shifts.

## Key conventions

- The demo env builds **without diagnostics**: `build_flags` of
  `[env:ch32v003]` carries `-DDEMO_UART=0 -DDEMO_SELFTEST=0`, and the same
  defaults sit in `examples/test_ch32v003/src/main.c` so a build outside
  PlatformIO is clean too. Result: 3968 B Flash / 412 B RAM and no
  printf/putchar/malloc symbol at all in `firmware.elf` (11520 B / 976 B
  with both diagnostics on). Do not override these through
  `PLATFORMIO_BUILD_FLAGS` — PlatformIO concatenates it with `build_flags`
  and you get `warning: "DEMO_UART" redefined`; flip the flags in
  `platformio.ini` instead.
- `led_set_fn` callback is the only GPIO interface — never write GPIO
  directly from led.c. Signature is `(uint8_t id, uint8_t level)`; levels are
  0..LED_LEVEL_MAX and are passed through unvalidated. Callback must not call
  led_*() (no reentrancy).
- Feature gates: `LED_USE_SEQUENCES`, `LED_USE_EFFECTS`, per-group
  `LED_USE_*_EFFECT`/`LED_USE_*_EFFECTS`, `LED_USE_PACKED_COMMANDS`,
  `LED_USE_EVENTS` (default 0) — effect functions, table entries and event
  hooks are compiled conditionally; enum values never change.
- The 32-bit packed command layout is a frozen public ABI: it is covered by
  `_Static_assert` in `src/led.c` (with a strict-C99 fallback) and by
  `T_SA(...)` in the tests. Any bit re-layout breaks the check.
  Durations are in **ticks**, not registers: `duration = win << (2*shift)`.
  `LED_TIME_ENC()` is the only place with division, and it only ever folds
  into compile-time constants.
- `led_start_effect()` is **idempotent**: calling it with the already-active
  effect does nothing. Use `led_restart_effect_for()` to force a restart.
- The repeat timer (`led_start_effect_for`) ticks even when every channel was
  detached from the effect; `led_effect_remaining()` returns the tick count
  (`LED_EFFECT_FOREVER` = infinite).
- `led_toggle()` switches on the *actual* output level (`> 0`).
- `led_init()` keeps the callback, invalidates the output cache and clears
  ownership; the PRNG is reset (RANDOM_FLASH is reproducible after re-init).
- Ownership is a single library-wide tag, NOT per channel. It is cleared at
  the end of `led_advance()` when no timed pattern is active, so the host must
  claim and start the pattern in one place.
- Effects are parametrized by `LED_COUNT`; effect masks are derived
  (`LED_MASK_ALL/EVEN/ODD` in led.c). Never hardcode 0x0F/0x05/0x0A.
- Sequence steps with `ticks == 0` last 1 tick (loader clamps them). A leading
  `LED_STEP_OFF(n)` step is the "start in n ticks" mechanism — there is
  deliberately no separate API.
- New effects: add function + enum value + table entries in `eff_table[]`
  and `eff_period[]` (both inside the matching `#if LED_USE_*` block) —
  no led_process()/tick_one() changes needed.
- New API must be mirrored in: `led.h` docs, `docs/LED_LIBRARY.md`,
  `README.md`, `CHANGELOG.md`, and host tests (`test/test_led_lib`).
- C99 only, no GCC extensions (`-pedantic-errors` must stay clean).

## Hardware pins

LEDs on **PC4–PC7** (GPIOC), active HIGH (level > 0 = on). LED0=PC4, LED1=PC5,
LED2=PC6, LED3=PC7.

## Clock quirk (CH32V003)

CH32V003 SysTick uses HCLK/8 by default (6MHz at 48MHz SYSCLK). Do NOT
assume SysTick runs at SYSCLK. Use `Delay_Ms()` from WCH SDK for timing —
it's calibrated.

## Tick model of the demo

The demo uses a blocking main loop: `led_advance(1); Delay_Ms(16);`
(≈62 Hz, 1 tick ≈ 16 ms). No interrupts in the current demo. For the
ISR pattern define `LED_ENTER_CRITICAL()/LED_EXIT_CRITICAL()` — mutators
are wrapped in them; `led_advance()` is intentionally NOT wrapped.

## Resource budget (measured, CH32V003 -Os RV32EC)

Library contribution = build with the library minus the same build without it.

| Configuration | Flash | RAM |
|---------------|-------|-----|
| 1 channel, packed commands only | 1244 B | 48 B |
| 3 channels, packed commands only | 1532 B | 68 B |
| 1 channel, plan subset (+sequences, +events) | 1620 B | 60 B |
| 1 channel, sequences only | 792 B | 28 B |
| 4 channels, everything on | 2660 B | 124 B |
| Demo firmware total (SDK + library) | 3968 B | 412 B |
| Rates firmware total (SDK + library) | 3376 B | 408 B |

`led.o` compiled standalone (4 channels, everything on): **3589 B** text,
4 B data, 122 B bss. Before the 3.0.1 size pass it was 3744 B — the whole
optimization pass below is worth ~155 B of the object.

Derived: core ≈ 660 B, packed-command module ≈ 580 B, sequences module
≈ 130 B. Per-channel state: 10 B without sequences, 20 B with; packed
FSM state: 21 B regardless of LED_COUNT.

> The per-configuration rows are only refreshed when the paired
> with/without-library build is actually re-run. Re-measure before quoting
> them; do not extrapolate from `led.o` size.

Keep these numbers in check when touching the core; measure with `pio run` +
`riscv-wch-elf-nm` (or a paired "with/without library" build, which is the only
reliable way — `nm` symbol sizes include link-time-relaxation padding).
