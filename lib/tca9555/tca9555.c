/*
 * tca9555 – 16-bit I2C port expander.
 * Contract in tca9555.h, tests in test/test_tca9555/.
 */
#include "tca9555.h"

#include <stdlib.h>

struct tca9555 {
    tca9555_io_t io;
    bool ready;
    uint16_t output_pins;
    uint16_t shadow;    /* OUTPUT register as last written */
    bool shadow_valid;  /* false after a failed write */
};

tca9555_t *tca9555_create(const tca9555_io_t *io)
{
    if (io == NULL || io->write == NULL || io->read == NULL) {
        return NULL;
    }
    tca9555_t *dev = calloc(1, sizeof(*dev));
    if (dev != NULL) {
        dev->io = *io;
    }
    return dev;
}

void tca9555_destroy(tca9555_t *dev)
{
    free(dev);
}

static int write16(tca9555_t *dev, uint8_t reg, uint16_t value)
{
    const uint8_t data[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
    return dev->io.write(dev->io.ctx, reg, data, 2);
}

static int read16(tca9555_t *dev, uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    const int r = dev->io.read(dev->io.ctx, reg, data, 2);
    if (r == 0) {
        *value = (uint16_t)(data[0] | (data[1] << 8));
    }
    return r;
}

tca9555_err_t tca9555_init(tca9555_t *dev, const tca9555_config_t *cfg)
{
    if (dev == NULL || cfg == NULL) {
        return TCA9555_ERR_ARG;
    }
    dev->ready = false;
    dev->shadow_valid = false;
    const uint16_t config = (uint16_t)~cfg->output_pins;
    if (write16(dev, TCA9555_REG_OUTPUT, cfg->initial_levels) != 0 ||
        write16(dev, TCA9555_REG_POLARITY, cfg->inverted_inputs) != 0 ||
        write16(dev, TCA9555_REG_CONFIG, config) != 0) {
        return TCA9555_ERR_IO;
    }
    uint16_t readback = 0;
    if (read16(dev, TCA9555_REG_CONFIG, &readback) != 0) {
        return TCA9555_ERR_IO;
    }
    if (readback != config) {
        return TCA9555_ERR_VERIFY;
    }
    dev->output_pins = cfg->output_pins;
    dev->shadow = cfg->initial_levels;
    dev->shadow_valid = true;
    dev->ready = true;
    return TCA9555_OK;
}

bool tca9555_is_ready(const tca9555_t *dev)
{
    return dev != NULL && dev->ready;
}

tca9555_err_t tca9555_read(tca9555_t *dev, uint16_t *levels)
{
    if (dev == NULL) {
        return TCA9555_ERR_ARG;
    }
    if (!dev->ready) {
        return TCA9555_ERR_NOT_READY;
    }
    if (levels == NULL) {
        return TCA9555_ERR_ARG;
    }
    uint16_t v;
    if (read16(dev, TCA9555_REG_INPUT, &v) != 0) {
        return TCA9555_ERR_IO;
    }
    *levels = v;
    return TCA9555_OK;
}

tca9555_err_t tca9555_write_masked(tca9555_t *dev, uint16_t mask, uint16_t levels)
{
    if (dev == NULL) {
        return TCA9555_ERR_ARG;
    }
    if (!dev->ready) {
        return TCA9555_ERR_NOT_READY;
    }
    if ((mask & (uint16_t)~dev->output_pins) != 0) {
        return TCA9555_ERR_ARG;
    }
    const uint16_t want = (uint16_t)((dev->shadow & (uint16_t)~mask) | (levels & mask));
    if (dev->shadow_valid && want == dev->shadow) {
        return TCA9555_OK;
    }
    if (write16(dev, TCA9555_REG_OUTPUT, want) != 0) {
        dev->shadow_valid = false;
        return TCA9555_ERR_IO;
    }
    dev->shadow = want;
    dev->shadow_valid = true;
    return TCA9555_OK;
}

tca9555_err_t tca9555_write_pin(tca9555_t *dev, uint8_t pin, bool high)
{
    if (dev == NULL) {
        return TCA9555_ERR_ARG;
    }
    if (!dev->ready) {
        return TCA9555_ERR_NOT_READY;
    }
    if (pin > 15) {
        return TCA9555_ERR_ARG;
    }
    const uint16_t bit = (uint16_t)(1u << pin);
    return tca9555_write_masked(dev, bit, high ? bit : 0);
}
