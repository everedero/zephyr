/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Silly demonstration: try to generate a tone at 48 kHz using the Zephyr
 * "standard" DAC driver, pacing each sample with k_sleep(K_USEC(20)).
 * k_sleep() has coarser-than-microsecond tick granularity in practice, so
 * the achieved sample rate is astoundingly far from 48 kHz. A custom trace
 * event is emitted at every loop iteration.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/tracing/tracing.h>

#include "sine.h"

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define DAC_NODE        DT_PHANDLE(ZEPHYR_USER_NODE, dac)
#define DAC_CHANNEL_ID  DT_PROP(ZEPHYR_USER_NODE, dac_channel_id)
#define DAC_RESOLUTION  DT_PROP(ZEPHYR_USER_NODE, dac_resolution)

/* Nominal sample period if the sleep were accurate: 1/48 kHz ~ 20us */
#define SAMPLE_PERIOD_USEC 20U

static const struct device *const dac_dev = DEVICE_DT_GET(DAC_NODE);

static const struct dac_channel_cfg dac_ch_cfg = {
	.channel_id = DAC_CHANNEL_ID,
	.resolution = DAC_RESOLUTION,
	.buffered = true,
};

int main(void)
{
	uint32_t sample_index;

	if (!device_is_ready(dac_dev)) {
		return 0;
	}

	int ret = dac_channel_setup(dac_dev, &dac_ch_cfg);

	if (ret != 0) {
		return 0;
	}

	sample_index = 0;

	while (1) {
		/* Custom trace event at every loop iteration. */
		sys_trace_named_event("sample", sample_index, 1);

		uint16_t value =
			wave_table[sample_index % ARRAY_SIZE(wave_table)];

		ret = dac_write_value(dac_dev, DAC_CHANNEL_ID, value);
		if (ret != 0) {
			return 0;
		}

//		sys_trace_named_event("sample", sample_index, 0);
		k_sleep(K_USEC(SAMPLE_PERIOD_USEC));

		sample_index++;
	}

	return 0;
}
