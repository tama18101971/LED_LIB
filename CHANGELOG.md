# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - 2026-09-03

Результат полного аудита библиотеки. Исправления дефектов, найденных
верификацией на хосте (run-length анализ эффектов, тайминги, матрица
конфигураций), а также новые API и инфраструктура тестов.

### Fixed
- **`led_start_effect()` идемпотентен**: повторный запуск уже активного
  эффекта больше не сбрасывает фазу. Раньше вызов в цикле опроса
  «замораживал» эффект на первой фазе (все диоды застывали включёнными
  для EMERGENCY/PAIR/ALTERNATING).
- **Таймер повторов тикает независимо от диодов**: если пользователь
  «оторвал» все диоды от эффекта (led_on/led_blink/led_play), таймер
  `led_start_effect_for()` теперь всё равно завершает эффект вовремя.
  Раньше запрос состояния эффекта залипал на «активен» навсегда.
- **RUNNING_FWD_HOLD / RUNNING_BWD_HOLD**: устранена паразитная вспышка
  крайнего диода на 1 тик при переходе фазы через 0 — все циклы эффекта
  теперь идентичны; первая позиция бега горит полные 10 тиков.
- **RUNNING_PP**: выдержка первой позиции теперь 10 тиков (было 9) —
  маска выдаётся до продвижения счётчиков, как в остальных эффектах.
- **Последний кадр таймированного эффекта** отрабатывает полный период
  (раньше обрезался на 1 тик из-за остановки в том же тике).
- **`led_toggle()`** переключает по фактическому состоянию вывода:
  горящий в ON_FOR/BLINK/SEQUENCE/EFFECT диод гаснет, а не «включается
  ещё раз». Раньше toggle горящего диода терял авто-выключение.
- **Смена callback** (`led_set_callback()`) инвалидирует кэш GPIO — новый
  обработчик получает полную картину состояний на ближайшем тике.
- **`led_init()`** сохраняет callback (повторная инициализация больше не
  глушит вывод молча), инвалидирует кэш GPIO (первый тик гасит все диоды)
  и сбрасывает PRNG RANDOM_FLASH — состояние библиотеки после
  `led_init()` полностью воспроизводимо.
- **`led_start_effect()` с некорректным/отключённым значением** больше не
  может оставить диоды зависшими в EFFECT-режиме с NULL-обработчиком.
- Убраны мёртвые ветки `if (counter > 0)` в обработчиках BLINK и
  дублированное условие в `led_start_effect()` (cppcheck).

### Changed (breaking)
- **`LED_COUNT`** теперь переопределяется из сборочной системы
  (`-DLED_COUNT=n`), допустимый диапазон **1..8** проверяется `#error`.
  Все маски и периоды эффектов производны от `LED_COUNT`.
- **Удалён макрос `handle_leds`** (загрязнение глобального пространства имён).
- **`led_init()`** больше не сбрасывает callback — используйте
  `led_set_callback(NULL)`.
- **`led_effect_is_running()` заменён** на пару запросов:
  `led_effect_active()` (работает ли эффект вообще, включая бесконечные)
  и `led_effect_remaining()` (остаток тиков, `LED_EFFECT_FOREVER` — бесконечно).
- **Мутаторы обёрнуты в `LED_ENTER_CRITICAL()`/`LED_EXIT_CRITICAL()`**
  (по умолчанию — пусто). Определите макросы для сценария
  «led_process() в прерывании». led_process() критической секцией не
  оборачивается намеренно.
- Шаг последовательности с `ticks == 0` трактуется как 1 тик
  (нулевая длительность в тиковой модели невозможна), зависание невозможно.
- Убрана зависимость от libc: `memset` заменён явным сбросом полей,
  `<string.h>` больше не включается (freestanding-совместимость).

### Added
- **`led_restart_effect_for(effect, repeats)`** — принудительный перезапуск
  эффекта с нулевой фазы (для повторного запуска по событию).
- **`led_effect_active()`**, **`led_effect_remaining()`** — запросы состояния
  эффекта (см. breaking выше).
- **`led_get_mode(id)`**, **`led_is_on(id)`** — запросы состояния диода.
- **`led_refresh()`** — принудительная пересинхронизация выводов после
  внешних изменений GPIO (переинициализация порта, сон).
- **Отключаемые модули** `LED_USE_*`: SEQUENCES, EFFECTS (+ 6 групп эффектов).
  Значения `led_effect_t` не меняются — ABI стабилен.
- **Макросы версии** `LED_LIB_VERSION_MAJOR/MINOR/PATCH/VERSION`.
- **Хост-тесты** `test/test_led_lib` (самодостаточный раннер, ~100 проверок):
  `pio test -e native` либо `gcc test/test_led_lib/test_led_lib.c src/led.c`.
  Проверяются тайминги всех режимов, run-length всех эффектов, матрица
  конфигураций LED_COUNT × LED_USE_*.
- **CI** (GitHub Actions): хост-тесты по матрице конфигураций с `-Werror`,
  строгая проверка `-pedantic-errors`, сборка прошивки CH32V003.
- Периоды эффектов производны от `LED_COUNT` (раньше таблица была
  рассчитана только на 4 диода).

### Performance
- Записи таблиц `eff_table[]`/`eff_period[]` и функции эффектов
  исключаются из прошивки вместе с отключёнными модулями (до ~0.7 КБ
  на CH32V003, если эффекты не используются).
- RAM состояния диодов сокращается с 20 до 8 байт на диод при
  `LED_USE_SEQUENCES=0`.

### Packaging
- Удалён несуществующий механизм `.pioignore`; состав пакета задаётся
  полем `export.exclude` в `library.json` (публикуются только
  `include/`, `src/`, `examples/`, `library.json`, `README.md`,
  `LICENSE`, `CHANGELOG.md`).
- Удалён неработающий ключ верхнего уровня `src_filter` (PlatformIO
  читает только `build.srcFilter`; защита от рекурсии обеспечивается
  стандартным лейаутом библиотеки `src/`).
- Версия манифеста и теги синхронизированы (2.0.0).

### Docs
- Синхронизированы с кодом: описание внутреннего состояния, точные
  цифры RAM (100 Б) и Flash (~2.4 КБ), инструкция по LED_COUNT,
  честное описание модели вызовов и ограничений ISR.

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
