# Библиотека управления светодиодами (LED Library)

## Оглавление

1. [Обзор](#обзор)
2. [Быстрый старт](#быстрый-старт)
3. [Архитектура](#архитектура)
4. [API справочник](#api-справочник)
5. [Работа с последовательностями](#работа-с-последовательностями)
6. [Глобальные эффекты](#глобальные-эффекты)
7. [Приоритеты управления](#приоритеты-управления)
8. [Примеры использования](#примеры-использования)
9. [Портирование на другие МК](#портирование-на-другие-мк)
10. [Потребление ресурсов](#потребление-ресурсов)

---

## Обзор

Библиотека управляет 4 независимыми светодиодами для bare-metal микроконтроллеров (без RTOS). Каждый диод имеет собственный конечный автомат и может выполнять свой сценарий независимо от остальных.

**Ключевые принципы:**
- Весь код выполняется в одной функции `led_process()`, вызываемой пользователем
- Нет таймеров, нет delay(), нет malloc — только статическая память (~90 байт)
- Доступ к GPIO только через пользовательский callback
- C99, портируемость: STM32, CH32, AVR, ESP32

**Что умеет:**
- Включение/выключение отдельных диодов
- Включение на заданное время (auto-off)
- Непрерывное мигание
- Воспроизведение пользовательских последовательностей (SOS, двойное мигание и т.д.)
- 12 встроенных глобальных эффектов (бегущий огонь, аварийная сигнализация, удержание и т.д.)
- Эффект на N повторов с авто-остановкой (`led_start_effect_for`)
- Запрос состояния эффектов и диодов (`led_effect_is_running`, `led_any_led_active`)
- Вспышка с последовательным погасанием (`led_flash_and_fade`)
- Расширяемость: новые эффекты добавляются без изменения кода библиотеки

---

## Быстрый старт

### 1. Подключение библиотеки

Скопируйте `include/led.h` и `src/led.c` в свой проект (или подключите
репозиторий как PlatformIO-библиотеку — см. `library.json`).

### 2. Реализация callback для GPIO

```c
#include "led.h"

void led_hw_set(uint8_t id, bool state) {
    // STM32:
    // HAL_GPIO_WritePin(LED_PORT, led_pins[id],
    //                   state ? GPIO_PIN_SET : GPIO_PIN_RESET);

    // AVR:
    // if (state) PORTB |= (1 << id);
    // else       PORTB &= ~(1 << id);

    // CH32V003:
    // if (state) GPIO_SetBits(GPIOC, (1 << (4 + id)));
    // else       GPIO_ResetBits(GPIOC, (1 << (4 + id)));
}
```

### 3. Инициализация и главный цикл

```c
int main(void) {
    // Инициализация системы
    SystemInit();
    gpio_init();

    // Инициализация библиотеки
    led_init();
    led_set_callback(led_hw_set);

    // Запуск мигания на LED0
    led_blink(0, 15, 15);  // 250 мс вкл / 250 мс выкл

    // Бесконечный цикл: обработка + задержка ~16 мс (60 Гц)
    while (1) {
        led_process();
        Delay_Ms(16);
    }
}
```

---

## Архитектура

### Модель исполнения

```
Пользовательский код
        |
        v
  led_process()  <--- вызывается 60 раз в секунду
        |
        +---> Обработка LED0 (конечный автомат)
        +---> Обработка LED1 (конечный автомат)
        +---> Обработка LED2 (конечный автомат)
        +---> Обработка LED3 (конечный автомат)
        |
        v
    hw_set(id, state)  <--- callback пользователя
        |
        v
    GPIO (PC4-PC7)
```

### Конечный автомат (State Machine)

Каждый LED находится ровно в одном из 6 режимов:

| Режим | Описание | Пример |
|-------|----------|--------|
| `LED_MODE_OFF` | Принудительно выключен | `led_off(0)` |
| `LED_MODE_ON` | Принудительно включён | `led_on(0)` |
| `LED_MODE_ON_FOR` | Включён на N тиков | `led_on_for(0, 300)` |
| `LED_MODE_BLINK` | Непрерывное мигание | `led_blink(0, 15, 15)` |
| `LED_MODE_SEQUENCE` | Воспроизведение последовательности | `led_play(0, &sos)` |
| `LED_MODE_EFFECT` | Управление глобальным эффектом | `led_start_effect(LED_EFFECT_RUNNING_FWD)` |

### Структура состояния одного LED

```c
typedef struct {
    led_mode_t mode;        // Текущий режим

    uint16_t counter;       // Счётчик тиков (для ON_FOR / BLINK)
    uint16_t on_ticks;      // Длительность ON-фазы (для BLINK)
    uint16_t off_ticks;     // Длительность OFF-фазы (для BLINK)
    bool     phase;         // true=ON фаза, false=OFF фаза (для BLINK)

    const led_step_t *seq;  // Указатель на массив шагов (для SEQUENCE)
    uint16_t seq_len;       // Количество шагов
    uint16_t seq_idx;       // Текущий индекс шага
    uint16_t seq_cnt;       // Счётчик текущего шага
    bool     seq_phase;     // Состояние текущего шага (ON/OFF)
    bool     seq_loop;      // Зациклена ли последовательность
} led_state_t;
```

**Потребление RAM:** 23 байта × 4 LED + ~14 байт глобальные = **~106 байт**

---

## API справочник

### Инициализация

```c
void led_init(void);
```
Сбрасывает все 4 диода в OFF, обнуляет все счётчики. Вызывать **один раз** перед использованием.

```c
void led_set_callback(led_set_fn fn);
```
Устанавливает callback-функцию для доступа к GPIO. Если `fn = NULL` — вывод отключается.

**Тип callback:**
```c
typedef void (*led_set_fn)(uint8_t id, bool state);
```

---

### Управление отдельным LED

#### led_on / led_off

```c
void led_on(uint8_t id);
void led_off(uint8_t id);
```

Мгновенное включение/выключение. **Отменяет** любой текущий эффект, мигание или последовательность на этом LED.

```c
led_on(0);   // LED0 включён (режим ON)
led_off(0);  // LED0 выключен (режим OFF)
```

#### led_toggle

```c
void led_toggle(uint8_t id);
```

Переключает состояние:
- Если LED был включён → выключается
- Если LED был в выключен/мигал/воспроизводил эффект → включается

```c
led_toggle(0);  // Если LED0 горел — погаснет. Если не горел — загорится.
```

#### led_on_for

```c
void led_on_for(uint8_t id, uint16_t ticks);
```

Включает LED на заданное число тиков, потом **автоматически выключает**.

| Параметр | Описание |
|----------|----------|
| `id` | Индекс диода (0..3) |
| `ticks` | Длительность в тиках (1 тик = 1/60 сек ≈ 16.7 мс) |

```c
led_on_for(1, 300);   // LED1 горит 5 секунд (300/60 = 5)
led_on_for(2, 60);    // LED2 горит 1 секунду
led_on_for(3, 1);     // LED3 мигнет на 1 тик (~17 мс)
```

**Таблица перевода:**

| Тики | Время |
|------|-------|
| 1 | 16.7 мс |
| 6 | 100 мс |
| 15 | 250 мс |
| 30 | 500 мс |
| 60 | 1 сек |
| 300 | 5 сек |
| 600 | 10 сек |
| 3600 | 1 мин |

#### led_blink

```c
void led_blink(uint8_t id, uint16_t on_ticks, uint16_t off_ticks);
```

Непрерывное мигание с заданными фазами.

| Параметр | Описание |
|----------|----------|
| `id` | Индекс диода (0..3) |
| `on_ticks` | Длительность включённой фазы |
| `off_ticks` | Длительность выключённой фазы |

```c
led_blink(0, 15, 15);   // 250 мс вкл / 250 мс выкл — ровное мигание
led_blink(0, 5, 25);    // 83 мс вкл / 417 мс выкл — короткая вспышка
led_blink(0, 30, 30);   // 500 мс вкл / 500 мс выкл — медленное мигание
led_blink(1, 3, 3);     // 50 мс вкл / 50 мс выкл — быстрое мерцание
```

Если любая из фаз = 0, диод выключается.

#### led_flash_and_fade

```c
void led_flash_and_fade(uint8_t count);
```

Вспышка `count` диодов с последовательным погасанием.

Включает диоды 0..count-1 одновременно, ждёт 1 секунду (60 тиков),
затем гасит по одному с интервалом ~167 мс (10 тиков), начиная с последнего.

| Параметр | Описание |
|----------|----------|
| `count` | Количество диодов (1..LED_COUNT) |

```c
led_flash_and_fade(4);  // все 4 вспыхивают → 1 сек → гаснут по одному
led_flash_and_fade(2);  // LED0+LED1 вспыхивают → гаснут
led_flash_and_fade(1);  // только LED0
```

---

### Воспроизведение последовательностей

#### led_play

```c
void led_play(uint8_t id, const led_sequence_t *seq);
```

Воспроизводит пользовательскую последовательность на указанном LED.

**Параметры:**
- `id` — индекс диода (0..3)
- `seq` — указатель на определение последовательности

---

## Работа с последовательностями

### Определение последовательности

Последовательность — это массив шагов, где каждый шаг задаёт состояние (ON/OFF) и длительность в тиках.

**Шаг (step):**
```c
typedef struct {
    bool     state;   // true = включить, false = выключить
    uint16_t ticks;   // Длительность в тиках
} led_step_t;
```

**Макросы для создания шагов:**
```c
#define LED_STEP_ON(ticks)   { true,  (ticks) }
#define LED_STEP_OFF(ticks)  { false, (ticks) }
```

**Определение последовательности:**
```c
typedef struct {
    const led_step_t *steps;   // Массив шагов
    uint16_t          count;   // Количество шагов
    bool              loop;    // true = зациклить, false = один раз
} led_sequence_t;
```

### Пример 1: Простое мигание

```c
// Мигание: 0.5 сек вкл / 0.5 сек выкл
static const led_step_t my_blink[] = {
    LED_STEP_ON(30),    // 30 тиков = 500 мс
    LED_STEP_OFF(30),   // 30 тиков = 500 мс
};

static const led_sequence_t my_blink_seq = {
    my_blink,
    sizeof(my_blink) / sizeof(my_blink[0]),  // = 2
    true   // зациклить
};

// Использование:
led_play(0, &my_blink_seq);
```

### Пример 2: SOS

```c
static const led_step_t sos[] = {
    // Буква S: ...
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(18),    // . (пауза перед O)

    // Буква O: ---
    LED_STEP_ON(18),  LED_STEP_OFF(6),     // -
    LED_STEP_ON(18),  LED_STEP_OFF(6),     // -
    LED_STEP_ON(18),  LED_STEP_OFF(18),    // - (пауза перед S)

    // Буква S: ...
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(60),    // . (пауза между повторами)
};

static const led_sequence_t sos_seq = {
    sos,
    sizeof(sos) / sizeof(sos[0]),  // = 18
    true
};

// Использование:
led_play(2, &sos_seq);
```

### Пример 3: Двойное мигание

```c
// Паттерн: ._.__  (вспышка, пауза, две вспышки, длинная пауза)
static const led_step_t double_blink[] = {
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(18),    // __ (пауза)
};

static const led_sequence_t double_blink_seq = {
    double_blink,
    sizeof(double_blink) / sizeof(double_blink[0]),
    true
};

led_play(0, &double_blink_seq);
```

### Пример 4: Тройное мигание

```c
// Паттерн: ._.__.__
static const led_step_t triple_blink[] = {
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(6),     // .
    LED_STEP_ON(6),   LED_STEP_OFF(18),    // ___ (пауза)
};

static const led_sequence_t triple_blink_seq = {
    triple_blink,
    sizeof(triple_blink) / sizeof(triple_blink[0]),
    true
};

led_play(1, &triple_blink_seq);
```

### Пример 5: Паттерн "авария"

```c
// Быстрое тройное мигание + длинная пауза
static const led_step_t hazard[] = {
    LED_STEP_ON(3),   LED_STEP_OFF(3),     // .
    LED_STEP_ON(3),   LED_STEP_OFF(3),     // .
    LED_STEP_ON(3),   LED_STEP_OFF(3),     // .
    LED_STEP_ON(3),   LED_STEP_OFF(3),     // .
    LED_STEP_ON(3),   LED_STEP_OFF(3),     // .
    LED_STEP_ON(3),   LED_STEP_OFF(3),     // .
    LED_STEP_ON(3),   LED_STEP_OFF(30),    // ___ (пауза)
};

static const led_sequence_t hazard_seq = {
    hazard,
    sizeof(hazard) / sizeof(hazard[0]),
    true
};

led_play(0, &hazard_seq);
```

### Пример 6: Однократное воспроизведение

```c
// Вспышка 3 раза и остановка
static const led_step_t flash3[] = {
    LED_STEP_ON(6),   LED_STEP_OFF(6),
    LED_STEP_ON(6),   LED_STEP_OFF(6),
    LED_STEP_ON(6),   LED_STEP_OFF(6),
};

static const led_sequence_t flash3_seq = {
    flash3,
    sizeof(flash3) / sizeof(flash3[0]),
    false   // НЕ зациклить — выполнить один раз
};

led_play(0, &flash3_seq);
// После 3 вспышек LED0 выключится
```

### Управление последовательностями

#### Остановка

```c
led_off(id);  // Принудительно остановить и выключить
```

#### Замена

```c
// Просто вызвать led_play() с новой последовательностью
led_play(0, &new_sequence);
// Старая последовательность мгновенно заменяется новой
```

#### Пауза/возобновление

Библиотека не имеет встроенной паузы. Пример с сохранением состояния:

```c
// Сохранить текущую последовательность
static const led_sequence_t *saved_seq[LED_COUNT] = {0};
static uint16_t saved_idx[LED_COUNT] = {0};

void my_pause(uint8_t id) {
    // Сохранить текущий индекс (нужно расширить led_state_t для доступа)
    led_off(id);  // Остановить
}

void my_resume(uint8_t id) {
    if (saved_seq[id]) {
        led_play(id, saved_seq[id]);
        saved_seq[id] = 0;
    }
}
```

**Примечание:** для полноценной паузы/возобновления нужно расширить библиотеку — добавить сохранение текущего индекса шага в `led_state_t` и функции `led_pause()` / `led_resume()`.

#### Проверка состояния

Библиотека не предоставляет функцию проверки текущего режима LED. Если нужно — добавьте в `led.c`:

```c
led_mode_t led_get_mode(uint8_t id) {
    if (id >= LED_COUNT) return LED_MODE_OFF;
    return leds[id].mode;
}
```

И объявите в `led.h`:
```c
led_mode_t led_get_mode(uint8_t id);
```

---

## Глобальные эффекты

Глобальные эффекты управляют **всеми 4 диодами** одновременно.

### Встроенные эффекты

| Эффект | Описание | Скорость |
|--------|----------|----------|
| `LED_EFFECT_RUNNING_FWD` | Бегущий огонь вперёд (0→3) | 1 позиция / 167 мс |
| `LED_EFFECT_RUNNING_BWD` | Бегущий огонь назад (3→0) | 1 позиция / 167 мс |
| `LED_EFFECT_RUNNING_PP` | Бегущий огонь туда-обратно | 1 позиция / 167 мс |
| `LED_EFFECT_RUNNING_CIRCLE` | Вращение по кругу | 1 позиция / 133 мс |
| `LED_EFFECT_PAIR_BLINK` | Попарное мигание (0+2, 1+3) | Переключение / 250 мс |
| `LED_EFFECT_EMERGENCY` | Аварийная сигнализация | Переключение / 83 мс |
| `LED_EFFECT_RANDOM_FLASH` | Случайные вспышки | Случайно |
| `LED_EFFECT_DOUBLE_BLINK` | Двойное мигание | Паттерн |
| `LED_EFFECT_TRIPLE_BLINK` | Тройное мигание | Паттерн |
| `LED_EFFECT_ALTERNATING` | Чередование чёт/нечёт | Переключение / 333 мс |
| `LED_EFFECT_RUNNING_FWD_HOLD` | Бегущий вперёд + пауза + 0.75с удержание | 1.75 сек/цикл |
| `LED_EFFECT_RUNNING_BWD_HOLD` | Бегущий назад + пауза + 0.75с удержание | 1.75 сек/цикл |

### Запуск и остановка эффектов

```c
// Запустить бегущий огонь (бесконечно)
led_start_effect(LED_EFFECT_RUNNING_FWD);

// Запустить аварийную сигнализацию (бесконечно)
led_start_effect(LED_EFFECT_EMERGENCY);

// Запустить эффект на N повторов, затем авто-остановка
led_start_effect_for(LED_EFFECT_RUNNING_FWD, 1);   // один цикл 0→3
led_start_effect_for(LED_EFFECT_EMERGENCY, 5);      // 5 вспышек

// Остановить эффект вручную (все LED в EFFECT-режиме выключатся)
led_stop_effect();
```

#### led_start_effect_for — эффект на N повторов

```c
void led_start_effect_for(led_effect_t effect, uint16_t repeats);
```

Запускает эффект на заданное число **полных циклов**, затем автоматически
останавливает (все диоды в EFFECT-режиме выключаются).

**Длительность одного цикла (таблица):**

| Эффект | Один цикл (тики) | Время при 60 Гц |
|--------|-------------------|-----------------|
| RUNNING_FWD | 40 | 667 мс |
| RUNNING_BWD | 40 | 667 мс |
| RUNNING_PP | 60 | 1 сек |
| RUNNING_CIRCLE | 32 | 533 мс |
| PAIR_BLINK | 30 | 500 мс |
| EMERGENCY | 10 | 167 мс |
| RANDOM_FLASH | 1 | repeats = тики |
| DOUBLE_BLINK | 80 | 1.33 сек |
| TRIPLE_BLINK | 105 | 1.75 сек |
| ALTERNATING | 40 | 667 мс |
| RUNNING_FWD_HOLD | 105 | 1.75 сек |
| RUNNING_BWD_HOLD | 105 | 1.75 сек |

**Примеры:**
```c
// Бегущий огонь один раз (0→1→2→3, потом все гаснут)
led_start_effect_for(LED_EFFECT_RUNNING_FWD, 1);

// Бегущий огонь три раза
led_start_effect_for(LED_EFFECT_RUNNING_FWD, 3);

// Аварийная сигнализация — 5 вспышек (~835 мс)
led_start_effect_for(LED_EFFECT_EMERGENCY, 5);

// Бесконечно (аналог led_start_effect)
led_start_effect(LED_EFFECT_RUNNING_FWD);
```

**Для RANDOM_FLASH** повторы считаются как тики (период = 1):
```c
// Случайные вспышки на 60 тиков (1 секунда)
led_start_effect_for(LED_EFFECT_RANDOM_FLASH, 60);
```

#### Проверка состояния эффектов и диодов

```c
// Проверка: выполняется ли сейчас эффект с ограничением повторов (led_start_effect_for)
if (led_effect_is_running()) {
    // эффект ещё активен
}

// Проверка: активен ли хотя бы один диод (ON_FOR, BLINK, SEQUENCE, EFFECT)
if (led_any_led_active()) {
    // идёт индикация
}
```

### Добавление своего эффекта

**Шаг 1:** Написать функцию-эффект. Она НЕ принимает номер тика, а возвращает **битовую маску** (бит i = 1 → LED i включён). Фазы и позиции продвигаются общими инкрементными счётчиками библиотеки (`fx_phase`, `fx_pos`, `fx_cnt`) и хелпером `fx_adv()` без деления:

```c
/**
 * Мой эффект: бегущий огонь 0→1→2→3→0, 1 позиция каждые 5 тиков.
 */
static uint8_t eff_my_custom(void) {
    fx_cnt = fx_adv(fx_cnt, 5);
    if (fx_cnt == 0) {
        fx_pos = fx_adv(fx_pos, LED_COUNT);
    }
    return (uint8_t)(1u << fx_pos);   // бит i = LED i включён
}
```

Если несколько эффектов одинаковы по структуре, но отличаются периодом/направлением — реализуйте ОДНУ функцию, различающую поведение через текущий `cur_effect` (см. `eff_running_fwd`, `eff_emergency` в `led.c`).

**Шаг 2:** Добавить в `led.h` в enum `led_effect_t`:

```c
typedef enum {
    // ... существующие эффекты ...
    LED_EFFECT_CUSTOM     = 13,  // НОВЫЙ ЭФФЕКТ
    LED_EFFECT_COUNT      = 14   // Увеличить на 1
} led_effect_t;
```

**Шаг 3:** Добавить в `led.c` в таблицу `eff_table[]` и `eff_period[]`:

```c
static const eff_fn eff_table[LED_EFFECT_COUNT] = {
    // ... существующие записи ...
    [LED_EFFECT_CUSTOM]    = eff_my_custom,
};

// Длительность одного цикла (в тиках) для led_start_effect_for()
static const uint16_t eff_period[LED_EFFECT_COUNT] = {
    // ... существующие записи ...
    [LED_EFFECT_CUSTOM]    = 20,  // 4 позиции × 5 тиков = 20
};
```

**Готово!** Теперь можно использовать:
```c
led_start_effect(LED_EFFECT_CUSTOM);
```

### Приоритет: per-LED vs эффект

```c
// Запускаем бегущий огонь на всех 4 LED
led_start_effect(LED_EFFECT_RUNNING_FWD);

// Через секунду "отрываем" LED2 от эффекта
led_blink(2, 15, 15);  // LED2 начинает мигать

// Теперь:
//   LED0, LED1, LED3 — бегущий огонь (EFFECT)
//   LED2 — мигание (BLINK)

// Останавливаем эффект
led_stop_effect();

// Теперь:
//   LED0, LED1, LED3 — выключены
//   LED2 — продолжает мигать
```

---

## Приоритеты управления

### Правила переключения

Вызов любой функции управления **мгновенно** переключает LED в новый режим, отменяя предыдущий:

| Вызов | Что происходит |
|-------|---------------|
| `led_on(id)` | Любой предыдущий режим отменяется → ON |
| `led_off(id)` | Любой предыдущий режим отменяется → OFF |
| `led_toggle(id)` | Если ON → OFF, иначе → ON |
| `led_on_for(id, N)` | Отменяет предыдущий → ON_FOR |
| `led_blink(id, ...)` | Отменяет предыдущий → BLINK |
| `led_play(id, seq)` | Отменяет предыдущий → SEQUENCE |
| `led_start_effect(...)` | Все LED в EFFECT (если не "оторваны") |
| `led_start_effect_for(..., N)` | Все LED в EFFECT на N повторов |
| `led_stop_effect()` | LED в EFFECT → OFF |
| `led_flash_and_fade(count)` | count LED → ON 1 сек → погасают по одному |

### Примеры

```c
// LED0 мигает
led_blink(0, 15, 15);

// Через 3 секунды останавливаем мигание
// Нельзя просто Delay_Ms(3000) — нужен цикл с led_process()!
uint32_t elapsed = 0;
while (elapsed < 180) {     // 180 × 16 мс ≈ 3 сек
    led_process();
    Delay_Ms(16);
    elapsed++;
}
led_off(0);

// LED1 включён на 5 сек, потом мигает
led_on_for(1, 300);
// ... ждём 5 сек (с led_process() в цикле) ...
led_blink(1, 10, 10);  // Заменяет ON_FOR на BLINK
```

---

## Примеры использования

### Пример 1: Базовый

```c
#include "led.h"

void led_hw_set(uint8_t id, bool state) {
    // Реализация для вашего МК
}

int main(void) {
    SystemInit();
    led_init();
    led_set_callback(led_hw_set);

    led_on(0);         // LED0 включён
    led_blink(1, 15, 15);  // LED1 мигает
    led_on_for(2, 300);    // LED2 горит 5 сек

    while (1) {
        led_process();
        Delay_Ms(16);
    }
}
```

### Пример 2: Последовательности

```c
// Определяем SOS
static const led_step_t sos[] = {
    LED_STEP_ON(6),  LED_STEP_OFF(6),
    LED_STEP_ON(6),  LED_STEP_OFF(6),
    LED_STEP_ON(6),  LED_STEP_OFF(18),
    LED_STEP_ON(18), LED_STEP_OFF(6),
    LED_STEP_ON(18), LED_STEP_OFF(6),
    LED_STEP_ON(18), LED_STEP_OFF(18),
    LED_STEP_ON(6),  LED_STEP_OFF(6),
    LED_STEP_ON(6),  LED_STEP_OFF(6),
    LED_STEP_ON(6),  LED_STEP_OFF(60),
};
static const led_sequence_t sos_seq = { sos, 18, true };

int main(void) {
    // ...
    led_play(0, &sos_seq);  // SOS на LED0

    while (1) {
        led_process();
        Delay_Ms(16);
    }
}
```

### Пример 3: Глобальные эффекты

```c
int main(void) {
    // ...
    led_start_effect(LED_EFFECT_RUNNING_FWD);  // бесконечно

    // Или на заданное число повторов:
    led_start_effect_for(LED_EFFECT_RUNNING_FWD, 3);  // 3 цикла

    while (1) {
        led_process();
        Delay_Ms(16);
    }
}
```

### Пример 4: Динамическое управление

```c
int main(void) {
    // ...
    led_blink(0, 15, 15);      // LED0 мигает
    led_start_effect(LED_EFFECT_EMERGENCY);  // Все в аварийном режиме

    // Ждём 5 сек, но led_process() продолжает работать!
    // Простой Delay_Ms(5000) заблокирует всё — нужен цикл.
    uint32_t elapsed = 0;
    while (elapsed < 300) {     // 300 тиков × 16 мс ≈ 5 сек
        led_process();
        Delay_Ms(16);
        elapsed++;
    }

    led_off(0);                 // Останавливаем LED0
    led_stop_effect();          // Останавливаем эффект

    while (1) {
        led_process();
        Delay_Ms(16);
    }
}
```

---

## Портирование на другие МК

### Необходимые шаги

1. **Скопировать** `include/led.h` и `src/led.c` в проект
2. **Реализовать** callback `led_hw_set()` для вашего GPIO
3. **Настроить** частоту вызова `led_process()` (60 Гц)

### Примеры реализации callback

**STM32 (HAL):**
```c
#include "stm32f1xx_hal.h"

static const uint16_t led_pins[] = {
    GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_2, GPIO_PIN_3
};

void led_hw_set(uint8_t id, bool state) {
    if (id >= 4) return;
    HAL_GPIO_WritePin(GPIOC, led_pins[id],
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
```

**AVR (ATmega328P):**
```c
#include <avr/io.h>

void led_hw_set(uint8_t id, bool state) {
    if (id >= 4) return;
    if (state) PORTB |= (1 << id);
    else       PORTB &= ~(1 << id);
}
```

**ESP32 (ESP-IDF):**
```c
#include "driver/gpio.h"

void led_hw_set(uint8_t id, bool state) {
    if (id >= 4) return;
    gpio_set_level(GPIO_NUM_2 + id, state ? 1 : 0);
}
```

### Настройка частоты тиков

**Блокирующий delay:**
```c
while (1) {
    led_process();
    Delay_Ms(16);  // ~60 Гц
}
```

**SysTick прерывание (ARM):**
```c
void SysTick_Handler(void) {
    static uint32_t prescaler = 0;
    if (++prescaler >= 16667) {  // 1 МГц / 60 = 16667
        prescaler = 0;
        led_process();
    }
}
```

**Hardware Timer:**
```c
// Настройка таймера на 60 Гц
// В обработчике прерывания:
void TIMER_IRQHandler(void) {
    led_process();
}
```

---

## Потребление ресурсов

### RAM

| Компонент | Размер |
|-----------|--------|
| `leds[4]` (конечные автоматы, упакованы) | 4 × 20 = 80 байт |
| `hw_set` (указатель) | 4 байта |
| `cur_effect` (enum) | 4 байта |
| `effect_remaining` | 4 байта |
| `out_mask` (кэш GPIO) | 1 байт |
| `driven_mask` (маска первых записей) | 1 байт |
| `fx_*` (счётчики эффектов) | 3 байта |
| **Итого** | **~97 байт** |

### Flash

| Компонент | Размер (приблизительно) |
|-----------|------------------------|
| Библиотека (ядро + эффекты) | ~2.8 КБ |
| Таблицы `eff_table[]` + `eff_period[]` | ~0.1 КБ |
| **Итого** | **~3.0 КБ** |

### Время выполнения led_process()

- Константное: **O(1)**
- Зависит только от количества LED (4)
- Нет переменных циклов, нет динамического выделения памяти
- При 60 Гц: выполняется за < 100 мкс

---

## Часто задаваемые вопросы

### Почему именно 60 Гц?

60 Гц — компромисс между:
- **Плавностью:** 60 обновлений/сек достаточно для плавного мигания
- **Энергопотреблением:** не нагружает МК чрезмерно
- **Точностью:** 1 тик = 16.7 мс, достаточно для большинства сценариев

Можно изменить частоту, изменив задержку в главном цикле.

### Как добавить 5-й LED?

Измените `LED_COUNT` в `led.h`:
```c
#define LED_COUNT 5
```

И добавьте обработку пина в `led_hw_set()`.

### Можно ли использовать более одного LED одновременно?

Да! Каждый LED работает **полностью независимо**:
```c
led_blink(0, 15, 15);      // LED0: мигает
led_on_for(1, 300);         // LED1: горит 5 сек
led_play(2, &sos);          // LED2: SOS
led_on(3);                  // LED3: постоянно горит
```

### Что будет, если вызвать led_process() реже 60 раз в секунду?

Эффекты будут работать **медленнее**:
- `led_blink(0, 15, 15)` при 30 Гц → 500 мс вкл / 500 мс выкл (вместо 250/250)
- `led_on_for(1, 300)` при 30 Гц → 10 сек (вместо 5)

Если вызывать **чаще** 60 Гц — эффекты будут **быстрее**.

### Как остановить мигание?

```c
led_off(0);  // Остановить и выключить
```

### Как остановить воспроизведение последовательности?

```c
led_off(0);  // Остановить и выключить
```

### Как временно приостановить эффект?

Библиотека не имеет встроенной паузы. Варианты:
1. `led_off(id)` — остановить и выключить
2. `led_on(id)` — принудительно включить
3. Сохранить состояние и восстановить позже (см. "Управление последовательностями")
