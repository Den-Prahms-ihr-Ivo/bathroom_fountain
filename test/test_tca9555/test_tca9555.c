/*
 * Tests for lib/tca9555 – requirements HW-20, HW-21.
 * The bus is a simulated chip with its eight registers and a log of every
 * transfer.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "tca9555.h"

#define MAX_LOG 32

typedef struct {
    bool write;
    uint8_t reg;
    size_t len;
    uint8_t data[2];
} xfer_t;

static uint8_t regs[8];
static uint16_t pins_in; /* what the outside world drives on input pins */
static xfer_t log_[MAX_LOG];
static int n_log;
static int fail_at; /* fail the n-th transfer from now (1 = next), 0 = never */
static bool config_stuck; /* the chip ignores CONFIG writes */

static tca9555_t *dev;
static tca9555_config_t cfg;

static void chip_update_inputs(void)
{
    const uint16_t conf = (uint16_t)(regs[6] | (regs[7] << 8));
    const uint16_t outl = (uint16_t)(regs[2] | (regs[3] << 8));
    const uint16_t pol = (uint16_t)(regs[4] | (regs[5] << 8));
    const uint16_t level = (uint16_t)((pins_in & conf) | (outl & ~conf));
    const uint16_t in = level ^ pol;
    regs[0] = (uint8_t)in;
    regs[1] = (uint8_t)(in >> 8);
}

static bool should_fail(void)
{
    if (fail_at > 0 && --fail_at == 0) {
        return true;
    }
    return false;
}

static int bus_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(n_log < MAX_LOG);
    log_[n_log].write = true;
    log_[n_log].reg = reg;
    log_[n_log].len = len;
    memcpy(log_[n_log].data, data, len > 2 ? 2 : len);
    n_log++;
    if (should_fail()) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        const uint8_t r = (uint8_t)((reg & 0x06) | ((reg + i) & 1)); /* stays inside the pair */
        if (r >= 2 && !(config_stuck && r >= 6)) {
            regs[r] = data[i];
        }
    }
    return 0;
}

static int bus_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(n_log < MAX_LOG);
    log_[n_log].write = false;
    log_[n_log].reg = reg;
    log_[n_log].len = len;
    n_log++;
    if (should_fail()) {
        return -1;
    }
    chip_update_inputs();
    for (size_t i = 0; i < len; i++) {
        data[i] = regs[(reg & 0x06) | ((reg + i) & 1)];
    }
    return 0;
}

static const tca9555_io_t io = {NULL, bus_write, bus_read};

void setUp(void)
{
    /* a chip that was left in some other state by the previous firmware */
    const uint8_t dirty[8] = {0, 0, 0x12, 0x34, 0xF0, 0x0F, 0x55, 0xAA};
    memcpy(regs, dirty, sizeof(regs));
    pins_in = 0xFFFF; /* pull-ups */
    n_log = 0;
    fail_at = 0;
    config_stuck = false;
    dev = NULL;
    /* carrier Rev A: LEDs on P01, P03, P05, P07 and P11 are outputs */
    cfg.output_pins = 0x02AA;
    cfg.initial_levels = 0x0000;
    cfg.inverted_inputs = 0x0155; /* switches P00, P02, P04, P06 and SD detect P10 pull to GND */
}

void tearDown(void)
{
    tca9555_destroy(dev);
    dev = NULL;
}

static void create(void)
{
    dev = tca9555_create(&io);
    TEST_ASSERT_NOT_NULL_MESSAGE(dev, "tca9555_create returned NULL for valid callbacks");
}

static void init(void)
{
    create();
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_init(dev, &cfg));
    n_log = 0;
}

static void expect_write(int i, uint8_t reg, uint16_t value)
{
    TEST_ASSERT_TRUE_MESSAGE(i < n_log, "transfer missing");
    TEST_ASSERT_TRUE(log_[i].write);
    TEST_ASSERT_EQUAL_HEX8(reg, log_[i].reg);
    TEST_ASSERT_EQUAL_size_t(2, log_[i].len);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)value, log_[i].data[0]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(value >> 8), log_[i].data[1]);
}

static void test_create(void)
{
    create();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n_log, "create does not touch the bus");
    TEST_ASSERT_FALSE(tca9555_is_ready(dev));
    tca9555_io_t bad = io;
    bad.write = NULL;
    TEST_ASSERT_NULL(tca9555_create(&bad));
    bad = io;
    bad.read = NULL;
    TEST_ASSERT_NULL(tca9555_create(&bad));
    TEST_ASSERT_NULL(tca9555_create(NULL));
    tca9555_destroy(NULL);
}

static void test_init_sequence(void)
{
    create();
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_init(dev, &cfg));
    TEST_ASSERT_TRUE(tca9555_is_ready(dev));
    TEST_ASSERT_EQUAL_INT(4, n_log);
    expect_write(0, TCA9555_REG_OUTPUT, 0x0000);   /* latches before direction */
    expect_write(1, TCA9555_REG_POLARITY, 0x0155);
    expect_write(2, TCA9555_REG_CONFIG, 0xFD55);   /* ~0x02AA */
    TEST_ASSERT_FALSE(log_[3].write);
    TEST_ASSERT_EQUAL_HEX8(TCA9555_REG_CONFIG, log_[3].reg);
    TEST_ASSERT_EQUAL_size_t(2, log_[3].len);
    TEST_ASSERT_EQUAL_HEX8(0x55, regs[6]);
    TEST_ASSERT_EQUAL_HEX8(0xFD, regs[7]);
}

static void test_calls_before_init_are_refused(void)
{
    create();
    uint16_t v = 0xBEEF;
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_NOT_READY, tca9555_read(dev, &v));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, v);
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_NOT_READY, tca9555_write_pin(dev, 1, true));
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_NOT_READY, tca9555_write_masked(dev, 0x0002, 0x0002));
    TEST_ASSERT_EQUAL_INT(0, n_log);
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_ARG, tca9555_init(dev, NULL));
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_init(dev, &cfg));
}

static void test_init_reports_bus_errors_at_every_step(void)
{
    for (int step = 1; step <= 4; step++) {
        tearDown();
        setUp();
        create();
        fail_at = step;
        TEST_ASSERT_EQUAL_INT(TCA9555_ERR_IO, tca9555_init(dev, &cfg));
        TEST_ASSERT_EQUAL_INT_MESSAGE(step, n_log, "init stops at the failed transfer");
        TEST_ASSERT_FALSE(tca9555_is_ready(dev));
        TEST_ASSERT_EQUAL_INT(TCA9555_ERR_NOT_READY, tca9555_write_pin(dev, 1, true));
        /* a second attempt works */
        TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_init(dev, &cfg));
        TEST_ASSERT_TRUE(tca9555_is_ready(dev));
    }
}

static void test_init_verifies_the_direction_register(void)
{
    create();
    config_stuck = true; /* reads back 0xAA55 from setUp */
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_VERIFY, tca9555_init(dev, &cfg));
    TEST_ASSERT_FALSE(tca9555_is_ready(dev));
    config_stuck = false;
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_init(dev, &cfg));
}

static void test_read_returns_all_pins_with_polarity(void)
{
    init();
    uint16_t v = 0;
    pins_in = 0xFFFF; /* nothing pressed */
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_read(dev, &v));
    /* inputs: inverted ones read 0, plain ones 1; outputs read their level 0 */
    TEST_ASSERT_EQUAL_HEX16(0xFC00, v);
    TEST_ASSERT_EQUAL_INT(1, n_log);
    TEST_ASSERT_FALSE(log_[0].write);
    TEST_ASSERT_EQUAL_HEX8(TCA9555_REG_INPUT, log_[0].reg);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2, log_[0].len, "both ports in one transfer");

    pins_in = (uint16_t)~0x0040; /* switch on P06 pressed (port 1 of the carrier) */
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_read(dev, &v));
    TEST_ASSERT_EQUAL_HEX16(0xFC40, v);
    pins_in = (uint16_t)~0x0100; /* SD card inserted: P10 low */
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_read(dev, &v));
    TEST_ASSERT_EQUAL_HEX16(0xFD00, v);
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_ARG, tca9555_read(dev, NULL));
}

static void test_read_error(void)
{
    init();
    uint16_t v = 0xBEEF;
    fail_at = 1;
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_IO, tca9555_read(dev, &v));
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, v);
    TEST_ASSERT_TRUE_MESSAGE(tca9555_is_ready(dev), "one failed read does not end the session");
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_read(dev, &v));
}

static void test_write_pin_uses_the_shadow(void)
{
    init();
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 1, true));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n_log, "one write, no read");
    expect_write(0, TCA9555_REG_OUTPUT, 0x0002);
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 9, true));
    expect_write(1, TCA9555_REG_OUTPUT, 0x0202);
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 1, false));
    expect_write(2, TCA9555_REG_OUTPUT, 0x0200);
    TEST_ASSERT_EQUAL_INT(3, n_log);
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 9, true));
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, n_log, "no change, no bus traffic");
    uint16_t v;
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_read(dev, &v));
    TEST_ASSERT_EQUAL_HEX16(0xFE00, v); /* the output reads back high */
}

static void test_write_masked(void)
{
    cfg.initial_levels = 0x0202;
    init();
    TEST_ASSERT_EQUAL_HEX8(0x02, regs[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02, regs[3]);
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_masked(dev, 0x00AA, 0xFF28));
    TEST_ASSERT_EQUAL_INT(1, n_log);
    expect_write(0, TCA9555_REG_OUTPUT, 0x0228); /* P11 kept, P01 cleared, P03 and P05 set */
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_masked(dev, 0x00AA, 0x0028));
    TEST_ASSERT_EQUAL_INT(1, n_log);
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_masked(dev, 0x0000, 0xFFFF));
    TEST_ASSERT_EQUAL_INT(1, n_log);
}

static void test_only_output_pins_can_be_written(void)
{
    init();
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_ARG, tca9555_write_pin(dev, 0, true)); /* a switch input */
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_ARG, tca9555_write_pin(dev, 16, true));
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_ARG, tca9555_write_masked(dev, 0x0003, 0x0003));
    TEST_ASSERT_EQUAL_INT(0, n_log);
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 7, true));
}

static void test_failed_write_is_repeated_next_time(void)
{
    init();
    fail_at = 1;
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_IO, tca9555_write_pin(dev, 3, true));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, regs[2], "the chip never saw it");
    /* the same request again must reach the bus although the shadow may think it is done */
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 3, true));
    TEST_ASSERT_EQUAL_INT(2, n_log);
    TEST_ASSERT_EQUAL_HEX8(0x08, regs[2]);
    /* and going back to the old level after a failure is sent too */
    fail_at = 1;
    TEST_ASSERT_EQUAL_INT(TCA9555_ERR_IO, tca9555_write_pin(dev, 5, true));
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 5, false));
    TEST_ASSERT_EQUAL_INT(4, n_log);
    expect_write(3, TCA9555_REG_OUTPUT, 0x0008);
}

static void test_init_again_resets_the_outputs(void)
{
    init();
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 9, true));
    n_log = 0;
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_init(dev, &cfg));
    expect_write(0, TCA9555_REG_OUTPUT, 0x0000);
    n_log = 0;
    TEST_ASSERT_EQUAL_INT(TCA9555_OK, tca9555_write_pin(dev, 9, true));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n_log, "the shadow followed the init");
    expect_write(0, TCA9555_REG_OUTPUT, 0x0200);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create);
    RUN_TEST(test_init_sequence);
    RUN_TEST(test_calls_before_init_are_refused);
    RUN_TEST(test_init_reports_bus_errors_at_every_step);
    RUN_TEST(test_init_verifies_the_direction_register);
    RUN_TEST(test_read_returns_all_pins_with_polarity);
    RUN_TEST(test_read_error);
    RUN_TEST(test_write_pin_uses_the_shadow);
    RUN_TEST(test_write_masked);
    RUN_TEST(test_only_output_pins_can_be_written);
    RUN_TEST(test_failed_write_is_repeated_next_time);
    RUN_TEST(test_init_again_resets_the_outputs);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
