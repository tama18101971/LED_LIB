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
/*  Мок GPIO                                                           */
/* ------------------------------------------------------------------ */

static unsigned char phys;      /* физическое состояние выводов */
static unsigned      gpio_calls;

static void mock_gpio(uint8_t id, bool state)
{
    uint8_t bit = (uint8_t)(1u << id);

    gpio_calls++;
    phys = state ? (uint8_t)(phys | bit) : (uint8_t)(phys & (uint8_t)~bit);
}

static void reset_mock(void)
{
    led_init();
    led_set_callback(mock_gpio);
    phys = 0;
    gpio_calls = 0;
}

static void run(unsigned n)
{
    while (n--) {
        led_process();
    }
}

/* ------------------------------------------------------------------ */
/*  Тесты: ручные режимы                                               */
/* ------------------------------------------------------------------ */

static void test_init_and_queries(void)
{
    reset_mock();
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    CHECK_EQ(led_is_on(0), 0);
    CHECK_EQ(led_any_led_active(), 0);
    led_on(0);
    CHECK_EQ(led_get_mode(0), LED_MODE_ON);
    CHECK_EQ(led_is_on(0), 1);
    CHECK_EQ(led_any_led_active(), 0);   /* ON — статичный режим */
    led_off(0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);
    /* Вне диапазона — безопасно. */
    led_on((uint8_t)LED_COUNT);
    led_off((uint8_t)LED_COUNT);
    led_toggle((uint8_t)LED_COUNT);
    led_on_for((uint8_t)LED_COUNT, 5);
    led_blink((uint8_t)LED_COUNT, 1, 1);
    CHECK_EQ(led_get_mode((uint8_t)LED_COUNT), LED_MODE_OFF);
    CHECK_EQ(led_is_on((uint8_t)LED_COUNT), 0);
}

static void test_on_off_toggle(void)
{
    uint8_t id1 = (LED_COUNT > 1) ? 1u : 0u;

    /* led_toggle работает по фактическому состоянию вывода. */
    reset_mock();
    led_on_for(0, 100);
    run(1);
    CHECK_EQ(led_is_on(0), 1);
    led_toggle(0);                       /* горит -> погаснет */
    run(200);
    CHECK_EQ(led_is_on(0), 0);
    CHECK_EQ(phys, 0);
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
        if (phys & 1u) {
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
        if (phys & 1u) {
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

#if LED_USE_SEQUENCES
static void test_sequence(void)
{
    static const led_step_t steps[] = { LED_STEP_ON(2), LED_STEP_OFF(3) };
    static const led_sequence_t once = { steps, 2, false };
    static const led_sequence_t loop = { steps, 2, true };
    unsigned i;
    char buf[21];

    /* Однократно: ON 2 тика, OFF 3, затем STOP. */
    reset_mock();
    led_play(0, &once);
    for (i = 0; i < 12; i++) {
        buf[i] = (phys & 1u) ? '#' : '.';
        led_process();
    }
    buf[12] = '\0';
    CHECK(strcmp(buf, "##..........") == 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_OFF);

    /* Зациклено: период 5 тиков. */
    reset_mock();
    led_play(0, &loop);
    for (i = 0; i < 20; i++) {
        buf[i] = (phys & 1u) ? '#' : '.';
        led_process();
    }
    buf[20] = '\0';
    CHECK(strcmp(buf, "##...##...##...##...") == 0);
    CHECK_EQ(led_get_mode(0), LED_MODE_SEQUENCE);

    /* Шаг с ticks == 0 длится 1 тик; зависаний нет. */
    {
        static const led_step_t zs[] = {
            LED_STEP_ON(0), LED_STEP_OFF(0), LED_STEP_ON(3)
        };
        static const led_sequence_t zseq = { zs, 3, false };

        reset_mock();
        led_play(0, &zseq);
        for (i = 0; i < 12; i++) {
            buf[i] = (phys & 1u) ? '#' : '.';
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
#endif

/* ------------------------------------------------------------------ */
/*  Тесты: эффекты                                                     */
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
        stream[i] = phys;
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
        a[i] = phys;
    }

    reset_mock();   /* led_init() сбрасывает PRNG (регрессия M9) */
    led_start_effect(LED_EFFECT_RANDOM_FLASH);
    for (i = 0; i < 8; i++) {
        led_process();
        b[i] = phys;
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
            a[i] = phys;
        }

        reset_mock();
        for (i = 0; i < 120; i++) {
            led_start_effect(TEST_EFFECT);   /* опросный вызов */
            led_process();
            b[i] = phys;
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
    CHECK_EQ(phys, 0);

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
                phys == (uint8_t)(1u << (LED_COUNT - 1u))) {
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
}
#endif /* TEST_EFFECT != LED_EFFECT_NONE */

#if LED_USE_EFFECTS
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
            if (t_off[k] == 0 && ((phys & (uint8_t)(1u << k)) == 0)) {
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
#endif

#endif /* LED_USE_EFFECTS */

/* ------------------------------------------------------------------ */
/*  Тесты: инфраструктура                                              */
/* ------------------------------------------------------------------ */

static unsigned char phys2;
static unsigned      calls2;

static void mock_gpio2(uint8_t id, bool state)
{
    (void)id;
    (void)state;
    calls2++;
    phys2 = 1;
}

static void test_gpio_cache_and_refresh(void)
{
    /* Кэш: без изменений - без вызовов callback. */
    reset_mock();
    led_on(0);
    run(50);
    CHECK(gpio_calls >= 1);
    {
        unsigned baseline = gpio_calls;

        run(50);
        CHECK_EQ(gpio_calls, baseline);   /* стабильный ON - ноль вызовов */
    }

    /* led_refresh() заставляет переписать все выводы (регрессия M7). */
    phys = 0;   /* вывод «сбили» снаружи */
    led_refresh();
    run(1);
    CHECK_EQ(phys, 0x01);

    /* Регрессия M6: новый callback получает полную картину состояния. */
    led_set_callback(NULL);
    phys2 = 0;
    calls2 = 0;
    led_set_callback(mock_gpio2);
    run(1);
    CHECK(calls2 > 0);
    CHECK_EQ(phys2, 1);
}

static void test_reinit_keeps_callback(void)
{
    /* led_init() в 2.0 сохраняет callback и инвалидирует кэш:
     * первый тик гасит все выводы. */
    reset_mock();
    led_on(0);
    led_on((uint8_t)(LED_COUNT - 1u));
    run(2);
    CHECK_EQ(phys, (unsigned)(0x01u | (1u << (LED_COUNT - 1u))));

    led_init();          /* callback сохранён */
    run(1);
    CHECK_EQ(phys, 0);   /* все диоды погашены */
    led_on((uint8_t)(LED_COUNT > 1 ? 1u : 0u));
    run(1);
    CHECK_EQ(phys, (unsigned)(1u << (LED_COUNT > 1 ? 1u : 0u)));
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("LED_LIB v%d.%d.%d host tests (LED_COUNT=%d)\n",
           LED_LIB_VERSION_MAJOR, LED_LIB_VERSION_MINOR, LED_LIB_VERSION_PATCH,
           LED_COUNT);

    test_init_and_queries();
    test_on_off_toggle();
    test_on_for_timing();
    test_blink();
#if LED_USE_SEQUENCES
    test_sequence();
#endif
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
    test_gpio_cache_and_refresh();
    test_reinit_keeps_callback();

    printf("checks: %d, failed: %d -> %s\n",
           checks_total, checks_failed, checks_failed ? "FAIL" : "PASS");
    return checks_failed ? 1 : 0;
}
