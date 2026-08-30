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
 *    - out_mask      — кэш последних выставленных состояний GPIO
 *    - fx_phase / fx_pos / fx_cnt — счётчики встроенных эффектов
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
 *    - eff_running_fwd     — fwd/bwd/circle (общая, по cur_effect)
 *    - eff_running_pp      — бегущий огонь туда-обратно
 *    - eff_emergency       — emergency/pair_blink/alternating (общая)
 *    - eff_double_blink    — double/triple мигание (общая)
 *    - eff_running_fwd_hold — fwd_hold/bwd_hold (общая)
 *    - eff_random_flash    — псевдослучайный генератор (xorshift32)
 *
 *    Эффекты возвращают БИТОВУЮ МАСКУ (бит i = LED i включён), а не
 *    массив bool — компактнее и быстрее на RV32.
 *    Все эффекты продвигают инкрементные счётчики (fx_*), поэтому
 *    не вызывают программное деление/умножение (__udivsi3/__mulsi3),
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
 *   leds[4]:           4 × 20 = 80 байт  (mode и флаги упакованы в 2 байта)
 *   hw_set:            4 байта (указатель на RV32/ARM)
 *   cur_effect:        4 байта (enum = int)
 *   effect_remaining:  4 байта
 *   out_mask:          1 байт
 *   driven_mask:       1 байт
 *   fx_*:              3 байта
 *   Итого:             ~97 байт (около 90)
 *
 * =====================================================================
 *  ВРЕМЯ ВЫПОЛНЕНИЯ led_process()
 * =====================================================================
 *
 *   - Цикл по 4 LED: 4 итерации фиксированного размера.
 *   - Внутри switch: до 6 case'ов, каждый O(1).
 *   - Эффект: 1 вызов функции + цикл по 4 LED.
 *   - GPIO callback вызывается только при ИЗМЕНЕНИИ состояния пина
 *     (кэш out_mask), а не на каждом тике: стабильные режимы
 *     (OFF/ON, устоявшиеся фазы BLINK/SEQUENCE) бесплатны.
 *   - Эффекты не содержат аппаратно дорогих операций: на RV32EC
 *     нет программного деления — фазы и позиции продвигаются
 *     инкрементными счётчиками.
 *   - Итого: O(1) — константное время, нет переменных циклов.
 */

#include "led.h"
#include <string.h>

/* ================================================================== */
/*  СОСТОЯНИЕ ОДНОГО СВЕТОДИОДА (внутренний тип, перенесён из led.h)    */
/* ================================================================== */

/**
 * Внутреннее состояние одного светодиода (конечный автомат).
 *
 * Поля разбиты на группы по назначению:
 *
 * 1) mode — текущий режим (определяет, какой обработчик вызывать)
 *    Хранится как uint8_t: enum led_mode_t в публичном API остаётся
 *    int, но внутри достаточно одного байта.
 *
 * 2) Счётчики для режимов ON_FOR и BLINK:
 *    - counter   — оставшееся количество тиков в текущей фазе
 *    - on_ticks  — длительность ON-фазы (только для BLINK)
 *    - off_ticks — длительность OFF-фазы (только для BLINK)
 *
 * 3) Воспроизведение последовательности (SEQUENCE):
 *    - seq       — указатель на массив шагов (из led_sequence_t)
 *    - seq_len   — общее количество шагов
 *    - seq_idx   — индекс текущего шага
 *    - seq_cnt   — оставшееся время текущего шага
 *
 * 4) Один байт flags с битовыми флагами:
 *    - LED_F_PHASE     — true = сейчас ON-фаза (BLINK)
 *    - LED_F_SEQ_PHASE — состояние текущего шага ON/OFF (SEQUENCE)
 *    - LED_F_SEQ_LOOP  — зациклена ли последовательность
 *
 * Поля расставлены так, чтобы минимизировать выравнивание:
 * указатель идёт первым (нужно выравнивание 4), все uint16_t следом.
 * Итого 20 байт на LED (было 24 из-за enum-int mode и разрозненных
 * bool) × 4 = 80 байт.
 */
#define LED_F_PHASE     0x01  /**< BLINK: сейчас ON-фаза */
#define LED_F_SEQ_PHASE 0x02  /**< SEQUENCE: состояние текущего шага */
#define LED_F_SEQ_LOOP  0x04  /**< SEQUENCE: зациклена */

typedef struct {
    const led_step_t *seq;   /* 0..3   */
    uint16_t          counter;    /* 4..5   */
    uint16_t          on_ticks;   /* 6..7   */
    uint16_t          off_ticks;  /* 8..9   */
    uint16_t          seq_len;    /* 10..11 */
    uint16_t          seq_idx;    /* 12..13 */
    uint16_t          seq_cnt;    /* 14..15 */
    uint8_t           mode;       /* 16     */
    uint8_t           flags;      /* 17     */
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
static uint32_t effect_remaining = 0;

/**
 * Кэш последних выставленных состояний GPIO.
 * Бит id = последнее состояние, переданное в hw_set для LED id.
 *
 * Позволяет отсекать избыточные вызовы callback: большинство режимов
 * (OFF, ON, устоявшиеся фазы BLINK/SEQUENCE) на каждом тике хотят
 * записать «то же самое». С этим кэшем реальная запись в порт
 * выполняется только при изменении состояния пина.
 */
static uint8_t out_mask = 0;

/**
 * Маска «пин уже выставлялся».
 * Бит id устанавливается при ПЕРВОЙ реальной записи на LED id.
 *
 * Нужна, чтобы первый тик после led_init() всё равно записал в GPIO
 * состояние режима (обычно OFF) даже при совпадении с out_mask:
 * прежний код писал на каждом тике и гарантировал выключение пинов,
 * попавших в высокий уровень во время сброса/перезапуска. С одним
 * out_mask первая запись OFF подавлялась бы как «не изменилась».
 */
static uint8_t driven_mask = 0;

/**
 * Счётчики встроенных эффектов.
 *
 * Эффекты намеренно НЕ используют глобальный счётчик тиков и формулы
 * вида (tick / N) % M: на MCU без аппаратного делителя (RV32EC —
 * CH32V003) любое деление раскрывается в программную процедуру
 * (~50–100 циклов на вызов). Вместо этого каждый эффект продвигает
 * инкрементные счётчики:
 *   fx_phase — фаза эффекта (для blink-подобных и удержание-эффектов)
 *   fx_pos   — текущая позиция (для бегущих огней)
 *   fx_cnt   — тиковый делитель внутри фазы/позиции
 *
 * Счётчики общие, т.к. одновременно активен не более ОДНОГО эффекта,
 * а повторно запускаемые эффекты сбрасываются в led_start_effect().
 */
static uint8_t fx_phase = 0;
static uint8_t fx_pos   = 0;
static uint8_t fx_cnt   = 0;

/* ================================================================== */
/*  ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ                                            */
/* ================================================================== */

/**
 * Безопасный вызов callback-функции GPIO.
 *
 * Если hw_set установлен (не NULL) и состояние пина ИЗМЕНИЛОСЬ
 * относительно кэша out_mask — обновляет кэш и вызывает callback.
 * Если hw_set == NULL или состояние не изменилось — ничего не делает.
 *
 * Это единственная точка, через которую библиотека взаимодействует
 * с аппаратурой. Благодаря ей (и кэшу out_mask) портруется на любые МК
 * и не дёргает GPIO при каждом тике без реальной необходимости.
 *
 * @param id     Индекс диода (0..3).
 * @param state  true = включить, false = выключить.
 */
static void apply_state(uint8_t id, bool state)
{
    uint8_t bit = (uint8_t)(1u << id);

    /*
     * Записывать в GPIO, только если:
     *   - пин ещё ни разу не выставлялся (driven_mask), — гарантия первой
     *     записи (в т.ч. принудительное выключение на первом тике), или
     *   - желаемое состояние отличается от последнего (out_mask).
     * В стабильных режимах (OFF/ON, устоявшиеся фазы BLINK/SEQUENCE)
     * повторные тики не дёргают callback.
     */
    if (hw_set && (!(driven_mask & bit) ||
                   (((out_mask >> id) ^ (uint8_t)state) & 1u))) {
        out_mask    = (uint8_t)((out_mask & (uint8_t)~bit) |
                                (state ? bit : 0u));
        driven_mask |= bit;
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
 * Благодаря кэшу out_mask реальная запись в GPIO происходит только
 * один раз — при переходе в OFF; все последующие тики бесплатны.
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
 * Аналогично process_off(), но включает диод — тоже только один раз,
 * остальные тики не дёргают GPIO.
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
 *   1. counter > 0: включить диод, декремент counter.
 *   2. counter стал 0: переключить режим в OFF, выключить диод.
 *
 * Ветка counter == 0 на входе недостижима: led_on_for() с ticks == 0
 * сразу переводит диод в OFF, а обнуление счётчика здесь тут же
 * переключает режим — следующий тик обрабатывается как OFF.
 *
 * Пример: led_on_for(1, 300) при 60 Гц → диод горит 5 секунд.
 *
 * @param id  Индекс диода.
 */
static void process_on_for(uint8_t id)
{
    /* counter гарантированно > 0: led_on_for() с ticks == 0 сразу
     * переводит диод в OFF, а обнуление счётчика ниже тут же меняет
     * режим — следующий тик обрабатывается как OFF. Ветка counter == 0
     * на входе недостижима и была удалена. */
    apply_state(id, true);
    leds[id].counter--;
    if (leds[id].counter == 0) {
        leds[id].mode = LED_MODE_OFF;
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

    if (s->flags & LED_F_PHASE) {
        /* ON-фаза: диод включён, считаем время до выключения */
        apply_state(id, true);
        if (s->counter > 0) {
            s->counter--;
        }
        if (s->counter == 0) {
            /* Переход в OFF-фазу */
            s->counter = s->off_ticks;
            s->flags  &= (uint8_t)~LED_F_PHASE;
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
            s->flags  |= LED_F_PHASE;
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
    apply_state(id, (s->flags & LED_F_SEQ_PHASE) != 0);

    /* Декремент счётчика текущего шага */
    if (s->seq_cnt > 0) {
        s->seq_cnt--;
    }

    /* Если шаг завершён — перейти к следующему */
    if (s->seq_cnt == 0) {
        s->seq_idx++;

        /* Проверка на конец последовательности */
        if (s->seq_idx >= s->seq_len) {
            if (s->flags & LED_F_SEQ_LOOP) {
                s->seq_idx = 0;       /* Зациклить: начать сначала */
            } else {
                s->mode = LED_MODE_OFF;
                apply_state(id, false);
                return;                /* Один раз: остановиться */
            }
        }

        /* Загрузить параметры нового шага */
        s->flags  = (uint8_t)((s->flags & (uint8_t)~LED_F_SEQ_PHASE) |
                              (s->seq[s->seq_idx].state
                                   ? LED_F_SEQ_PHASE : 0));
        s->seq_cnt = s->seq[s->seq_idx].ticks;
        apply_state(id, (s->flags & LED_F_SEQ_PHASE) != 0);
    }
}

/* ================================================================== */
/*  ВСТРОЕННЫЕ ФУНКЦИИ ЭФФЕКТОВ                                        */
/* ================================================================== */

/**
 * Продвижение инкрементного счётчика эффектов.
 *
 * Универсальный шаг для fx_*: увеличивает значение на 1 и заворачивает
 * его в диапазон 0..max-1. Одна общая функция вместо дублирования
 * логики инкремента/сброса в каждом эффекте.
 */
static uint8_t fx_adv(uint8_t v, uint8_t max)
{
    return (uint8_t)((v + 1 < max) ? v + 1 : 0);
}

/**
 * Бегущий огонь (общий вариант для трёх эффектов).
 *
 * Одна функция обслуживает LED_EFFECT_RUNNING_FWD, LED_EFFECT_RUNNING_BWD
 * и LED_EFFECT_RUNNING_CIRCLE, которые отличаются только направлением
 * и периодом смены позиции:
 *
 *   RUNNING_FWD     — вперёд 0→1→2→3, период 10 тиков
 *   RUNNING_BWD     — назад  3→2→1→0, период 10 тиков
 *   RUNNING_CIRCLE  — вперёд 0→1→2→3, период 8 тиков
 *
 * Направление и период выбираются по текущему эффекту (cur_effect).
 * Позиция продвигается инкрементом — без деления глобального тика.
 *
 * @return Битовая маска: бит i = LED i включён.
 */
static uint8_t eff_running_fwd(void)
{
    bool bwd = (cur_effect == LED_EFFECT_RUNNING_BWD);
    uint8_t period = (cur_effect == LED_EFFECT_RUNNING_CIRCLE) ? 8 : 10;
    uint8_t pos = bwd ? (uint8_t)(LED_COUNT - 1 - fx_pos) : fx_pos;

    fx_cnt = fx_adv(fx_cnt, period);
    if (fx_cnt == 0) {
        /* Позиция всегда продвигается ВПЕРЁД; для bwd направление
         * обеспечивает зеркало на строке отображения (pos). Попытка
         * дополнительно декрементировать fx_pos давала бы двойной
         * разворот и последовательность 3,0,1,2... вместо 3,2,1,0... */
        fx_pos = fx_adv(fx_pos, LED_COUNT);
    }
    return (uint8_t)(1u << pos);
}

/**
 * Бегущий огонь туда-обратно: 0→1→2→3→2→1→0→1→...
 *
 * Скорость: 1 позиция каждые 10 тиков.
 * Направление хранится в fx_phase: 0 = вперёд, 1 = назад.
 * Требует LED_COUNT >= 2.
 */
static uint8_t eff_running_pp(void)
{
    fx_cnt = fx_adv(fx_cnt, 10);
    if (fx_cnt != 0) return (uint8_t)(1u << fx_pos);

    if (fx_phase == 0) {
        if (fx_pos + 1 >= LED_COUNT) {
            fx_phase = 1;
            fx_pos   = (uint8_t)(LED_COUNT - 2);
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
    return (uint8_t)(1u << fx_pos);
}

/**
 * Мигание (общий вариант для трёх эффектов).
 *
 * Одна функция обслуживает LED_EFFECT_EMERGENCY, LED_EFFECT_PAIR_BLINK
 * и LED_EFFECT_ALTERNATING, которые отличаются только периодом
 * переключения и паттерном:
 *
 *   EMERGENCY    — все 4 диода сразу, период 5 тиков
 *   PAIR_BLINK   — чёт/нечет в противофазе, период 15 тиков
 *   ALTERNATING  — чёт/нечет в противофазе, период 20 тиков
 *
 * Период и паттерн выбираются по текущему глобальному эффекту
 * (cur_effect) — одно копирование кода вместо трёх функций.
 * Фаза хранится в fx_phase (0 = чётные горят / все включены).
 */
static uint8_t eff_emergency(void)
{
    bool on = (fx_phase == 0);
    bool all = (cur_effect == LED_EFFECT_EMERGENCY);
    uint8_t period;

    if (cur_effect == LED_EFFECT_ALTERNATING) {
        period = 20;
    } else if (cur_effect == LED_EFFECT_EMERGENCY) {
        period = 5;
    } else {
        period = 15;
    }

    fx_cnt = fx_adv(fx_cnt, period);
    if (fx_cnt == 0) {
        fx_phase ^= 1;
    }

    /* 00001111 — все 4; биты чётных/нечётных — для противофазы */
    return (uint8_t)(all ? (on ? 0x0F : 0x00)
                         : (on ? 0x05 : 0x0A));
}

/**
 * Случайные вспышки — генератор псевдослучайных чисел (xorshift32).
 *
 * Алгоритм: xorshift32 — быстрый и компактный PRNG.
 * При каждом вызове генерируется новое 32-битное слово,
 * младшие 4 бита которого определяют состояние 4 диодов.
 *
 * Начальное seed: 0xDEADBEEF (может быть любым != 0).
 */
static uint8_t eff_random_flash(void)
{
    static uint32_t rng = 0xDEADBEEF;

    /* xorshift32: 3 XOR-сдвига дают максимальный период 2^32 - 1 */
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;

    /* Младшие 4 бита — состояние 4 диодов */
    return (uint8_t)(rng & 0x0F);
}

/**
 * Двойное/тройное мигание: ._.__ / ._.__.__ (все диоды вместе).
 *
 * Одна функция обслуживает LED_EFFECT_DOUBLE_BLINK и
 * LED_EFFECT_TRIPLE_BLINK; паттерн и длина периода выбираются
 * по текущему эффекту:
 *
 *   DOUBLE: 16 фаз, ON на 0-1 и 4-5, OFF на 6-15
 *   TRIPLE: 21 фаза, ON на 0,1,4,5,8,9, OFF на остальных
 *
 * Делитель: 5 тиков на фазу.
 */
static uint8_t eff_double_blink(void)
{
    bool triple = (cur_effect == LED_EFFECT_TRIPLE_BLINK);
    bool on;

    if (triple) {
        /* ON на фазах 0,1,4,5,8,9; OFF на остальных */
        on = (fx_phase < 10) && ((fx_phase & 3) < 2);
    } else {
        /* ON на фазах 0-1 и 4-5, OFF на остальных */
        on = (fx_phase < 2) || (fx_phase >= 4 && fx_phase < 6);
    }

    fx_cnt = fx_adv(fx_cnt, 5);
    if (fx_cnt == 0) {
        fx_phase = fx_adv(fx_phase, triple ? 21 : 16);
    }

    return on ? (uint8_t)0x0F : (uint8_t)0x00;
}

/**
 * Бегущий огонь + пауза + удержание крайнего диода 0.75 сек.
 *
 * Одна функция обслуживает LED_EFFECT_RUNNING_FWD_HOLD и
 * LED_EFFECT_RUNNING_BWD_HOLD: направление и удерживаемый диод
 * выбираются по текущему эффекту.
 *
 * Фазы (fx_phase = 0..104, 1 фаза = 1 тик):
 *   0-39:   бегущий огонь 0→1→2→3 (или 3→2→1→0), 10 тиков/позиция
 *   40-59:  пауза (все выключены)
 *   60-104: крайний диод горит 45 тиков (0.75 сек)
 *   Период: 105 тиков
 */
static uint8_t eff_running_fwd_hold(void)
{
    bool bwd = (cur_effect == LED_EFFECT_RUNNING_BWD_HOLD);
    uint8_t mask = 0;

    if (fx_phase < 40) {
        mask = (uint8_t)(1u << (bwd ? (uint8_t)(LED_COUNT - 1 - fx_pos)
                                    : fx_pos));
    } else if (fx_phase >= 60) {
        mask = (uint8_t)(1u << (bwd ? 0 : (uint8_t)(LED_COUNT - 1)));
    }

    /* Продвижение фазы: 0..104 */
    fx_phase = fx_adv(fx_phase, 105);

    if (fx_phase < 40) {
        /* Внутри бегущего участка — инкрементная позиция без деления */
        fx_cnt = fx_adv(fx_cnt, 10);
        if (fx_cnt == 0) {
            fx_pos = fx_adv(fx_pos, LED_COUNT);
        }
    } else {
        /* Подготовка следующего цикла: стартовая позиция */
        fx_cnt = 0;
        fx_pos = 0;
    }

    return mask;
}

/* ================================================================== */
/*  ТАБЛИЦА ЭФФЕКТОВ (dispatch table)                                  */
/* ================================================================== */

/**
 * Тип функции-эффекта: возвращает битовую маску состояний 4 диодов.
 *
 * Бит i результата = желаемое состояние LED i (1 = включён).
 * Это компактнее и быстрее на RV32, чем записывать массив bool[].
 *
 * Это ключ к расширяемости: чтобы добавить новый эффект, достаточно:
 *   1) Написать функцию с этой сигнатурой.
 *   2) Добавить её в eff_table[] ниже.
 *   3) Добавить значение в enum led_effect_t в led.h.
 *
 * Аналогично led_process() эффекты не зависят от глобального тика:
 * фазы и позиции продвигаются инкрементными счётчиками (fx_*),
 * поэтому в функцию не передаётся номер тика.
 */
typedef uint8_t (*eff_fn)(void);

/**
 * Таблица эффектов — индексируется по enum led_effect_t.
 *
 * Индекс таблицы = значение enum. Нулевой элемент (LED_EFFECT_NONE)
 * равен NULL — эффект не вызывается.
 *
 * Добавление нового эффекта:
 *   1) Написать статическую функцию eff_my_new().
 *   2) В led.h добавить: LED_EFFECT_MY_NEW (перед COUNT), COUNT увеличить.
 *   3) В таблицу добавить: [LED_EFFECT_MY_NEW] = eff_my_new,
 *
 * Несколько enum-значений могут указывать на ОДНУ функцию — эффекты,
 * отличающиеся только периодом/направлением, различаются через
 * cur_effect внутри общей функции (см. eff_running_fwd и др.).
 */
static const eff_fn eff_table[LED_EFFECT_COUNT] = {
    [LED_EFFECT_NONE]             = (void *)0,
    [LED_EFFECT_RUNNING_FWD]      = eff_running_fwd,
    [LED_EFFECT_RUNNING_BWD]      = eff_running_fwd,
    [LED_EFFECT_RUNNING_PP]       = eff_running_pp,
    [LED_EFFECT_RUNNING_CIRCLE]   = eff_running_fwd,
    [LED_EFFECT_PAIR_BLINK]       = eff_emergency,
    [LED_EFFECT_EMERGENCY]        = eff_emergency,
    [LED_EFFECT_RANDOM_FLASH]     = eff_random_flash,
    [LED_EFFECT_DOUBLE_BLINK]     = eff_double_blink,
    [LED_EFFECT_TRIPLE_BLINK]     = eff_double_blink,
    [LED_EFFECT_ALTERNATING]      = eff_emergency,
    [LED_EFFECT_RUNNING_FWD_HOLD] = eff_running_fwd_hold,
    [LED_EFFECT_RUNNING_BWD_HOLD] = eff_running_fwd_hold
};

/**
 * Длительность одного цикла эффекта (в тиках).
 * Индекс = enum led_effect_t. Используется для пересчёта повторов в тики.
 *
 * RANDOM_FLASH не имеет цикла — период = 1 (repeats = ticks).
 */
static const uint16_t eff_period[LED_EFFECT_COUNT] = {
    [LED_EFFECT_NONE]             = 0,
    [LED_EFFECT_RUNNING_FWD]      = 40,   /* 4 позиции × 10 тиков  */
    [LED_EFFECT_RUNNING_BWD]      = 40,
    [LED_EFFECT_RUNNING_PP]       = 60,   /* 6 позиций × 10 тиков  */
    [LED_EFFECT_RUNNING_CIRCLE]   = 32,   /* 4 позиции × 8 тиков   */
    [LED_EFFECT_PAIR_BLINK]       = 30,   /* 2 фазы × 15 тиков     */
    [LED_EFFECT_EMERGENCY]        = 10,   /* 2 фазы × 5 тиков      */
    [LED_EFFECT_RANDOM_FLASH]     = 1,    /* нет цикла */
    [LED_EFFECT_DOUBLE_BLINK]     = 80,   /* 16 фаз × 5 тиков      */
    [LED_EFFECT_TRIPLE_BLINK]     = 105,  /* 21 фаза × 5 тиков     */
    [LED_EFFECT_ALTERNATING]      = 40,   /* 2 фазы × 20 тиков     */
    [LED_EFFECT_RUNNING_FWD_HOLD] = 105,  /* 40 + 20 пауза + 45 удержание */
    [LED_EFFECT_RUNNING_BWD_HOLD] = 105
};

/* ================================================================== */
/*  ПУБЛИЧНЫЙ API: инициализация / callback / process                   */
/* ================================================================== */

void led_init(void)
{
    /*
     * Сброс всех 4 конечных автоматов: нулевые значения дают mode = OFF
     * (LED_MODE_OFF == 0), flags = 0 и все поля = 0.
     */
    memset(leds, 0, sizeof(leds));

    /* Сброс глобальных переменных */
    hw_set           = (void *)0;
    cur_effect       = LED_EFFECT_NONE;
    effect_remaining = 0;
    out_mask         = 0;
    driven_mask      = 0;
    fx_phase         = 0;
    fx_pos           = 0;
    fx_cnt           = 0;
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
 *   1) Цикл по 4 LED: для каждого вызывается обработчик по режиму.
 *   2) Если хотя бы один LED в EFFECT-режиме — вызвать функцию эффекта,
 *      которая запишет состояния в effect_st[], и применить их.
 *
 * Важно: состояние меняется только через apply_state(), поэтому
 * GPIO callback не вызывается на каждом тике: кэш out_mask отсекает
 * запись при неизменном состоянии пина.
 *
 * Важно: эффект применяется ПОСЛЕ ручных режимов, поэтому
 * если LED в EFFECT-режиме, его ручной обработчик НЕ вызывается.
 * Но если пользователь вызвал led_on(id) для LED в EFFECT-режиме,
 * этот LED переключится в ON и эффект его НЕ будет менять.
 */
void led_process(void)
{
    uint8_t i;
    uint8_t effect_mask = 0;  /* Маска состояний эффекта (бит i = LED i) */
    bool any_effect = false;

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

        /* Вызвать функцию эффекта: она вернёт битовую маску состояний */
        effect_mask = eff_table[cur_effect]();

        /* Применить состояния только к тем LED, которые в EFFECT-режиме */
        for (i = 0; i < LED_COUNT; i++) {
            if (leds[i].mode == LED_MODE_EFFECT) {
                apply_state(i, (effect_mask & (uint8_t)(1u << i)) != 0);
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
    leds[id].flags     = LED_F_PHASE;
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
    if (id >= LED_COUNT || !seq || !seq->steps || seq->count == 0) return;

    /* Установить режим SEQUENCE */
    leds[id].mode      = LED_MODE_SEQUENCE;

    /* Загрузить параметры последовательности */
    leds[id].seq       = seq->steps;
    leds[id].seq_len   = seq->count;

    /* Начать с первого шага */
    leds[id].seq_idx   = 0;
    leds[id].flags     = (uint8_t)((seq->loop ? LED_F_SEQ_LOOP : 0) |
                                   (seq->steps[0].state
                                        ? LED_F_SEQ_PHASE : 0));
    leds[id].seq_cnt   = seq->steps[0].ticks;

    /* Немедленно применить первое состояние */
    apply_state(id, (leds[id].flags & LED_F_SEQ_PHASE) != 0);
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

    if (effect == LED_EFFECT_NONE) {
        led_stop_effect();
        return;
    }

    cur_effect       = effect;
    effect_remaining = 0;
    /* Сброс счётчиков эффекта, чтобы новый эффект стартовал с фазы 0 */
    fx_phase = 0;
    fx_pos   = 0;
    fx_cnt   = 0;
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
        effect_remaining = (uint32_t)repeats * eff_period[effect];
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

uint8_t led_effect_is_running(void)
{
    return effect_remaining > 0 ? 1 : 0;
}

uint8_t led_any_led_active(void)
{
    uint8_t i;
    for (i = 0; i < LED_COUNT; i++) {
        if (leds[i].mode == LED_MODE_ON_FOR ||
            leds[i].mode == LED_MODE_BLINK ||
            leds[i].mode == LED_MODE_SEQUENCE ||
            leds[i].mode == LED_MODE_EFFECT) {
            return 1;
        }
    }
    return 0;
}

void led_flash_and_fade(uint8_t count)
{
    uint8_t i;
    if (count == 0 || count > LED_COUNT) return;

    /* Остановить текущий эффект, если есть */
    led_stop_effect();

    /* Выключить неиспользуемые диоды */
    for (i = count; i < LED_COUNT; i++) {
        set_mode_off(i);
    }

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
        uint16_t on_time = 60 + (count - 1 - i) * 10;
        led_on_for(i, on_time);
    }
}
