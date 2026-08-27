/**
 * @file led.c
 * @brief Реализация библиотеки управления 4 светодиодами.
 *
 * =====================================================================
 *  СТРУКТУРА КОДА
 * =====================================================================
 *
 * 1) Внутреннее состояние (static-переменные):
 *    - leds[4]       — массив конечных автоматов (по одному на LED)
 *    - hw_set        — callback для доступа к GPIO
 *    - cur_effect    — текущий глобальный эффект
 *    - g_tick        — глобальный счётчик тиков (растёт бесконечно)
 *
 * 2) Вспомогательные функции:
 *    - apply_state() — безопасный вызов hw_set (проверяет на NULL)
 *    - set_mode_off() / set_mode_on() — мгновенный сброс/установка режима
 *
 * 3) Обработчики конечных автоматов (вызываются из led_process()):
 *    - process_off()       — просто выключает
 *    - process_on()        — просто включает
 *    - process_on_for()    — включён, декремент, авто-выключение при 0
 *    - process_blink()     — двухфазный цикл ON→OFF→ON→...
 *    - process_sequence()  — проигрывание массива шагов
 *
 * 4) Функции эффектов (таблица eff_table[]):
 *    - eff_running_fwd / bwd / pp / circle — бегущий огонь
 *    - eff_pair_blink / emergency / etc — мигание
 *    - eff_random_flash — псевдослучайный генератор (xorshift32)
 *
 * 5) Публичный API:
 *    - led_init / led_set_callback / led_process
 *    - led_on / led_off / led_toggle / led_on_for / led_blink / led_play
 *    - led_start_effect / led_stop_effect
 *
 * =====================================================================
 *  ПОТРЕБЛЕНИЕ RAM
 * =====================================================================
 *
 *   leds[4]:           4 × 20 = 80 байт
 *   hw_set:            2 байта (указатель на AVR/ARM)
 *   cur_effect:        1 байт
 *   g_tick:            4 байта
 *   Итого:             ~87 байт (около 90)
 *
 * =====================================================================
 *  ВРЕМЯ ВЫПОЛНЕНИЯ led_process()
 * =====================================================================
 *
 *   - Цикл по 4 LED: 4 итерации фиксированного размера.
 *   - Внутри switch: до 6 case'ов, каждый O(1).
 *   - Эффект: 1 вызов функции + цикл по 4 LED.
 *   - Итого: O(1) — константное время, нет переменных циклов.
 */

#include "led.h"

/* ================================================================== */
/*  СОСТОЯНИЕ ОДНОГО СВЕТОДИОДА (внутренний тип, перенесён из led.h)    */
/* ================================================================== */

/**
 * Внутреннее состояние одного светодиода (конечный автомат).
 *
 * Поля разбиты на группы по назначению:
 *
 * 1) mode — текущий режим (определяет, какой обработчик вызывать)
 *
 * 2) Счётчики для режимов ON_FOR и BLINK:
 *    - counter   — оставшееся количество тиков в текущей фазе
 *    - on_ticks  — длительность ON-фазы (только для BLINK)
 *    - off_ticks — длительность OFF-фазы (только для BLINK)
 *    - phase     — true = сейчас ON-фаза, false = OFF-фаза (BLINK)
 *
 * 3) Воспроизведение последовательности (SEQUENCE):
 *    - seq       — указатель на массив шагов (из led_sequence_t)
 *    - seq_len   — общее количество шагов
 *    - seq_idx   — индекс текущего шага
 *    - seq_cnt   — оставшееся время текущего шага
 *    - seq_phase — состояние текущего шага (ON/OFF)
 *    - seq_loop  — зациклена ли последовательность
 *
 * Итого: 20 байт на LED × 4 = 80 байт + глобальные ~10 байт ≈ 90 байт.
 */
typedef struct {
    led_mode_t mode;

    /* Счётчики для ON_FOR / BLINK */
    uint16_t counter;
    uint16_t on_ticks;
    uint16_t off_ticks;
    bool     phase;

    /* Воспроизведение последовательности */
    const led_step_t *seq;
    uint16_t          seq_len;
    uint16_t          seq_idx;
    uint16_t          seq_cnt;
    bool              seq_phase;
    bool              seq_loop;
} led_state_t;

/* ================================================================== */
/*  ВНУТРЕННЕЕ СОСТОЯНИЕ (static)                                       */
/* ================================================================== */

/**
 * Массив конечных автоматов для 4 светодиодов.
 * Каждый элемент — полная копия led_state_t с полями для всех режимов.
 * Инициализируется в led_init(), изменяется в led_process().
 */
static led_state_t  leds[LED_COUNT];

/**
 * Callback-функция для доступа к GPIO.
 * Устанавливается через led_set_callback().
 * Если NULL — apply_state() ничего не делает (безопасность).
 */
static led_set_fn   hw_set = (void *)0;

/**
 * Текущий глобальный эффект.
 * Хранит значение из enum led_effect_t.
 * Если LED_EFFECT_NONE — эффект неактивен.
 */
static led_effect_t cur_effect = LED_EFFECT_NONE;

/**
 * Оставшееся время работы эффекта (в тиках).
 * Если 0 — эффект работает бесконечно (режим по умолчанию).
 * Если > 0 — декрементируется в led_process(), при достижении 0
 * вызывается led_stop_effect().
 */
static uint16_t effect_remaining = 0;

/**
 * Глобальный счётчик тиков.
 * Инкрементируется на +1 при каждом вызове led_process().
 * Используется эффектами для вычисления позиций/фаз.
 * Тип uint32_t — переполнится через ~828 дней при 60 Гц.
 */
static uint32_t g_tick = 0;

/* ================================================================== */
/*  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ                                            */
/* ================================================================== */

/**
 * Безопасный вызов callback-функции GPIO.
 *
 * Если hw_set установлен (не NULL) — вызывает его с указанным
 * состоянием. Если hw_set == NULL — ничего не делает.
 *
 * Это единственная точка, через которую библиотека взаимодействует
 * с аппаратурой. Благодаря этому она портируется между любыми МК.
 *
 * @param id     Индекс диода (0..3).
 * @param state  true = включить, false = выключить.
 */
static void apply_state(uint8_t id, bool state)
{
    if (hw_set) {
        hw_set(id, state);
    }
}

/**
 * Мгновенный переход в режим OFF.
 *
 * Сбрасывает режим и счётчик, выключает диод через GPIO.
 * Используется led_off(), led_stop_effect(), process_on_for() (при
 * истечении таймера), process_sequence() (при окончании без цикла).
 *
 * @param id  Индекс диода.
 */
static void set_mode_off(uint8_t id)
{
    leds[id].mode    = LED_MODE_OFF;
    leds[id].counter = 0;
    apply_state(id, false);
}

/**
 * Мгновенный переход в режим ON.
 *
 * Устанавливает режим ON, обнуляет счётчик, включает диод.
 * Используется led_on(), led_toggle() (когда LED выключен).
 *
 * @param id  Индекс диода.
 */
static void set_mode_on(uint8_t id)
{
    leds[id].mode    = LED_MODE_ON;
    leds[id].counter = 0;
    apply_state(id, true);
}

/* ================================================================== */
/*  ОБРАБОТЧИКИ КОНЕЧНЫХ АВТОМАТОВ                                      */
/* ================================================================== */

/**
 * Обработчик режима OFF — просто поддерживает выключенное состояние.
 *
 * Вызывается при каждом тике, пока диод в режиме OFF.
 * Принудительно выключает диод (на случай, если он был включён
 * аппаратно).
 *
 * @param id  Индекс диода.
 */
static void process_off(uint8_t id)
{
    apply_state(id, false);
}

/**
 * Обработчик режима ON — просто поддерживает включённое состояние.
 *
 * Аналогично process_off(), но включает диод.
 *
 * @param id  Индекс диода.
 */
static void process_on(uint8_t id)
{
    apply_state(id, true);
}

/**
 * Обработчик режима ON_FOR — включён на заданное число тиков.
 *
 * Логика:
 *   1. Если counter > 0: включить диод, декремент counter.
 *   2. Если counter стал 0: переключить режим в OFF, выключить диод.
 *   3. Если counter == 0 (сразу): выключить диод.
 *
 * Пример: led_on_for(1, 300) при 60 Гц → диод горит 5 секунд.
 *
 * @param id  Индекс диода.
 */
static void process_on_for(uint8_t id)
{
    if (leds[id].counter > 0) {
        apply_state(id, true);
        leds[id].counter--;
        if (leds[id].counter == 0) {
            leds[id].mode = LED_MODE_OFF;
            apply_state(id, false);
        }
    } else {
        apply_state(id, false);
    }
}

/**
 * Обработчик режима BLINK — непрерывное мигание с разными фазами.
 *
 * Двухфазный конечный автомат:
 *
 *   phase = true  (ON-фаза):
 *     - Включить диод
 *     - Декремент counter
 *     - Если counter == 0: перейти в OFF-фазу,
 *       загрузить counter = off_ticks
 *
 *   phase = false (OFF-фаза):
 *     - Выключить диод
 *     - Декремент counter
 *     - Если counter == 0: перейти в ON-фазу,
 *       загрузить counter = on_ticks
 *
 * Пример: led_blink(0, 15, 15) при 60 Гц:
 *   ON-фаза:  15 тиков (250 мс) — диод горит
 *   OFF-фаза: 15 тиков (250 мс) — диод не горит
 *   Повторяется бесконечно.
 *
 * @param id  Индекс диода.
 */
static void process_blink(uint8_t id)
{
    led_state_t *s = &leds[id];

    if (s->phase) {
        /* ON-фаза: диод включён, считаем время до выключения */
        apply_state(id, true);
        if (s->counter > 0) {
            s->counter--;
        }
        if (s->counter == 0) {
            /* Переход в OFF-фазу */
            s->counter = s->off_ticks;
            s->phase   = false;
        }
    } else {
        /* OFF-фаза: диод выключен, считаем время до включения */
        apply_state(id, false);
        if (s->counter > 0) {
            s->counter--;
        }
        if (s->counter == 0) {
            /* Переход в ON-фазу */
            s->counter = s->on_ticks;
            s->phase   = true;
        }
    }
}

/**
 * Обработчик режима SEQUENCE — воспроизведение массива шагов.
 *
 * Последовательность — это массив структур { state, ticks }.
 * Обработчик проходит по массиву, выдерживая каждую паузу.
 *
 * Алгоритм:
 *   1. Применить текущий state (ON или OFF) через GPIO.
 *   2. Декремент seq_cnt (счётчик текущего шага).
 *   3. Если seq_cnt == 0:
 *      a. Перейти к следующему шагу (seq_idx++).
 *      b. Если шагов больше нет:
 *         - Если loop == true: вернуться к шагу 0.
 *         - Если loop == false: перейти в OFF, завершить.
 *      c. Загрузить state и ticks нового шага.
 *      d. Применить новый state через GPIO.
 *
 * Пример (SOS):
 *   Шаг 0: ON  6 тиков (100 мс) — точка
 *   Шаг 1: OFF 6 тиков (100 мс) — пауза
 *   Шаг 2: ON  6 тиков (100 мс) — точка
 *   ...
 *   Шаг 17: OFF 60 тиков (1 сек) — пауза между повторами
 *   → Возврат к шагу 0 (loop = true)
 *
 * @param id  Индекс диода.
 */
static void process_sequence(uint8_t id)
{
    led_state_t *s = &leds[id];

    /* Применить состояние текущего шага */
    apply_state(id, s->seq_phase);

    /* Декремент счётчика текущего шага */
    if (s->seq_cnt > 0) {
        s->seq_cnt--;
    }

    /* Если шаг завершён — перейти к следующему */
    if (s->seq_cnt == 0) {
        s->seq_idx++;

        /* Проверка на конец последовательности */
        if (s->seq_idx >= s->seq_len) {
            if (s->seq_loop) {
                s->seq_idx = 0;       /* Зациклить: начать сначала */
            } else {
                s->mode = LED_MODE_OFF;
                apply_state(id, false);
                return;                /* Один раз: остановиться */
            }
        }

        /* Загрузить параметры нового шага */
        s->seq_phase = s->seq[s->seq_idx].state;
        s->seq_cnt   = s->seq[s->seq_idx].ticks;
        apply_state(id, s->seq_phase);
    }
}

/* ================================================================== */
/*  ВСТРОЕННЫЕ ФУНКЦИИ ЭФФЕКТОВ                                        */
/* ================================================================== */

/**
 * Бегущий огонь вперёд: LED0 → LED1 → LED2 → LED3 → LED0 → ...
 *
 * Скорость: 1 позиция каждые 10 тиков (~167 мс).
 * Горит ровно 1 диод.
 *
 * @param tick  Глобальный счётчик тиков.
 * @param st    Выходной массив состояний [LED_COUNT].
 */
static void eff_running_fwd(uint32_t tick, bool st[LED_COUNT])
{
    uint8_t pos = (uint8_t)((tick / 10) % LED_COUNT);
    uint8_t i;
    for (i = 0; i < LED_COUNT; i++) {
        st[i] = (i == pos);
    }
}

/**
 * Бегущий огонь назад: LED3 → LED2 → LED1 → LED0 → LED3 → ...
 *
 * Скорость: 1 позиция каждые 10 тиков.
 */
static void eff_running_bwd(uint32_t tick, bool st[LED_COUNT])
{
    uint8_t pos = (uint8_t)((tick / 10) % LED_COUNT);
    uint8_t i;
    for (i = 0; i < LED_COUNT; i++) {
        st[i] = (i == (LED_COUNT - 1 - pos));
    }
}

/**
 * Бегущий огонь туда-обратно: 0→1→2→3→2→1→0→1→...
 *
 * Период = 2 * LED_COUNT - 2 = 6 позиций.
 * Скорость: 1 позиция каждые 10 тиков.
 */
static void eff_running_pp(uint32_t tick, bool st[LED_COUNT])
{
    uint8_t period = LED_COUNT * 2 - 2;
    uint8_t pos    = (uint8_t)((tick / 10) % period);
    uint8_t i;

    /* Отражение: позиции 3,4,5 → 3,2,1 */
    if (pos >= LED_COUNT) {
        pos = period - pos;
    }

    for (i = 0; i < LED_COUNT; i++) {
        st[i] = (i == pos);
    }
}

/**
 * Вращение по кругу: 0→1→2→3→0→... (тот же eff_running_fwd).
 *
 * Скорость: 1 позиция каждые 8 тиков (~133 мс) — чуть быстрее.
 */
static void eff_running_circle(uint32_t tick, bool st[LED_COUNT])
{
    uint8_t pos = (uint8_t)((tick / 8) % LED_COUNT);
    uint8_t i;
    for (i = 0; i < LED_COUNT; i++) {
        st[i] = (i == pos);
    }
}

/**
 * Попарное мигание: LED0+LED2 / LED1+LED3.
 *
 * Чётные и нечётные диоды мигают в противофазе.
 * Скорость: переключение каждые 15 тиков (~250 мс).
 */
static void eff_pair_blink(uint32_t tick, bool st[LED_COUNT])
{
    bool on = ((tick / 15) % 2 == 0);
    st[0] = on;
    st[1] = !on;
    st[2] = on;
    st[3] = !on;
}

/**
 * Аварийная сигнализация: все 4 диода мигают одновременно.
 *
 * Скорость: переключение каждые 5 тиков (~83 мс) — быстро.
 */
static void eff_emergency(uint32_t tick, bool st[LED_COUNT])
{
    bool on = ((tick / 5) % 2 == 0);
    uint8_t i;
    for (i = 0; i < LED_COUNT; i++) {
        st[i] = on;
    }
}

/**
 * Случайные вспышки — генератор псевдослучайных чисел (xorshift32).
 *
 * Алгоритм: xorshift32 — быстрый и компактный PRNG.
 * При каждом вызове генерируется новое 32-битное слово,
 * младшие 4 бита которого определяют состояние 4 диодов.
 *
 * Начальное seed: 0xDEADBEEF (может быть любым != 0).
 *
 * @param tick  Используется только для совместимости сигнатуры.
 */
static void eff_random_flash(uint32_t tick, bool st[LED_COUNT])
{
    static uint32_t rng = 0xDEADBEEF;
    uint8_t i;
    (void)tick;  /* Не используется в этом эффекте */

    /* xorshift32: 3 XOR-сдвига дают максимальный период 2^32 - 1 */
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;

    /* Младшие 4 бита — состояние 4 диодов */
    for (i = 0; i < LED_COUNT; i++) {
        st[i] = ((rng >> i) & 1) != 0;
    }
}

/**
 * Двойное мигание: ._.__  (все 4 диода вместе).
 *
 * Паттерн на 16 фаз (при делителе 5):
 *   Фазы 0-1:  ON  (100 мс)
 *   Фазы 2-3:  OFF (100 мс)
 *   Фазы 4-5:  ON  (100 мс)
 *   Фазы 6-15: OFF (200 мс) — пауза перед повтором
 */
static void eff_double_blink(uint32_t tick, bool st[LED_COUNT])
{
    uint8_t phase = (uint8_t)((tick / 5) % 16);
    bool on;
    uint8_t i;

    if (phase < 2)       on = true;
    else if (phase < 4)  on = false;
    else if (phase < 6)  on = true;
    else                 on = false;

    for (i = 0; i < LED_COUNT; i++) {
        st[i] = on;
    }
}

/**
 * Тройное мигание: ._.__.__  (все 4 диода вместе).
 *
 * Паттерн на 21 фазу (при делителе 5):
 *   Фазы 0-1:  ON   (100 мс)
 *   Фазы 2-3:  OFF  (100 мс)
 *   Фазы 4-5:  ON   (100 мс)
 *   Фазы 6-7:  OFF  (100 мс)
 *   Фазы 8-9:  ON   (100 мс)
 *   Фазы 10-20: OFF (220 мс) — пауза перед повтором
 */
static void eff_triple_blink(uint32_t tick, bool st[LED_COUNT])
{
    uint8_t phase = (uint8_t)((tick / 5) % 21);
    bool on;
    uint8_t i;

    if (phase < 2)       on = true;
    else if (phase < 4)  on = false;
    else if (phase < 6)  on = true;
    else if (phase < 8)  on = false;
    else if (phase < 10) on = true;
    else                 on = false;

    for (i = 0; i < LED_COUNT; i++) {
        st[i] = on;
    }
}

/**
 * Чередование: LED0+LED2 / LED1+LED3 (медленное).
 *
 * Скорость: переключение каждые 20 тиков (~333 мс).
 * Визуально: нечётные и чётные диоды меняются местами.
 */
static void eff_alternating(uint32_t tick, bool st[LED_COUNT])
{
    bool on = ((tick / 20) % 2 == 0);
    st[0] = on;
    st[1] = !on;
    st[2] = on;
    st[3] = !on;
}

/* ================================================================== */
/*  ТАБЛИЦА ЭФФЕКТОВ (dispatch table)                                  */
/* ================================================================== */

/**
 * Тип функции-эффекта: принимает номер тика и записывает состояния.
 *
 * Это ключ к расширяемости: чтобы добавить новый эффект, достаточно:
 *   1) Написать функцию с этой сигнатурой.
 *   2) Добавить её в eff_table[] ниже.
 *   3) Добавить значение в enum led_effect_t в led.h.
 *
 * Никаких изменений в led_process() или другом коде не требуется.
 */
typedef void (*eff_fn)(uint32_t, bool[LED_COUNT]);

/**
 * Таблица эффектов — индексируется по enum led_effect_t.
 *
 * Индекс таблицы = значение enum. Нулевой элемент (LED_EFFECT_NONE)
 * равен NULL — эффект не вызывается.
 *
 * Добавление нового эффекта:
 *   1) Написать статическую функцию eff_my_new().
 *   2) В led.h добавить: LED_EFFECT_MY_NEW = 11 (перед COUNT).
 *   3) В COUNT увеличить до 12.
 *   4) В таблицу добавить: [LED_EFFECT_MY_NEW] = eff_my_new,
 */
static const eff_fn eff_table[LED_EFFECT_COUNT] = {
    [LED_EFFECT_NONE]           = (void *)0,
    [LED_EFFECT_RUNNING_FWD]    = eff_running_fwd,
    [LED_EFFECT_RUNNING_BWD]    = eff_running_bwd,
    [LED_EFFECT_RUNNING_PP]     = eff_running_pp,
    [LED_EFFECT_RUNNING_CIRCLE] = eff_running_circle,
    [LED_EFFECT_PAIR_BLINK]     = eff_pair_blink,
    [LED_EFFECT_EMERGENCY]      = eff_emergency,
    [LED_EFFECT_RANDOM_FLASH]   = eff_random_flash,
    [LED_EFFECT_DOUBLE_BLINK]   = eff_double_blink,
    [LED_EFFECT_TRIPLE_BLINK]   = eff_triple_blink,
    [LED_EFFECT_ALTERNATING]    = eff_alternating
};

/**
 * Длительность одного цикла эффекта (в тиках).
 * Индекс = enum led_effect_t. Используется для пересчёта повторов в тики.
 *
 * RANDOM_FLASH не имеет цикла — период = 1 (repeats = ticks).
 */
static const uint16_t eff_period[LED_EFFECT_COUNT] = {
    [LED_EFFECT_NONE]           = 0,
    [LED_EFFECT_RUNNING_FWD]    = 40,   /* 4 позиции × 10 тиков  */
    [LED_EFFECT_RUNNING_BWD]    = 40,
    [LED_EFFECT_RUNNING_PP]     = 60,   /* 6 позиций × 10 тиков  */
    [LED_EFFECT_RUNNING_CIRCLE] = 32,   /* 4 позиции × 8 тиков   */
    [LED_EFFECT_PAIR_BLINK]     = 30,   /* 2 фазы × 15 тиков     */
    [LED_EFFECT_EMERGENCY]      = 10,   /* 2 фазы × 5 тиков      */
    [LED_EFFECT_RANDOM_FLASH]   = 1,    /* нет цикла */
    [LED_EFFECT_DOUBLE_BLINK]   = 80,   /* 16 фаз × 5 тиков      */
    [LED_EFFECT_TRIPLE_BLINK]   = 105,  /* 21 фаза × 5 тиков     */
    [LED_EFFECT_ALTERNATING]    = 40    /* 2 фазы × 20 тиков     */
};

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: инициализация / callback / process                   */
/* ================================================================== */

void led_init(void)
{
    uint8_t i;

    /*
     * Сброс всех 4 конечных автоматов в начальное состояние:
     *   mode = OFF, все счётчики = 0, указатели = NULL.
     */
    for (i = 0; i < LED_COUNT; i++) {
        leds[i].mode      = LED_MODE_OFF;
        leds[i].counter   = 0;
        leds[i].on_ticks  = 0;
        leds[i].off_ticks = 0;
        leds[i].phase     = false;
        leds[i].seq       = (void *)0;
        leds[i].seq_len   = 0;
        leds[i].seq_idx   = 0;
        leds[i].seq_cnt   = 0;
        leds[i].seq_phase = false;
        leds[i].seq_loop  = false;
    }

    /* Сброс глобальных переменных */
    hw_set     = (void *)0;
    cur_effect = LED_EFFECT_NONE;
    g_tick     = 0;
}

void led_set_callback(led_set_fn fn)
{
    hw_set = fn;
}

/**
 * Главная функция — обработка всех LED за один тик.
 *
 * Вызывается пользователем 60 раз в секунду (или с другой частотой).
 * Внутри:
 *   1) g_tick++ — инкремент глобального счётчика.
 *   2) Цикл по 4 LED: для каждого вызывается обработчик по режиму.
 *   3) Если хотя бы один LED в EFFECT-режиме — вызвать функцию эффекта,
 *      которая запишет состояния в effect_st[], и применить их.
 *
 * Важно: эффект применяется ПОСЛЕ ручных режимов, поэтому
 * если LED в EFFECT-режиме, его ручной обработчик НЕ вызывается.
 * Но если пользователь вызвал led_on(id) для LED в EFFECT-режиме,
 * этот LED переключится в ON и эффект его НЕ будет менять.
 */
void led_process(void)
{
    uint8_t i;
    bool effect_st[LED_COUNT];  /* Буфер для состояний эффекта */
    bool any_effect = false;

    /* Глобальный счётчик тиков */
    g_tick++;

    /* Шаг 1: обработка каждого LED в его ручном режиме */
    for (i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode == LED_MODE_EFFECT) {
            /* Этот LED управляется эффектом — пометить, обработаем позже */
            any_effect = true;
        } else {
            /* Вызвать обработчик для текущего режима */
            switch (leds[i].mode) {
                case LED_MODE_OFF:
                    process_off(i);
                    break;
                case LED_MODE_ON:
                    process_on(i);
                    break;
                case LED_MODE_ON_FOR:
                    process_on_for(i);
                    break;
                case LED_MODE_BLINK:
                    process_blink(i);
                    break;
                case LED_MODE_SEQUENCE:
                    process_sequence(i);
                    break;
                default:
                    /* Неизвестный режим — безопасно выключить */
                    process_off(i);
                    break;
            }
        }
    }

    /* Шаг 2: применить эффект к LED в режиме EFFECT */
    if (any_effect && cur_effect != LED_EFFECT_NONE &&
        eff_table[cur_effect]) {

        /* Вызвать функцию эффекта: она запишет состояния в effect_st[] */
        eff_table[cur_effect](g_tick, effect_st);

        /* Применить состояния только к тем LED, которые в EFFECT-режиме */
        for (i = 0; i < LED_COUNT; i++) {
            if (leds[i].mode == LED_MODE_EFFECT) {
                apply_state(i, effect_st[i]);
            }
        }

        /* Авто-остановка эффекта по таймеру */
        if (effect_remaining > 0) {
            effect_remaining--;
            if (effect_remaining == 0) {
                led_stop_effect();
            }
        }
    }
}

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: управление отдельным LED                             */
/* ================================================================== */

void led_on(uint8_t id)
{
    if (id >= LED_COUNT) return;
    set_mode_on(id);
}

void led_off(uint8_t id)
{
    if (id >= LED_COUNT) return;
    set_mode_off(id);
}

/**
 * Переключить состояние диода.
 *
 * Если диод был включён (режим ON) → выключить.
 * Если диод был в ЛЮБОМ другом режиме (OFF, BLINK, EFFECT, SEQUENCE) → включить.
 *
 * Это самый предсказуемый вариант: toggle всегда даёт явный результат.
 * Независимо от того, мигал диод, воспроизводил SOS или управлялся
 * эффектом — после toggle он будет включён или выключен.
 */
void led_toggle(uint8_t id)
{
    if (id >= LED_COUNT) return;
    if (leds[id].mode == LED_MODE_ON) {
        set_mode_off(id);
    } else {
        set_mode_on(id);
    }
}

/**
 * Включить диод на заданное число тиков.
 *
 * Пример: led_on_for(1, 300) → LED1 горит 300/60 = 5 секунд.
 *
 * Если ticks == 0 — диод сразу выключается (безопасность).
 *
 * @param id     Индекс диода (0..3).
 * @param ticks  Длительность в тиках (1 тик = 1/60 сек).
 */
void led_on_for(uint8_t id, uint16_t ticks)
{
    if (id >= LED_COUNT) return;
    if (ticks == 0) {
        set_mode_off(id);
        return;
    }
    leds[id].mode    = LED_MODE_ON_FOR;
    leds[id].counter = ticks;
    apply_state(id, true);
}

/**
 * Непрерывное мигание диода с заданными фазами.
 *
 * Пример: led_blink(0, 15, 15) → 250 мс вкл / 250 мс выкл.
 * Пример: led_blink(0, 5, 25)  → 83 мс вкл / 417 мс выкл (напряжённое).
 *
 * Если любая из фаз = 0 — диод выключается.
 *
 * @param id        Индекс диода.
 * @param on_ticks  Время включённой фазы (в тиках).
 * @param off_ticks Время выключённой фазы (в тиках).
 */
void led_blink(uint8_t id, uint16_t on_ticks, uint16_t off_ticks)
{
    if (id >= LED_COUNT) return;
    if (on_ticks == 0 || off_ticks == 0) {
        set_mode_off(id);
        return;
    }
    leds[id].mode      = LED_MODE_BLINK;
    leds[id].counter   = on_ticks;
    leds[id].on_ticks  = on_ticks;
    leds[id].off_ticks = off_ticks;
    leds[id].phase     = true;
    apply_state(id, true);
}

/**
 * Воспроизвести пользовательскую последовательность.
 *
 * Пример (SOS):
 *   static const led_step_t sos[] = {
 *       LED_STEP_ON(6),  LED_STEP_OFF(6),
 *       LED_STEP_ON(6),  LED_STEP_OFF(6),
 *       LED_STEP_ON(6),  LED_STEP_OFF(18),
 *       LED_STEP_ON(18), LED_STEP_OFF(6),
 *       LED_STEP_ON(18), LED_STEP_OFF(6),
 *       LED_STEP_ON(18), LED_STEP_OFF(18),
 *       LED_STEP_ON(6),  LED_STEP_OFF(6),
 *       LED_STEP_ON(6),  LED_STEP_OFF(6),
 *       LED_STEP_ON(6),  LED_STEP_OFF(60),
 *   };
 *   static const led_sequence_t sos_seq = {
 *       sos, sizeof(sos)/sizeof(sos[0]), true
 *   };
 *   led_play(2, &sos_seq);
 *
 * @param id   Индекс диода (0..3).
 * @param seq  Указатель на определение последовательности.
 */
void led_play(uint8_t id, const led_sequence_t *seq)
{
    if (id >= LED_COUNT || !seq || seq->count == 0) return;

    /* Установить режим SEQUENCE */
    leds[id].mode      = LED_MODE_SEQUENCE;

    /* Загрузить параметры последовательности */
    leds[id].seq       = seq->steps;
    leds[id].seq_len   = seq->count;
    leds[id].seq_loop  = seq->loop;

    /* Начать с первого шага */
    leds[id].seq_idx   = 0;
    leds[id].seq_phase = seq->steps[0].state;
    leds[id].seq_cnt   = seq->steps[0].ticks;

    /* Немедленно применить первое состояние */
    apply_state(id, leds[id].seq_phase);
}

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: глобальные эффекты                                   */
/* ================================================================== */

/**
 * Запустить глобальный эффект.
 *
 * Все 4 диода переходят в режим LED_MODE_EFFECT.
 * Эффект определяется функцией из таблицы eff_table[].
 *
 * Если диод уже в EFFECT-режиме — ничего не меняется (безопасность).
 *
 * @param effect  Один из встроенных эффектов (LED_EFFECT_*).
 */
void led_start_effect(led_effect_t effect)
{
    uint8_t i;
    if (effect >= LED_EFFECT_COUNT) return;

    cur_effect = effect;
    for (i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode != LED_MODE_EFFECT) {
            leds[i].mode = LED_MODE_EFFECT;
        }
    }
}

void led_start_effect_for(led_effect_t effect, uint16_t repeats)
{
    led_start_effect(effect);
    if (effect < LED_EFFECT_COUNT) {
        effect_remaining = repeats * eff_period[effect];
    }
}

/**
 * Остановить глобальный эффект.
 *
 * Все диоды, которые были в режиме EFFECT, выключаются.
 * Диоды, "оторванные" вручную (через led_on/off/blink/etc),
 * сохраняют своё состояние.
 */
void led_stop_effect(void)
{
    uint8_t i;
    for (i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode == LED_MODE_EFFECT) {
            set_mode_off(i);
        }
    }
    cur_effect = LED_EFFECT_NONE;
    effect_remaining = 0;
}

void led_flash_and_fade(uint8_t count)
{
    uint8_t i;
    if (count == 0 || count > LED_COUNT) return;

    /* Остановить текущий эффект, если есть */
    led_stop_effect();

    /*
     * Включить count диодов одновременно, затем погасить по одному.
     *
     * Каждый диод получает разную длительность led_on_for():
     *   LED[0]          — горит дольше всех (последний гаснет)
     *   LED[count-1]    — гаснет первым
     *
     * Интервал между погасаниями: 10 тиков (~167 мс).
     * Время до начала погасания: 60 тиков (1 сек).
     *
     * Пример для count=4:
     *   tick  0-60:  все 4 горят
     *   tick 60:     LED3 гаснет
     *   tick 70:     LED2 гаснет
     *   tick 80:     LED1 гаснет
     *   tick 90:     LED0 гаснет
     */
    for (i = 0; i < count; i++) {
        uint16_t on_time = 120 + (count - 1 - i) * 10;
        led_on_for(i, on_time);
    }
}
