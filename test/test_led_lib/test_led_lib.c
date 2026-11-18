/*
 * SPDX-License-Identifier: MIT
 *
 * Host-side regression tests for LED_LIB (framework = custom).
 * Run:  pio test -e native
 * or:   gcc -std=c99 -Iinclude test/test_led_lib/test_led_lib.c src/led.c -o t && ./t
 *
 * The suite adapts to LED_COUNT and LED_USE_* configuration.
 * Exit code 0 = all checks passed.
 */

#include <stdio.h>
#include <string.h>
#include "led.h"

/* ------------------------------------------------------------------ */
/*  Мини-фреймворк                                                     */
/* ------------------------------------------------------------------ */

static int checks_failed;
static int checks_total;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks_total++;                                                      \
        if (!(cond)) {                                                       \
            checks_failed++;                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long _va = (long)(a), _vb = (long)(b);                               \
        checks_total++;                                                      \
        if (_va != _vb) {                                                    \
            checks_failed++;                                                 \
            printf("  FAIL %s:%d: %s == %s (got %ld, want %ld)\n",           \
                   __FILE__, __LINE__, #a, #b, _va, _vb);                    \
        }                                                                    \
    } while (0)

/* Локальные копии производных масок (в led.c они internal). */
#define T_MASK_ALL  ((uint8_t)((1u << LED_COUNT) - 1u))
#define T_MASK_EVEN ((uint8_t)(0x55u & T_MASK_ALL))
#define T_MASK_ODD  ((uint8_t)(0xAAu & T_MASK_ALL))

/*
 * Эффект, доступный в текущей конфигурации, для тестов подсистемы
 * эффектов в целом (запуск/идемпотентность/таймер/остановка).
 */
#if LED_USE_RUNNING_EFFECTS
#define TEST_EFFECT        LED_EFFECT_RUNNING_FWD
#define TEST_EFFECT_PERIOD (LED_COUNT * 10)
#elif LED_USE_BLINK_EFFECTS
#define TEST_EFFECT        LED_EFFECT_EMERGENCY
#define TEST_EFFECT_PERIOD 10
#elif LED_USE_PINGPONG_EFFECT
#define TEST_EFFECT        LED_EFFECT_RUNNING_PP
#define TEST_EFFECT_PERIOD ((2 * LED_COUNT - 2) * 10)
#elif LED_USE_MULTI_BLINK_EFFECTS
#define TEST_EFFECT        LED_EFFECT_DOUBLE_BLINK
#define TEST_EFFECT_PERIOD 80
#elif LED_USE_RANDOM_EFFECT
#define TEST_EFFECT        LED_EFFECT_RANDOM_FLASH
#define TEST_EFFECT_PERIOD 1
#elif LED_USE_HOLD_EFFECTS
#define TEST_EFFECT        LED_EFFECT_RUNNING_FWD_HOLD
#define TEST_EFFECT_PERIOD (LED_COUNT * 10 + 65)
#else
#define TEST_EFFECT        LED_EFFECT_NONE
#define TEST_EFFECT_PERIOD 0
#endif

/* ------------------------------------------------------------------ */
/*  Мок вывода: уровни + журнал вызовов                                 */
/* ------------------------------------------------------------------ */

/** Физические уровни выводов (0 = гас, 1..LED_LEVEL_MAX = «горит»). */
static uint8_t phys_level[LED_COUNT];
static unsigned gpio_calls;

/** Журнал вызовов callback — для сравнения потоков (инвариант advance). */
#define LOG_MAX 8192
static uint8_t  log_id[LOG_MAX];
static uint8_t  log_lvl[LOG_MAX];
static unsigned log_len;
static int      log_overflow;

static void mock_gpio(uint8_t id, uint8_t level)
{
    gpio_calls++;
    if (id < LED_COUNT) {
        phys_level[id] = level;
    }
    if (log_len < LOG_MAX) {
        log_id[log_len] = id;
        log_lvl[log_len] = level;
        log_len++;
    } else {
        log_overflow = 1;
    }
}

/** Битовая маска «горящих» каналов — для старых тестов эффектов. */
static unsigned char phys_mask(void)
{
    unsigned char m = 0;
    uint8_t i;

    for (i = 0; i < LED_COUNT; i++) {
        if (phys_level[i] > 0u) {
            m = (unsigned char)(m | (1u << i));
        }
    }
    return m;
}

static void reset_mock(void)
{
    led_init();
    led_set_callback(mock_gpio);
    for (log_len = 0; log_len < LOG_MAX; log_len++) {
        log_id[log_len] = 0;
        log_lvl[log_len] = 0;
    }
    log_len      = 0;
    log_overflow = 0;
    gpio_calls   = 0;
    memset(phys_level, 0, sizeof(phys_level));
}

static void run(unsigned n)
{
    while (n--) {
        led_process();
    }
}

/* ------------------------------------------------------------------ */
/*  Тестовые фикстуры                                                  */
/* ------------------------------------------------------------------ */

#if LED_USE_SEQUENCES
static const led_step_t seq_steps[] = {
    LED_STEP_LVL(2, 2),   /* уровень 2 на 2 тика */
    LED_STEP_OFF(3),      /* гашение на 3 тика    */
    LED_STEP_ON(1)        /* вспышка на 1 тик     */
};
static const led_sequence_t seq_once = LED_SEQ(seq_steps);
static const led_sequence_t seq_loop = LED_SEQ_LOOP(seq_steps);

/* Ведущий шаг OFF = «стартовать через N тиков» (отдельного API нет). */
static const led_step_t delay_steps[] = {
    LED_STEP_OFF(4),
    LED_STEP_ON(2),
    LED_STEP_OFF(1)
};
static const led_sequence_t delay_seq = LED_SEQ(delay_steps);
#endif

#if LED_USE_PACKED_COMMANDS
/* «3 коротких вспышки 15/60 тиков на уровне 1» — эталон из ТЗ. */
static const uint32_t pk_err[]  = { LED_CMD(1u, 3u, 15u, 60u, 1u, 0u) };
static const uint32_t pk_cmd[]  = { LED_CMD(1u, 1u, 4u, 6u, 2u, 0u) };
static const uint32_t pk_stop[] = { LED_CMD(1u, 1u, 4u, 6u, 2u, 0u), LED_SYS_STOP };
static const uint32_t pk_rep2[] = { LED_CMD(1u, 1u, 4u, 6u, 2u, 0u),
                                    LED_SYS_REP_AT(2u) };
static const uint32_t pk_inf[]  = { LED_CMD(1u, 1u, 4u, 6u, 2u, 0u),
                                    LED_SYS_REP_START };
/* Вырожденная таблица: все фазы нулевые + бесконечный повтор. */
static const uint32_t pk_spin[] = { LED_CMD(1u, 15u, 0u, 0u, 1u, 0u),
                                    LED_SYS_REP_START };
/* Таблица из одних системных команд: guard обязан погасить её сразу. */
static const uint32_t pk_sys[]  = { LED_SYS_REP_START };
/* Две команды подряд, без явной границы -> STOP по умолчанию. */
static const uint32_t pk_two[]  = { LED_CMD(1u, 2u, 2u, 3u, 1u, 0u),
                                    LED_CMD(1u, 1u, 5u, 4u, 1u, 0u) };
#endif

/* ------------------------------------------------------------------ */
/*  Тесты: ручные режимы                                               */
/* ------------------------------------------------------------------ */

static void test_init_and_queries(void)
{
    reset_mock();
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    CHECK_EQ(led_is_on(0), 0);
    CHECK_EQ(led_get_level(0), 0);
    CHECK_EQ(led_any_led_active(), 0);
    CHECK_EQ(led_pattern_active(), 0);
    led_on(0);
    CHECK_EQ(led_get_mode(0), LED_MODE_ON);
    CHECK_EQ(led_is_on(0), 1);
    CHECK_EQ(led_get_level(0), 1);
    CHECK_EQ(led_any_led_active(), 0);   /* ON — статичный режим */
    led_off(0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    /* Вне диапазона — безопасно. */
    led_on((uint8_t)LED_COUNT);
    led_off((uint8_t)LED_COUNT);
    led_toggle((uint8_t)LED_COUNT);
    led_on_for((uint8_t)LED_COUNT, 5);
    led_blink((uint8_t)LED_COUNT, 1, 1);
    led_blink_level((uint8_t)LED_COUNT, 1, 1, 0, 1);
    led_set_level((uint8_t)LED_COUNT, 2);
    CHECK_EQ(led_get_mode((uint8_t)LED_COUNT), LED_MODE_OFF);
    CHECK_EQ(led_is_on((uint8_t)LED_COUNT), 0);
    CHECK_EQ(led_get_level((uint8_t)LED_COUNT), 0);
}

static void test_on_off_toggle(void)
{
    uint8_t id1 = (LED_COUNT > 1) ? 1u : 0u;

    /* led_toggle работает по фактическому уровню вывода. */
    reset_mock();
    led_on_for(0, 100);
    run(1);
    CHECK_EQ(led_is_on(0), 1);
    led_toggle(0);                       /* горит -> погаснет */
    run(200);
    CHECK_EQ(led_is_on(0), 0);
    CHECK_EQ(phys_mask(), 0);
    led_toggle(0);                       /* погас -> загорится */
    run(1);
    CHECK_EQ(led_is_on(0), 1);

    reset_mock();
    led_blink(id1, 5, 5);
    run(1);
    CHECK_EQ(led_is_on(id1), 1);
    led_toggle(id1);                     /* горящий мигающий диод гаснет */
    run(20);
    CHECK_EQ(led_is_on(id1), 0);
    CHECK_EQ(led_get_mode(id1), LED_MODE_OFF);
}

static void test_on_for_timing(void)
{
    unsigned i, on;

    reset_mock();
    led_on_for(0, 5);
    on = 0;
    for (i = 0; i < 10; i++) {
        /* Сэмплируем до led_process: считаем тиковые интервалы,
         * в течение которых вывод был в активном уровне. */
        if (phys_level[0] > 0u) {
            on++;
        }
        led_process();
    }
    CHECK_EQ(on, 5);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);

    /* ticks == 0 -> мгновенное выключение */
    led_on(0);
    led_on_for(0, 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    CHECK_EQ(led_is_on(0), 0);
}

static void test_blink(void)
{
    unsigned i, on = 0, off = 0;

    reset_mock();
    led_blink(0, 3, 7);
    for (i = 0; i < 100; i++) {
        led_process();
        if (phys_level[0] > 0u) {
            on++;
        } else {
            off++;
        }
    }
    CHECK_EQ(on, 30);
    CHECK_EQ(off, 70);

    /* Нулевые фазы запрещены -> OFF */
    led_on(0);
    led_blink(0, 0, 5);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    led_blink(0, 5, 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
}

#if LED_COUNT >= 4
/**
 * Реальная задача с заказчика: четыре диода с разными частотами
 * (1 раз в 4 с, 1 раз в 2 с, 1 раз в 1 с, 2 раза в 1 с) при тике 1 кГц.
 * Это ровно то, что делает examples/rates_ch32v003.
 *
 * Проверяется ровно то, ради чего у каналов свои автоматы:
 *   - периоды точны (число вспышек за 4 с = 1 / 2 / 4 / 8), накопления
 *     ошибки от цикла к циклу нет;
 *   - длительность вспышки равна заданной (проверяются и короткая
 *     вспышка 100 мс, и скважность 50 % из примера);
 *   - пакетный advance() (спящий хост) даёт тот же результат, что и
 *     тик за тиком: таймеры uint16 не переполняются на длинном периоде,
 *     потому что длинные промежутки проматываются кусками по 0xFFFE.
 *
 * @param duty50  true — скважность 50 % (как в примере), false — 100 мс.
 */
static void check_rates(bool duty50)
{
    static const uint16_t period[4]  = { 4000u, 2000u, 1000u, 500u };
    static const unsigned  expect[4] = { 1u, 2u, 4u, 8u }; /* за 4 с */
    unsigned fronts_tick[4]  = { 0u, 0u, 0u, 0u };
    unsigned lit_tick[4]     = { 0u, 0u, 0u, 0u };
    unsigned fronts_batch[4] = { 0u, 0u, 0u, 0u };
    unsigned flash_at[4][5];                      /* тики первых вспышек */
    uint8_t  was_on[4] = { 0u, 0u, 0u, 0u };
    uint8_t  i;
    unsigned k, f;

    for (i = 0; i < 4; i++) {
        for (f = 0; f < 5; f++) {
            flash_at[i][f] = 0;
        }
    }

    /* --- тик за тиком, 4.2 с (с запасом на последнюю вспышку) --- */
    reset_mock();
    for (i = 0; i < 4; i++) {
        uint16_t half = (uint16_t)(period[i] / 2u);

        led_blink(i, duty50 ? half : (uint16_t)100u,
                 duty50 ? half : (uint16_t)(period[i] - 100u));
    }
    for (k = 0; k < 4200u; k++) {
        led_advance(1u);
        for (i = 0; i < 4; i++) {
            uint8_t now = (phys_level[i] > 0u) ? 1u : 0u;

            if ((now != 0u) && (was_on[i] == 0u)) {
                if (fronts_tick[i] < 5u) {
                    flash_at[i][fronts_tick[i]] = k + 1u;
                }
                if ((k + 1u) <= 4000u) {
                    fronts_tick[i]++;    /* вспышка целиком в окне 4 с */
                }
            }
            if ((now != 0u) && ((k + 1u) <= 4000u)) {
                lit_tick[i]++;
            }
            was_on[i] = now;
        }
    }

    /* --- пакетами по 10 мс: спящий хост с тиком 1 кГц --- */
    reset_mock();
    for (i = 0; i < 4; i++) {
        uint16_t half = (uint16_t)(period[i] / 2u);

        led_blink(i, duty50 ? half : (uint16_t)100u,
                 duty50 ? half : (uint16_t)(period[i] - 100u));
        was_on[i] = 0;
    }
    for (k = 0; k < 400u; k++) {
        led_advance(10u);
        for (i = 0; i < 4; i++) {
            uint8_t now = (phys_level[i] > 0u) ? 1u : 0u;

            if ((now != 0u) && (was_on[i] == 0u)) {
                fronts_batch[i]++;
            }
            was_on[i] = now;
        }
    }

    for (i = 0; i < 4; i++) {
        uint16_t on = duty50 ? (uint16_t)(period[i] / 2u) : (uint16_t)100u;

        CHECK_EQ(fronts_tick[i], expect[i]);
        CHECK_EQ(fronts_batch[i], expect[i]);
        CHECK_EQ(lit_tick[i], expect[i] * (unsigned)on);
        /* Вспышки стартуют на 1-м тике и далее ровно через период:
         * накопления ошибки от цикла к циклу нет. */
        for (f = 0; f < 5u; f++) {
            if (flash_at[i][f] != 0u) {
                CHECK_EQ(flash_at[i][f], 1u + f * (unsigned)period[i]);
            }
        }
    }
}

static void test_independent_rates(void)
{
    check_rates(false);   /* короткая вспышка 100 мс */
    check_rates(true);    /* скважность 50 % (examples/rates_ch32v003) */
}
#endif /* LED_COUNT >= 4 */

#if LED_USE_SEQUENCES
static void test_sequence(void)
{
    char buf[21];

    /* Однократно: уровень 2 два тика, гашение 3, вспышка 1, затем STOP. */
    reset_mock();
    led_play(0, &seq_once);
    {
        unsigned i;

        for (i = 0; i < 12; i++) {
            buf[i] = (phys_level[0] > 0u) ? '#' : '.';
            led_process();
        }
        buf[12] = '\0';
    }
    CHECK(strcmp(buf, "##...#......") == 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);

    /* Зациклено: период 6 тиков, три периода подряд. */
    reset_mock();
    led_play(0, &seq_loop);
    {
        unsigned i, t = 0, k;

        for (k = 0; k < 3u; k++) {
            buf[t++] = '#';
            buf[t++] = '#';
            buf[t++] = '.';
            buf[t++] = '.';
            buf[t++] = '.';
            buf[t++] = '#';
        }
        buf[t] = '\0';
        for (i = 0; i < t; i++) {
            if (buf[i] != ((phys_level[0] > 0u) ? '#' : '.')) {
                buf[i] = '?';
            }
            led_process();
        }
        CHECK(strcmp(buf, "##...###...###...#") == 0);
    }
    CHECK_EQ(led_get_mode(0), LED_MODE_SEQUENCE);

    /* Шаги нулевой длительности живут 1 тик; зависаний нет. */
    {
        static const led_step_t zs[] = {
            LED_STEP_ON(0), LED_STEP_OFF(0), LED_STEP_ON(3)
        };
        static const led_sequence_t zseq = LED_SEQ(zs);
        unsigned i;

        reset_mock();
        led_play(0, &zseq);
        for (i = 0; i < 12; i++) {
            buf[i] = (phys_level[0] > 0u) ? '#' : '.';
            led_process();
        }
        buf[12] = '\0';
        CHECK(strcmp(buf, "#.###.......") == 0);
        CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    }

    /* Некорректные указатели игнорируются. */
    reset_mock();
    led_play(0, NULL);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    {
        static const led_sequence_t bad1 = { NULL, 2, false };
        static const led_step_t st[1]    = { LED_STEP_ON(1) };
        static const led_sequence_t bad2 = { st, 0, false };

        led_play(0, &bad1);
        led_play(0, &bad2);
        CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    }
}

/* R4: стартовая задержка = ведущий шаг LED_STEP_OFF(n). */
static void test_leading_off_step(void)
{
    char buf[16];

    reset_mock();
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    CHECK_EQ(led_get_level(0), 0);
    led_play(0, &delay_seq);
    /* Первый шаг применён немедленно, но это 0: выход не «съеден»,
     * задержка реально длится 4 тика. */
    CHECK_EQ(led_get_level(0), 0);
    {
        unsigned i;

        for (i = 0; i < 8; i++) {
            buf[i] = (phys_level[0] > 0u) ? '#' : '.';
            led_process();
        }
        buf[8] = '\0';
    }
    CHECK(strcmp(buf, "....##..") == 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
}
#endif

/* ------------------------------------------------------------------ */
/*  R2: уровневый выход                                               */
/* ------------------------------------------------------------------ */

static void test_levels(void)
{
    /* Статический уровень — основной API аналогового канала. */
    reset_mock();
    led_set_level(0, 3u);
    CHECK_EQ(led_get_level(0), 3);
    CHECK_EQ(led_is_on(0), 1);
    CHECK_EQ(led_get_mode(0), LED_MODE_ON);
    run(5);
    CHECK_EQ(led_get_level(0), 3);   /* уровень держится, а не мигает */
    led_off(0);
    CHECK_EQ(led_get_level(0), 0);
    CHECK_EQ(led_is_on(0), 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    led_on(0);
    CHECK_EQ(led_get_level(0), 1);

    /* led_toggle работает по «уровень > 0», а не по 1. */
    led_set_level(0, 5u);
    led_toggle(0);
    CHECK_EQ(led_get_level(0), 0);
    led_toggle(0);
    CHECK_EQ(led_get_level(0), 1);

    /* led_blink_level: произвольные уровни фаз (сэмпл ПОСЛЕ advance). */
    reset_mock();
    led_blink_level(0, 4u, 2u, 0u, 3u);
    {
        uint8_t seen[5];
        unsigned k;

        for (k = 0; k < 5u; k++) {
            led_advance(1u);
            seen[k] = phys_level[0];
        }
        CHECK_EQ(seen[0], 4);
        CHECK_EQ(seen[1], 4);
        CHECK_EQ(seen[2], 0);
        CHECK_EQ(seen[3], 0);
        CHECK_EQ(seen[4], 0);
    }
    /* led_blink — обёртка над уровнями 1/0: 2 тика вкл, 2 тика выкл. */
    reset_mock();
    led_blink(0, 2u, 2u);
    run(2);
    CHECK_EQ(led_get_level(0), 1);
    run(2);
    CHECK_EQ(led_get_level(0), 0);
    run(2);
    CHECK_EQ(led_get_level(0), 1);

    /* Нулевые фазы -> OFF, как и у led_blink. */
    led_blink_level(0, 4u, 0u, 0u, 3u);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    led_blink_level(0, 4u, 2u, 0u, 0u);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);

    /* led_get_level() отражает состояние и без callback'а. */
    reset_mock();
    led_set_callback(NULL);
    led_set_level(0, 6u);
    CHECK_EQ(led_get_level(0), 6);
    CHECK_EQ(led_is_on(0), 1);
    led_set_callback(mock_gpio);
    run(1);
    CHECK_EQ(phys_level[0], 6);
}

#if LED_USE_SEQUENCES
/* Уровневые шаги последовательности. */
static void test_sequence_levels(void)
{
    static const led_step_t lv[] = {
        LED_STEP_LVL(1, 1), LED_STEP_LVL(3, 2), LED_STEP_LVL(7, 1), LED_STEP_OFF(1)
    };
    static const led_sequence_t sq = LED_SEQ(lv);
    char buf[8];

    reset_mock();
    led_play(0, &sq);
    buf[0] = (char)('0' + phys_level[0]);
    led_process();
    buf[1] = (char)('0' + phys_level[0]);
    led_process();
    buf[2] = (char)('0' + phys_level[0]);
    led_process();
    buf[3] = (char)('0' + phys_level[0]);
    led_process();
    buf[4] = (char)('0' + phys_level[0]);
    buf[5] = '\0';
    CHECK(strcmp(buf, "13370") == 0);
}
#endif

/* ------------------------------------------------------------------ */
/*  R1: событийное продвижение времени                                 */
/* ------------------------------------------------------------------ */

/** Набор сценариев для проверки инварианта advance(1)xN == advance(N). */
enum {
    TC_STATIC = 0,
    TC_ON_FOR,
    TC_BLINK,
    TC_BLINK_LEVEL,
    TC_SEQ,
    TC_EFFECT,
    TC_PACKED,
    TC_COUNT_CASES
};

static void setup_case(int c)
{
    switch (c) {
        case TC_STATIC:
            led_set_level(0, 3u);
            break;
        case TC_ON_FOR:
            led_on_for(0, 7u);
            break;
        case TC_BLINK:
            led_blink(0, 3u, 5u);
            break;
        case TC_BLINK_LEVEL:
            led_blink_level(0, 4u, 3u, 1u, 5u);
            break;
#if LED_USE_SEQUENCES
        case TC_SEQ:
            led_play(0, &seq_loop);
            break;
#endif
#if TEST_EFFECT != LED_EFFECT_NONE
        case TC_EFFECT:
            led_start_effect(TEST_EFFECT);
            break;
#endif
#if LED_USE_PACKED_COMMANDS
        case TC_PACKED:
            led_packed_run(pk_err, LED_ARRAY_LEN(pk_err));
            break;
#endif
        default:
            break;
    }
}

static int case_enabled(int c)
{
    switch (c) {
#if !LED_USE_SEQUENCES
        case TC_SEQ:
            return 0;
#endif
#if TEST_EFFECT == LED_EFFECT_NONE
        case TC_EFFECT:
            return 0;
#endif
#if !LED_USE_PACKED_COMMANDS
        case TC_PACKED:
            return 0;
#endif
        default:
            return 1;
    }
}

static void log_copy(uint8_t *ids, uint8_t *lvls, unsigned *len)
{
    *len = log_len;
    memcpy(ids, log_id, (size_t)(log_len * sizeof(uint8_t)));
    memcpy(lvls, log_lvl, (size_t)(log_len * sizeof(uint8_t)));
}

/** R1: led_advance(1) x N тождественно led_advance(N). */
static void test_advance_invariant(void)
{
    static uint8_t ida[LOG_MAX], lvla[LOG_MAX], idb[LOG_MAX], lvlb[LOG_MAX];
    static uint8_t pa[LED_COUNT], pb[LED_COUNT];
    const unsigned n = 200u;
    int c;

    for (c = 0; c < TC_COUNT_CASES; c++) {
        unsigned la, lb, t;
        int same;

        if (!case_enabled(c)) {
            continue;
        }

        /* Поток A: тик за тиком. */
        reset_mock();
        setup_case(c);
        for (t = 0; t < n; t++) {
            led_advance(1u);
        }
        log_copy(ida, lvla, &la);
        memcpy(pa, phys_level, sizeof(pa));

        /* Поток B: одним вызовом. */
        reset_mock();
        setup_case(c);
        led_advance(n);
        log_copy(idb, lvlb, &lb);
        memcpy(pb, phys_level, sizeof(pb));

        CHECK(!log_overflow);
        CHECK_EQ(la, lb);
        same = (memcmp(ida, idb, (size_t)(la * sizeof(uint8_t))) == 0) &&
               (memcmp(lvla, lvlb, (size_t)(la * sizeof(uint8_t))) == 0);
        if (!same) {
            unsigned k;

            for (k = 0; k < la && k < lb; k++) {
                if (ida[k] != idb[k] || lvla[k] != lvlb[k]) {
                    printf("  case %d: first divergence at call %u: "
                           "(%u,%u) vs (%u,%u)\n", c, k,
                           ida[k], lvla[k], idb[k], lvlb[k]);
                    break;
                }
            }
        }
        CHECK(same);
        CHECK(memcmp(pa, pb, sizeof(pa)) == 0);
    }
}

/** R1: ноль тиков не двигает FSM, но обслуживает sync и ownership. */
static void test_advance_zero(void)
{
    unsigned before;

    reset_mock();
    led_blink(0, 3u, 5u);
    run(1);
    before = log_len;
    led_advance(0);
    CHECK_EQ(log_len, before);

    reset_mock();
    before = log_len;
    led_advance(0);
    CHECK_EQ(log_len - before, LED_COUNT);
    led_set_owner(LED_OWNER_STROBE);
    led_advance(0);
    CHECK_EQ(led_claim(), LED_OWNER_NONE);
}

/**
 * R1: длинные таймеры и пакетное продвижение сильно больше счётчика.
 * Здесь работает ограничение «один промах не длиннее 0xFFFE тиков»
 * (счётчики каналов — uint16_t) и кодирование больших длительностей
 * в упакованных командах.
 */
static void test_advance_long(void)
{
    reset_mock();
    led_on_for(0, 0xFFFFu);
    CHECK_EQ(led_ticks_until_next(), 0xFFFF);
    led_advance(0xFFFEu);
    CHECK_EQ(led_get_level(0), 1);
    led_advance(1u);                 /* ровно на последнем тике -> OFF */
    CHECK_EQ(led_get_level(0), 0);
    CHECK_EQ(led_pattern_active(), 0);

    /* Пакет сильно длиннее паттерна. */
    reset_mock();
    led_on_for(0, 3u);
    led_advance(100000u);
    CHECK_EQ(led_get_level(0), 0);
    CHECK_EQ(led_pattern_active(), 0);
    CHECK_EQ(led_ticks_until_next(), (long)LED_TICKS_FOREVER);

#if LED_USE_PACKED_COMMANDS
    /* Длительность 258048 тиков = 63 * 4^6 (верхняя граница shift=6). */
    reset_mock();
    led_packed_once(LED_CMD(1u, 1u, 258048u, 0u, 1u, 0u));
    CHECK_EQ(led_ticks_until_next(), 258048);
    led_advance(258047u);
    CHECK_EQ(led_get_level(0), 1);
    led_advance(1u);
    CHECK_EQ(led_get_level(0), 0);      /* t1 == 0: переход на этом же тике */
    led_advance(1u);
    CHECK_EQ(led_pattern_active(), 0);
    CHECK_EQ(led_get_level(0), 0);
#endif
}

/** R1: тик до следующего изменения никогда не равен нулю. */
static void test_ticks_until_next(void)
{
    static uint8_t ida[LOG_MAX], lvla[LOG_MAX], idb[LOG_MAX], lvlb[LOG_MAX];
    static uint8_t pa[LED_COUNT], pb[LED_COUNT];
    const unsigned n = 200u;
    int c;

    /* Ничего не запущено -> изменений не будет. */
    reset_mock();
    CHECK_EQ(led_ticks_until_next(), (long)LED_TICKS_FOREVER);
    led_blink(0, 4u, 4u);
    led_advance(1u);
    CHECK_EQ(led_ticks_until_next(), 3);   /* остаток ON-фазы 4-1 */
    led_advance(2u);
    CHECK_EQ(led_ticks_until_next(), 1);   /* последний тик ON-фазы */

    for (c = 0; c < TC_COUNT_CASES; c++) {
        unsigned la, lb, t;
        int same, bad = 0;

        if (!case_enabled(c)) {
            continue;
        }

        /* Опорный поток: тик за тиком. */
        reset_mock();
        setup_case(c);
        for (t = 0; t < n; t++) {
            led_advance(1u);
        }
        log_copy(ida, lvla, &la);
        memcpy(pa, phys_level, sizeof(pa));

        /* «Спящий хост»: спим ровно led_ticks_until_next() тиков,
         * суммарно покрывая те же n тиков, что и опросный вариант. */
        reset_mock();
        setup_case(c);
        {
            unsigned advanced = 0;

            while (advanced < n) {
                uint32_t wait = led_ticks_until_next();

                if (wait == (uint32_t)LED_TICKS_FOREVER) {
                    /* Таймерных паттернов нет: время ничего не меняет.
                     * advance(0) выполняет только синхронизацию выводов. */
                    led_advance(0u);
                    advanced = n;
                    break;
                }
                if (wait == 0u) {
                    bad = 1;          /* 0 недопустим по контракту API */
                    break;
                }
                if (wait > (n - advanced)) {
                    wait = (uint32_t)(n - advanced);
                }
                led_advance(wait);
                advanced += (unsigned)wait;
            }
            if (advanced != n && bad == 0) {
                bad = 3;              /* не сошлись с n тиками */
            }
        }
        log_copy(idb, lvlb, &lb);
        memcpy(pb, phys_level, sizeof(pb));

        if (bad != 0) {
            checks_total++;
            checks_failed++;
            printf("  FAIL sleeping-host case %d, err=%d\n", c, bad);
        }
        CHECK(!log_overflow);
        CHECK_EQ(la, lb);
        same = (memcmp(ida, idb, (size_t)(la * sizeof(uint8_t))) == 0) &&
               (memcmp(lvla, lvlb, (size_t)(la * sizeof(uint8_t))) == 0);
        CHECK(same);
        CHECK(memcmp(pa, pb, sizeof(pa)) == 0);
    }

    /* Статические режимы: снать можно вечно. */
    reset_mock();
    led_set_level(0, 2u);
    run(3);
    CHECK_EQ(led_ticks_until_next(), (long)LED_TICKS_FOREVER);
}

/** R1/R6: сценарий сна с конечным числом пробуждений. */
static void test_sleep_wake_sequence(void)
{
    static const uint32_t steps[] = { 0u, 1u, 1u, 4u, 2u, 1u, 10u, 0u, 0u };
    static uint8_t stream[64];
    static uint8_t expect[64];
    unsigned t, k = 0, w;
    int ok = 1;

    reset_mock();
    led_blink(0, 5u, 7u);
    for (t = 0; t < 40u; t++) {
        stream[t] = (uint8_t)(phys_level[0] > 0u ? 1u : 0u);
        led_advance(1u);
    }

    reset_mock();
    led_blink(0, 5u, 7u);
    for (t = 0; t < 40u; t += w) {
        w = led_ticks_until_next();
        if (w == (uint32_t)LED_TICKS_FOREVER) {
            w = 1u;
        }
        if (w == 0u) {
            ok = 0;
            break;
        }
        if (t + w > 40u) {
            w = 40u - t;
        }
        /* Пробуждение: снимок состояния снаружи + переход внутрь. */
        for (k = 0; k < w; k++) {
            expect[t + k] = (uint8_t)(phys_level[0] > 0u ? 1u : 0u);
            led_advance(1u);
        }
    }
    CHECK(ok);
    for (t = 0; t < 40u; t++) {
        if (stream[t] != expect[t]) {
            ok = 0;
            break;
        }
    }
    CHECK(ok);
    (void)steps;
}

/* ------------------------------------------------------------------ */
/*  Эффекты (без изменений относительно 2.0, маска вместо уровня)      */
/* ------------------------------------------------------------------ */

#if LED_USE_EFFECTS

#define STREAM_MAX 720
static uint8_t stream[STREAM_MAX];
static unsigned stream_len;

static void collect(led_effect_t e, unsigned ticks)
{
    unsigned i;

    reset_mock();
    led_start_effect(e);
    if (ticks > STREAM_MAX) {
        ticks = STREAM_MAX;
    }
    for (i = 0; i < ticks; i++) {
        led_process();
        stream[i] = phys_mask();
    }
    stream_len = ticks;
}

/** Сжатие в run-length: mask[i]/len[i]. Возвращает число серий. */
static unsigned to_runs(uint8_t *mask, unsigned *len, unsigned max_runs)
{
    unsigned i, n = 0;

    for (i = 0; i < stream_len; i++) {
        if (n > 0 && mask[n - 1] == stream[i]) {
            len[n - 1]++;
        } else {
            if (n >= max_runs) {
                break;
            }
            mask[n] = stream[i];
            len[n]  = 1;
            n++;
        }
    }
    return n;
}

/** Все ли серии в потоке имеют длительность ровно d? */
static int all_runs_of(unsigned d)
{
    uint8_t  m[64];
    unsigned l[64];
    unsigned n, i;

    n = to_runs(m, l, 64);
    for (i = 0; i < n; i++) {
        if (l[i] != d) {
            return 0;
        }
    }
    return n > 0;
}

#if LED_USE_RUNNING_EFFECTS
static void test_running_effects(void)
{
#if LED_COUNT == 1
    /* Единственная позиция: поток константен. */
    (void)all_runs_of;
    collect(LED_EFFECT_RUNNING_FWD, 40);
    {
        int ok = 1;
        unsigned t;

        for (t = 0; t < stream_len; t++) {
            ok = ok && (stream[t] == 0x01);
        }
        CHECK(ok);
    }
#else
    /* FWD: каждая позиция ровно 10 тиков, порядок 0..N-1. */
    collect(LED_EFFECT_RUNNING_FWD, (unsigned)LED_COUNT * 10u * 2u);
    CHECK(all_runs_of(10));
    {
        uint8_t  m[64];
        unsigned l[64];
        unsigned n, i;

        n = to_runs(m, l, 64);
        CHECK_EQ(n, 2u * LED_COUNT);   /* два полных цикла */
        for (i = 0; i < n; i++) {
            CHECK_EQ(m[i], (uint8_t)(1u << (i % LED_COUNT)));
        }
    }
#endif

    /* BWD: зеркальный порядок N-1..0. */
    collect(LED_EFFECT_RUNNING_BWD, (unsigned)LED_COUNT * 10u);
    {
        uint8_t  m[64];
        unsigned l[64];
        unsigned n, i;

        n = to_runs(m, l, 64);
        for (i = 0; i < n; i++) {
            CHECK_EQ(m[i], (uint8_t)(1u << (LED_COUNT - 1u - (i % LED_COUNT))));
        }
    }

#if LED_COUNT >= 2
    /* CIRCLE: выдержка 8 тиков. */
    collect(LED_EFFECT_RUNNING_CIRCLE, (unsigned)LED_COUNT * 8u);
    CHECK(all_runs_of(8));
#endif
}
#endif /* LED_USE_RUNNING_EFFECTS */

#if LED_USE_PINGPONG_EFFECT
static void test_pingpong(void)
{
    /* Равномерная выдержка 10 тиков, включая первую позицию (регрессия L1). */
    collect(LED_EFFECT_RUNNING_PP, (unsigned)(2 * LED_COUNT - 2) * 10u);
    CHECK(all_runs_of(10));
}
#endif

#if LED_USE_BLINK_EFFECTS
static void test_blink_effects(void)
{
    uint8_t  m[64];
    unsigned l[64];
    unsigned n;

    /* EMERGENCY: 5 тиков все / 5 тиков никто. */
    collect(LED_EFFECT_EMERGENCY, 20);
    n = to_runs(m, l, 64);
    CHECK_EQ(n, 4);
    CHECK_EQ(m[0], T_MASK_ALL);
    CHECK_EQ(l[0], 5);
    CHECK_EQ(m[1], 0);
    CHECK_EQ(l[1], 5);

    /* PAIR_BLINK: чёт/нечет по 15 тиков. */
    collect(LED_EFFECT_PAIR_BLINK, 30);
    n = to_runs(m, l, 64);
    CHECK_EQ(n, 2);
    CHECK_EQ(m[0], T_MASK_EVEN);
    CHECK_EQ(l[0], 15);
    CHECK_EQ(m[1], T_MASK_ODD);
    CHECK_EQ(l[1], 15);

    /* ALTERNATING: тот же паттерн, период 20. */
    collect(LED_EFFECT_ALTERNATING, 40);
    n = to_runs(m, l, 64);
    CHECK_EQ(n, 2);
    CHECK_EQ(m[0], T_MASK_EVEN);
    CHECK_EQ(l[0], 20);
}
#endif

#if LED_USE_MULTI_BLINK_EFFECTS
static void test_multi_blink(void)
{
    uint8_t  m[64];
    unsigned l[64];
    unsigned n;

    /* DOUBLE: ON10 OFF10 ON10 OFF50. */
    collect(LED_EFFECT_DOUBLE_BLINK, 80);
    n = to_runs(m, l, 64);
    CHECK_EQ(n, 4);
    CHECK_EQ(m[0], T_MASK_ALL);
    CHECK_EQ(l[0], 10);
    CHECK_EQ(m[1], 0);
    CHECK_EQ(l[1], 10);
    CHECK_EQ(m[2], T_MASK_ALL);
    CHECK_EQ(l[2], 10);
    CHECK_EQ(m[3], 0);
    CHECK_EQ(l[3], 50);

    /* TRIPLE: ON10 OFF10 ON10 OFF10 ON10 OFF55. */
    collect(LED_EFFECT_TRIPLE_BLINK, 105);
    n = to_runs(m, l, 64);
    CHECK_EQ(n, 6);
    CHECK_EQ(l[0], 10);
    CHECK_EQ(l[1], 10);
    CHECK_EQ(l[2], 10);
    CHECK_EQ(l[3], 10);
    CHECK_EQ(l[4], 10);
    CHECK_EQ(l[5], 55);
    CHECK_EQ(m[5], 0);
}
#endif

#if LED_USE_HOLD_EFFECTS
static void test_hold_effects(void)
{
    uint8_t  m[64];
    unsigned l[64];
    unsigned n, i, period = (unsigned)LED_COUNT * 10u + 65u;
#if LED_COUNT < 2
    (void)m;
    (void)l;
    (void)n;
    (void)i;
#endif

    /* Регрессия H4: все циклы эффекта должны быть идентичны
     * (раньше при переходе фазы через 0 возникала паразитная вспышка). */
    collect(LED_EFFECT_RUNNING_FWD_HOLD, period * 3u);
    {
        int identical = 1;
        unsigned t;

        for (t = 0; t < period; t++) {
            if (stream[t] != stream[period + t] ||
                stream[t] != stream[2u * period + t]) {
                identical = 0;
                break;
            }
        }
        CHECK(identical);
    }

    /* Структура: LED_COUNT серий бега по 10 тиков, пауза 20, удержание 45.
     * (Для LED_COUNT=1 hold совпадает с позицией бега — серии сливаются,
     * поэтому структурные проверки имеют смысл только при LED_COUNT >= 2.) */
#if LED_COUNT >= 2
    n = to_runs(m, l, 64);
    CHECK(n >= (unsigned)LED_COUNT + 2u);
    for (i = 0; i < LED_COUNT; i++) {
        CHECK_EQ(l[i], 10);
        CHECK_EQ(m[i], (uint8_t)(1u << i));
    }
    CHECK_EQ(l[LED_COUNT], 20);                                   /* пауза     */
    CHECK_EQ(m[LED_COUNT], 0);
    CHECK_EQ(l[LED_COUNT + 1u], 45);                              /* удержание */
    CHECK_EQ(m[LED_COUNT + 1u], (uint8_t)(1u << (LED_COUNT - 1u)));

    /* BWD_HOLD: удерживается диод 0. */
    collect(LED_EFFECT_RUNNING_BWD_HOLD, period);
    n = to_runs(m, l, 64);
    CHECK_EQ(m[n - 1u], (uint8_t)(1u << 0));
    CHECK_EQ(l[n - 1u], 45);
#endif
}
#endif

#if LED_USE_RANDOM_EFFECT
static void test_random_reset(void)
{
    uint8_t a[8], b[8];
    unsigned i;

    reset_mock();
    led_start_effect(LED_EFFECT_RANDOM_FLASH);
    for (i = 0; i < 8; i++) {
        led_process();
        a[i] = phys_mask();
    }

    reset_mock();   /* led_init() сбрасывает PRNG (регрессия M9) */
    led_start_effect(LED_EFFECT_RANDOM_FLASH);
    for (i = 0; i < 8; i++) {
        led_process();
        b[i] = phys_mask();
    }
    CHECK(memcmp(a, b, sizeof(a)) == 0);
}
#endif

#if TEST_EFFECT != LED_EFFECT_NONE
static void test_effect_start_stop(void)
{
    /* Регрессия H1: идемпотентность. Поток с ре-армом каждый тик должен
     * совпадать с потоком однократного запуска (раньше ре-арм
     * «замораживал» эффект на первой фазе). */
    {
        uint8_t a[STREAM_MAX];
        uint8_t b[STREAM_MAX];
        unsigned i;

        reset_mock();
        led_start_effect(TEST_EFFECT);
        for (i = 0; i < 120; i++) {
            led_process();
            a[i] = phys_mask();
        }

        reset_mock();
        for (i = 0; i < 120; i++) {
            led_start_effect(TEST_EFFECT);   /* опросный вызов */
            led_process();
            b[i] = phys_mask();
        }
        CHECK(memcmp(a, b, 120) == 0);
    }

    /* led_start_effect(LED_EFFECT_NONE) останавливает эффект. */
    reset_mock();
    led_start_effect(TEST_EFFECT);
    run(1);
    led_start_effect(LED_EFFECT_NONE);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    CHECK_EQ(led_effect_active(), 0);

    /* led_stop_effect() гасит только EFFECT-диоды. */
    reset_mock();
    led_start_effect(TEST_EFFECT);
    run(1);
    led_on(0);                  /* «оторвать» LED0 */
    led_stop_effect();
    CHECK_EQ(led_get_mode(0), LED_MODE_ON);
    CHECK_EQ(led_is_on(0), 1);
    CHECK_EQ(led_get_mode(LED_COUNT - 1u), LED_MODE_OFF);
    CHECK_EQ(led_effect_active(), 0);

    /* Некорректные значения игнорируются. */
    reset_mock();
    led_start_effect((led_effect_t)99);
    led_start_effect((led_effect_t)(LED_EFFECT_COUNT + 1));
    CHECK_EQ(led_effect_active(), 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
}

static void test_effect_timer(void)
{
    /* led_start_effect_for: remaining = repeats * period; авто-стоп. */
    reset_mock();
    led_start_effect_for(TEST_EFFECT, 5);
    CHECK_EQ(led_effect_active(), 1);
    CHECK_EQ(led_effect_remaining(), 5u * TEST_EFFECT_PERIOD);
    run(5u * TEST_EFFECT_PERIOD + 1u);
    CHECK_EQ(led_effect_active(), 0);
    CHECK_EQ(led_effect_remaining(), 0);
    CHECK_EQ(phys_mask(), 0);

    /* repeats = 0 -> бесконечно (LED_EFFECT_FOREVER). */
    reset_mock();
    led_start_effect_for(TEST_EFFECT, 0);
    CHECK_EQ(led_effect_active(), 1);
    CHECK_EQ(led_effect_remaining(), (long)LED_EFFECT_FOREVER);
    run(1000);
    CHECK_EQ(led_effect_active(), 1);
    led_stop_effect();

#if LED_USE_RUNNING_EFFECTS && LED_COUNT >= 2
    /* Регрессия L2: последний кадр таймированного эффекта живёт полный
     * период (10 тиков), затем эффект останавливается. */
    {
        unsigned i, high_last = 0;
        unsigned last_start = (unsigned)(LED_COUNT - 1u) * 10u;

        reset_mock();
        led_start_effect_for(LED_EFFECT_RUNNING_FWD, 1);
        for (i = 0; i < last_start + 11u; i++) {
            led_process();
            if (i >= last_start && i < last_start + 10u &&
                phys_mask() == (uint8_t)(1u << (LED_COUNT - 1u))) {
                high_last++;
            }
        }
        CHECK_EQ(high_last, 10);   /* было 9 до исправления */
        CHECK_EQ(led_effect_active(), 0);
    }
#endif

    /* Регрессия H2: таймер тикает, даже если все диоды «оторваны». */
    reset_mock();
    led_start_effect_for(TEST_EFFECT, 2);
    {
        uint8_t k;

        for (k = 0; k < LED_COUNT; k++) {
            led_on(k);
        }
    }
    run(2u * TEST_EFFECT_PERIOD + 100u);
    CHECK_EQ(led_effect_active(), 0);
    CHECK_EQ(led_effect_remaining(), 0);

    /* led_restart_effect_for() перезапускает даже активный эффект. */
    reset_mock();
    led_start_effect_for(TEST_EFFECT, 100);
    run(23);
    CHECK_EQ(led_effect_remaining(), 100u * TEST_EFFECT_PERIOD - 23u);
    led_restart_effect_for(TEST_EFFECT, 100);
    CHECK_EQ(led_effect_remaining(), 100u * TEST_EFFECT_PERIOD);

    /* Инициализация делает эффект неактивным и сбрасывает его запросы. */
    led_init();
    CHECK_EQ(led_effect_active(), 0);
    CHECK_EQ(led_effect_remaining(), 0);
}
#endif /* TEST_EFFECT != LED_EFFECT_NONE */

static void test_flash_and_fade(void)
{
    unsigned i, t_off[LED_COUNT];

    reset_mock();
    for (i = 0; i < LED_COUNT; i++) {
        t_off[i] = 0;
    }
    led_flash_and_fade((uint8_t)LED_COUNT);
    for (i = 0; i < 200; i++) {
        unsigned k;

        led_process();
        for (k = 0; k < LED_COUNT; k++) {
            if (t_off[k] == 0 && phys_level[k] == 0u) {
                t_off[k] = i + 1u;
            }
        }
    }
    /* Диоды гаснут в обратном порядке (LED0 последним) с шагом 10 тиков:
     * LED[k] гаснет на 60 + (LED_COUNT-1-k)*10 тике. */
    {
        unsigned k;

        for (k = 0; k < LED_COUNT; k++) {
            CHECK_EQ(t_off[k], 60u + ((unsigned)LED_COUNT - 1u - k) * 10u);
        }
    }
    /* Некорректные значения игнорируются. */
    led_flash_and_fade(0);
    led_flash_and_fade((uint8_t)(LED_COUNT + 1));
}

#endif /* LED_USE_EFFECTS */

/* ------------------------------------------------------------------ */
/*  R5: упакованные команды                                           */
/* ------------------------------------------------------------------ */

#if LED_USE_PACKED_COMMANDS

/*
 * Статические проверки формата слова (та же гарантия, что в led.c).
 * При компиляции как C11 используется _Static_assert, при строгом C99 —
 * эквивалентный приём с массивом отрицательного размера (проверка
 * выполняется на этапе компиляции в обоих случаях).
 */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#  define T_SA(cond, tag) _Static_assert(cond, #tag)
#else
#  define T_SA(cond, tag) typedef char t_static_assert_##tag[(cond) ? 1 : -1]
#endif
T_SA(LED_CMD(0xFu, 0xFu, 1032192u, 1032192u, 7u, 7u) == 0xFFFFFFFFu, full_word);
T_SA(LED_CMD_DEV(0x04780F31u)  == 1u,  golden_dev);
T_SA(LED_CMD_COUNT(0x04780F31u) == 3u, golden_count);
T_SA(LED_CMD_T0F(0x04780F31u)   == 15u, golden_t0);
T_SA(LED_CMD_T1F(0x04780F31u)   == 60u, golden_t1);
T_SA(LED_CMD_T0(0x04780F31u)    == 15u, golden_t0_ticks);
T_SA(LED_CMD_T1(0x04780F31u)    == 60u, golden_t1_ticks);
T_SA(LED_CMD_LVL0(0x04780F31u)  == 1u,  golden_l0);
T_SA(LED_CMD_LVL1(0x04780F31u)  == 0u,  golden_l1);
T_SA(0u == LED_SYS_STOP, zero_is_stop);
T_SA(LED_CMD_IS_SYS(LED_SYS_REP_START) && !LED_CMD_IS_SYS(0x20u), sys_test);

/** Собрать эталонную строку: 3 × (15 '#' + 60 '.') + хвост. */
static void expected_err(char *out, unsigned rep_on, unsigned rep_off,
                         unsigned tail)
{
    unsigned t = 0, k, i;

    for (i = 0; i < 3u; i++) {
        for (k = 0; k < rep_on; k++) {
            out[t++] = '#';
        }
        for (k = 0; k < rep_off; k++) {
            out[t++] = '.';
        }
    }
    for (k = 0; k < tail; k++) {
        out[t++] = '.';
    }
    out[t] = '\0';
}

static void test_packed_format(void)
{
    /* Round-trip: закодированное поле -> тики (значения, представимые
     * точно: win * 4^shift при win = 1..63). */
    static const uint32_t exact[] = { 1u, 2u, 63u, 64u, 100u, 252u,
                                      1008u, 4032u, 16128u, 64512u, 258048u,
                                      1032192u };
    unsigned i;

    for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        uint32_t w = LED_CMD(1u, 1u, exact[i], exact[i], 1u, 1u);

        CHECK_EQ(LED_CMD_T0(w), exact[i]);
        CHECK_EQ(LED_CMD_T1(w), exact[i]);
        CHECK_EQ(LED_CMD_T0S(w), LED_CMD_T1S(w));
        CHECK_EQ(LED_CMD_T0W(w), LED_CMD_T1W(w));
    }

    /* Вне диапазона — clamp к максимуму; 0 — «фаза без ожидания». */
    CHECK_EQ(LED_CMD_T0(LED_CMD(1u, 1u, 5000000u, 0u, 1u, 1u)), 1032192u);
    CHECK_EQ(LED_CMD_T0(LED_CMD(1u, 1u, UINT32_MAX, 0u, 1u, 1u)), 1032192u);
    CHECK_EQ(LED_CMD_T0(LED_CMD(1u, 1u, 0u, 0u, 1u, 1u)), 0);
    CHECK_EQ(LED_CMD_T1(LED_CMD(1u, 1u, 1u, 0u, 1u, 1u)), 0);

    /* Граница точности: 6-битная мантисса при шаге 4^shift даёт
     * относительную погрешность не хуже 1/32 (round-to-nearest; это
     * теоретический предел для данного формата). На 1 тик снизу
     * ошибки нет вовсе, значения до 63 тиков кодируются точно. */
    for (i = 1; i < 5000u; i++) {
        uint32_t enc = LED_CMD_T0(LED_CMD(1u, 1u, i, 0u, 1u, 1u));
        unsigned diff = (unsigned)((enc > i) ? (enc - i) : (i - enc));

        if (diff * 32u > i) {
            printf("  encoding error at %u ticks: %lu (+/- %u)\n",
                   i, (unsigned long)enc, diff);
            CHECK(0);
            break;
        }
    }
    for (i = 1; i < 64u; i++) {
        CHECK_EQ(LED_CMD_T0(LED_CMD(1u, 1u, i, 0u, 1u, 1u)), i);
    }
}

static void test_packed_timing(void)
{
    char got[300], want[300];
    unsigned i;

    expected_err(want, 15u, 60u, 15u);

    reset_mock();
    led_packed_run(pk_err, LED_ARRAY_LEN(pk_err));
    CHECK_EQ(led_pattern_active(), 1);
    /* Первый тик фазы 0 отдан сразу, до него можно проспать t0 тиков. */
    CHECK_EQ(led_ticks_until_next(), 15);
    for (i = 0; i < 240u; i++) {
        got[i] = (phys_level[0] > 0u) ? '#' : '.';
        led_advance(1u);
    }
    got[240] = '\0';
    CHECK(strcmp(got, want) == 0);
    /* 3 × 75 = 225 тиков паттерна, дальше — остановка и тишина. */
    CHECK_EQ(led_pattern_active(), 0);
    CHECK_EQ(led_ticks_until_next(), (long)LED_TICKS_FOREVER);
    CHECK_EQ(led_get_level(0), 0);

    /* То же через led_packed_once() — одиночная команда без массива. */
    reset_mock();
    led_packed_once(LED_CMD(1u, 2u, 3u, 4u, 5u, 0u));
    CHECK_EQ(led_get_level(0), 5);
    CHECK_EQ(led_ticks_until_next(), 3);
    run(3);
    CHECK_EQ(led_get_level(0), 0);
    run(4);
    CHECK_EQ(led_get_level(0), 5);
    run(3);
    CHECK_EQ(led_get_level(0), 0);
    run(4);
    CHECK_EQ(led_pattern_active(), 0);
    CHECK_EQ(led_get_level(0), 0);
}

/** Сколько раз «пара фаз» отработала: считаем фронты 0 -> level0. */
static unsigned packed_pairs(uint32_t cmd)
{
    unsigned fronts = 0, t;
    uint8_t prev;   /* до старта паттерна выход был погашен */

    reset_mock();
    led_packed_run(&cmd, 1u);
    prev = 0u;
    for (t = 0; t < 400u && led_pattern_active() != 0u; t++) {
        led_advance(1u);
        if (prev == 0u && phys_level[0] > 0u) {
            fronts++;
        }
        prev = phys_level[0];
    }
    return fronts;
}

/** То же, но для массива: n тиков, подсчёт фронтов 0 -> «горит». */
static unsigned packed_fronts(const uint32_t *cmds, unsigned n_cmds,
                              unsigned ticks)
{
    unsigned fronts = 0, t;
    uint8_t prev;

    reset_mock();
    led_packed_run(cmds, (uint16_t)n_cmds);
    prev = 0u;
    for (t = 0; t < ticks; t++) {
        led_advance(1u);
        if (prev == 0u && phys_level[0] > 0u) {
            fronts++;
        }
        prev = phys_level[0];
    }
    return fronts;
}

static void test_packed_sys_commands(void)
{
    /* count = 1 -> одна пара фаз. */
    CHECK_EQ(packed_pairs(pk_cmd[0]), 1u);
    /* count = 15 -> бесконечно (считаем 400 тиков — 16+ пар). */
    CHECK(packed_pairs(LED_CMD(1u, 15u, 1u, 1u, 1u, 0u)) > 10u);

    /* Явный STOP на границе массива. */
    reset_mock();
    led_packed_run(pk_stop, LED_ARRAY_LEN(pk_stop));
    run(50);
    CHECK_EQ(led_pattern_active(), 0);

    /* REP_AT(2) -> команда выполняется 1 + 2 = 3 раза. */
    CHECK_EQ(packed_fronts(pk_rep2, LED_ARRAY_LEN(pk_rep2), 200u), 3u);
    CHECK_EQ(led_pattern_active(), 0);

    /* REP_START -> бесконечно, останавливается только led_packed_stop(). */
    reset_mock();
    led_packed_run(pk_inf, LED_ARRAY_LEN(pk_inf));
    run(100);
    CHECK_EQ(led_pattern_active(), 1);
    CHECK(packed_fronts(pk_inf, LED_ARRAY_LEN(pk_inf), 100u) > 5u);
    led_packed_stop();
    CHECK_EQ(led_pattern_active(), 0);
    CHECK_EQ(led_get_level(0), 0);
    run(50);
    CHECK_EQ(led_pattern_active(), 0);

    /* Нет явной границы -> STOP по умолчанию после двух команд:
     * команда 1 (count=2) даёт 2 фронта, команда 2 (count=1) — ещё 1. */
    CHECK_EQ(packed_fronts(pk_two, LED_ARRAY_LEN(pk_two), 100u), 3u);
    CHECK_EQ(led_pattern_active(), 0);

    /* Некорректные аргументы игнорируются. */
    led_packed_run(NULL, 4u);
    led_packed_run(pk_cmd, 0u);
    CHECK_EQ(led_pattern_active(), 0);

    /* Уже запущенный паттерн перезапускается, каналы гасятся. */
    reset_mock();
    led_packed_run(pk_inf, LED_ARRAY_LEN(pk_inf));
    run(2);
    CHECK(phys_level[0] > 0u);
    led_packed_run(pk_cmd, LED_ARRAY_LEN(pk_cmd));
    CHECK_EQ(led_get_level(0), 2);
}

static void test_packed_guard(void)
{
    /* Таблица из одних системных команд: guard обязан погасить её сразу,
     * иначе бесконечный цикл внутри одного led_advance(). */
    reset_mock();
    led_packed_run(pk_sys, LED_ARRAY_LEN(pk_sys));
    CHECK_EQ(led_pattern_active(), 0);

    /* Вырожденные фазы (t0 = t1 = 0) + REP_START: паттерн живёт, но
     * advance() не зависает и не «пропускает» переходы — уровень
     * чередуется каждый тик. */
    reset_mock();
    led_packed_run(pk_spin, LED_ARRAY_LEN(pk_spin));
    CHECK_EQ(led_pattern_active(), 1);
    {
        unsigned t;
        int alternates = 1;

        for (t = 0; t < 50u; t++) {
            uint8_t before = phys_level[0];

            led_advance(1u);
            if (phys_level[0] == before) {
                alternates = 0;
            }
        }
        CHECK(alternates);
    }
    led_packed_stop();
    CHECK_EQ(led_pattern_active(), 0);
}

static void test_packed_device_mask(void)
{
    /* Старшие биты маски сверх LED_COUNT отсекаются, а не ломают выводы. */
    uint32_t cmd = LED_CMD(0xFu, 1u, 2u, 2u, 1u, 0u);

    reset_mock();
    led_packed_once(cmd);
    CHECK_EQ(led_get_mode(0), LED_MODE_PACKED);  /* канал забирается */
    CHECK_EQ(led_get_level(0), 1);                 /* уровень фазы 0 */
    run(2);
    CHECK_EQ(led_get_level(0), 0);                 /* конец фазы 0 -> t1 */
    run(2);
    CHECK_EQ(led_pattern_active(), 0);
    led_packed_stop();
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    run(10);
    CHECK_EQ(led_get_level(0), 0);

#if LED_COUNT > 1
    /* Каналы вне маски сохраняют свои паттерны. */
    {
        uint8_t  last  = (uint8_t)(LED_COUNT - 1u);
        uint8_t  mask  = (uint8_t)((1u << (LED_COUNT - 1u)) - 1u);
        uint32_t m_cmd = LED_CMD((uint32_t)mask, 1u, 2u, 2u, 1u, 0u);

        CHECK(mask != 0u);
        reset_mock();
        led_blink(last, 2u, 2u);
        led_packed_run(&m_cmd, 1u);
        CHECK_EQ(led_get_mode(0), LED_MODE_PACKED);
        CHECK_EQ(led_get_mode(last), LED_MODE_BLINK);
        CHECK_EQ(led_get_level(last), 1);          /* свой паттерн жив */
        led_packed_stop();
        CHECK_EQ(led_get_mode(last), LED_MODE_BLINK);
        CHECK_EQ(led_pattern_active(), 1);         /* blink продолжается */
    }
#endif

    /* Per-LED вызов «оторвает» канал от паттерна: пакетное продвижение
     * больше не возвращает ему уровень 0 (регрессия). */
    reset_mock();
    led_packed_run(pk_cmd, LED_ARRAY_LEN(pk_cmd));
    run(4);
    CHECK_EQ(led_get_level(0), 0);                /* конец фазы 0 */
    led_blink(0, 2u, 2u);                          /* отрыв */
    CHECK_EQ(led_get_mode(0), LED_MODE_BLINK);
    led_advance(2u);
    CHECK_EQ(led_get_level(0), 1);                /* свой паттерн жив */
    CHECK_EQ(led_get_mode(0), LED_MODE_BLINK);
    led_advance(100u);
    CHECK_EQ(led_get_level(0), 1);                /* и не гасится пакетом */
    CHECK_EQ(led_get_mode(0), LED_MODE_BLINK);
}

#endif /* LED_USE_PACKED_COMMANDS */

/* ------------------------------------------------------------------ */
/*  R3: владение индикатором                                          */
/* ------------------------------------------------------------------ */

static void test_owner(void)
{
    /* Пусто из коробки. */
    reset_mock();
    CHECK_EQ(led_claim(), LED_OWNER_NONE);
    CHECK_EQ(led_stop_owner(LED_OWNER_BEACON), 0);
    CHECK_EQ(led_stop_owner(LED_OWNER_NONE), 0);

    /* Захват и снятие. */
    led_set_owner(LED_OWNER_BEACON);
    CHECK_EQ(led_claim(), LED_OWNER_BEACON);
    led_release(LED_OWNER_WARNING);            /* не мой — игнор */
    CHECK_EQ(led_claim(), LED_OWNER_BEACON);
    led_release(LED_OWNER_BEACON);
    CHECK_EQ(led_claim(), LED_OWNER_NONE);

    /* Владение без паттерна снимается ближайшим advance(): индикатор
     * свободен, пока не идёт ни один таймерный паттерн. */
    led_set_owner(LED_OWNER_STROBE);
    led_advance(10u);
    CHECK_EQ(led_claim(), LED_OWNER_NONE);

    /* Арбитраж: «это моё?» проверяется до запуска. */
    reset_mock();
    led_set_owner(LED_OWNER_BEACON);
    led_on_for(0, 4u);
    CHECK_EQ(led_claim(), LED_OWNER_BEACON);
    CHECK_EQ(led_pattern_active(), 1);

    /* После завершения паттерна владение снимается само. */
    led_advance(5u);
    CHECK_EQ(led_pattern_active(), 0);
    CHECK_EQ(led_claim(), LED_OWNER_NONE);

    /* led_stop_owner() останавливает только «своё» и снимает владение. */
    reset_mock();
    led_set_owner(LED_OWNER_WARNING);
    led_on_for(0, 100u);
    CHECK_EQ(led_stop_owner(LED_OWNER_STROBE), 0);   /* не мой */
    CHECK_EQ(led_claim(), LED_OWNER_WARNING);
    CHECK_EQ(led_stop_owner(LED_OWNER_WARNING), 1);  /* мой */
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    CHECK_EQ(led_get_level(0), 0);
    CHECK_EQ(led_claim(), LED_OWNER_NONE);
    CHECK_EQ(led_pattern_active(), 0);

    /* Статичные режимы владение не отменяет: их задал хост вручную. */
    reset_mock();
    led_set_level(0, 4u);
    led_set_owner(LED_OWNER_UI);
    CHECK_EQ(led_stop_owner(LED_OWNER_UI), 1);
    CHECK_EQ(led_get_level(0), 4);
    CHECK_EQ(led_get_mode(0), LED_MODE_ON);

    /* led_init() снимает владение. */
    led_set_owner(LED_OWNER_UI);
    led_init();
    CHECK_EQ(led_claim(), LED_OWNER_NONE);
    led_set_callback(mock_gpio);

#if LED_USE_EFFECTS
    /* Остановка чужого эффекта. */
    reset_mock();
    led_set_owner(LED_OWNER_STROBE);
#  if TEST_EFFECT != LED_EFFECT_NONE
    led_start_effect(TEST_EFFECT);
    CHECK_EQ(led_stop_owner(LED_OWNER_STROBE), 1);
    CHECK_EQ(led_effect_active(), 0);
    CHECK_EQ(led_claim(), LED_OWNER_NONE);
#  endif
#endif
#if LED_USE_PACKED_COMMANDS
    /* Остановка чужого упакованного паттерна. */
    reset_mock();
    led_set_owner(LED_OWNER_BEACON);
    led_packed_run(pk_inf, LED_ARRAY_LEN(pk_inf));
    CHECK_EQ(led_stop_owner(LED_OWNER_BEACON), 1);
    CHECK_EQ(led_pattern_active(), 0);
    CHECK_EQ(led_get_level(0), 0);
#endif
}

/* ------------------------------------------------------------------ */
/*  R6: события                                                        */
/* ------------------------------------------------------------------ */

#if LED_USE_EVENTS
static unsigned ev_cycle, ev_end, ev_state;
static uint32_t ev_arg;
static uint8_t  ev_id, ev_cycle_id, ev_end_id;

static void mock_event(uint8_t id, led_event_t ev, uint32_t arg)
{
    ev_id  = id;
    ev_arg = arg;
    if (ev == LED_EV_CYCLE_START) {
        ev_cycle++;
        ev_cycle_id = id;
    } else if (ev == LED_EV_PATTERN_END) {
        ev_end++;
        ev_end_id = id;
    } else if (id == 0) {
        /* События уровня считаем только для канала 0: первый advance
         * «дописал» бы состояния остальных каналов (led_init сбросил
         * кэш выводов), и они не относятся к проверяемому паттерну. */
        ev_state++;
    }
}

static void ev_reset(void)
{
    ev_cycle = 0;
    ev_end   = 0;
    ev_state = 0;
    ev_arg   = 0;
    ev_id    = 0;
    ev_cycle_id = 0;
    ev_end_id = 0;
    led_set_event_callback(mock_event);
}

static void test_events(void)
{
    /* ON_FOR: старт цикла и завершение паттерна. */
    reset_mock();
    ev_reset();
    led_on_for(0, 3u);
    CHECK_EQ(ev_cycle, 1);
    CHECK_EQ(ev_id, 0);
    led_advance(3u);
    CHECK_EQ(ev_end, 1);
    CHECK_EQ(ev_state, 2);      /* вкл на 1-м тике, выкл на 3-м */

    /* BLINK: CYCLE_START на каждом круге (3 тика + 4 тика = 7). */
    reset_mock();
    ev_reset();
    led_blink(0, 3u, 4u);
    CHECK_EQ(ev_cycle, 1);
    run(14);
    CHECK_EQ(ev_cycle, 3);      /* старт + 2 круга */
    led_blink_level(0, 2u, 3u, 0u, 4u);
    CHECK_EQ(ev_cycle, 4);

#if LED_USE_SEQUENCES
    /* SEQUENCE: старт + каждый виток цикла. */
    reset_mock();
    ev_reset();
    led_play(0, &seq_loop);     /* период 6 тиков */
    CHECK_EQ(ev_cycle, 1);
    run(12);
    CHECK_EQ(ev_cycle, 3);
    run(100);
    CHECK_EQ(ev_end, 0);        /* зациклено: не завершается */
    led_play(0, &seq_once);
    run(10);
    CHECK_EQ(ev_end, 1);
#endif

#if LED_USE_PACKED_COMMANDS
    /* Упакованные команды: CYCLE_START на каждую загруженную команду,
     * PATTERN_END по завершении. */
    reset_mock();
    ev_reset();
    led_packed_run(pk_stop, LED_ARRAY_LEN(pk_stop));
    CHECK_EQ(ev_cycle, 1);
    run(30);
    CHECK_EQ(ev_end, 1);
    CHECK_EQ(ev_id, 0);

    /* Один сохранённый ID — на старте и завершении packed-паттерна. */
    {
        uint8_t id = (LED_COUNT > 1u) ? 1u : 0u;
        uint8_t dev = (uint8_t)(1u << id);
        uint32_t cmd;

#if LED_COUNT >= 4
        dev = (uint8_t)(dev | 0x08u);
#endif
        cmd = LED_CMD(dev, 1u, 10u, 10u, 1u, 0u);
        reset_mock();
        ev_reset();
        led_set_owner(LED_OWNER_BEACON);
        led_packed_run(&cmd, 1u);
        CHECK_EQ(ev_cycle_id, id);
        led_set_level(id, 0u);
#if LED_COUNT >= 4
        led_set_level(3u, 0u);
#endif
        CHECK_EQ(led_stop_owner(LED_OWNER_BEACON), 1);
        CHECK_EQ(ev_end, 1);
        CHECK_EQ(ev_end_id, id);
    }
#endif

#if LED_USE_EFFECTS && (TEST_EFFECT != LED_EFFECT_NONE)
    reset_mock();
    ev_reset();
    led_start_effect(TEST_EFFECT);
    CHECK_EQ(ev_cycle, 1);
    led_stop_effect();
    CHECK_EQ(ev_end, 1);
#endif

    /* Уровень приходит в arg события состояния. */
    reset_mock();
    ev_reset();
    ev_state = 0;
    led_set_level(0, 6u);
    CHECK_EQ(ev_state, 1);
    CHECK_EQ(ev_arg, 6u);
    CHECK_EQ(ev_id, 0);

    /* Без аппаратного callback событие состояния не отправляется. */
    reset_mock();
    ev_reset();
    led_set_callback(NULL);
    led_set_level(0, 6u);
    CHECK_EQ(ev_state, 0);

    /* Refresh повторно пишет уровень и повторно сообщает событие. */
    reset_mock();
    ev_reset();
    led_set_callback(mock_gpio);
    led_set_level(0, 6u);
    CHECK_EQ(ev_state, 1);
    led_refresh();
    led_advance(0u);
    CHECK_EQ(ev_state, 2);

    /* Снятие callback'а. */
    led_set_event_callback(NULL);
    ev_state = 0;
    led_set_level(0, 2u);
    CHECK_EQ(ev_state, 0);
}
#endif /* LED_USE_EVENTS */

/* ------------------------------------------------------------------ */
/*  Тесты: инфраструктура                                              */
/* ------------------------------------------------------------------ */

static uint8_t  phys2;
static unsigned calls2;

static void mock_gpio2(uint8_t id, uint8_t level)
{
    (void)id;
    if (level > phys2) {
        phys2 = level;
    }
    calls2++;
}

static void test_gpio_cache_and_refresh(void)
{
    unsigned baseline;

    /* Кэш: без изменений - без вызовов callback. */
    reset_mock();
    led_on(0);
    run(50);
    CHECK(gpio_calls >= 1);
    baseline = gpio_calls;
    run(50);
    CHECK_EQ(gpio_calls, baseline);   /* стабильный ON - ноль вызовов */

    /* Смена уровня — ровно один вызов. */
    led_set_level(0, 5u);
    CHECK_EQ(gpio_calls, baseline + 1u);

    /* led_refresh() заставляет переписать все выводы (регрессия M7). */
    for (log_len = 0; log_len < LED_COUNT; log_len++) {
        phys_level[log_len] = 0;
    }
    log_len = 0;
    led_refresh();
    run(1);
    CHECK_EQ(phys_level[0], 5);

    /* Регрессия M6: новый callback получает полную картину состояния. */
    led_set_callback(NULL);
    phys2 = 0;
    calls2 = 0;
    led_set_callback(mock_gpio2);
    run(1);
    CHECK(calls2 > 0);
    CHECK(phys2 > 0);
    led_set_callback(mock_gpio);
}

static void test_reinit_keeps_callback(void)
{
    /* led_init() в 2.0+ сохраняет callback и инвалидирует кэш:
     * первый тик гасит все выводы. */
    reset_mock();
    led_on(0);
    led_on((uint8_t)(LED_COUNT - 1u));
    run(2);
    CHECK_EQ(phys_mask(), (unsigned)(0x01u | (1u << (LED_COUNT - 1u))));

    led_init();          /* callback сохранён */
    run(1);
    CHECK_EQ(phys_mask(), 0);   /* все диоды погашены */
    led_on((uint8_t)(LED_COUNT > 1 ? 1u : 0u));
    run(1);
    CHECK_EQ(phys_mask(), (unsigned)(1u << (LED_COUNT > 1 ? 1u : 0u)));
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("LED_LIB v%d.%d.%d host tests (LED_COUNT=%d, packed=%d, events=%d)\n",
           LED_LIB_VERSION_MAJOR, LED_LIB_VERSION_MINOR, LED_LIB_VERSION_PATCH,
           LED_COUNT, LED_USE_PACKED_COMMANDS, LED_USE_EVENTS);

    test_init_and_queries();
    test_on_off_toggle();
    test_on_for_timing();
    test_blink();
#if LED_COUNT >= 4
    test_independent_rates();
#endif
    test_levels();
#if LED_USE_SEQUENCES
    test_sequence();
    test_sequence_levels();
    test_leading_off_step();
#endif
    test_advance_invariant();
    test_advance_zero();
    test_advance_long();
    test_ticks_until_next();
    test_sleep_wake_sequence();
#if LED_USE_EFFECTS
#if LED_USE_RUNNING_EFFECTS
    test_running_effects();
#endif
#if LED_USE_PINGPONG_EFFECT
    test_pingpong();
#endif
#if LED_USE_BLINK_EFFECTS
    test_blink_effects();
#endif
#if LED_USE_MULTI_BLINK_EFFECTS
    test_multi_blink();
#endif
#if LED_USE_HOLD_EFFECTS
    test_hold_effects();
#endif
#if LED_USE_RANDOM_EFFECT
    test_random_reset();
#endif
#if TEST_EFFECT != LED_EFFECT_NONE
    test_effect_start_stop();
    test_effect_timer();
#endif
    test_flash_and_fade();
#endif
#if LED_USE_PACKED_COMMANDS
    test_packed_format();
    test_packed_timing();
    test_packed_sys_commands();
    test_packed_guard();
    test_packed_device_mask();
#endif
    test_owner();
#if LED_USE_EVENTS
    test_events();
#endif
    test_gpio_cache_and_refresh();
    test_reinit_keeps_callback();

    printf("checks: %d, failed: %d -> %s\n",
           checks_total, checks_failed, checks_failed ? "FAIL" : "PASS");
    return checks_failed ? 1 : 0;
}
