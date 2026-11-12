// SPDX-License-Identifier: GPL-2.0-or-later
#include <assert.h>
#include <stdio.h>

#include "../kernel/h868_metrics.h"

static void test_strength_boundaries(void)
{
	assert(h868_strength_percent(0) == 100);
	assert(h868_strength_percent(37) == 100);
	assert(h868_strength_percent(38) == 99);
	assert(h868_strength_percent(99) == 38);
	assert(h868_strength_percent(116) == 21);
	assert(h868_strength_percent(117) == 0);
	assert(h868_strength_percent(255) == 0);
}

static void test_quality_boundaries(void)
{
	assert(h868_quality_percent(7248463) == 5);
	assert(h868_quality_percent(7248462) == 7);
	assert(h868_quality_percent(78804) == 50);
	assert(h868_quality_percent(78803) == 54);
	assert(h868_quality_percent(18015) == 81);
	assert(h868_quality_percent(6855) == 100);
	assert(h868_quality_percent(1) == 100);
}

static void test_quality_decreases_with_raw_measurement(void)
{
	unsigned int previous = h868_quality_percent(1);
	unsigned int raw;

	for (raw = 4094; raw <= 0xffffff; raw += 4093) {
		unsigned int current = h868_quality_percent(raw);

		assert(current <= previous);
		previous = current;
	}
}

int main(void)
{
	test_strength_boundaries();
	test_quality_boundaries();
	test_quality_decreases_with_raw_measurement();
	puts("h868 metric conversions: ok");
	return 0;
}
