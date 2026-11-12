// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <stdio.h>

#include "../kernel/h868_ofdm.h"

int main(void)
{
	assert(h868_transmission_mode(0x00) == TRANSMISSION_MODE_2K);
	assert(h868_transmission_mode(0x40) == TRANSMISSION_MODE_4K);
	assert(h868_transmission_mode(0xa9) == TRANSMISSION_MODE_8K);
	assert(h868_transmission_mode(0xc0) == TRANSMISSION_MODE_AUTO);

	assert(h868_guard_interval(0x00) == GUARD_INTERVAL_1_32);
	assert(h868_guard_interval(0x10) == GUARD_INTERVAL_1_16);
	assert(h868_guard_interval(0xa9) == GUARD_INTERVAL_1_8);
	assert(h868_guard_interval(0x30) == GUARD_INTERVAL_1_4);
	assert(h868_guard_interval(0xff) == GUARD_INTERVAL_1_4);
	puts("h868 OFDM decoding: ok");
	return 0;
}
