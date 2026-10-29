/*
 * tas5825m – power sequencing, volume and faults for the TAS5825M.
 *
 * The contract is in tas5825m.h, the tests in test/test_tas5825m/.
 */
#include "tas5825m.h"

#include <stddef.h>
#include <stdlib.h>

struct tas5825m {
    tas5825m_io_t io;
    bool powered;
};

tas5825m_t *tas5825m_create(const tas5825m_io_t *io)
{
    if (io == NULL || io->write_reg == NULL || io->read_reg == NULL
        || io->set_pdn == NULL || io->delay_ms == NULL) {
        return NULL;
    }
    tas5825m_t *dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return NULL;
    }
    dev->io = *io;
    dev->io.set_pdn(dev->io.ctx, false); /* known off state */
    return dev;
}

void tas5825m_destroy(tas5825m_t *dev)
{
    free(dev);
}

static bool wr(tas5825m_t *dev, uint8_t reg, uint8_t value)
{
    return dev->io.write_reg(dev->io.ctx, reg, value) == 0;
}

static void delay(tas5825m_t *dev, uint32_t ms)
{
    dev->io.delay_ms(dev->io.ctx, ms);
}

static void cut_power(tas5825m_t *dev)
{
    dev->io.set_pdn(dev->io.ctx, false);
    dev->powered = false;
}

tas5825m_err_t tas5825m_power_up(tas5825m_t *dev)
{
    if (dev == NULL) {
        return TAS5825M_ERR_ARG;
    }
    if (dev->powered) {
        return TAS5825M_OK;
    }

    dev->io.set_pdn(dev->io.ctx, true);
    delay(dev, TAS5825M_PDN_SETTLE_MS);

    /* && stops at the first failed write: no I2C after an error. */
    bool ok = wr(dev, TAS5825M_REG_PAGE, 0)
           && wr(dev, TAS5825M_REG_BOOK, 0)
           && wr(dev, TAS5825M_REG_DIG_VOL, TAS5825M_VOL_MUTE)
           && wr(dev, TAS5825M_REG_DEVICE_CTRL2, TAS5825M_CTRL2_HIZ);
    if (ok) {
        delay(dev, TAS5825M_DSP_BOOT_MS);
        ok = wr(dev, TAS5825M_REG_DEVICE_CTRL2, TAS5825M_CTRL2_PLAY)
          && wr(dev, TAS5825M_REG_FAULT_CLEAR, TAS5825M_FAULT_CLEAR_ALL);
    }
    if (!ok) {
        cut_power(dev);
        return TAS5825M_ERR_IO;
    }
    dev->powered = true;
    return TAS5825M_OK;
}

tas5825m_err_t tas5825m_power_down(tas5825m_t *dev)
{
    if (dev == NULL) {
        return TAS5825M_ERR_ARG;
    }
    if (!dev->powered) {
        cut_power(dev);
        return TAS5825M_OK;
    }

    const bool ok = wr(dev, TAS5825M_REG_DEVICE_CTRL2, TAS5825M_CTRL2_HIZ);
    if (ok) {
        delay(dev, TAS5825M_STOP_MS);
    }
    cut_power(dev); /* even after an I2C error: a hard stop beats a stuck amplifier */
    return ok ? TAS5825M_OK : TAS5825M_ERR_IO;
}

tas5825m_err_t tas5825m_set_volume(tas5825m_t *dev, uint8_t code)
{
    if (dev == NULL) {
        return TAS5825M_ERR_ARG;
    }
    if (!dev->powered) {
        return TAS5825M_ERR_STATE;
    }
    if (code < TAS5825M_VOL_0DB) {
        code = TAS5825M_VOL_0DB; /* never boost */
    }
    return wr(dev, TAS5825M_REG_DIG_VOL, code) ? TAS5825M_OK : TAS5825M_ERR_IO;
}

tas5825m_err_t tas5825m_read_faults(tas5825m_t *dev, tas5825m_faults_t *out)
{
    if (dev == NULL || out == NULL) {
        return TAS5825M_ERR_ARG;
    }
    if (!dev->powered) {
        return TAS5825M_ERR_STATE;
    }

    tas5825m_faults_t f;
    const bool ok = dev->io.read_reg(dev->io.ctx, TAS5825M_REG_CHAN_FAULT, &f.chan) == 0
                 && dev->io.read_reg(dev->io.ctx, TAS5825M_REG_GLOBAL_FAULT1, &f.global1) == 0
                 && dev->io.read_reg(dev->io.ctx, TAS5825M_REG_GLOBAL_FAULT2, &f.global2) == 0
                 && dev->io.read_reg(dev->io.ctx, TAS5825M_REG_WARNING, &f.warning) == 0;
    if (!ok) {
        return TAS5825M_ERR_IO;
    }
    *out = f;
    return TAS5825M_OK;
}

tas5825m_err_t tas5825m_clear_faults(tas5825m_t *dev)
{
    if (dev == NULL) {
        return TAS5825M_ERR_ARG;
    }
    if (!dev->powered) {
        return TAS5825M_ERR_STATE;
    }
    return wr(dev, TAS5825M_REG_FAULT_CLEAR, TAS5825M_FAULT_CLEAR_ALL) ? TAS5825M_OK : TAS5825M_ERR_IO;
}

bool tas5825m_is_powered(const tas5825m_t *dev)
{
    return dev != NULL && dev->powered;
}

bool tas5825m_faults_any(const tas5825m_faults_t *faults)
{
    return faults != NULL && (faults->chan != 0 || faults->global1 != 0 || faults->global2 != 0);
}
