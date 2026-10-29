/*
 * Tests for lib/tas5825m – AUD-05/06/07 and fault handling.
 *
 * A fake board records every callback (I2C write/read, PDN, delay) in an
 * event log, keeps a register file, and can fail a chosen I2C operation.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "tas5825m.h"

/* ---------------------------------------------------------------- fake --- */

typedef enum { EV_WRITE, EV_READ, EV_PDN, EV_DELAY } ev_type_t;

typedef struct {
    ev_type_t type;
    uint8_t reg;
    uint8_t val;   /* written value, or PDN level */
    uint32_t ms;
    bool ok;
} ev_t;

#define MAX_EV 512
static ev_t ev[MAX_EV];
static int nev;
static uint8_t regs[256];
static int i2c_ops;   /* I2C operations since the last fake_clear() */
static int fail_at;   /* fail this I2C operation (0-based), -1 = never */
static bool pdn_level;
static bool overflow;

static void push(ev_t e)
{
    if (nev < MAX_EV) {
        ev[nev++] = e;
    } else {
        overflow = true;
    }
}

static int fake_write(void *ctx, uint8_t reg, uint8_t value)
{
    (void)ctx;
    const bool ok = (i2c_ops++ != fail_at);
    push((ev_t){EV_WRITE, reg, value, 0, ok});
    if (!ok) {
        return -1;
    }
    regs[reg] = value;
    return 0;
}

static int fake_read(void *ctx, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    const bool ok = (i2c_ops++ != fail_at);
    push((ev_t){EV_READ, reg, 0, 0, ok});
    if (!ok) {
        return -1;
    }
    *value = regs[reg];
    return 0;
}

static void fake_pdn(void *ctx, bool high)
{
    (void)ctx;
    pdn_level = high;
    push((ev_t){EV_PDN, 0, high ? 1 : 0, 0, true});
}

static void fake_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    push((ev_t){EV_DELAY, 0, 0, ms, true});
}

static const tas5825m_io_t fake_io = {NULL, fake_write, fake_read, fake_pdn, fake_delay};

static void fake_clear(void)
{
    nev = 0;
    i2c_ops = 0;
    fail_at = -1;
}

static void fake_reset(void)
{
    fake_clear();
    memset(regs, 0, sizeof(regs));
    pdn_level = true; /* unknown at boot; create must pull it low */
    overflow = false;
}

/* Index of the first event at or after `from` matching type/reg/val (-1 = any), or -1. */
static int find(int from, ev_type_t type, int reg, int val)
{
    for (int i = from; i < nev; i++) {
        if (ev[i].type != type) continue;
        if (reg >= 0 && ev[i].reg != (uint8_t)reg) continue;
        if (val >= 0 && ev[i].val != (uint8_t)val) continue;
        return i;
    }
    return -1;
}

static int find_last(ev_type_t type, int reg, int val)
{
    int last = -1;
    for (int i = find(0, type, reg, val); i >= 0; i = find(i + 1, type, reg, val)) {
        last = i;
    }
    return last;
}

static int count(ev_type_t type, int reg, int val)
{
    int n = 0;
    for (int i = find(0, type, reg, val); i >= 0; i = find(i + 1, type, reg, val)) {
        n++;
    }
    return n;
}

static int count_i2c(void)
{
    return count(EV_WRITE, -1, -1) + count(EV_READ, -1, -1);
}

static int first_i2c_from(int from)
{
    const int w = find(from, EV_WRITE, -1, -1);
    const int r = find(from, EV_READ, -1, -1);
    if (w < 0) return r;
    if (r < 0) return w;
    return w < r ? w : r;
}

static uint32_t delay_between(int a, int b)
{
    uint32_t sum = 0;
    for (int i = a + 1; i < b; i++) {
        if (ev[i].type == EV_DELAY) sum += ev[i].ms;
    }
    return sum;
}

/* ------------------------------------------------------------- fixture --- */

static tas5825m_t *dev;

void setUp(void)
{
    fake_reset();
    dev = tas5825m_create(&fake_io);
    TEST_ASSERT_NOT_NULL_MESSAGE(dev, "tas5825m_create returned NULL");
    fake_clear();
}

void tearDown(void)
{
    tas5825m_destroy(dev);
    dev = NULL;
    TEST_ASSERT_FALSE_MESSAGE(overflow, "event log overflow");
}

static void power_up_ok(void)
{
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_up(dev));
    TEST_ASSERT_TRUE(tas5825m_is_powered(dev));
    fake_clear();
}

/* Checks the documented power-up sequence in the current log. */
static void assert_power_up_sequence(void)
{
    const int i_pdn = find(0, EV_PDN, -1, 1);
    TEST_ASSERT_TRUE_MESSAGE(i_pdn >= 0, "PDN must go high");
    const int i_first = first_i2c_from(0);
    TEST_ASSERT_TRUE_MESSAGE(i_first > i_pdn, "no I2C before PDN is high");
    TEST_ASSERT_TRUE_MESSAGE(delay_between(i_pdn, i_first) >= TAS5825M_PDN_SETTLE_MS,
                             "wait after PDN high before the first I2C access");

    const int i_page = find(i_pdn, EV_WRITE, TAS5825M_REG_PAGE, 0);
    const int i_book = i_page < 0 ? -1 : find(i_page, EV_WRITE, TAS5825M_REG_BOOK, 0);
    const int i_vol = find(i_pdn, EV_WRITE, TAS5825M_REG_DIG_VOL, TAS5825M_VOL_MUTE);
    const int i_hiz = find(i_pdn, EV_WRITE, TAS5825M_REG_DEVICE_CTRL2, TAS5825M_CTRL2_HIZ);
    const int i_play = find(i_pdn, EV_WRITE, TAS5825M_REG_DEVICE_CTRL2, TAS5825M_CTRL2_PLAY);
    const int i_clear = find(i_pdn, EV_WRITE, TAS5825M_REG_FAULT_CLEAR, TAS5825M_FAULT_CLEAR_ALL);

    TEST_ASSERT_TRUE_MESSAGE(i_page >= 0, "select page 0");
    TEST_ASSERT_TRUE_MESSAGE(i_book > i_page, "select book 0 after page 0");
    TEST_ASSERT_TRUE_MESSAGE(i_vol >= 0, "DIG_VOL = mute during power-up");
    TEST_ASSERT_TRUE_MESSAGE(i_hiz >= 0, "DEVICE_CTRL2 = Hi-Z (0x02)");
    TEST_ASSERT_TRUE_MESSAGE(i_play >= 0, "DEVICE_CTRL2 = play (0x03)");
    TEST_ASSERT_TRUE_MESSAGE(i_clear >= 0, "FAULT_CLEAR = 0x80");

    TEST_ASSERT_TRUE_MESSAGE(i_book < i_vol && i_book < i_hiz, "book/page first");
    TEST_ASSERT_TRUE_MESSAGE(i_vol < i_play, "muted before play");
    TEST_ASSERT_TRUE_MESSAGE(i_hiz < i_play, "Hi-Z before play");
    TEST_ASSERT_TRUE_MESSAGE(delay_between(i_hiz, i_play) >= TAS5825M_DSP_BOOT_MS,
                             "wait for the DSP between Hi-Z and play");
    TEST_ASSERT_TRUE_MESSAGE(i_clear > i_play, "clear faults after play");

    TEST_ASSERT_EQUAL_INT_MESSAGE(i_play, find_last(EV_WRITE, TAS5825M_REG_DEVICE_CTRL2, -1),
                                  "play is the last DEVICE_CTRL2 write");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, count(EV_WRITE, TAS5825M_REG_DEVICE_CTRL2, -1),
                                  "exactly Hi-Z then play");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(EV_WRITE, TAS5825M_REG_DIG_VOL, -1),
                                  "power-up writes the volume once, as mute");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, find(i_pdn, EV_PDN, -1, 0), "PDN stays high");
    TEST_ASSERT_TRUE(pdn_level);
}

/* ---------------------------------------------------------------- tests --- */

static void test_create_rejects_missing_io(void)
{
    TEST_ASSERT_NULL(tas5825m_create(NULL));
    tas5825m_io_t io = fake_io;
    io.write_reg = NULL;
    TEST_ASSERT_NULL(tas5825m_create(&io));
    io = fake_io;
    io.read_reg = NULL;
    TEST_ASSERT_NULL(tas5825m_create(&io));
    io = fake_io;
    io.set_pdn = NULL;
    TEST_ASSERT_NULL(tas5825m_create(&io));
    io = fake_io;
    io.delay_ms = NULL;
    TEST_ASSERT_NULL(tas5825m_create(&io));
}

static void test_create_pulls_pdn_low_without_i2c(void)
{
    fake_reset();
    tas5825m_t *d = tas5825m_create(&fake_io);
    const bool created = (d != NULL);
    const bool powered = tas5825m_is_powered(d);
    tas5825m_destroy(d); /* free before asserting, so a failure does not leak */
    TEST_ASSERT_TRUE(created);
    TEST_ASSERT_TRUE(find(0, EV_PDN, -1, 0) >= 0);
    TEST_ASSERT_FALSE(pdn_level);
    TEST_ASSERT_EQUAL_INT(0, count_i2c());
    TEST_ASSERT_FALSE(powered);
}

static void test_create_copies_the_io_struct(void)
{
    tas5825m_io_t io = fake_io;
    tas5825m_t *d = tas5825m_create(&io);
    TEST_ASSERT_NOT_NULL(d);
    memset(&io, 0, sizeof(io)); /* the caller's struct may go out of scope */
    fake_clear();
    const tas5825m_err_t r = tas5825m_power_up(d);
    tas5825m_destroy(d);
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, r);
    TEST_ASSERT_TRUE(count_i2c() > 0);
}

static void test_power_up_sequence(void)
{
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_up(dev));
    TEST_ASSERT_TRUE(tas5825m_is_powered(dev));
    assert_power_up_sequence();
}

static void test_power_up_when_on_does_no_io(void)
{
    power_up_ok();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_up(dev));
    TEST_ASSERT_EQUAL_INT(0, nev);
    TEST_ASSERT_TRUE(tas5825m_is_powered(dev));
}

static void test_power_up_failure_at_every_i2c_step_cuts_power(void)
{
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_up(dev));
    const int n_ops = count_i2c();
    TEST_ASSERT_TRUE(n_ops >= 5);

    for (int k = 0; k < n_ops; k++) {
        tas5825m_destroy(dev);
        fake_reset();
        dev = tas5825m_create(&fake_io);
        TEST_ASSERT_NOT_NULL(dev);
        fake_clear();
        fail_at = k;

        TEST_ASSERT_EQUAL_INT_MESSAGE(TAS5825M_ERR_IO, tas5825m_power_up(dev), "failed I2C must be reported");
        TEST_ASSERT_FALSE(tas5825m_is_powered(dev));
        TEST_ASSERT_FALSE_MESSAGE(pdn_level, "PDN must be low after a failed power-up");

        int i_fail = -1;
        for (int i = 0; i < nev; i++) {
            if ((ev[i].type == EV_WRITE || ev[i].type == EV_READ) && !ev[i].ok) {
                i_fail = i;
                break;
            }
        }
        TEST_ASSERT_TRUE(i_fail >= 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, first_i2c_from(i_fail + 1), "no I2C after the failure");
        TEST_ASSERT_TRUE(find(i_fail + 1, EV_PDN, -1, 0) >= 0);

        /* a later attempt runs the full sequence again */
        fake_clear();
        TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_up(dev));
        assert_power_up_sequence();
    }
}

static void test_power_down_sequence(void)
{
    power_up_ok();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_down(dev));
    TEST_ASSERT_FALSE(tas5825m_is_powered(dev));
    TEST_ASSERT_FALSE(pdn_level);

    const int i_hiz = find(0, EV_WRITE, TAS5825M_REG_DEVICE_CTRL2, TAS5825M_CTRL2_HIZ);
    const int i_low = find(0, EV_PDN, -1, 0);
    TEST_ASSERT_TRUE_MESSAGE(i_hiz >= 0, "Hi-Z before cutting power");
    TEST_ASSERT_TRUE(i_low > i_hiz);
    TEST_ASSERT_TRUE_MESSAGE(delay_between(i_hiz, i_low) >= TAS5825M_STOP_MS,
                             "let the outputs stop before PDN goes low");
    TEST_ASSERT_EQUAL_INT(0, count(EV_WRITE, TAS5825M_REG_DEVICE_CTRL2, TAS5825M_CTRL2_PLAY));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, first_i2c_from(i_low + 1), "no I2C once PDN is low");
}

static void test_power_down_when_off_does_no_i2c(void)
{
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_down(dev));
    TEST_ASSERT_EQUAL_INT(0, count_i2c());
    TEST_ASSERT_FALSE(pdn_level);
    TEST_ASSERT_FALSE(tas5825m_is_powered(dev));
}

static void test_power_down_io_error_still_cuts_power(void)
{
    power_up_ok();
    fail_at = 0;
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_IO, tas5825m_power_down(dev));
    TEST_ASSERT_FALSE(pdn_level);
    TEST_ASSERT_FALSE(tas5825m_is_powered(dev));
}

static void test_up_down_up_runs_full_sequence_each_time(void)
{
    for (int round = 0; round < 3; round++) {
        fake_clear();
        TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_up(dev));
        assert_power_up_sequence();
        fake_clear();
        TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_power_down(dev));
        TEST_ASSERT_FALSE(pdn_level);
    }
}

static void test_volume_requires_power(void)
{
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_STATE, tas5825m_set_volume(dev, 0x50));
    TEST_ASSERT_EQUAL_INT(0, nev);
}

static void test_volume_is_a_single_register_write(void)
{
    power_up_ok();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_set_volume(dev, 0x50));
    TEST_ASSERT_EQUAL_INT(1, nev);
    TEST_ASSERT_EQUAL_INT(EV_WRITE, ev[0].type);
    TEST_ASSERT_EQUAL_HEX8(TAS5825M_REG_DIG_VOL, ev[0].reg);
    TEST_ASSERT_EQUAL_HEX8(0x50, ev[0].val);

    fake_clear();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_set_volume(dev, TAS5825M_VOL_MUTE));
    TEST_ASSERT_EQUAL_INT(1, nev);
    TEST_ASSERT_EQUAL_HEX8(TAS5825M_VOL_MUTE, ev[0].val);

    fake_clear();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_set_volume(dev, TAS5825M_VOL_0DB));
    TEST_ASSERT_EQUAL_HEX8(TAS5825M_VOL_0DB, regs[TAS5825M_REG_DIG_VOL]);
}

static void test_volume_never_boosts(void)
{
    power_up_ok();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_set_volume(dev, 0x00)); /* +24 dB requested */
    TEST_ASSERT_EQUAL_HEX8(TAS5825M_VOL_0DB, regs[TAS5825M_REG_DIG_VOL]);
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_set_volume(dev, 0x2F));
    TEST_ASSERT_EQUAL_HEX8(TAS5825M_VOL_0DB, regs[TAS5825M_REG_DIG_VOL]);
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_set_volume(dev, 0x31));
    TEST_ASSERT_EQUAL_HEX8(0x31, regs[TAS5825M_REG_DIG_VOL]);
}

static void test_volume_io_error_keeps_the_device_on(void)
{
    power_up_ok();
    fail_at = 0;
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_IO, tas5825m_set_volume(dev, 0x40));
    TEST_ASSERT_TRUE(tas5825m_is_powered(dev));
    TEST_ASSERT_TRUE(pdn_level);
    fake_clear();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_set_volume(dev, 0x40));
}

static void test_read_faults(void)
{
    tas5825m_faults_t f;
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_STATE, tas5825m_read_faults(dev, &f));
    TEST_ASSERT_EQUAL_INT(0, nev);

    power_up_ok();
    regs[TAS5825M_REG_CHAN_FAULT] = 0x01;
    regs[TAS5825M_REG_GLOBAL_FAULT1] = 0x04;
    regs[TAS5825M_REG_GLOBAL_FAULT2] = 0x00;
    regs[TAS5825M_REG_WARNING] = 0x08;
    memset(&f, 0xAA, sizeof(f));
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_read_faults(dev, &f));
    TEST_ASSERT_EQUAL_HEX8(0x01, f.chan);
    TEST_ASSERT_EQUAL_HEX8(0x04, f.global1);
    TEST_ASSERT_EQUAL_HEX8(0x00, f.global2);
    TEST_ASSERT_EQUAL_HEX8(0x08, f.warning);
    TEST_ASSERT_EQUAL_INT(1, count(EV_READ, TAS5825M_REG_CHAN_FAULT, -1));
    TEST_ASSERT_EQUAL_INT(1, count(EV_READ, TAS5825M_REG_GLOBAL_FAULT1, -1));
    TEST_ASSERT_EQUAL_INT(1, count(EV_READ, TAS5825M_REG_GLOBAL_FAULT2, -1));
    TEST_ASSERT_EQUAL_INT(1, count(EV_READ, TAS5825M_REG_WARNING, -1));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, count(EV_WRITE, -1, -1), "reading faults must not clear them");
}

static void test_read_faults_io_error(void)
{
    power_up_ok();
    tas5825m_faults_t f;
    fail_at = 2;
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_IO, tas5825m_read_faults(dev, &f));
    TEST_ASSERT_TRUE(tas5825m_is_powered(dev));
}

static void test_faults_any(void)
{
    tas5825m_faults_t f = {0, 0, 0, 0};
    TEST_ASSERT_FALSE(tas5825m_faults_any(&f));
    f.warning = 0x0F;
    TEST_ASSERT_FALSE_MESSAGE(tas5825m_faults_any(&f), "a temperature warning alone is not a fault");
    f = (tas5825m_faults_t){0x02, 0, 0, 0};
    TEST_ASSERT_TRUE(tas5825m_faults_any(&f));
    f = (tas5825m_faults_t){0, 0x80, 0, 0};
    TEST_ASSERT_TRUE(tas5825m_faults_any(&f));
    f = (tas5825m_faults_t){0, 0, 0x01, 0};
    TEST_ASSERT_TRUE(tas5825m_faults_any(&f));
    TEST_ASSERT_FALSE(tas5825m_faults_any(NULL));
}

static void test_clear_faults(void)
{
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_STATE, tas5825m_clear_faults(dev));
    TEST_ASSERT_EQUAL_INT(0, nev);
    power_up_ok();
    TEST_ASSERT_EQUAL_INT(TAS5825M_OK, tas5825m_clear_faults(dev));
    TEST_ASSERT_EQUAL_INT(1, nev);
    TEST_ASSERT_EQUAL_INT(EV_WRITE, ev[0].type);
    TEST_ASSERT_EQUAL_HEX8(TAS5825M_REG_FAULT_CLEAR, ev[0].reg);
    TEST_ASSERT_EQUAL_HEX8(TAS5825M_FAULT_CLEAR_ALL, ev[0].val);
}

static void test_null_arguments(void)
{
    tas5825m_faults_t f;
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_ARG, tas5825m_power_up(NULL));
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_ARG, tas5825m_power_down(NULL));
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_ARG, tas5825m_set_volume(NULL, 0x40));
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_ARG, tas5825m_read_faults(NULL, &f));
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_ARG, tas5825m_clear_faults(NULL));
    power_up_ok();
    TEST_ASSERT_EQUAL_INT(TAS5825M_ERR_ARG, tas5825m_read_faults(dev, NULL));
    TEST_ASSERT_EQUAL_INT(0, nev);
    TEST_ASSERT_FALSE(tas5825m_is_powered(NULL));
    tas5825m_destroy(NULL);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_rejects_missing_io);
    RUN_TEST(test_create_pulls_pdn_low_without_i2c);
    RUN_TEST(test_create_copies_the_io_struct);
    RUN_TEST(test_power_up_sequence);
    RUN_TEST(test_power_up_when_on_does_no_io);
    RUN_TEST(test_power_up_failure_at_every_i2c_step_cuts_power);
    RUN_TEST(test_power_down_sequence);
    RUN_TEST(test_power_down_when_off_does_no_i2c);
    RUN_TEST(test_power_down_io_error_still_cuts_power);
    RUN_TEST(test_up_down_up_runs_full_sequence_each_time);
    RUN_TEST(test_volume_requires_power);
    RUN_TEST(test_volume_is_a_single_register_write);
    RUN_TEST(test_volume_never_boosts);
    RUN_TEST(test_volume_io_error_keeps_the_device_on);
    RUN_TEST(test_read_faults);
    RUN_TEST(test_read_faults_io_error);
    RUN_TEST(test_faults_any);
    RUN_TEST(test_clear_faults);
    RUN_TEST(test_null_arguments);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
