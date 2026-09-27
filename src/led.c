/*
 * SPDX-License-Identifier: MIT
 *
 * LED_LIB — bare-metal LED control library for independent LEDs.
 * Copyright (c) 2025-2026 tama18101971
 * See LICENSE for the full MIT license text.
 */

/**
 * @file led.c
 * @brief Реализация библиотеки управления светодиодами (v3.0).
 *
 * =====================================================================
 *  СТРУКТУРА КОДА
 * =====================================================================
 *
 * 1) Внутреннее состояние (static-переменные):
 *    - leds[]        — массив конечных автоматов (по одному на канал)
 *    - hw_set        — callback для доступа к выводу
 *    - out_level[]   — кэш уровней, реально отданных в вывод
 *    - driven_mask   — маска «вывод уже записывался хотя бы раз»
 *    - cur_owner     — текущий владелец индикации (led_owner_t)
 *    - cur_effect / effect_remaining / fx_* / rng_state — глобальный эффект
 *    - pc_*          — конечный автомат упакованных команд
 *
 * 2) Вспомогательные функции:
 *    - apply_level() — единственная точка записи в вывод (с кэшем уровней)
 *    - set_mode_*()  — мгновенная смена режима
 *    - sync_outputs()— перезапись всех выводов кэшированными уровнями
 *
 * 3) Обработчики конечных автоматов (вызываются из tick_one()):
 *    - process_on_for(), process_blink(), process_sequence()
 *
 * 4) Функции эффектов (таблица eff_table[], отдельные группы отключаются
 *    макросами LED_USE_*): эффекты возвращают битовую маску и продвигают
 *    инкрементные счётчики fx_* — без деления и умножения в горячем пути.
 *
 * 5) Конечный автомат упакованных команд (packed_*): 4 байта на пару фаз.
 *
 * 6) Продвижение времени: led_advance() / led_ticks_until_next().
 *    Тики без переходов проматываются пачкой (idle_ticks) — O(1) на тик,
 *    переходы обрабатываются строго по одному, поэтому
 *    led_advance(1) × N тождественно led_advance(N).
 *
 * 7) Публичный API. Мутаторы обёрнуты в LED_ENTER/EXIT_CRITICAL()
 *    (по умолчанию — пусто); внутренние *_locked()-функции выполняют
 *    работу, чтобы избегать вложенных критических секций.
 *
 * =====================================================================
 *  ПОТРЕБЛЕНИЕ РЕСУРСОВ (LED_COUNT = 4, все модули включены)
 * =====================================================================
 *
 *   RAM:  leds[4] × 20 = 80 Б + ~44 Б глобальных = ~124 Б
 *   Flash: ~3 КБ; модули LED_USE_* = 0 убирают соответствующий код.
 *
 * =====================================================================
 *  МОДЕЛЬ ВЫЗОВОВ
 * =====================================================================
 *
 *   led_advance() — из одного контекста; хост сам задаёт тик и может
 *   спать между вызовами (см. рецепт «спящий хост» в led.h).
 *   Мутаторы (led_on/led_blink/led_play/…) — из того же контекста либо
 *   из другого, если определены LED_ENTER/EXIT_CRITICAL() (запрет
 *   прерываний). led_advance() критической секцией не оборачивается.
 */

#include "led.h"
#include <stddef.h>   /* NULL — входит в набор freestanding-заголовков C99 */

/* ================================================================== */
/*  СТАТИЧЕСКИЕ ПРОВЕРКИ ФОРМАТА УПАКОВАННОЙ КОМАНДЫ                   */
/* ================================================================== */

/*
 * Раскладка 32-битного слова команды — часть публичного ABI, поэтому
 * проверяется на этапе компиляции ВСЕМ, кто подключает библиотеку, а не
 * только тестами. C11 _Static_assert даёт читаемый диагноз; при
 * компиляции как C99 используется эквивалент через массив отрицательного
 * размера (нарушение constraint -> ошибка компиляции), чтобы проект
 * оставался строгим C99.
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#  define LED_SA(cond, tag) _Static_assert(cond, #tag)
#else
#  define LED_SA(cond, tag) typedef char led_static_assert_##tag[(cond) ? 1 : -1]
#endif

/* Все поля на максимуме == 0xFFFFFFFF: ни пересечений, ни пропусков. */
LED_SA(LED_CMD(0xFu, 0xFu, 1032192u, 1032192u, 7u, 7u) == 0xFFFFFFFFu,
       cmd_full_word);
LED_SA(LED_CMD_DEV(0xFFFFFFFFu)    == 0x0Fu, cmd_span_device);
LED_SA(LED_CMD_COUNT(0xFFFFFFFFu)  == 0x0Fu, cmd_span_count);
LED_SA(LED_CMD_T0F(0xFFFFFFFFu)    == 0x1FFu, cmd_span_time0);
LED_SA(LED_CMD_T1F(0xFFFFFFFFu)    == 0x1FFu, cmd_span_time1);
LED_SA(LED_CMD_LVL0(0xFFFFFFFFu)   == 0x07u, cmd_span_level0);
LED_SA(LED_CMD_LVL1(0xFFFFFFFFu)   == 0x07u, cmd_span_level1);
/* Нулевое слово — системная команда STOP. */
LED_SA(0u == LED_SYS_STOP, cmd_zero_is_stop);
LED_SA(LED_CMD_IS_SYS(LED_SYS_STOP) && (LED_CMD_OP(LED_SYS_STOP) == 0u) &&
       (LED_CMD_STEPS(LED_SYS_STOP) == 0u), cmd_stop_fields);
/* Golden: «3 вспышки 15/60 тиков на уровне 1» = LED_CMD(1, 3, 15, 60, 1, 0). */
LED_SA(LED_CMD(1u, 3u, 15u, 60u, 1u, 0u) == 0x04780F31u, cmd_golden_err);
LED_SA(LED_CMD_DEV(0x04780F31u)    == 1u,   golden_device);
LED_SA(LED_CMD_COUNT(0x04780F31u)  == 3u,   golden_count);
LED_SA(LED_CMD_T0F(0x04780F31u)    == 15u,  golden_t0_field);
LED_SA(LED_CMD_T1F(0x04780F31u)    == 60u,  golden_t1_field);
LED_SA(LED_CMD_T0(0x04780F31u)     == 15u,  golden_t0_ticks);
LED_SA(LED_CMD_T1(0x04780F31u)     == 60u,  golden_t1_ticks);
LED_SA(LED_CMD_LVL0(0x04780F31u)   == 1u,   golden_level0);
LED_SA(LED_CMD_LVL1(0x04780F31u)   == 0u,   golden_level1);
/* Round-trip кодирование/декодирование длительностей (1 тик, 63, границы). */
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u,      1u, 0u, 0u, 0u)) == 1u,      rt_ticks_1);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u,     63u, 0u, 0u, 0u)) == 63u,     rt_ticks_63);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u,     64u, 0u, 0u, 0u)) == 64u,     rt_ticks_64);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u,    252u, 0u, 0u, 0u)) == 252u,    rt_ticks_252);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u,   1008u, 0u, 0u, 0u)) == 1008u,   rt_ticks_1008);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u,  16128u, 0u, 0u, 0u)) == 16128u,  rt_ticks_16128);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u, 1032192u, 0u, 0u, 0u)) == 1032192u,
       rt_ticks_max);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u, 2000000u, 0u, 0u, 0u)) == 1032192u,
       rt_ticks_clamped);
LED_SA(LED_CMD_T0(LED_CMD(0u, 0u, 0u, 0u, 0u, 0u)) == 0u, rt_ticks_zero);
/* Уровень по умолчанию осмыслен для аналогового канала. */
LED_SA(LED_LEVEL_MAX >= 1u && LED_LEVEL_MAX <= 255u, level_max_range);

/* ================================================================== */
/*  ФЛАГИ СОСТОЯНИЯ                                                    */
/* ================================================================== */

#define LED_F_PHASE     0x01  /**< BLINK: сейчас первая фаза           */
#define LED_F_SEQ_LOOP  0x02  /**< SEQUENCE: зациклена                 */

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

/** Полная маска каналов: «выводы уже синхронизированы». */
#define T_MASK_ALL    ((uint8_t)((1u << LED_COUNT) - 1u))

/* ================================================================== */
/*  СОСТОЯНИЕ ОДНОГО КАНАЛА                                           */
/* ================================================================== */

/**
 * Внутреннее состояние одного канала (конечный автомат).
 *
 * Поля сгруппированы так, чтобы минимизировать выравнивание:
 * указатель идёт первым (выравнивание 4), затем uint16_t, затем байты.
 * При LED_USE_SEQUENCES = 1 — 20 байт на канал, иначе — 10 байт.
 *
 * Уровень хранится в байте (а не флагом): аналоговый канал выводит
 * не просто вкл/выкл. Второй уровень нужен только BLINK'у для фазы,
 * которая сейчас не активна.
 */
typedef struct {
#if LED_USE_SEQUENCES
    const led_step_t *seq;       /* 0..3   */
    uint16_t          seq_len;   /* 4..5   */
    uint16_t          seq_idx;   /* 6..7   */
    uint16_t          seq_cnt;   /* 8..9   */
#endif
    uint16_t counter;            /* счётчик тиков текущей фазы        */
    uint16_t on_ticks;           /* BLINK: длительность 1-й фазы      */
    uint16_t off_ticks;          /* BLINK: длительность 2-й фазы      */
    uint8_t  level;              /**< текущий уровень вывода          */
    uint8_t  level2;             /**< BLINK: уровень 2-й фазы         */
    uint8_t  mode;               /* led_mode_t, упакован в байт       */
    uint8_t  flags;              /* LED_F_*                           */
#if LED_USE_EVENTS
    uint8_t  pass;               /**< номер повтора для LED_EV_CYCLE_START */
#endif
} led_state_t;

/* ================================================================== */
/*  ВНУТРЕННЕЕ СОСТОЯНИЕ                                               */
/* ================================================================== */

static led_state_t leds[LED_COUNT];

/** Callback записи в вывод. NULL = вывод отключён. */
static led_set_fn hw_set = NULL;

/**
 * Кэш уровней, реально отданных в вывод, и маска «вывод уже
 * записывался». Благодаря кэшу callback вызывается только при фактическом
 * изменении уровня; driven_mask гарантирует первую запись после
 * led_init()/led_set_callback()/led_refresh().
 */
static uint8_t out_level[LED_COUNT] = { 0 };
static uint8_t driven_mask = 0;

/**
 * Текущий владелец индикации. Ставится только явно (led_set_owner()),
 * снимается явно (led_release()/led_stop_owner()) либо автоматически —
 * в конце ближайшего led_advance(), если ни одного таймерного паттерна
 * не осталось (см. owner_check()).
 */
static uint8_t cur_owner = (uint8_t)LED_OWNER_NONE;

#if LED_USE_EFFECTS

/** Активный эффект (uint8_t вместо enum — экономия RAM). */
static uint8_t cur_effect = (uint8_t)LED_EFFECT_NONE;

/**
 * Остаток эффекта в тиках. LED_EFFECT_FOREVER — бесконечный режим.
 * Отсчитывается в led_advance() независимо от того, сколько диодов
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

#if LED_USE_PACKED_COMMANDS

/**
 * Конечный автомат упакованных команд.
 *
 * Модель: текущая команда = пара фаз. При загрузке применяется level0 и
 * ставится таймер на t0; по истечении применяется level1 и ставится
 * таймер на t1; по истечении второй фазы уменьшается pc_left (15 =
 * бесконечно) и либо повторяется пара, либо выполняется следующая
 * команда массива. Граница массива и системные команды обрабатываются
 * функцией packed_next().
 */
static uint8_t        pc_active;  /**< 1 = паттерн исполняется              */
static const uint32_t *pc_cmds;    /**< массив; NULL = одиночная команда    */
static uint16_t       pc_count;   /**< число слов в массиве                */
static uint16_t       pc_index;   /**< индекс следующего слова             */
static uint32_t       pc_cmd;     /**< текущее слово                       */
static uint32_t       pc_timer;   /**< тиков до конца текущей фазы         */
static uint8_t        pc_dev;     /**< маска каналов текущей команды       */
static uint8_t        pc_live;    /**< объединение масок с начала паттерна */
static uint8_t        pc_left;    /**< оставшиеся повторы пары фаз         */
static uint8_t        pc_passes;  /**< дополнительные проходы по массиву   */
static uint8_t        pc_phase;   /**< 0 — фаза 0, 1 — фаза 1              */

#endif /* LED_USE_PACKED_COMMANDS */

#if LED_USE_EVENTS
/** Callback событий. NULL = события не отправляются. */
static led_event_fn ev_cb = NULL;
#endif

/* ================================================================== */
/*  ЗАПИСЬ В ВЫВОД                                                     */
/* ================================================================== */

/**
 * Вызвать callback, только если уровень вывода реально изменился
 * (или вывод ещё ни разу не записывался). Единственная точка
 * взаимодействия с аппаратурой.
 *
 * @param id     Индекс канала (0..LED_COUNT-1).
 * @param level  Новый уровень (0 = off).
 */
static void apply_level(uint8_t id, uint8_t level)
{
    uint8_t bit = (uint8_t)(1u << id);

    if ((driven_mask & bit) && (out_level[id] == level)) {
        return;
    }
    /* Кэш уровня обновляется даже без callback'а: led_get_level()/
     * led_is_on() обязаны отражать логическое состояние канала, а не
     * наличие подключённого обработчика. */
    out_level[id] = level;
    if (hw_set == NULL) {
        return;
    }
    driven_mask |= bit;
    hw_set(id, level);
#if LED_USE_EVENTS
    if (ev_cb != NULL) {
        ev_cb(id, LED_EV_STATE, (uint32_t)level);
    }
#endif
}

/**
 * Принудительно перезаписать все выводы кэшированными уровнями.
 *
 * Идемпотентна с точки зрения callback'а: apply_level() пропускает
 * неизменившиеся уровни, поэтому дополнительный проход не меняет
 * последовательность вызовов (инвариант advance(1)×N == advance(N)).
 * Нужна для led_init()/led_refresh()/led_set_callback() и для случая
 * «таймерных паттернов нет вообще» — иначе новый/сброшенный вывод
 * остался бы незаписанным.
 */
static void sync_outputs(void)
{
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        apply_level(i, out_level[i]);
    }
}

#if LED_USE_EVENTS
/**
 * Сообщить о начале цикла паттерна. Для глобальных паттернов (эффект,
 * упакованные команды) id = младший канал маски — идентификатора
 * «подсистемы» у глобального паттерна нет.
 */
static void ev_cycle_start(uint8_t id, uint32_t arg)
{
    if (ev_cb != NULL) {
        ev_cb(id, LED_EV_CYCLE_START, arg);
    }
}

/** Сообщить о завершении паттерна. */
static void ev_pattern_end(uint8_t id)
{
    if (ev_cb != NULL) {
        ev_cb(id, LED_EV_PATTERN_END, 0u);
    }
}
#endif /* LED_USE_EVENTS */

/* ================================================================== */
/*  СМЕНА РЕЖИМА (контекст уже защищён критической секцией)            */
/* ================================================================== */

static void set_mode_level(uint8_t id, uint8_t level)
{
    leds[id].mode    = (level != 0u) ? (uint8_t)LED_MODE_ON
                                     : (uint8_t)LED_MODE_OFF;
    leds[id].counter = 0;
    leds[id].level   = level;
    apply_level(id, level);
}

static void set_mode_off(uint8_t id)
{
    set_mode_level(id, 0u);
}

static void set_mode_on(uint8_t id)
{
    set_mode_level(id, 1u);
}

static void on_for_locked(uint8_t id, uint16_t ticks)
{
    if (ticks == 0) {
        set_mode_off(id);
        return;
    }
    leds[id].mode    = LED_MODE_ON_FOR;
    leds[id].counter = ticks;
    leds[id].level   = 1u;
    apply_level(id, 1u);
#if LED_USE_EVENTS
    if (ev_cb != NULL) {
        leds[id].pass = 0;
        ev_cycle_start(id, 0u);
    }
#endif
}

static void blink_locked(uint8_t id, uint8_t on_level, uint16_t on_ticks,
                         uint8_t off_level, uint16_t off_ticks)
{
    if (on_ticks == 0 || off_ticks == 0) {
        set_mode_off(id);
        return;
    }
    leds[id].mode      = LED_MODE_BLINK;
    leds[id].counter   = on_ticks;
    leds[id].on_ticks  = on_ticks;
    leds[id].off_ticks = off_ticks;
    leds[id].level     = on_level;
    leds[id].level2    = off_level;
    leds[id].flags     = LED_F_PHASE;
    apply_level(id, on_level);
#if LED_USE_EVENTS
    if (ev_cb != NULL) {
        leds[id].pass = 0;
        ev_cycle_start(id, 0u);
    }
#endif
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
    leds[id].flags   = (uint8_t)((seq->loop ? LED_F_SEQ_LOOP : 0));
    /* Нулевая длительность шага трактуется как 1 тик. Первый шаг
     * применяется немедленно — в том числе ведущий LED_STEP_OFF(n),
     * то есть «стартовать через n тиков» без отдельного API. */
    leds[id].seq_cnt = (seq->steps[0].ticks != 0) ? seq->steps[0].ticks : 1u;
    leds[id].level   = seq->steps[0].level;
    apply_level(id, leds[id].level);
#if LED_USE_EVENTS
    if (ev_cb != NULL) {
        leds[id].pass = 0;
        ev_cycle_start(id, 0u);
    }
#endif
}
#endif

/* ================================================================== */
/*  ОБРАБОТЧИКИ КОНЕЧНЫХ АВТОМАТОВ                                     */
/* ================================================================== */

/**
 * ON_FOR: канал горит ровно ticks тиков, затем авто-выключение.
 * counter гарантированно > 0 на входе: led_on_for() с ticks == 0
 * сразу переводит канал в OFF.
 */
static void process_on_for(uint8_t id)
{
    apply_level(id, 1u);
    leds[id].counter--;
    if (leds[id].counter == 0) {
        leds[id].mode = LED_MODE_OFF;
        leds[id].level = 0u;
        apply_level(id, 0u);
#if LED_USE_EVENTS
        if (ev_cb != NULL) {
            ev_pattern_end(id);
        }
#endif
    }
}

/**
 * BLINK: двухфазный цикл произвольных уровней. counter > 0 на входе
 * гарантирован: нулевые фазы отсекаются в led_blink_level(), при смене
 * фазы всегда загружается ненулевая длительность противоположной фазы.
 */
static void process_blink(uint8_t id)
{
    led_state_t *s = &leds[id];

    apply_level(id, s->level);
    s->counter--;
    if (s->counter == 0) {
        uint8_t prev = s->level;

        /* Уровни меняются местами: активным становится второй, а level2
         * снова хранит «неактивный» — так не нужен отдельный флаг фазы. */
        s->level  = s->level2;
        s->level2 = prev;
        if (s->flags & LED_F_PHASE) {
            s->counter = s->off_ticks;
            s->flags  &= (uint8_t)~LED_F_PHASE;
        } else {
            s->counter = s->on_ticks;
            s->flags  |= LED_F_PHASE;
#if LED_USE_EVENTS
            if (ev_cb != NULL) {
                s->pass++;
                ev_cycle_start(id, (uint32_t)s->pass);
            }
#endif
        }
    }
}

#if LED_USE_SEQUENCES
/**
 * SEQUENCE: проигрывание массива шагов. seq_cnt > 0 на входе
 * гарантирован: загрузчик шага трактует ticks == 0 как 1 тик.
 */
static void process_sequence(uint8_t id)
{
    led_state_t *s = &leds[id];

    apply_level(id, s->level);

    s->seq_cnt--;
    if (s->seq_cnt == 0) {
        s->seq_idx++;

        if (s->seq_idx >= s->seq_len) {
            if (s->flags & LED_F_SEQ_LOOP) {
                s->seq_idx = 0;
#if LED_USE_EVENTS
                if (ev_cb != NULL) {
                    s->pass++;
                    ev_cycle_start(id, (uint32_t)s->pass);
                }
#endif
            } else {
                s->mode  = LED_MODE_OFF;
                s->level = 0u;
                apply_level(id, 0u);
#if LED_USE_EVENTS
                if (ev_cb != NULL) {
                    ev_pattern_end(id);
                }
#endif
                return;
            }
        }

        s->level   = s->seq[s->seq_idx].level;
        s->seq_cnt = (s->seq[s->seq_idx].ticks != 0)
                         ? s->seq[s->seq_idx].ticks : 1u;
        /* Новый уровень применяется немедленно: шаг длится ровно
         * ticks тиков, как и фазы blink (проверено тайминг-тестом). */
        apply_level(id, s->level);
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
 * маски автоматически отсекаются в led_advance()).
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
/*  КОНЕЧНЫЙ АВТОМАТ УПАКОВАННЫХ КОМАНД                                */
/* ================================================================== */

#if LED_USE_PACKED_COMMANDS

/**
 * Применить уровень level ко всем каналам маски dev, которые ещё
 * принадлежат паттерну.
 *
 * Проверка режима — это «оторвать» канал: любой per-LED вызов
 * (led_on/led_blink/led_play/…) переводит канал в свой режим, после
 * чего паттерн его больше не трогает (ровно как глобальный эффект).
 */
static void packed_apply(uint8_t dev, uint8_t level)
{
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        if ((dev & (uint8_t)(1u << i)) != 0u &&
            leds[i].mode == (uint8_t)LED_MODE_PACKED) {
            apply_level(i, level);
        }
    }
}

/** Загрузить обычную (не системную) команду: уровень фазы 0 + таймер. */
static void packed_load(uint32_t w)
{
    uint8_t dev = LED_CMD_DEV(w);
    uint8_t i;

    pc_cmd   = w;
    pc_dev   = dev;
    pc_live  = (uint8_t)(pc_live | dev);
    pc_left  = LED_CMD_COUNT(w);
    pc_phase = 0u;

    /* Упакованный паттерн ЗАБИРАЕТ свои каналы: переводим их в
     * LED_MODE_PACKED, чтобы их собственные FSM не конфликтовали за
     * вывод (ровно как глобальный эффект). */
    for (i = 0; i < LED_COUNT; i++) {
        if ((dev & (uint8_t)(1u << i)) != 0u) {
            leds[i].mode    = LED_MODE_PACKED;
            leds[i].counter = 0;
        }
    }

    packed_apply(dev, LED_CMD_LVL0(w));
    pc_timer = LED_CMD_T0(w);
#if LED_USE_EVENTS
    if (ev_cb != NULL) {
        uint8_t id = 0;

        for (i = 0; i < LED_COUNT; i++) {
            if ((dev & (uint8_t)(1u << i)) != 0u) {
                id = i;
                break;
            }
        }
        ev_cycle_start(id, (uint32_t)pc_passes);
    }
#endif
}

/**
 * Сбросить конечный автомат и погасить все каналы, которые он забирал
 * (объединение масок с начала паттерна — маска может меняться от
 * команды к команде).
 */
static void packed_reset(void)
{
    uint8_t live = pc_live;
#if LED_USE_EVENTS
    bool was_active = (pc_active != 0u);
    uint8_t id = 0;
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        if ((live & (uint8_t)(1u << i)) != 0u) {
            id = i;
            break;
        }
    }
#endif

    pc_active = 0;
    pc_cmds   = NULL;
    pc_count  = 0;
    pc_index  = 0;
    pc_dev    = 0;
    pc_live   = 0;
    pc_left   = 0;
    pc_phase  = 0;
    pc_timer  = 0;

    if (live != 0u) {
        /* Гасим и возвращаем в OFF только те каналы, которые паттерн
         * действительно забирал: «оторванные» каналы не трогаем. */
        uint8_t i;

        packed_apply(live, 0u);
        for (i = 0; i < LED_COUNT; i++) {
            if (leds[i].mode == (uint8_t)LED_MODE_PACKED) {
                leds[i].mode = (uint8_t)LED_MODE_OFF;
            }
        }
    }
#if LED_USE_EVENTS
    if (was_active && ev_cb != NULL) {
        ev_pattern_end(id);
    }
#endif
}

/**
 * Перейти к следующей фазе/команде. false = паттерн завершён.
 *
 * Системные команды (биты 5..31 == 0) исполняются немедленно в любой
 * точке массива: STOP, REP_START (бесконечный повтор) и REP_AT(n)
 * (ещё n проходов). Защита guard ограничивает число системных команд
 * за один вызов led_advance(), чтобы вырожденная таблица с нулевыми
 * длительностями не закрутила бесконечный цикл внутри одного advance.
 */
static bool packed_next(void)
{
    uint8_t guard = (uint8_t)LED_PACKED_GUARD;

    for (;;) {
        uint32_t w;

        if (pc_index >= pc_count) {
            packed_reset();
            return false;
        }
        w = pc_cmds[pc_index++];
        if (!LED_CMD_IS_SYS(w)) {
            packed_load(w);
            return true;
        }
        if (guard == 0u) {
            packed_reset();
            return false;
        }
        guard--;

        if (LED_CMD_OP(w) == 1u) {              /* LED_SYS_REP_START */
            pc_index  = 0;
            pc_passes = 0;
        } else if (LED_CMD_OP(w) == 2u) {       /* LED_SYS_REP_AT(n) */
            if (pc_passes >= LED_CMD_STEPS(w)) {
                packed_reset();
                return false;
            }
            pc_passes++;
            pc_index = 0;
        } else {                                /* LED_SYS_STOP */
            packed_reset();
            return false;
        }
    }
}

/** Один тик конечного автомата упакованных команд. */
static void packed_tick(void)
{
    if (pc_active == 0u) {
        return;
    }
    if (pc_timer > 0u) {
        pc_timer--;
    }
    if (pc_timer != 0u) {
        return;
    }

    if (pc_phase == 0u) {
        /* Конец 1-й фазы: применяем уровень 2-й и ставим её таймер. */
        pc_phase = 1u;
        packed_apply(pc_dev, LED_CMD_LVL1(pc_cmd));
        pc_timer = LED_CMD_T1(pc_cmd);
    } else {
        /* Пара фаз отработала: считаем повтор. 15 = бесконечно,
         * 0 = пара была последней. */
        if ((pc_left != 15u) && (pc_left != 0u)) {
            pc_left--;
        }
        if (pc_left != 0u) {
            pc_phase = 0u;
            packed_apply(pc_dev, LED_CMD_LVL0(pc_cmd));
            pc_timer = LED_CMD_T0(pc_cmd);
        } else {
            (void)packed_next();
        }
    }
}

#endif /* LED_USE_PACKED_COMMANDS */

/* ================================================================== */
/*  ГЛОБАЛЬНЫЕ ЭФФЕКТЫ: запуск/остановка (контекст защищён)            */
/* ================================================================== */

#if LED_USE_EFFECTS

static void stop_effect_inner(void)
{
    uint8_t i;
#if LED_USE_EVENTS
    bool was_active = (cur_effect != (uint8_t)LED_EFFECT_NONE);
#endif

    for (i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode == LED_MODE_EFFECT) {
            set_mode_off(i);
        }
    }
    cur_effect       = (uint8_t)LED_EFFECT_NONE;
    effect_remaining = 0;
#if LED_USE_EVENTS
    if (was_active && ev_cb != NULL) {
        ev_pattern_end(0u);
    }
#endif
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
        leds[i].level = 0u;
    }
#if LED_USE_EVENTS
    if (ev_cb != NULL) {
        ev_cycle_start(0u, 0u);
    }
#endif
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
/*  ПРОДВИЖЕНИЕ ВРЕМЕНИ                                                */
/* ================================================================== */

/** Идёт ли хотя бы один таймерный паттерн (per-LED, эффект, packed). */
static bool patterns_active(void)
{
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        switch (leds[i].mode) {
            case LED_MODE_ON_FOR:
            case LED_MODE_BLINK:
            case LED_MODE_SEQUENCE:
            case LED_MODE_EFFECT:
                return true;
            default:
                break;
        }
    }
#if LED_USE_EFFECTS
    if (cur_effect != (uint8_t)LED_EFFECT_NONE) {
        return true;
    }
#endif
#if LED_USE_PACKED_COMMANDS
    if (pc_active != 0u) {
        return true;
    }
#endif
    return false;
}

/**
 * Снять владение, если таймерных паттернов не осталось.
 *
 * Вызывается в конце led_advance(). Проверка намеренно делается на
 * каждом advance, а не «по событию окончания»: у паттернов много точек
 * завершения (таймер канала, конец последовательности, остановка
 * эффекта, STOP упакованной команды), и единственное место, где хост
 * гарантированно «видит» состояние, — момент возврата из advance.
 *
 * Следствие (и это же контракт): объявлять владение нужно вместе с
 * запуском паттерна, в одном месте кода. Владение, заявленное без
 * паттерна, снимается ближайшим advance — индикатор свободен.
 */
static void owner_check(void)
{
    if ((cur_owner != (uint8_t)LED_OWNER_NONE) && !patterns_active()) {
        cur_owner = (uint8_t)LED_OWNER_NONE;
    }
}

/** Тиков до ближайшего изменения вывода (внутренняя, без CRITICAL). */
static uint32_t next_change_ticks(void)
{
    uint32_t best = LED_TICKS_FOREVER;
    uint8_t  i;

    for (i = 0; i < LED_COUNT; i++) {
        switch (leds[i].mode) {
            case LED_MODE_ON_FOR:
            case LED_MODE_BLINK:
                if ((uint32_t)leds[i].counter < best) {
                    best = (uint32_t)leds[i].counter;
                }
                break;
#if LED_USE_SEQUENCES
            case LED_MODE_SEQUENCE:
                if ((uint32_t)leds[i].seq_cnt < best) {
                    best = (uint32_t)leds[i].seq_cnt;
                }
                break;
#endif
            default:
                break;
        }
    }
#if LED_USE_EFFECTS
    /* Эффекты тикают по одному: их счётчики (fx_phase/fx_pos/fx_cnt)
     * связаны модульной арифметикой, быстрый проход по ним потребовал бы
     * деления, а ради 60 Гц-эффектов это не нужно. */
    if (cur_effect != (uint8_t)LED_EFFECT_NONE) {
        best = 1u;
    }
#endif
#if LED_USE_PACKED_COMMANDS
    if (pc_active != 0u) {
        uint32_t t = (pc_timer != 0u) ? pc_timer : 1u;

        if (t < best) {
            best = t;
        }
    }
#endif
    return best;
}

/**
 * Промахнуть n тиков, в течение которых переходов не будет.
 *
 * Вызывается только когда next_change_ticks() > n, поэтому все
 * счётчики строго больше n и остаются >= 1 — «проматывание» эквивалентно
 * n вызовам led_advance(1) с точки зрения состояния. Никаких вызовов
 * callback здесь не происходит: переходов по построению нет.
 *
 * @param n  Число тиков; <= 0xFFFE (ограничение uint16_t-счётчиков).
 */
static void idle_ticks(uint32_t n)
{
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        switch (leds[i].mode) {
            case LED_MODE_ON_FOR:
                leds[i].counter = (uint16_t)((uint32_t)leds[i].counter - n);
                apply_level(i, leds[i].level);
                break;
            case LED_MODE_BLINK:
                leds[i].counter = (uint16_t)((uint32_t)leds[i].counter - n);
                /* Уровень текущей фазы обязателен: смена фазы в
                 * process_blink() происходит на последнем тике фазы, и
                 * новый уровень «догоняется» первым же тиком следующей
                 * фазы. Без apply_level() здесь пакетный advance()
                 * показал бы старый уровень дольше, чем тик за тиком. */
                apply_level(i, leds[i].level);
                break;
#if LED_USE_SEQUENCES
            case LED_MODE_SEQUENCE:
                leds[i].seq_cnt = (uint16_t)((uint32_t)leds[i].seq_cnt - n);
                apply_level(i, leds[i].level);
                break;
#endif
            default:
                break;
        }
    }
#if LED_USE_PACKED_COMMANDS
    if (pc_active != 0u) {
        pc_timer -= n;   /* n < pc_timer гарантировано */
    }
#endif
    /* Эффект сюда не попадает: при активном эффекте next_change_ticks()
     * возвращает 1, поэтому n всегда равен 0, а effect_remaining
     * уменьшается в tick_one(). */
}

/** Один тик: обработка переходов всех каналов и глобальных паттернов. */
static void tick_one(void)
{
    uint8_t i;

    /* Шаг 1: каналы в ручных режимах. */
    for (i = 0; i < LED_COUNT; i++) {
        switch (leds[i].mode) {
            case LED_MODE_OFF:
                apply_level(i, 0u);
                break;
            case LED_MODE_ON:
                apply_level(i, leds[i].level);
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
            case LED_MODE_PACKED:
                /* Канал забрала упакованная команда: уровень применяет
                 * packed_tick() (или packed_apply() при загрузке). */
                break;
            default:
                /* Неизвестный режим — самовосстановление в OFF. */
                leds[i].mode    = LED_MODE_OFF;
                leds[i].counter = 0;
                leds[i].level   = 0u;
                apply_level(i, 0u);
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
                    leds[i].level = (uint8_t)((effect_mask >> i) & 1u);
                    apply_level(i, leds[i].level);
                }
            }

            if (effect_remaining != LED_EFFECT_FOREVER) {
                effect_remaining--;
            }
        }
    }
#endif

#if LED_USE_PACKED_COMMANDS
    /* Шаг 3: упакованные команды (приоритетнее эффекта: их каналы
     * переведены в LED_MODE_OFF, поэтому конфликта за вывод нет). */
    packed_tick();
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
        leds[i].level     = 0;
        leds[i].level2    = 0;
        leds[i].mode      = LED_MODE_OFF;
        leds[i].flags     = 0;
#if LED_USE_EVENTS
        leds[i].pass      = 0;
#endif
        out_level[i]      = 0;
    }

    /*
     * Кэш выводов инвалидируется: первый led_advance() принудительно
     * запишет все выводы (в т.ч. погасит диоды, включённые до
     * инициализации). Callback сохраняется — повторная инициализация
     * не отключает вывод.
     */
    driven_mask = 0;

    cur_owner = (uint8_t)LED_OWNER_NONE;

#if LED_USE_EFFECTS
    cur_effect       = (uint8_t)LED_EFFECT_NONE;
    effect_remaining = 0;
    fx_phase         = 0;
    fx_pos           = 0;
    fx_cnt           = 0;
    rng_state        = 0xDEADBEEFu;
#endif

#if LED_USE_PACKED_COMMANDS
    pc_active = 0;
    pc_cmds   = NULL;
    pc_count  = 0;
    pc_index  = 0;
    pc_cmd    = 0;
    pc_timer  = 0;
    pc_dev    = 0;
    pc_live   = 0;
    pc_left   = 0;
    pc_passes = 0;
    pc_phase  = 0;
#endif

    LED_EXIT_CRITICAL();
}

void led_set_callback(led_set_fn fn)
{
    LED_ENTER_CRITICAL();
    hw_set      = fn;
    driven_mask = 0;   /* новый обработчик получит полную картину */
    LED_EXIT_CRITICAL();
}

void led_refresh(void)
{
    LED_ENTER_CRITICAL();
    driven_mask = 0;
    LED_EXIT_CRITICAL();
}

void led_advance(uint32_t ticks)
{
    /*
     * Идемпотентная синхронизация выводов — до продвижения времени.
     * Именно здесь, а не в конце: первый «полный драйв» после
     * led_init()/led_refresh()/led_set_callback() должен занимать
     * ОДНО И ТО ЖЕ место в потоке вызовов callback при любом разбиении
     * advance(1)xN и advance(N). Повторные вызовы не порождают
     * callback'ов (кэш уровней), так что на тайминги это не влияет.
     * Это же обслуживает случай «таймерных паттернов нет вообще».
     * Проверка маски — оптимизация горячего пути: полный драйв
     * нужен один раз, а не на каждом advance().
     */
    if (driven_mask != T_MASK_ALL) {
        sync_outputs();
    }

    while (ticks > 0u) {
        uint32_t step = next_change_ticks();

        if (step == LED_TICKS_FOREVER) {
            break;   /* таймерных паттернов нет: время ни на что не влияет */
        }
        /* Защитные ограничения: 0 недопустим (см. контракт API), а
         * счётчики каналов — uint16_t, поэтому один промах не длиннее
         * 0xFFFE тиков. */
        if (step == 0u) {
            step = 1u;
        } else if (step > 0xFFFEu) {
            step = 0xFFFEu;
        }

        if (step > ticks) {
            if (ticks != 0u) {
                idle_ticks(ticks);
            }
            ticks = 0u;
        } else {
            /* Проматываем тики без переходов, затем обрабатываем тик,
             * на котором переход происходит. */
            if (step > 1u) {
                idle_ticks(step - 1u);
            }
            ticks -= step;
            tick_one();
        }
    }

    owner_check();
}

uint32_t led_ticks_until_next(void)
{
    return next_change_ticks();
}

void led_process(void)
{
    led_advance(1u);
}

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: управление отдельным каналом                        */
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

void led_set_level(uint8_t id, uint8_t level)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    set_mode_level(id, level);
    LED_EXIT_CRITICAL();
}

void led_toggle(uint8_t id)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    /* Переключение по фактическому уровню вывода, а не по режиму. */
    if (out_level[id] > 0u) {
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
    blink_locked(id, 1u, on_ticks, 0u, off_ticks);
    LED_EXIT_CRITICAL();
}

void led_blink_level(uint8_t id, uint8_t on_level, uint16_t on_ticks,
                     uint8_t off_level, uint16_t off_ticks)
{
    if (id >= LED_COUNT) {
        return;
    }
    LED_ENTER_CRITICAL();
    blink_locked(id, on_level, on_ticks, off_level, off_ticks);
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
/*  ПУБЛИЧНЫЙ API: владение индикатором                                */
/* ================================================================== */

void led_set_owner(led_owner_t owner)
{
    LED_ENTER_CRITICAL();
    cur_owner = owner;
    LED_EXIT_CRITICAL();
}

led_owner_t led_claim(void)
{
    return (led_owner_t)cur_owner;
}

void led_release(led_owner_t owner)
{
    LED_ENTER_CRITICAL();
    if (cur_owner == (uint8_t)owner) {
        cur_owner = (uint8_t)LED_OWNER_NONE;
    }
    LED_EXIT_CRITICAL();
}

/**
 * Остановить все таймерные паттерны (per-LED, эффект, упакованные
 * команды). Статичные LED_MODE_ON/OFF не трогаются: это явное решение
 * хоста, а не часть паттерна.
 */
static void stop_all_patterns(void)
{
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        switch (leds[i].mode) {
            case LED_MODE_ON_FOR:
            case LED_MODE_BLINK:
            case LED_MODE_SEQUENCE:
            case LED_MODE_EFFECT:
                set_mode_off(i);
                break;
            default:
                break;
        }
    }
#if LED_USE_EFFECTS
    stop_effect_inner();
#endif
#if LED_USE_PACKED_COMMANDS
    if (pc_active != 0u) {
        packed_reset();
    }
#endif
}

uint8_t led_stop_owner(led_owner_t owner)
{
    uint8_t stopped = 0;

    LED_ENTER_CRITICAL();
    if (owner != (uint8_t)LED_OWNER_NONE && cur_owner == (uint8_t)owner) {
        stop_all_patterns();
        cur_owner = (uint8_t)LED_OWNER_NONE;
        stopped   = 1;
    }
    LED_EXIT_CRITICAL();

    return stopped;
}

uint8_t led_pattern_active(void)
{
    uint8_t active;

    LED_ENTER_CRITICAL();
    active = patterns_active() ? 1u : 0u;
    LED_EXIT_CRITICAL();

    return active;
}

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: упакованные команды                                 */
/* ================================================================== */

#if LED_USE_PACKED_COMMANDS

/**
 * Подготовить конечный автомат к новому паттерну: погасить и сбросить
 * предыдущий (у него могли быть другие маски каналов), занять
 * состояние, загрузить первую команду.
 *
 * @param cmds   Массив слов; NULL = одиночная команда (следующего слова
 *               не будет, любой переход на границе завершает паттерн).
 * @param count  Число слов.
 */
static void packed_begin(const uint32_t *cmds, uint16_t count)
{
    if (pc_active != 0u) {
        packed_reset();
    }
    pc_cmds   = cmds;
    pc_count  = count;
    pc_index  = 0;
    pc_passes = 0;
    pc_live   = 0;
    pc_active = 1;
}

void led_packed_run(const uint32_t cmds[], uint16_t count)
{
    if (cmds == NULL || count == 0u) {
        return;
    }

    LED_ENTER_CRITICAL();
    packed_begin(cmds, count);
    (void)packed_next();   /* первый переход происходит на первом тике */
    LED_EXIT_CRITICAL();
}

void led_packed_once(uint32_t cmd)
{
    LED_ENTER_CRITICAL();
    if (LED_CMD_IS_SYS(cmd)) {
        /* Системная команда без массива не имеет смысла: немедленный
         * STOP (иначе она зациклила бы пустую таблицу). */
        if (pc_active != 0u) {
            packed_reset();
        }
    } else {
        packed_begin(NULL, 0);
        packed_load(cmd);
    }
    LED_EXIT_CRITICAL();
}

void led_packed_stop(void)
{
    LED_ENTER_CRITICAL();
    if (pc_active != 0u) {
        packed_reset();
    }
    LED_EXIT_CRITICAL();
}

#endif /* LED_USE_PACKED_COMMANDS */

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: события                                             */
/* ================================================================== */

#if LED_USE_EVENTS
void led_set_event_callback(led_event_fn fn)
{
    LED_ENTER_CRITICAL();
    ev_cb = fn;
    LED_EXIT_CRITICAL();
}
#endif

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
    return (out_level[id] > 0u) ? 1u : 0u;
}

uint8_t led_get_level(uint8_t id)
{
    if (id >= LED_COUNT) {
        return 0;
    }
    return out_level[id];
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
