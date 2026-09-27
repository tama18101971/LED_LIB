/**
 * @file main.c
 * @brief Демонстрация библиотеки LED v3.0 на CH32V003 (WCH NoneOS SDK).
 *
 * ======================================================================
 *  АППАРАТНАЯ КОНФИГУРАЦИЯ
 * ======================================================================
 *
 *   МК:        CH32V003F4P6 (RISC-V 48 МГц, 16 КБ Flash, 2 КБ RAM)
 *   Диоды:     PC4 (LED0), PC5 (LED1), PC6 (LED2), PC7 (LED3)
 *   Уровень:   HIGH (любой level > 0 = «горит», см. led_hw_set)
 *   UART:      USART1, TX = PD5 (вывод по умолчанию WCH SDK), 115200 8N1
 *   Питание:   3.3В
 *
 *   Текст выводится в UART через printf(): WCH SDK переопределяет _write()
 *   на отправку в USART1 (см. Debug/ch32v00x/debug.c).
 *
 * ======================================================================
 *  ЧТО ПРОВЕРЯЕТСЯ НА ЖЕЛЕЗЕ
 * ======================================================================
 *
 *   1) САМОТЕСТ (выполняется один раз при старте, ДО демо-сценария):
 *        - главный инвариант v3: led_advance(1) x N тождественно
 *          led_advance(N) — сравнивается ПОЛНАЯ последовательность вызовов
 *          callback, а не только конечное состояние;
 *        - led_ticks_until_next() никогда не возвращает 0;
 *        - семантика владения: владение без паттерна снимается ближайшим
 *          advance; led_stop_owner() гасит ТОЛЬКО паттерны владельца.
 *      Трассировка callback'а пишется в RAM (2 x 192 байта) и сравнивается
 *      на самом чипе — это проверка настоящей прошивки, а не хоста.
 *
 *   2) ДЕМО-СЦЕНАРИЙ (все программы работают ПАРАЛЛЕЛЬНО), плюс
 *      диагностическая строка состояния раз в секунду.
 *
 * ======================================================================
 *  ДЕМО-СЦЕНАРИЙ (все программы работают ПАРАЛЛЕЛЬНО)
 * ======================================================================
 *
 *   t = 0.0 с   LED0: непрерывное мигание 15/15 тиков (240/240 мс)
 *               LED1: SOS-последовательность (зациклена)
 *               LED2+LED3: LED_EFFECT_EMERGENCY, 5 вспышек, авто-стоп
 *
 *   t = 6.0 с   Маяк: подсистема LED_OWNER_BEACON забирает индикатор
 *               (проверка led_claim()), гасит LED0 и запускает
 *               упакованные команды — 3 вспышки уровня 1 по 15 тиков
 *               с паузами 60/420 тиков, повторяется до снятия владения.
 *
 *   t = 12.0 с  Маяк останавливается через led_stop_owner(). ВАЖНО: это
 *               останавливает ВСЕ таймерные паттерны, а не только каналы
 *               маяка — владение в библиотеке глобальное (одна метка на
 *               индикатор), per-pattern владельца не существует, поэтому
 *               «остановить только своё» физически означает «погасить
 *               индикатор» (см. led.h, led_stop_owner). Наблюдаемый
 *               эффект: погас и SOS на LED1 тоже. Чтобы SOS вернулся,
 *               хост должен заново объявить владение и запустить
 *               последовательность — см. комментарий в main().
 *
 *   Порядок важен: led_start_effect_for() вызывается ПЕРЕД per-LED
 *   вызовами — иначе эффект «заберёт» диоды, предназначенные для
 *   мигания и SOS.
 *
 * ======================================================================
 *  ЧАСТОТА ОБНОВЛЕНИЯ
 * ======================================================================
 *
 *   Тик выбирает ХОСТ. Этот пример — опрос ~60 Гц: один тик ≈ 16 мс.
 *   Для «спящего» МК вместо Delay_Ms() программируется аппаратный
 *   таймер пробуждения на led_ticks_until_next() тиков, а после
 *   пробуждения вызывается led_advance(elapsed_ticks) — см. led.h.
 *
 *   ВАЖНО про UART-печать: printf() на 115200 бод занимает ~87 мкс на
 *   символ, то есть строка в ~70 символов съедает ~6 мс из 16 мс тика.
 *   Это НЕ искажает тайминги паттернов: библиотеке всегда передаётся
 *   ровно один led_advance(1) за итерацию, поэтому все длительности
 *   остаются точными В ТИКАХ. Медленнее становится только «стенные
 *   часы» (строка состояния печатается по счётчику тиков, а не по
 *   времени). Если точность стенных часов важна — уменьшите интервал
 *   печати или отключите STATE_PERIOD_TICKS.
 *
 *   Задержка реализована через Delay_Ms() из WCH SDK (блокирующая,
 *   калиброванная по SysTick). Для приложений с жёсткими требованиями
 *   к точности такта замените на аппаратный таймер и определяйте
 *   LED_ENTER_CRITICAL()/LED_EXIT_CRITICAL() (см. led.h).
 */

#include "led.h"
#include "gpio_setup.h"
#include "ch32v00x.h"
#include "debug.h"

/* ================================================================== */
/*  НАСТРОЙКИ СБОРКИ ПРИМЕРА                                         */
/* ================================================================== */

/**
 * DEMO_UART (0 или 1, по умолчанию 1) — диагностический вывод в UART.
 *
 *   1 (по умолчанию): баннер, таблица самотеста, периодическая строка
 *                     состояния и сообщения о событиях сценария.
 *   0: printf не вызывается НИГДЕ, поэтому линкер не подтягивает
 *      vfprintf — и прошивка теряет ~7.5 КБ (на CH32V003 это 70 % -> ~30 %
 *      Flash). Печать — единственный крупный потребитель места в этом
 *      примере; сама библиотека от неё не зависит.
 *
 *   Отключить:
 *     pio run -e ch32v003 --target upload   с -DDEMO_UART=0 в build_flags,
 *     либо разово:  set PLATFORMIO_BUILD_FLAGS="-std=c99 -DDEMO_UART=0"
 *
 * DEMO_SELFTEST (0 или 1, по умолчанию 1) — самотест на целевом МК.
 *
 *   Требует ~0.5 КБ Flash и 384 Б RAM (два буфера трассировки), но НЕ
 *   требует printf: сам тест — только вызовы библиотеки и сравнения.
 *   По умолчанию следует за DEMO_UART — если UART выключен, а самотест
 *   оставлен, результат сообщается миганием LED3 (см. fault_loop), иначе
 *   провал на плате без кабеля остался бы незаметным.
 *   При DEMO_SELFTEST=0 уходит и буферы трассировки, и весь этот блок.
 */
#ifndef DEMO_UART
#define DEMO_UART 1
#endif

#ifndef DEMO_SELFTEST
#define DEMO_SELFTEST 1
#endif

/** Скорость UART для диагностического вывода. */
#define UART_BAUD            115200u

/** Печать строки состояния раз в N тиков (~1 с при 1 тик = 16 мс). */
#define STATE_PERIOD_TICKS   62u

/* ================================================================== */
/*  ПРЕДОПРЕДЕЛЁННЫЕ ПОСЛЕДОВАТЕЛЬНОСТИ                                */
/* ================================================================== */

/**
 * SOS: ... --- ...
 *
 *   . (точка) = 6 тиков  ON (~96 мс)
 *   - (тире)  = 18 тиков ON (~288 мс)
 *   пауза между элементами = 6 тиков, между буквами = 18 тиков,
 *   пауза между повторами = 60 тиков (~1 с)
 */
static const led_step_t sos_steps[] = {
    /* Буква S: ... */
    LED_STEP_ON(6),   LED_STEP_OFF(6),
    LED_STEP_ON(6),   LED_STEP_OFF(6),
    LED_STEP_ON(6),   LED_STEP_OFF(18),

    /* Буква O: --- */
    LED_STEP_ON(18),  LED_STEP_OFF(6),
    LED_STEP_ON(18),  LED_STEP_OFF(6),
    LED_STEP_ON(18),  LED_STEP_OFF(18),

    /* Буква S: ... + пауза между повторами */
    LED_STEP_ON(6),   LED_STEP_OFF(6),
    LED_STEP_ON(6),   LED_STEP_OFF(6),
    LED_STEP_ON(6),   LED_STEP_OFF(60),
};

/** Длина считается макросом LED_SEQ_LOOP — sizeof писать руками не нужно. */
static const led_sequence_t sos = LED_SEQ_LOOP(sos_steps);

/* ================================================================== */
/*  МАЯК: УПАКОВАННЫЕ КОМАНДЫ (4 байта на пару фаз + счётчик)         */
/* ================================================================== */

/**
 * Одна вспышка = одна команда (15 тиков горит, 60 тиков пауза).
 * count = 1 — пара фаз выполняется один раз, 15 = бесконечно.
 * Длительности кодируются макросом LED_CMD (деление только при сборке).
 */
#define BEACON_CH(dev, on_ms, off_ms) \
    LED_CMD((dev), 1u, (on_ms), (off_ms), 1u, 0u)

static const uint32_t beacon[] = {
    BEACON_CH(0x1u,  15u,  60u),   /* 1-я вспышка: 15 мс горит  */
    BEACON_CH(0x1u,  15u, 420u),   /* 2-я вспышка + длинная пауза */
    BEACON_CH(0x1u,  15u,  60u),   /* 3-я вспышка               */
    LED_SYS_REP_START              /* повторять, пока не снято  */
};

/* ================================================================== */
/*  САМОТЕСТ: ТРАССИРОВКА CALLBACK'А                                 */
/* ================================================================== */

#if DEMO_SELFTEST

/**
 * Максимум записей трассировки. За 40 тиков callback может быть вызван
 * не более 4 (LED_COUNT) раз на тик плюс один полный sync_outputs()
 * после led_init(), то есть 4 * 40 + 4 = 164. Запас на трансфазные
 * события (их callback тоже пишет) — 192.
 */
#define TRACE_MAX  192u

static uint8_t *trace_dst;   /**< NULL = трассировка выключена     */
static uint16_t trace_len;   /**< сколько записей накоплено        */
static uint8_t  trace_ovf;   /**< трассировка переполнилась       */

/**
 * Callback вывода с записью трассировки. Пишет всегда (чтобы диоды
 * работали как обычно), но дополнительно запоминает пары «id, level»
 * в один байт: 3 бита на канал (0..7) + 3 бита на уровень (0..7).
 * Уровни выше 7 в упакованных командах невозможны, а уровни из
 * последовательностей здесь равны 0/1, поэтому сжатие без потерь.
 */
static void trace_hw_set(uint8_t id, uint8_t level)
{
    if (trace_dst != NULL) {
        if ((id < LED_COUNT) && (level <= 7u) && (trace_len < TRACE_MAX)) {
            trace_dst[trace_len] = (uint8_t)((id << 3) | level);
        } else {
            trace_ovf = 1u;   /* переполнение или не pack-ится */
        }
        trace_len++;
    }
    led_hw_set(id, level);
}

/** Паттерн упакованных команд для самотеста: 3 пары фаз на LED3. */
static const uint32_t st_packed[] = {
    LED_CMD(0x8u, 3u, 2u, 2u, 3u, 1u)
};

/**
 * Детерминированный сценарий, задевающий ВСЕ режимы каналов сразу:
 * глобальный эффект (забирает все каналы), затем per-LED вызовы
 * «отвязывают» LED0 (BLINK), LED1 (SEQUENCE), LED2 (ON_FOR), а
 * упакованная команда забирает LED3 (PACKED). Никакой случайности:
 * led_init() сбрасывает и PRNG, и все счётчики, поэтому два прогона
 * обязаны дать побайтово одинаковую последовательность.
 */
static void st_setup(void)
{
    led_init();
    led_set_callback(trace_hw_set);
    led_start_effect_for(LED_EFFECT_EMERGENCY, 3u);
    led_blink(0, 3u, 2u);
    led_play(1, &sos);
    led_on_for(2, 4u);
    led_packed_run(st_packed, LED_ARRAY_LEN(st_packed));
}

/** Число тиков, на котором проверяется инвариант батчинга. */
#define ST_TICKS 40u

/**
 * ГЛАВНЫЙ ИНВАРИАНТ v3: led_advance(1) x N тождественно led_advance(N).
 * Сравнивается вся последовательность вызовов callback (а не только
 * конечное состояние выводов) — иначе пропуск перехода остался бы
 * незамеченным.
 *
 * @return 1 = PASS, 0 = FAIL.
 */
static uint8_t st_batching(void)
{
    static uint8_t trace_a[TRACE_MAX];
    static uint8_t trace_b[TRACE_MAX];
    uint16_t       len_a, len_b;
    uint8_t        i;

    /* Проход 1: тик за тиком. */
    trace_dst = NULL;
    trace_ovf = 0u;
    trace_len = 0u;
    trace_dst = trace_a;
    st_setup();
    for (i = 0; i < ST_TICKS; i++) {
        led_advance(1u);
    }
    len_a = trace_len;

    /* Проход 2: одним пакетом из ST_TICKS тиков. */
    trace_len = 0u;
    trace_dst = trace_b;
    st_setup();
    led_advance((uint32_t)ST_TICKS);
    len_b = trace_len;

    trace_dst = NULL;

    if ((trace_ovf != 0u) || (len_a != len_b)) {
        return 0u;
    }
    for (i = 0; i < (uint8_t)len_a; i++) {
        if (trace_a[i] != trace_b[i]) {
            return 0u;
        }
    }
    return 1u;
}

/**
 * Контракт API: led_ticks_until_next() никогда не возвращает 0
 * (0 означал бы «переход сейчас же», но нулевой интервал — это вырожденный
 * случай, которого в API быть не должно). Проверяем на живом сценарии.
 */
static uint8_t st_ticks_never_zero(void)
{
    uint8_t i;

    trace_dst = NULL;
    st_setup();
    for (i = 0; i < ST_TICKS; i++) {
        if (led_ticks_until_next() == 0u) {
            return 0u;
        }
        led_advance(1u);
    }
    return 1u;
}

/**
 * Семантика владения индикатором:
 *   (a) владение, заявленное БЕЗ паттерна, снимается ближайшим advance —
 *       это следствие определения patterns_active() в led_advance();
 *   (b) led_stop_owner() с ЧУЖИМ владельцем ничего не делает;
 *   (c) led_stop_owner() со СВОИМ владельцем гасит паттерны и снимает
 *       владение.
 */
static uint8_t st_ownership(void)
{
    uint8_t ok = 1u;

    trace_dst = NULL;

    /* (a) владение без паттерна.
     * ПРЕДУСЛОВИЕ: ни одного таймерного паттерна не идёт. Иначе снятие
     * владения и не произойдёт — оно по определению привязано к
     * patterns_active(). Поэтому здесь led_init(), а не st_setup(). */
    led_init();
    led_set_owner(LED_OWNER_UI);
    if (led_claim() != LED_OWNER_UI) {
        ok = 0u;
    }
    led_advance(1u);
    if (led_claim() != LED_OWNER_NONE) {
        ok = 0u;
    }

    /* (b) чужой владелец не трогает ничего */
    st_setup();
    led_set_owner(LED_OWNER_BEACON);
    if (led_stop_owner(LED_OWNER_WARNING) != 0u) {
        ok = 0u;                     /* не должен ничего останавливать */
    }
    if (led_claim() != LED_OWNER_BEACON) {
        ok = 0u;                     /* владение не должно перехватываться */
    }
    if (led_get_mode(0) != LED_MODE_BLINK) {
        ok = 0u;                     /* паттерн LED0 должен уцелеть */
    }

    /* (c) свой владелец останавливает */
    if (led_stop_owner(LED_OWNER_BEACON) == 0u) {
        ok = 0u;
    }
    if (led_claim() != LED_OWNER_NONE) {
        ok = 0u;
    }
    if (led_get_mode(0) != LED_MODE_OFF) {
        ok = 0u;                     /* таймерный паттерн обязан погаснуть */
    }
    if (led_any_led_active() != 0u) {
        ok = 0u;                     /* ни одного паттерна не осталось   */
    }
    return ok;
}

#endif /* DEMO_SELFTEST */

/**
 * Прогнать все проверки самотеста.
 *
 * @return 0 = все прошли, иначе битовая маска провалов:
 *         1 — батчинг-инвариант, 2 — ticks_until_next() != 0,
 *         4 — семантика владения.
 */
#if DEMO_SELFTEST
static uint8_t st_run_all(void)
{
    uint8_t fail = 0u;

    if (st_batching() == 0u) {
        fail |= 1u;
    }
    if (st_ticks_never_zero() == 0u) {
        fail |= 2u;
    }
    if (st_ownership() == 0u) {
        fail |= 4u;
    }
    return fail;
}
#endif

/**
 * Сообщить о провале самотеста БЕЗ UART: LED3 мигает `code` раз
 * (длинная пауза между пачками), и demo не запускается. Так регресс
 * виден на плате, даже если кабеля нет.
 *
 * Код — та же маска, что возвращает st_run_all(), поэтому число вспышек
 * однозначно указывает, какой тест упал (1, 2, 4 или их сумма).
 *
 * Мигание строится НА САМОЙ БИБЛИОТЕКЕ (led_on_for + led_advance) — это
 * заодно проверяет, что advance() корректно отрабатывает пакетные
 * вызовы на живом железе.
 */
#if DEMO_SELFTEST && !DEMO_UART
static void fault_loop(uint8_t code)
{
    uint8_t i;

    for (;;) {
        for (i = 0; i < code; i++) {
            led_on_for(3u, 6u);
            led_advance(6u);      /* ON_FOR сам себя гасит на 6-м тике */
            led_advance(24u);
        }
        led_off(3u);
        led_advance(160u);         /* длинная пауза между пачками */
    }
}
#endif

/** Короткое имя режима — для читаемой строки состояния. */
#if DEMO_UART
static const char *mode_name(led_mode_t m)
{
    switch (m) {
    case LED_MODE_OFF:      return "off";
    case LED_MODE_ON:       return "on ";
    case LED_MODE_ON_FOR:   return "onf";
    case LED_MODE_BLINK:    return "blnk";
    case LED_MODE_SEQUENCE: return "seq ";
    case LED_MODE_EFFECT:   return "eff ";
    case LED_MODE_PACKED:   return "pack";
    default:                return "????";
    }
}
#endif /* DEMO_UART */

/* ================================================================== */
/*  ТОЧКА ВХОДА                                                        */
/* ================================================================== */

/** Счётчик тиков демо (~1 тик = 16 мс). */
static uint32_t demo_ticks;

int main(void)
{
#if DEMO_SELFTEST
    /* Единственная переменная самотеста: st_run_all() возвращает битовую
     * маску провалов. Отдельные флаги на каждый тест не нужны — их можно
     * восстановить по битам, и заодно не появляется «set but not used»
     * в сборке без UART, где печати нет. */
    uint8_t st_fail;
#endif

    SystemCoreClockUpdate();   /* синхронизировать SystemCoreClock с RCC  */
    Delay_Init();              /* калибровка Delay_Ms() по SysTick        */
#if DEMO_UART
    USART_Printf_Init(UART_BAUD);
#endif

    /* PC4-PC7: push-pull output, начальный уровень LOW */
    gpio_init_leds();

    /* ================================================================== */
    /*  ДИАГНОСТИКА ПО UART                                                */
    /* ================================================================== */

#if DEMO_UART
    printf("\r\n\r\n=== LED_LIB v%u.%u.%u : CH32V003 demo ===\r\n",
           (unsigned)LED_LIB_VERSION_MAJOR,
           (unsigned)LED_LIB_VERSION_MINOR,
           (unsigned)LED_LIB_VERSION_PATCH);
    printf("LED_COUNT=%u  LEVEL_MAX=%u  effects=%u sequences=%u packed=%u\r\n",
           (unsigned)LED_COUNT, (unsigned)LED_LEVEL_MAX,
           (unsigned)LED_USE_EFFECTS, (unsigned)LED_USE_SEQUENCES,
           (unsigned)LED_USE_PACKED_COMMANDS);
#endif

    /* ================================================================== */
    /*  САМОТЕСТ: ключевые инварианты библиотеки на настоящем железе      */
    /* ================================================================== */

#if DEMO_SELFTEST
    led_init();
    led_set_callback(trace_hw_set);
    st_fail = st_run_all();

#  if DEMO_UART
    printf("\r\n--- self-test ---\r\n");
    printf("  advance(1)x%u == advance(%u) : %s\r\n",
           (unsigned)ST_TICKS, (unsigned)ST_TICKS,
           (st_fail & 1u) ? "FAIL" : "PASS");
    printf("  ticks_until_next() != 0      : %s\r\n",
           (st_fail & 2u) ? "FAIL" : "PASS");
    printf("  ownership semantics          : %s\r\n",
           (st_fail & 4u) ? "FAIL" : "PASS");
    printf("  SELFTEST: %s (code 0x%02X)\r\n",
           (st_fail == 0u) ? "ALL PASS" : "FAILURE", (unsigned)st_fail);
#  else
    /* Без UART провал обязан быть виден: мигаем код и не запускаем demo. */
    if (st_fail != 0u) {
        fault_loop(st_fail);
    }
#  endif
#endif /* DEMO_SELFTEST */

    /* ================================================================== */
    /*  ДЕМО-СЦЕНАРИЙ                                                     */
    /* ================================================================== */

    /* 1) Эффект СНАЧАЛА: переводит ВСЕ диоды в режим EFFECT. */
    led_init();
    led_set_callback(led_hw_set);
    led_start_effect_for(LED_EFFECT_EMERGENCY, 5);   /* LED2+LED3: 5 вспышек */

    /* 2) Per-LED вызовы ПОСЛЕ него «отвязывают» свои диоды от эффекта. */
    led_blink(0, 15, 15);   /* LED0: мигание ~240/240 мс */
    led_play(1, &sos);      /* LED1: SOS, зациклено      */

#if DEMO_UART
    printf("\r\n--- demo running (1 tick ~ 16 ms) ---\r\n");
#endif

    /* ================================================================== */
    /*  ГЛАВНЫЙ ЦИКЛ                                                      */
    /* ================================================================== */

    while (1) {
        /*
         * Событийное продвижение: сколько тиков можно проспать.
         * Для опросного МК значение просто игнорируется, для
         * «спящего» — это программирование таймера пробуждения.
         */
        (void)led_ticks_until_next();
        led_advance(1);
        demo_ticks++;
        Delay_Ms(16);   /* ~62 Гц: 1 тик ≈ 16 мс */

        /* t ≈ 6 с: маяк забирает индикатор, если тот свободен. */
        if (demo_ticks == 375u) {
            if (led_claim() == LED_OWNER_NONE) {
                led_set_owner(LED_OWNER_BEACON);
                led_off(0);                  /* отпустить LED0 под маяк */
                led_packed_run(beacon, LED_ARRAY_LEN(beacon));
#  if DEMO_UART
                printf("[t=%2us] BEACON claims indicator, packed pattern started\r\n",
                       (unsigned)demo_ticks);
#  endif
            } else {
#  if DEMO_UART
                printf("[t=%2us] BEACON skipped: indicator busy (owner=%u)\r\n",
                       (unsigned)demo_ticks, (unsigned)led_claim());
#  endif
            }
        }

        /* t ≈ 12 с: маяк останавливается, LED0 возвращается к миганию.
         *
         * ВНИМАНИЕ, тонкость API: led_stop_owner() гасит ВСЕ таймерные
         * паттерны, а не только каналы маяка. Владение — одна метка на
         * весь индикатор, а не per-pattern, поэтому «остановить своё» =
         * «погасить индикатор». SOS на LED1 (led_play) при этом тоже
         * гаснет: владение было объявлено маяком, значит весь индикатор
         * в этот момент его. Статичные LED_MODE_ON/OFF не трогаются.
         *
         * Возобновить SOS должен хост: снова объявить владение и
         * перезапустить последовательность. Здесь этого не делаем, чтобы
         * было видно фактическое поведение функции. */
        if (demo_ticks == 750u) {
            if (led_stop_owner(LED_OWNER_BEACON)) {
                led_blink(0, 15, 15);
#  if DEMO_UART
                printf("[t=%2us] BEACON stopped (ALL timed patterns off, incl. LED1 SOS)\r\n",
                       (unsigned)demo_ticks);
#  endif
            } else {
#  if DEMO_UART
                printf("[t=%2us] BEACON was not owner, nothing stopped\r\n",
                       (unsigned)demo_ticks);
#  endif
            }
        }

        /* Периодическая строка состояния: владелец, признак «идёт ли
         * индикация», сколько тиков можно проспать, и режим/уровень
         * каждого канала. Печатается по счётчику тиков, поэтому её
         * период в тиках точен независимо от времени печати.
         *
         * Всё это — чистая диагностика, поэтому блок собран так, чтобы
         * при DEMO_UART=0 не осталось даже «хвоста» кода: без него
         * линкер не тянет vfprintf, mode_name() и printf целиком. */
#if DEMO_UART
        if ((demo_ticks % STATE_PERIOD_TICKS) == 0u) {
            uint32_t next = led_ticks_until_next();
            uint8_t  id;

            printf("t=%5u owner=%u act=%u next=",
                   (unsigned)demo_ticks, (unsigned)led_claim(),
                   (unsigned)led_pattern_active());
            if (next == (uint32_t)LED_TICKS_FOREVER) {
                printf("FOREVER");
            } else {
                printf("%5lu", (unsigned long)next);
            }
            for (id = 0; id < LED_COUNT; id++) {
                printf(" | L%u %s v%u", (unsigned)id,
                       mode_name(led_get_mode(id)),
                       (unsigned)led_get_level(id));
            }
            printf("\r\n");
        }
#endif /* DEMO_UART */
    }
}
