# LED_LIB — библиотека управления светодиодами

Bare-metal библиотека для **1–8 независимых светодиодов** на любом MCU (без RTOS).
Чистый C99, ~100 байт RAM на состояние библиотеки (при LED_COUNT = 4), без malloc/free
и без зависимости от libc. Доступ к GPIO — только через пользовательский callback
(`led_set_fn`), поэтому библиотека не содержит аппаратно-зависимого кода.

## Возможности

- 4 независимых конечных автомата: `led_on`, `led_off`, `led_toggle`,
  `led_on_for`, `led_blink`, `led_play` (произвольные последовательности, SOS и т.п.).
- 12 встроенных глобальных эффектов (бегущий огонь, мигания, случайные вспышки…)
  с автоповтором и автоостановкой (`led_start_effect_for`, `led_restart_effect_for`).
- Идемпотентный запуск эффектов — безопасно вызывать в цикле опроса.
- `led_toggle()` переключает по **фактическому** состоянию вывода.
- Запросы состояния: `led_get_mode()`, `led_is_on()`, `led_any_led_active()`,
  `led_effect_active()`, `led_effect_remaining()`.
- О(LED_COUNT) время выполнения `led_process()`, отсутствие программного деления
  и умножения в горячем пути (эффекты используют инкрементные счётчики).
- Кэширование записи GPIO: callback вызывается только при изменении пина;
  `led_refresh()` принудительно пересинхронизирует выводы.
- Настраиваемое число диодов `LED_COUNT` (1..8) через `-D`, полностью
  конфигурационно-независимые эффекты; отключаемые модули (`LED_USE_*`)
  экономят Flash на малых МК.
- Мутаторы атомарны относительно `led_process()` при определённых
  `LED_ENTER_CRITICAL()`/`LED_EXIT_CRITICAL()` (см. led.h).

## Установка

**Из GitHub:**

```ini
lib_deps = https://github.com/tama18101971/LED_LIB.git
```

**Вручную:** скопируйте `include/led.h` и `src/led.c` в проект.

## Демо (CH32V003)

```bash
pio run                # сборка из корня репозитория
pio run -t upload      # прошивка через WCH-Link
pio device monitor     # монитор порта (115200)
```

Демо собирает `examples/test_ch32v003` (исходники) с библиотекой из корня —
копий `led.h`/`led.c` в примере нет (стандартный PlatformIO-лейаут библиотеки).

## Тесты (без железа)

```bash
pio test -e native     # нужен gcc/clang/mingw в PATH
```

Хост-тесты проверяют тайминги всех режимов, run-length всех эффектов,
границы конфигураций (LED_COUNT 1..8 × LED_USE_*) и регрессии исправлений.

## Минимальный пример

```c
void led_hw_set(uint8_t id, bool state) {
    /* ваша GPIO-запись для LED id */
}

int main(void) {
    led_init();
    led_set_callback(led_hw_set);

    /* 1) ЭФФЕКТ СНАЧАЛА: led_start_effect() переводит ВСЕ диоды в режим
     *    EFFECT. Любая per-LED функция, вызванная ПОСЛЕ неё, «отвязывает»
     *    конкретный диод от эффекта. Повторный вызов того же эффекта
     *    идемпотентен и не сбрасывает фазу. */
    led_start_effect_for(LED_EFFECT_EMERGENCY, 5); /* LED2+LED3: 5 вспышек */

    /* 2) ПОТОМ per-LED: эти диоды больше не участвуют в эффекте */
    led_blink(0, 15, 15);    /* LED0 мигает: 250 мс вкл / 250 мс выкл */
    led_on_for(1, 300);      /* LED1 горит 5 секунд (при точных 60 Гц) */

    for (;;) {
        led_process();       /* вызывать 60 раз в секунду — тикают FSM */
        Delay_Ms(16);        /* ~62 Гц: 1 тик ≈ 16 мс */
    }
    return 0;
}
```

> Важно: ни один вызов API не блокирует процессор. `led_on_for()`, `led_blink()`
> и эффекты только переключают внутренние режимы; фактическое срабатывание
> делает `led_process()`, который нужно вызывать периодически (обычно 60 раз/сек).

## led_process() из прерывания

Если `led_process()` вызывается из таймерного прерывания, а остальной API —
из главного цикла, определите критическую секцию, чтобы мутаторы были
атомарными относительно обработчика:

```c
#define LED_ENTER_CRITICAL()  __disable_irq()
#define LED_EXIT_CRITICAL()   __enable_irq()
```

Подробно — раздел «Модель вызовов и потокобезопасность» в `include/led.h`.

## Настройка

```ini
build_flags =
    -DLED_COUNT=6                 ; 1..8 диодов (маски uint8_t)
    -DLED_USE_SEQUENCES=0         ; убрать led_play() и поля SEQUENCE
    -DLED_USE_EFFECTS=0           ; убрать всю подсистему эффектов
    ; или точечно: LED_USE_RUNNING_EFFECTS, LED_USE_PINGPONG_EFFECT,
    ; LED_USE_BLINK_EFFECTS, LED_USE_MULTI_BLINK_EFFECTS,
    ; LED_USE_RANDOM_EFFECT, LED_USE_HOLD_EFFECTS
    -DLED_ENTER_CRITICAL='portENTER_CRITICAL()'
    -DLED_EXIT_CRITICAL='portEXIT_CRITICAL()'
```

## Ресурсы (LED_COUNT = 4, все модули включены)

| Параметр | Значение |
|----------|----------|
| RAM | ~100 байт (80 Б — состояния диодов + 18 Б глобальные) |
| Flash | ~2.4 КБ (меньше при отключении модулей `LED_USE_*`) |
| `led_process()` | O(LED_COUNT), без делений/умножений |
| Зависимости | нет libc (`<stddef.h>` — freestanding-заголовок) |

## Документация

- [docs/LED_LIBRARY.md](docs/LED_LIBRARY.md) — полное описание API, эффектов,
  последовательностей и добавления новых.
- [CHANGELOG.md](CHANGELOG.md) — история версий.

## Лицензия

MIT. См. [LICENSE](LICENSE).

Copyright (c) 2025–2026 tama18101971.
