/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef H868_OFDM_H
#define H868_OFDM_H

#include <linux/dvb/frontend.h>

static inline enum fe_transmit_mode h868_transmission_mode(unsigned int raw)
{
	static const enum fe_transmit_mode modes[] = {
		TRANSMISSION_MODE_2K, TRANSMISSION_MODE_4K,
		TRANSMISSION_MODE_8K, TRANSMISSION_MODE_AUTO,
	};

	return modes[(raw >> 6) & 3];
}

static inline enum fe_guard_interval h868_guard_interval(unsigned int raw)
{
	static const enum fe_guard_interval intervals[] = {
		GUARD_INTERVAL_1_32, GUARD_INTERVAL_1_16,
		GUARD_INTERVAL_1_8, GUARD_INTERVAL_1_4,
	};

	return intervals[(raw >> 4) & 3];
}

#endif
