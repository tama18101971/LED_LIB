/*
 * SPDX-License-Identifier: MIT
 *
 * LED_LIB — bare-metal LED control library for independent LEDs.
 * Copyright (c) 2025-2026 tama18101971
 * See LICENSE for the full MIT license text.
 */

/**
 * @file led.c
 * @brief Реализация библиотеки управления светодиодами.
 *
 * =====================================================================
 *  СТРУКТУРА КОДА
 * =====================================================================
 *
 * 1) Внутреннее состояние (static-переменные):
 *    - leds[]           — массив конечных автоматов (по одному на LED)
 *    - hw_set           — callback для доступа к GPIO
 *    - out_mask         — кэш последних выставленных состояний GPIO
 *    - driven_mask      — маска «пин уже записывался хотя бы раз»
 *    - cur_effect       — активный глобальный эффект (uint8_t, 0..12)
 *    - effect_remaining — остаток эффекта в тиках (LED_EFFECT_FOREVER =
 *                         бесконечный режим)
 *    - fx_phase/pos/cnt — счётчики эффектов (общие: активен один эффект)
 *    - rng_state        — состояние xorshift32 для RANDOM_FLASH
 *
 * 2) Вспомогательные функции:
 *    - apply_state()    — единственная точка записи в GPIO (с кэшем)
 *    - set_mode_off/on()— мгновенная смена режима
 *
 * 3) Обработчики конечных автоматов (вызываются из led_process()):
 *    - process_on_for(), process_blink(), process_sequence()
 *
 * 4) Функции эффектов (таблица eff_table[], отдельные группы отключаются
 *    макросами LED_USE_*): эффекты возвращают битовую маску и продвигают
 *    инкрементные счётчики fx_* — без деления и умножения в горячем пути.
 *
 * 5) Публичный API. Мутаторы обёрнуты в LED_ENTER/EXIT_CRITICAL()
 *    (по умолчанию — пусто); внутренние *_locked()-функции выполняют
 *    работу, чтобы избегать вложенных критических секций.
 *
 * =====================================================================
 *  ПОТРЕБЛЕНИЕ РЕСУРСОВ (LED_COUNT = 4, все модули включены)
 * =====================================================================
 *
 *   RAM:  leds[4] × 20 = 80 Б + ~18 Б глобальных = ~100 Б
 *   Flash: ~2.4 КБ; модули LED_USE_* = 0 убирают соответствующий код.
 *
 * =====================================================================
 *  МОДЕЛЬ ВЫЗОВОВ
 * =====================================================================
 *
 *   led_process() — из одного контекста с постоянной частотой (60 Гц).
 *   Мутаторы (led_on/led_blink/led_play/…) — из того же контекста либо
 *   из другого, если определены LED_ENTER/EXIT_CRITICAL() (запрет
 *   прерываний). led_process() критической секцией не оборачивается.
 *   Подробно — в документации led.h.
 */

#include "led.h"
#include <stddef.h>   /* NULL — входит в набор freestanding-заголовков C99 */

/* ================================================================== */
/*  ФЛАГИ СОСТОЯНИЯ                                                    */
/* ================================================================== */

#define LED_F_PHASE     0x01  /**< BLINK: сейчас ON-фаза            */
#define LED_F_SEQ_PHASE 0x02  /**< SEQUENCE: состояние текущего шага */
#define LED_F_SEQ_LOOP  0x04  /**< SEQUENCE: зациклена              */

/* ================================================================== */
/*  ПРОИЗВОДНЫЕ ПЕРИОДЫ ЭФФЕКТОВ (зависят от LED_COUNT)                */
/* ================================================================== */

#if LED_USE_RUNNING_EFFECTS
#define LED_RUN_STEP     ((uint8_t)10u)   /* выдержка позиции: 10 тиков      */
#define LED_CIRCLE_STEP  ((uint8_t)8u)    /* выдержка позиции в CIRCLE: 8    */
#define LED_RUN_PERIOD   ((uint16_t)(LED_COUNT * 10u))   /* полный цикл FWD/BWD */
#define LED_CIRCLE_PERIOD ((uint16_t)(LED_COUNT * 8u))   /* полный цикл CIRCLE */
#endif

#if LED_USE_HOLD_EFFECTS
#define LED_HOLD_RUN     ((uint8_t)(LED_COUNT * 10u))             /* бег        */
#define LED_HOLD_PAUSE   ((uint8_t)20u)                           /* пауза      */
#define LED_HOLD_TIME    ((uint8_t)45u)                           /* удержание  */
#define LED_HOLD_PERIOD  ((uint8_t)(LED_COUNT * 10u + 65u))       /* весь цикл  */
#endif

/* Маски паттернов, производные от LED_COUNT (работают для 1..8 диодов). */
#define LED_MASK_ALL  ((uint8_t)((1u << LED_COUNT) - 1u))
#define LED_MASK_EVEN ((uint8_t)(0x55u & LED_MASK_ALL))  /**< диоды 0,2,4,… */
#define LED_MASK_ODD  ((uint8_t)(0xAAu & LED_MASK_ALL))  /**< диоды 1,3,5,… */

/* ================================================================== */
/*  СОСТОЯНИЕ ОДНОГО СВЕТОДИОДА                                        */
/* ================================================================== */

/**
 * Внутреннее состояние одного светодиода (конечный автомат).
 *
 * Поля сгруппированы так, чтобы минимизировать выравнивание:
 * указатель идёт первым (выравнивание 4), затем uint16_t, затем байты.
 * При LED_USE_SEQUENCES = 1 — 20 байт на LED, иначе — 8 байт.
 */
typedef struct {
#if LED_USE_SEQUENCES
    const led_step_t *seq;       /* 0..3   */
    uint16_t          seq_len;   /* 4..5   */
    uint16_t          seq_idx;   /* 6..7   */
    uint16_t          seq_cnt;   /* 8..9   */
#endif
    uint16_t counter;            /* счётчик тиков текущей фазы        */
    uint16_t on_ticks;           /* BLINK: длительность ON-фазы       */
    uint16_t off_ticks;          /* BLINK: длительность OFF-фазы      */
    uint8_t  mode;               /* led_mode_t, упакован в байт       */
    uint8_t  flags;              /* LED_F_*                           */
} led_state_t;

/* ================================================================== */
/*  ВНУТРЕННЕЕ СОСТОЯНИЕ                                               */
/* ================================================================== */

static led_state_t leds[LED_COUNT];

/** Callback записи в GPIO. NULL = вывод отключён. */
static led_set_fn hw_set = NULL;

/**
 * Кэш последних состояний GPIO (бит id = состояние LED id) и маска
 * «пин уже записывался». Благодаря кэшу callback вызывается только при
 * фактическом изменении вывода; driven_mask гарантирует первую запись
 * после led_init()/led_set_callback()/led_refresh().
 */
static uint8_t out_mask    = 0;
static uint8_t driven_mask = 0;

#if LED_USE_EFFECTS

/** Активный эффект (uint8_t вместо enum — экономия RAM). */
static uint8_t cur_effect = (uint8_t)LED_EFFECT_NONE;

/**
 * Остаток эффекта в тиках. LED_EFFECT_FOREVER — бесконечный режим.
 * Отсчитывается в led_process() независимо от того, сколько диодов
 * осталось «привязано» к эффекту. Нулевая инициализация: guard идёт
 * по cur_effect, значение читается только при активном эффекте.
 */
static uint32_t effect_remaining = 0;

/**
 * Счётчики эффектов (общие, т.к. одновременно активен один эффект):
 *   fx_phase — фаза (blink-подобные эффекты, удержание, направление PP)
 *   fx_pos   — позиция бегущего огня
 *   fx_cnt   — тиковый делитель внутри фазы/позиции
 */
static uint8_t fx_phase = 0;
static uint8_t fx_pos   = 0;
static uint8_t fx_cnt   = 0;

/** Состояние xorshift32 для RANDOM_FLASH; сбрасывается в led_init(). */
static uint32_t rng_state = 0xDEADBEEFu;

#endif /* LED_USE_EFFECTS */

/* ================================================================== */
/*  ЗАПИСЬ В GPIO                                                      */
/* ================================================================== */

/**
 * Вызвать callback, только если состояние вывода реально изменилось
 * (или пин ещё ни разу не записывался). Единственная точка
 * взаимодействия с аппаратурой.
 *
 * @param id     Индекс диода (0..LED_COUNT-1).
 * @param state  true = включить, false = выключить.
 */
static void apply_state(uint8_t id, bool state)
{
    uint8_t bit  = (uint8_t)(1u << id);
    bool    curr = (out_mask & bit) != 0;

    if (hw_set == NULL) {
        return;
    }
    if (!(driven_mask & bit) || (curr != state)) {
        out_mask     = state ? (uint8_t)(out_mask | bit)
                             : (uint8_t)(out_mask & (uint8_t)~bit);
        driven_mask |= bit;
        hw_set(id, state);
    }
}

/* ================================================================== */
/*  СМЕНА РЕЖИМА (контекст уже защищён критической секцией)            */
/* ================================================================== */

static void set_mode_off(uint8_t id)
{
    leds[id].mode    = LED_MODE_OFF;
    leds[id].counter = 0;
    apply_state(id, false);
}

static void set_mode_on(uint8_t id)
{
    leds[id].mode    = LED_MODE_ON;
    leds[id].counter = 0;
    apply_state(id, true);
}

static void on_for_locked(uint8_t id, uint16_t ticks)
{
    if (ticks == 0) {
        set_mode_off(id);
        return;
    }
    leds[id].mode    = LED_MODE_ON_FOR;
    leds[id].counter = ticks;
    apply_state(id, true);
}

static void blink_locked(uint8_t id, uint16_t on_ticks, uint16_t off_ticks)
{
    if (on_ticks == 0 || off_ticks == 0) {
        set_mode_off(id);
        return;
    }
    leds[id].mode      = LED_MODE_BLINK;
    leds[id].counter   = on_ticks;
    leds[id].on_ticks  = on_ticks;
    leds[id].off_ticks = off_ticks;
    leds[id].flags     = LED_F_PHASE;
    apply_state(id, true);
}

#if LED_USE_SEQUENCES
static void play_locked(uint8_t id, const led_sequence_t *seq)
{
    if (seq == NULL || seq->steps == NULL || seq->count == 0) {
        return;
    }
    leds[id].mode    = LED_MODE_SEQUENCE;
    leds[id].seq     = seq->steps;
    leds[id].seq_len = seq->count;
    leds[id].seq_idx = 0;
    /* Нулевая длительность шага трактуется как 1 тик. */
    leds[id].seq_cnt = (seq->steps[0].ticks != 0) ? seq->steps[0].ticks : 1u;
    leds[id].flags   = (uint8_t)((seq->loop ? LED_F_SEQ_LOOP : 0) |
                                 (seq->steps[0].state ? LED_F_SEQ_PHASE : 0));
    apply_state(id, (leds[id].flags & LED_F_SEQ_PHASE) != 0);
}
#endif

/* ================================================================== */
/*  ОБРАБОТЧИКИ КОНЕЧНЫХ АВТОМАТОВ                                     */
/* ================================================================== */

/**
 * ON_FOR: диод горит ровно ticks тиков, затем авто-выключение.
 * counter гарантированно > 0 на входе: led_on_for() с ticks == 0
 * сразу переводит диод в OFF.
 */
static void process_on_for(uint8_t id)
{
    apply_state(id, true);
    leds[id].counter--;
    if (leds[id].counter == 0) {
        leds[id].mode = LED_MODE_OFF;
        apply_state(id, false);
    }
}

/**
 * BLINK: двухфазный цикл. counter > 0 на входе гарантирован:
 * нулевые фазы отсекаются в led_blink(), при смене фазы всегда
 * загружается ненулевая длительность противоположной фазы.
 */
static void process_blink(uint8_t id)
{
    led_state_t *s = &leds[id];

    if (s->flags & LED_F_PHASE) {
        apply_state(id, true);
        s->counter--;
        if (s->counter == 0) {
            s->counter = s->off_ticks;
            s->flags  &= (uint8_t)~LED_F_PHASE;
        }
    } else {
        apply_state(id, false);
        s->counter--;
        if (s->counter == 0) {
            s->counter = s->on_ticks;
            s->flags  |= LED_F_PHASE;
        }
    }
}

#if LED_USE_SEQUENCES
/**
 * SEQUENCE: проигрывание массива шагов. seq_cnt > 0 на входе
 * гарантирован: загрузчик шага трактовет ticks == 0 как 1 тик.
 */
static void process_sequence(uint8_t id)
{
    led_state_t *s = &leds[id];

    apply_state(id, (s->flags & LED_F_SEQ_PHASE) != 0);

    s->seq_cnt--;
    if (s->seq_cnt == 0) {
        s->seq_idx++;

        if (s->seq_idx >= s->seq_len) {
            if (s->flags & LED_F_SEQ_LOOP) {
                s->seq_idx = 0;
            } else {
                s->mode = LED_MODE_OFF;
                apply_state(id, false);
                return;
            }
        }

        s->flags = (uint8_t)((s->flags & (uint8_t)~LED_F_SEQ_PHASE) |
                             (s->seq[s->seq_idx].state ? LED_F_SEQ_PHASE : 0u));
        s->seq_cnt = (s->seq[s->seq_idx].ticks != 0)
                         ? s->seq[s->seq_idx].ticks : 1u;
        /* Новое состояние применяется немедленно: шаг длится ровно
         * ticks тиков, как и фазы blink (проверено тайминг-тестом). */
        apply_state(id, (s->flags & LED_F_SEQ_PHASE) != 0);
    }
}
#endif

/* ================================================================== */
/*  ФУНКЦИИ ЭФФЕКТОВ                                                   */
/* ================================================================== */

#if LED_USE_EFFECTS

/**
 * Инкремент счётчика с заворотом в диапазоне 0..max-1.
 * Общая функция вместо дублирования инкрементов в каждом эффекте.
 */
static uint8_t fx_adv(uint8_t v, uint8_t max)
{
    return (uint8_t)((v + 1u < max) ? v + 1u : 0u);
}

#if LED_USE_RUNNING_EFFECTS
/**
 * Бегущий огонь: RUNNING_FWD (0→N-1), RUNNING_BWD (N-1→0),
 * RUNNING_CIRCLE (вперёд быстрее). Период выбирается по cur_effect.
 *
 * Маска вычисляется ДО продвижения счётчиков: каждая позиция горит
 * ровно period тиков, включая самую первую после запуска.
 */
static uint8_t eff_running(void)
{
    bool    bwd    = (cur_effect == (uint8_t)LED_EFFECT_RUNNING_BWD);
    uint8_t period = (cur_effect == (uint8_t)LED_EFFECT_RUNNING_CIRCLE)
                         ? LED_CIRCLE_STEP : LED_RUN_STEP;
    uint8_t pos    = bwd ? (uint8_t)(LED_COUNT - 1u - fx_pos) : fx_pos;

    fx_cnt = fx_adv(fx_cnt, period);
    if (fx_cnt == 0) {
        fx_pos = fx_adv(fx_pos, (uint8_t)LED_COUNT);
    }
    return (uint8_t)(1u << pos);
}
#endif

#if LED_USE_PINGPONG_EFFECT
/**
 * Бегущий огонь туда-обратно: 0→1→…→N-1→N-2→…→0→1→…
 *
 * Маска выдаётся ДО продвижения счётчиков — каждая позиция, включая
 * первую, горит ровно 10 тиков (одинаковая дисциплина с eff_running()).
 * Направление хранится в fx_phase: 0 = вперёд, 1 = назад.
 */
static uint8_t eff_running_pp(void)
{
    uint8_t mask = (uint8_t)(1u << fx_pos);

    fx_cnt = fx_adv(fx_cnt, (uint8_t)10u);
    if (fx_cnt == 0) {
        if (fx_phase == 0) {
            if (fx_pos + 1u >= LED_COUNT) {
                fx_phase = 1;
                fx_pos   = (uint8_t)(LED_COUNT - 2u);
            } else {
                fx_pos++;
            }
        } else {
            if (fx_pos == 0) {
                fx_phase = 0;
                fx_pos   = 1;
            } else {
                fx_pos--;
            }
        }
    }
    return mask;
}
#endif

#if LED_USE_BLINK_EFFECTS
/**
 * Групповое мигание: EMERGENCY (все диоды, 5/5 тиков),
 * PAIR_BLINK (чёт/нечет, 15/15), ALTERNATING (чёт/нечет, 20/20).
 * Маски чёт/нечет производятся от LED_COUNT и работают для 1..8 диодов.
 */
static uint8_t eff_group_blink(void)
{
    bool    on  = (fx_phase == 0);
    bool    all = (cur_effect == (uint8_t)LED_EFFECT_EMERGENCY);
    uint8_t period;

    if (cur_effect == (uint8_t)LED_EFFECT_ALTERNATING) {
        period = 20;
    } else if (cur_effect == (uint8_t)LED_EFFECT_EMERGENCY) {
        period = 5;
    } else {
        period = 15;
    }

    fx_cnt = fx_adv(fx_cnt, period);
    if (fx_cnt == 0) {
        fx_phase ^= 1u;
    }

    return all ? (on ? LED_MASK_ALL : (uint8_t)0)
               : (on ? LED_MASK_EVEN : LED_MASK_ODD);
}
#endif

#if LED_USE_MULTI_BLINK_EFFECTS
/**
 * Двойное/тройное мигание (все диоды вместе).
 *   DOUBLE: 16 фаз × 5 тиков; ON на фазах 0-1 и 4-5.
 *   TRIPLE: 21 фаза × 5 тиков; ON на фазах 0,1,4,5,8,9.
 */
static uint8_t eff_multi_blink(void)
{
    bool triple = (cur_effect == (uint8_t)LED_EFFECT_TRIPLE_BLINK);
    bool on;

    if (triple) {
        on = (fx_phase < 10) && ((fx_phase & 3u) < 2u);
    } else {
        on = (fx_phase < 2) || (fx_phase >= 4 && fx_phase < 6);
    }

    fx_cnt = fx_adv(fx_cnt, (uint8_t)5u);
    if (fx_cnt == 0) {
        fx_phase = fx_adv(fx_phase, triple ? (uint8_t)21u : (uint8_t)16u);
    }

    return on ? LED_MASK_ALL : (uint8_t)0;
}
#endif

#if LED_USE_RANDOM_EFFECT
/**
 * Случайные вспышки — xorshift32 (период 2^32-1). Младшие 4 бита
 * определяют состояние диодов (при LED_COUNT > 4 старшие биты
 * маски автоматически отсекаются в led_process()).
 */
static uint8_t eff_random_flash(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (uint8_t)(rng_state & 0x0Fu);
}
#endif

#if LED_USE_HOLD_EFFECTS
/**
 * Бегущий огонь + пауза + удержание крайнего диода 45 тиков (0.75 с).
 * Обслуживает RUNNING_FWD_HOLD и RUNNING_BWD_HOLD.
 *
 * Фазы (fx_phase = 0..LED_HOLD_PERIOD-1, 1 фаза = 1 тик):
 *   0 .. RUN-1          — бег (10 тиков на позицию)
 *   RUN .. RUN+PAUSE-1  — пауза (все выключены)
 *   RUN+PAUSE .. конец  — крайний диод горит
 *
 * Периоды производны от LED_COUNT, поэтому эффект корректен для 1..8
 * диодов. При переходе фазы через 0 (новый цикл) счётчики бега
 * сбрасываются — все циклы эффекта идентичны.
 */
static uint8_t eff_running_hold(void)
{
    bool    bwd  = (cur_effect == (uint8_t)LED_EFFECT_RUNNING_BWD_HOLD);
    uint8_t mask = 0;
    uint8_t run  = LED_HOLD_RUN;

    if (fx_phase < run) {
        mask = (uint8_t)(1u << (bwd ? (uint8_t)(LED_COUNT - 1u - fx_pos)
                                    : fx_pos));
    } else if (fx_phase >= (uint8_t)(run + LED_HOLD_PAUSE)) {
        mask = (uint8_t)(1u << (bwd ? 0u : (uint8_t)(LED_COUNT - 1u)));
    }

    fx_phase = fx_adv(fx_phase, LED_HOLD_PERIOD);

    if (fx_phase == 0) {
        /* Новый цикл: бег начинается с чистого счётчика. */
        fx_cnt = 0;
        fx_pos = 0;
    } else if (fx_phase < run) {
        fx_cnt = fx_adv(fx_cnt, (uint8_t)10u);
        if (fx_cnt == 0) {
            fx_pos = fx_adv(fx_pos, (uint8_t)LED_COUNT);
        }
    } else {
        fx_cnt = 0;
        fx_pos = 0;
    }

    return mask;
}
#endif

/* ------------------------------------------------------------------ */
/*  ТАБЛИЦА ЭФФЕКТОВ                                                   */
/* ------------------------------------------------------------------ */

/** Тип функции-эффекта: возвращает битовую маску (бит i = LED i). */
typedef uint8_t (*eff_fn)(void);

/**
 * Таблица эффектов, индексируется значениями led_effect_t.
 * Записи отключённых модулей отсутствуют → NULL → led_start_effect()
 * такие эффекты игнорирует. Значения enum от конфигурации не зависят.
 */
static const eff_fn eff_table[LED_EFFECT_COUNT] = {
    [LED_EFFECT_NONE]             = NULL,
#if LED_USE_RUNNING_EFFECTS
    [LED_EFFECT_RUNNING_FWD]      = eff_running,
    [LED_EFFECT_RUNNING_BWD]      = eff_running,
    [LED_EFFECT_RUNNING_CIRCLE]   = eff_running,
#endif
#if LED_USE_PINGPONG_EFFECT
    [LED_EFFECT_RUNNING_PP]       = eff_running_pp,
#endif
#if LED_USE_BLINK_EFFECTS
    [LED_EFFECT_PAIR_BLINK]       = eff_group_blink,
    [LED_EFFECT_EMERGENCY]        = eff_group_blink,
    [LED_EFFECT_ALTERNATING]      = eff_group_blink,
#endif
#if LED_USE_RANDOM_EFFECT
    [LED_EFFECT_RANDOM_FLASH]     = eff_random_flash,
#endif
#if LED_USE_MULTI_BLINK_EFFECTS
    [LED_EFFECT_DOUBLE_BLINK]     = eff_multi_blink,
    [LED_EFFECT_TRIPLE_BLINK]     = eff_multi_blink,
#endif
#if LED_USE_HOLD_EFFECTS
    [LED_EFFECT_RUNNING_FWD_HOLD] = eff_running_hold,
    [LED_EFFECT_RUNNING_BWD_HOLD] = eff_running_hold,
#endif
};

/**
 * Длительность одного цикла эффекта (в тиках) для пересчёта повторов.
 * Индекс и правила заполнения — те же, что у eff_table[].
 */
static const uint16_t eff_period[LED_EFFECT_COUNT] = {
    [LED_EFFECT_NONE]             = 0,
#if LED_USE_RUNNING_EFFECTS
    [LED_EFFECT_RUNNING_FWD]      = LED_RUN_PERIOD,
    [LED_EFFECT_RUNNING_BWD]      = LED_RUN_PERIOD,
    [LED_EFFECT_RUNNING_CIRCLE]   = LED_CIRCLE_PERIOD,
#endif
#if LED_USE_PINGPONG_EFFECT
    [LED_EFFECT_RUNNING_PP]       = (uint16_t)((2u * (unsigned)LED_COUNT - 2u) * 10u),
#endif
#if LED_USE_BLINK_EFFECTS
    [LED_EFFECT_PAIR_BLINK]       = 30,   /* 2 фазы × 15 тиков */
    [LED_EFFECT_EMERGENCY]        = 10,   /* 2 фазы × 5 тиков  */
    [LED_EFFECT_ALTERNATING]      = 40,   /* 2 фазы × 20 тиков */
#endif
#if LED_USE_RANDOM_EFFECT
    [LED_EFFECT_RANDOM_FLASH]     = 1,    /* без цикла: repeats = тики */
#endif
#if LED_USE_MULTI_BLINK_EFFECTS
    [LED_EFFECT_DOUBLE_BLINK]     = 80,   /* 16 фаз × 5 тиков */
    [LED_EFFECT_TRIPLE_BLINK]     = 105,  /* 21 фаза × 5 тиков */
#endif
#if LED_USE_HOLD_EFFECTS
    [LED_EFFECT_RUNNING_FWD_HOLD] = (uint16_t)(LED_HOLD_RUN + LED_HOLD_PAUSE + LED_HOLD_TIME),
    [LED_EFFECT_RUNNING_BWD_HOLD] = (uint16_t)(LED_HOLD_RUN + LED_HOLD_PAUSE + LED_HOLD_TIME),
#endif
};

#endif /* LED_USE_EFFECTS */

/* ================================================================== */
/*  ГЛОБАЛЬНЫЕ ЭФФЕКТЫ: запуск/остановка (контекст защищён)            */
/* ================================================================== */

#if LED_USE_EFFECTS

static void stop_effect_inner(void)
{
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode == LED_MODE_EFFECT) {
            set_mode_off(i);
        }
    }
    cur_effect       = (uint8_t)LED_EFFECT_NONE;
    effect_remaining = 0;
}

/**
 * Единая точка запуска эффекта.
 *
 * Идемпотентна: если этот же эффект уже активен, вызов не меняет
 * ничего (безопасно вызывать в цикле опроса). force = true обходит
 * идемпотентность — перезапуск с нулевой фазы.
 *
 * @param effect Один из LED_EFFECT_*.
 * @param ticks  Длительность в тиках; 0 = бесконечно.
 * @param force  true — перезапустить, даже если эффект уже активен.
 */
static void launch_effect(led_effect_t effect, uint32_t ticks, bool force)
{
    uint8_t i;

    if ((unsigned int)effect >= (unsigned int)LED_EFFECT_COUNT) {
        return;
    }
    if (effect == LED_EFFECT_NONE) {
        stop_effect_inner();
        return;
    }
    if (eff_table[(uint8_t)effect] == NULL) {
        return;   /* модуль эффекта отключён конфигурацией LED_USE_* */
    }
    if (!force && cur_effect == (uint8_t)effect) {
        return;   /* идемпотентность */
    }

    cur_effect       = (uint8_t)effect;
    effect_remaining = (ticks != 0) ? ticks : LED_EFFECT_FOREVER;
    fx_phase = 0;
    fx_pos   = 0;
    fx_cnt   = 0;

    for (i = 0; i < LED_COUNT; i++) {
        leds[i].mode = LED_MODE_EFFECT;
    }
}

/** Длительность одного цикла эффекта в тиках (0 для некорректных значений). */
static uint16_t effect_period(led_effect_t effect)
{
    if ((unsigned int)effect < (unsigned int)LED_EFFECT_COUNT) {
        return eff_period[(uint8_t)effect];
    }
    return 0;
}

#endif /* LED_USE_EFFECTS */

/* ================================================================== */
/*  ГЛАВНАЯ ФУНКЦИЯ                                                    */
/* ================================================================== */

void led_process(void)
{
    uint8_t i;

    /* Шаг 1: диоды в ручных режимах. */
    for (i = 0; i < LED_COUNT; i++) {
        switch (leds[i].mode) {
            case LED_MODE_OFF:
                apply_state(i, false);
                break;
            case LED_MODE_ON:
                apply_state(i, true);
                break;
            case LED_MODE_ON_FOR:
                process_on_for(i);
                break;
            case LED_MODE_BLINK:
                process_blink(i);
                break;
#if LED_USE_SEQUENCES
            case LED_MODE_SEQUENCE:
                process_sequence(i);
                break;
#endif
            case LED_MODE_EFFECT:
#if LED_USE_EFFECTS
                /* Обрабатывается ниже, единым вызовом эффекта. */
#else
                set_mode_off(i);   /* подсистема эффектов отключена */
#endif
                break;
            default:
                /* Неизвестный режим — самовосстановление в OFF. */
                leds[i].mode = LED_MODE_OFF;
                leds[i].counter = 0;
                apply_state(i, false);
                break;
        }
    }

#if LED_USE_EFFECTS
    /* Шаг 2: глобальный эффект. */
    if (cur_effect != (uint8_t)LED_EFFECT_NONE) {
        /*
         * Таймер проверяется ДО отрисовки кадра: последний кадр остаётся
         * видимым полный тик, затем эффект останавливается. Отсчёт идёт
         * независимо от числа диодов, оставшихся в режиме EFFECT:
         * если пользователь «оторвал» все диоды, эффект всё равно
         * завершится вовремя.
         */
        if (effect_remaining == 0) {
            stop_effect_inner();
        } else {
            uint8_t effect_mask = eff_table[cur_effect]();

            for (i = 0; i < LED_COUNT; i++) {
                if (leds[i].mode == LED_MODE_EFFECT) {
                    apply_state(i, (effect_mask & (uint8_t)(1u << i)) != 0);
                }
            }

            if (effect_remaining != LED_EFFECT_FOREVER) {
                effect_remaining--;
            }
        }
    }
#endif
}

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: жизненный цикл                                      */
/* ================================================================== */

void led_init(void)
{
    uint8_t i;

    LED_ENTER_CRITICAL();

    /* Явный сброс полей — без зависимости от libc (memset). */
    for (i = 0; i < LED_COUNT; i++) {
#if LED_USE_SEQUENCES
        leds[i].seq     = NULL;
        leds[i].seq_len = 0;
        leds[i].seq_idx = 0;
        leds[i].seq_cnt = 0;
#endif
        leds[i].counter   = 0;
        leds[i].on_ticks  = 0;
        leds[i].off_ticks = 0;
        leds[i].mode      = LED_MODE_OFF;
        leds[i].flags     = 0;
    }

    /*
     * Кэш GPIO инвалидируется: первый led_process() принудительно
     * запишет все выводы (в т.ч. погасит диоды, включённые до инициализации).
     * Callback сохраняется — повторная инициализация не отключает вывод.
     */
    out_mask    = 0;
    driven_mask = 0;

#if LED_USE_EFFECTS
    cur_effect       = (uint8_t)LED_EFFECT_NONE;
    effect_remaining = 0;
    fx_phase         = 0;
    fx_pos           = 0;
    fx_cnt           = 0;
    rng_state        = 0xDEADBEEFu;
#endif

    LED_EXIT_CRITICAL();
}

void led_set_callback(led_set_fn fn)
{
    LED_ENTER_CRITICAL();
    hw_set      = fn;
    driven_mask = 0;   /* новый обработчик получит полную картину на след. тике */
    LED_EXIT_CRITICAL();
}

void led_refresh(void)
{
    LED_ENTER_CRITICAL();
    driven_mask = 0;
    LED_EXIT_CRITICAL();
}

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: управление отдельным LED                            */
/* ================================================================== */

void led_on(uint8_t id)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    set_mode_on(id);
    LED_EXIT_CRITICAL();
}

void led_off(uint8_t id)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    set_mode_off(id);
    LED_EXIT_CRITICAL();
}

void led_toggle(uint8_t id)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    /* Переключение по фактическому состоянию вывода, а не по режиму. */
    if ((out_mask & (uint8_t)(1u << id)) != 0) {
        set_mode_off(id);
    } else {
        set_mode_on(id);
    }
    LED_EXIT_CRITICAL();
}

void led_on_for(uint8_t id, uint16_t ticks)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    on_for_locked(id, ticks);
    LED_EXIT_CRITICAL();
}

void led_blink(uint8_t id, uint16_t on_ticks, uint16_t off_ticks)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    blink_locked(id, on_ticks, off_ticks);
    LED_EXIT_CRITICAL();
}

#if LED_USE_SEQUENCES
void led_play(uint8_t id, const led_sequence_t *seq)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    play_locked(id, seq);
    LED_EXIT_CRITICAL();
}
#endif

void led_flash_and_fade(uint8_t count)
{
    uint8_t i;

    if (count == 0 || count > LED_COUNT) {
        return;
    }

    LED_ENTER_CRITICAL();
#if LED_USE_EFFECTS
    stop_effect_inner();
#endif
    for (i = count; i < LED_COUNT; i++) {
        set_mode_off(i);
    }
    /* LED[count-1] гаснет первым (60 тиков), LED[0] — последним. */
    for (i = 0; i < count; i++) {
        on_for_locked(i, (uint16_t)(60u + (uint16_t)(count - 1u - i) * 10u));
    }
    LED_EXIT_CRITICAL();
}

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: глобальные эффекты                                  */
/* ================================================================== */

#if LED_USE_EFFECTS

void led_start_effect(led_effect_t effect)
{
    LED_ENTER_CRITICAL();
    launch_effect(effect, 0, false);
    LED_EXIT_CRITICAL();
}

void led_start_effect_for(led_effect_t effect, uint16_t repeats)
{
    uint32_t ticks = (repeats != 0)
                         ? (uint32_t)repeats * effect_period(effect)
                         : 0;

    LED_ENTER_CRITICAL();
    launch_effect(effect, ticks, false);
    LED_EXIT_CRITICAL();
}

void led_restart_effect_for(led_effect_t effect, uint16_t repeats)
{
    uint32_t ticks = (repeats != 0)
                         ? (uint32_t)repeats * effect_period(effect)
                         : 0;

    LED_ENTER_CRITICAL();
    launch_effect(effect, ticks, true);
    LED_EXIT_CRITICAL();
}

void led_stop_effect(void)
{
    LED_ENTER_CRITICAL();
    stop_effect_inner();
    LED_EXIT_CRITICAL();
}

uint8_t led_effect_active(void)
{
    return (cur_effect != (uint8_t)LED_EFFECT_NONE) ? 1u : 0u;
}

uint32_t led_effect_remaining(void)
{
    return effect_remaining;
}

#endif /* LED_USE_EFFECTS */

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: запросы состояния                                   */
/* ================================================================== */

led_mode_t led_get_mode(uint8_t id)
{
    if (id >= LED_COUNT) {
        return LED_MODE_OFF;
    }
    return (led_mode_t)leds[id].mode;
}

uint8_t led_is_on(uint8_t id)
{
    if (id >= LED_COUNT) {
        return 0;
    }
    return ((out_mask & (uint8_t)(1u << id)) != 0) ? 1u : 0u;
}

uint8_t led_any_led_active(void)
{
    uint8_t i;
    uint8_t active = 0;

    LED_ENTER_CRITICAL();
    for (i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode == LED_MODE_ON_FOR ||
            leds[i].mode == LED_MODE_BLINK ||
            leds[i].mode == LED_MODE_SEQUENCE ||
            leds[i].mode == LED_MODE_EFFECT) {
            active = 1;
            break;
        }
    }
    LED_EXIT_CRITICAL();

    return active;
}
