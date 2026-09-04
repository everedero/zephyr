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
#include <zephyr/sys/printk.h>
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
	uint64_t last_report_us;
	uint64_t samples_since_report;
	uint64_t peak_loop_us;
	uint32_t sample_index;

	if (!device_is_ready(dac_dev)) {
		printk("silly: DAC device not ready\n");
		return 0;
	}

	int ret = dac_channel_setup(dac_dev, &dac_ch_cfg);

	if (ret != 0) {
		printk("silly: DAC channel setup failed %d\n", ret);
		return 0;
	}

	printk("silly: k_sleep-paced DAC tone on channel %d, ~20us/sample\n",
	       DAC_CHANNEL_ID);

	peak_loop_us = 0;
	samples_since_report = 0;
	sample_index = 0;
	last_report_us = k_uptime_get() * 1000U;

	while (1) {
		uint64_t loop_start_us = k_cyc_to_us_near64(k_cycle_get_32());
		uint16_t value =
			wave_table[sample_index % ARRAY_SIZE(wave_table)];

		ret = dac_write_value(dac_dev, DAC_CHANNEL_ID, value);
		if (ret != 0) {
			printk("silly: dac_write_value failed %d\n", ret);
			return 0;
		}

		/* Custom trace event at every loop iteration. */
		sys_trace_named_event("sample", sample_index, value);

		k_sleep(K_USEC(SAMPLE_PERIOD_USEC));

		uint64_t loop_us = k_cyc_to_us_near64(k_cycle_get_32())
				   - loop_start_us;

		if (loop_us > peak_loop_us) {
			peak_loop_us = loop_us;
		}

		samples_since_report++;
		uint64_t now_us = k_uptime_get() * 1000U;

		if (now_us - last_report_us >= 1000000U) {
			uint32_t rate = (uint32_t)(samples_since_report * 1000000U /
						   (now_us - last_report_us));

			printk("silly: %u samples/s (peak loop %.2fus)\n",
			       rate, (double)peak_loop_us);

			samples_since_report = 0;
			last_report_us = now_us;
			peak_loop_us = 0;
		}

		sample_index++;
	}

	return 0;
}
