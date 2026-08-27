# LED_LIB — библиотека управления 4 светодиодами

Bare-metal библиотека для 4 независимых светодиодов на любом MCU (без RTOS).
Чистый C99, ~90 байт RAM, без malloc/free. Доступ к GPIO — только через
пользовательский callback (`led_set_fn`).

## Структура

```
LED_LIB/
├── include/led.h           — публичный API библиотеки
├── src/led.c               — реализация (внутреннее состояние тоже здесь)
├── library.json            — манифест PlatformIO-библиотеки
├── platformio.ini          — сборка демо из корня (src_dir → examples/)
├── docs/LED_LIBRARY.md     — полная документация
├── AGENTS.md               — соглашения проекта
└── examples/test_ch32v003/ — демо для CH32V003F4P6 (только исходники)
```

## Быстрый старт (демо CH32V003)

```bash
pio run                # сборка из корня репозитория
pio run -t upload      # прошивка через WCH-Link
pio device monitor     # монитор порта (115200)
```

## Использование в своём проекте

Скопируйте `include/led.h` и `src/led.c` или подключите репозиторий как
PlatformIO-зависимость (см. `library.json`). Подробности — в
[docs/LED_LIBRARY.md](docs/LED_LIBRARY.md).
