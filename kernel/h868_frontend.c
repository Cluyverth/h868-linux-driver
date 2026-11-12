// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * H868 ISDB-T frontend.
 *
 * The demodulator's Toshiba-family register map is not sufficient evidence
 * to identify a particular TC905xx part. Do not replace this program with
 * another Toshiba demodulator's init table.
 * The bridge must power the board before attaching/initializing this frontend.
 */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <media/dvbdev.h>

#include "h868.h"
#include "h868_metrics.h"
#include "h868_ofdm.h"
#include "tda18271.h"

#define H868_DEMOD_ADDR		0x10
#define H868_TUNER_ADDR		0x60
#define H868_BANDWIDTH_HZ	6000000U
#define H868_FREQUENCY_MIN_HZ	45000000U
#define H868_FREQUENCY_MAX_HZ	864000000U

struct h868_reg {
	u8 reg;
	u8 value;
};

struct h868_frontend {
	struct dvb_frontend fe;
	struct i2c_adapter *demod;
	struct mutex lock;
	u32 frequency;
	bool initialized;
	bool tuned;
};

/* Each pair is a separate register transaction, not a contiguous burst. */
static const struct h868_reg h868_cold_init[] = {
	{ 0x01, 0xd0 }, { 0x16, 0x09 }, { 0x31, 0x0f }, { 0x32, 0xc3 },
	{ 0x38, 0x00 }, { 0x39, 0x0f }, { 0x34, 0x0f }, { 0x20, 0x00 },
	{ 0x22, 0x80 }, { 0x23, 0x38 }, { 0xef, 0x01 }, { 0x04, 0x00 },
	{ 0x03, 0x00 }, { 0x31, 0x0f }, { 0x32, 0x84 },
};

/* ISDB-T is dispatched through dvbt_6 by the Linux TDA18271 driver. */
static struct tda18271_std_map h868_tuner_std_map = {
	.dvbt_6 = {
		.if_freq = 3893,
		.agc_mode = 3,
		.std = 0,
		.fm_rfn = 0,
		.if_lvl = 7,
		.rfagc_top = 0x37,
	},
};

static int h868_demod_write(struct h868_frontend *state, u8 reg, u8 value)
{
	u8 data[] = { reg, value };
	struct i2c_msg msg = {
		.addr = H868_DEMOD_ADDR,
		.buf = data,
		.len = sizeof(data),
	};
	int ret;

	ret = i2c_transfer(state->demod, &msg, 1);
	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

static int h868_demod_read(struct h868_frontend *state, u8 reg, u8 *value)
{
	struct i2c_msg msgs[] = {
		{
			.addr = H868_DEMOD_ADDR,
			.buf = &reg,
			.len = 1,
		},
		{
			.addr = H868_DEMOD_ADDR,
			.flags = I2C_M_RD,
			.buf = value,
			.len = 1,
		},
	};
	int ret;

	ret = i2c_transfer(state->demod, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static void h868_invalidate_metrics(struct dvb_frontend *fe)
{
	struct dtv_frontend_properties *properties = &fe->dtv_property_cache;

	properties->strength.len = 1;
	properties->strength.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	properties->cnr.len = 1;
	properties->cnr.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
}

static int h868_update_metrics(struct h868_frontend *state, bool locked)
{
	struct dtv_frontend_properties *properties = &state->fe.dtv_property_cache;
	u32 quality = 0;
	u8 value;
	int i, ret;

	ret = h868_demod_read(state, 0x82, &value);
	if (ret)
		return ret;
	properties->strength.stat[0].uvalue =
		h868_strength_percent(value) * 65535U / 100;
	properties->strength.stat[0].scale = FE_SCALE_RELATIVE;
	if (!locked)
		return 0;

	for (i = 0; i < 3; i++) {
		ret = h868_demod_read(state, 0x8b + i, &value);
		if (ret)
			return ret;
		quality = quality << 8 | value;
	}
	if (quality) {
		properties->cnr.stat[0].uvalue =
			h868_quality_percent(quality) * 65535U / 100;
		properties->cnr.stat[0].scale = FE_SCALE_RELATIVE;
	}
	return 0;
}

/* Caller holds state->lock. Always use the hardware-validated cold program. */
static int h868_demod_init(struct h868_frontend *state)
{
	unsigned int i;
	int ret;

	state->initialized = false;
	state->tuned = false;
	for (i = 0; i < ARRAY_SIZE(h868_cold_init); i++) {
		ret = h868_demod_write(state, h868_cold_init[i].reg,
				       h868_cold_init[i].value);
		if (ret)
			return ret;
	}
	state->initialized = true;
	return 0;
}

static int h868_frontend_init(struct dvb_frontend *fe)
{
	struct h868_frontend *state = fe->demodulator_priv;
	int ret;

	mutex_lock(&state->lock);
	ret = h868_demod_init(state);
	mutex_unlock(&state->lock);
	return ret;
}

static int h868_frontend_sleep(struct dvb_frontend *fe)
{
	struct h868_frontend *state = fe->demodulator_priv;
	int ret = 0;

	mutex_lock(&state->lock);
	/* Hold acquisition in reset. Board power and tuner standby are external. */
	if (state->initialized)
		ret = h868_demod_write(state, 0x01, 0xd0);
	state->initialized = false;
	state->tuned = false;
	h868_invalidate_metrics(fe);
	mutex_unlock(&state->lock);
	return ret;
}

static int h868_set_frontend(struct dvb_frontend *fe)
{
	struct h868_frontend *state = fe->demodulator_priv;
	struct dtv_frontend_properties *c = &fe->dtv_property_cache;
	int ret;

	if (c->delivery_system != SYS_ISDBT ||
	    c->frequency < H868_FREQUENCY_MIN_HZ ||
	    c->frequency > H868_FREQUENCY_MAX_HZ ||
	    (c->bandwidth_hz && c->bandwidth_hz != H868_BANDWIDTH_HZ))
		return -EINVAL;

	/* The initialization program supports full-band television, not ISDB-Tsb. */
	if (c->isdbt_sb_mode != 0 && c->isdbt_sb_mode != 0xff)
		return -EOPNOTSUPP;
	/* No layer-selection register program has been established for H868. */
	if (c->isdbt_layer_enabled != 0x07)
		return -EOPNOTSUPP;
	if (!fe->ops.tuner_ops.set_params)
		return -ENODEV;

	mutex_lock(&state->lock);
	state->tuned = false;
	h868_invalidate_metrics(fe);
	if (!state->initialized) {
		ret = h868_demod_init(state);
		if (ret)
			goto out;
	}

	/* Zero means AUTO in DVBv5. This board's validated bandwidth is 6 MHz. */
	c->bandwidth_hz = H868_BANDWIDTH_HZ;
	ret = fe->ops.tuner_ops.set_params(fe);
	if (ret)
		goto out;
	ret = h868_demod_write(state, 0x01, 0x50);
	if (!ret) {
		state->frequency = c->frequency;
		state->tuned = true;
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

static int h868_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
	struct h868_frontend *state = fe->demodulator_priv;
	u8 value;
	int ret = 0;

	*status = 0;
	mutex_lock(&state->lock);
	h868_invalidate_metrics(fe);
	if (!state->initialized || !state->tuned)
		goto out;
	ret = h868_demod_read(state, 0x80, &value);
	if (ret)
		goto out;
	/* Signal and lock indications are active low. */
	if (!(value & 0x08))
		*status = FE_HAS_SIGNAL;
	if (!(value & 0x0a))
		*status |= FE_HAS_CARRIER | FE_HAS_VITERBI | FE_HAS_SYNC |
			   FE_HAS_LOCK;
	ret = h868_update_metrics(state, *status & FE_HAS_LOCK);

out:
	mutex_unlock(&state->lock);
	return ret;
}

static enum dvbfe_algo h868_get_frontend_algo(struct dvb_frontend *fe)
{
	return DVBFE_ALGO_HW;
}

static int h868_tune(struct dvb_frontend *fe, bool re_tune,
		     unsigned int mode_flags, unsigned int *delay,
		     enum fe_status *status)
{
	int ret;

	*status = 0;
	*delay = msecs_to_jiffies(100);
	if (re_tune) {
		ret = h868_set_frontend(fe);
		if (ret)
			return ret;
	}
	return h868_read_status(fe, status);
}

static int h868_get_frontend(struct dvb_frontend *fe,
			     struct dtv_frontend_properties *props)
{
	struct h868_frontend *state = fe->demodulator_priv;
	u8 status, ofdm;
	int ret = 0;

	mutex_lock(&state->lock);
	props->delivery_system = SYS_ISDBT;
	props->bandwidth_hz = H868_BANDWIDTH_HZ;
	if (state->tuned)
		props->frequency = state->frequency;
	props->transmission_mode = TRANSMISSION_MODE_AUTO;
	props->guard_interval = GUARD_INTERVAL_AUTO;
	/* TMCC decoding and calibrated error statistics are not established. */
	props->pre_bit_error.len = 1;
	props->pre_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	props->pre_bit_count.len = 1;
	props->pre_bit_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	props->post_bit_error.len = 1;
	props->post_bit_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	props->post_bit_count.len = 1;
	props->post_bit_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	props->block_error.len = 1;
	props->block_error.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	props->block_count.len = 1;
	props->block_count.stat[0].scale = FE_SCALE_NOT_AVAILABLE;
	if (state->initialized && state->tuned) {
		ret = h868_demod_read(state, 0x80, &status);
		if (ret)
			goto out;
		if (!(status & 0x0a)) {
			ret = h868_demod_read(state, 0xb0, &ofdm);
			if (ret)
				goto out;
			props->transmission_mode = h868_transmission_mode(ofdm);
			props->guard_interval = h868_guard_interval(ofdm);
		}
	}
out:
	mutex_unlock(&state->lock);
	return ret;
}

static void h868_frontend_release(struct dvb_frontend *fe)
{
	kfree(fe->demodulator_priv);
}

static const struct dvb_frontend_ops h868_frontend_ops = {
	.delsys = { SYS_ISDBT },
	.info = {
		.name = "H868 ISDB-T",
		.frequency_min_hz = H868_FREQUENCY_MIN_HZ,
		.frequency_max_hz = H868_FREQUENCY_MAX_HZ,
		.frequency_stepsize_hz = 62500,
		.caps = FE_CAN_INVERSION_AUTO | FE_CAN_FEC_1_2 | FE_CAN_FEC_2_3 |
			FE_CAN_FEC_3_4 | FE_CAN_FEC_5_6 | FE_CAN_FEC_7_8 |
			FE_CAN_FEC_AUTO | FE_CAN_QPSK | FE_CAN_QAM_16 |
			FE_CAN_QAM_64 | FE_CAN_QAM_AUTO |
			FE_CAN_TRANSMISSION_MODE_AUTO | FE_CAN_GUARD_INTERVAL_AUTO |
			FE_CAN_HIERARCHY_AUTO,
	},
	.release = h868_frontend_release,
	.init = h868_frontend_init,
	.sleep = h868_frontend_sleep,
	.get_frontend_algo = h868_get_frontend_algo,
	.tune = h868_tune,
	.set_frontend = h868_set_frontend,
	.get_frontend = h868_get_frontend,
	.read_status = h868_read_status,
};

struct dvb_frontend *h868_frontend_attach(struct i2c_adapter *demod,
					 struct i2c_adapter *tuner)
{
	struct tda18271_config tuner_config = {
		.std_map = &h868_tuner_std_map,
		.role = TDA18271_MASTER,
		.gate = TDA18271_GATE_DIGITAL,
		.small_i2c = TDA18271_39_BYTE_CHUNK_INIT,
		.delay_cal = 1,
	};
	struct h868_frontend *state;

	if (!demod || !tuner ||
	    !i2c_check_functionality(demod, I2C_FUNC_I2C) ||
	    !i2c_check_functionality(tuner, I2C_FUNC_I2C))
		return NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	state->demod = demod;
	mutex_init(&state->lock);
	state->fe.ops = h868_frontend_ops;
	state->fe.demodulator_priv = state;
	h868_invalidate_metrics(&state->fe);

	/* A physical tuner ID read validates C1/C2, not the demodulator model. */
	if (!dvb_attach(tda18271_attach, &state->fe, H868_TUNER_ADDR,
			tuner, &tuner_config)) {
		kfree(state);
		return NULL;
	}

	return &state->fe;
}
