/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef H868_METRICS_H
#define H868_METRICS_H

static inline unsigned int h868_strength_percent(unsigned int raw)
{
	if (raw < 38)
		return 100;
	if (raw < 117)
		return 137 - raw;
	return 0;
}

static inline unsigned int h868_quality_percent(unsigned int raw)
{
	static const unsigned int thresholds[] = {
		7248462, 4685229, 3296173, 2416198, 1815312, 1386228,
		1070606, 833547, 652760, 513314, 404834, 319904, 253101,
		200382, 158687, 125672, 99517, 78803, 62408, 49446,
		39212, 31142, 24785, 19782, 15846, 12748, 10307, 8380,
		6855,
	};
	unsigned int low = 0, high = sizeof(thresholds) / sizeof(thresholds[0]);
	unsigned int quality;

	while (low < high) {
		unsigned int middle = low + (high - low) / 2;

		if (raw > thresholds[middle])
			high = middle;
		else
			low = middle + 1;
	}
	quality = low < sizeof(thresholds) / sizeof(thresholds[0]) ? low + 2 : 30;
	if (quality < 19)
		return quality * 50 / 19;
	return 50 + (quality - 19) * 50 / 11;
}

#endif
