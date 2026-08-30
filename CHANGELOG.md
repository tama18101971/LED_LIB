# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2026-08-30

### Fixed
- README minimal example: `led_start_effect_for()` must come BEFORE per-LED
  calls (`led_blink`, `led_on_for`) — the effect claims all non-EFFECT LEDs,
  and per-LED calls after it "detach" the specific LEDs. Clarified that the
  API never blocks; `led_process()` drives the state machines.
- Demo: use the corrected effect-then-per-LED example in `main.c`
  (LED2+LED3 emergency flashes, LED0 blink, LED1 on-for 5 s).

## [1.0.0] - 2026-08-30

### Added
- First public release.
- Per-LED state machines: OFF, ON, ON_FOR, BLINK, SEQUENCE (`led_play`)
  for 4 independent LEDs on any bare-metal MCU (C99, no RTOS, no malloc).
- 12 built-in global effects with auto-stop repeats
  (`led_start_effect` / `led_start_effect_for`) and a dispatch table
  (`eff_table[]`) for easy extension.
- Minimal RAM footprint: ~97 bytes for library state (20 bytes per LED).
- GPIO access only through a user callback (`led_set_fn`) — the library
  contains no hardware-specific code.
- CH32V003 demo (PlatformIO, WCH NoneOS SDK) in `examples/test_ch32v003`.

### Performance
- GPIO writes are cached (`out_mask` + `driven_mask`): the callback fires
  only on an actual pin change or the first drive per LED.
- Effects advance shared incremental counters (`fx_phase/fx_pos/fx_cnt`,
  `fx_adv()`) instead of dividing a global tick counter — no software
  divide/multiply (`__udivsi3`/`__mulsi3`) in the 60 Hz hot path.
- Effects return a bitmask of LED states (`uint8_t (*)(void)`) rather than
  writing a `bool` array; built-in effects merged into shared functions
  (13 → 6) which are selected by `cur_effect`.