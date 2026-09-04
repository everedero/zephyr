/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Silly demonstration: try to generate a tone at 48 kHz using the Zephyr
 * "standard" DAC driver, pacing each sample with k_busy_wait(20). Unlike the
 * k_sleep variant, k_busy_wait() busy-spins for the requested microseconds,
 * so the achieved sample rate is much closer to the nominal 48 kHz. A
 * custom trace event is emitted at every loop iteration.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/tracing/tracing.h>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define DAC_NODE        DT_PHANDLE(ZEPHYR_USER_NODE, dac)
#define DAC_CHANNEL_ID  DT_PROP(ZEPHYR_USER_NODE, dac_channel_id)
#define DAC_RESOLUTION  DT_PROP(ZEPHYR_USER_NODE, dac_resolution)

#define DAC_MAX_VAL     BIT(DAC_RESOLUTION)
#define WAVE_TABLE_LEN  256

/* Nominal sample period: 1/48 kHz ~ 20us */
#define SAMPLE_PERIOD_USEC 20U

/* 8-bit signed sine table (approx), one cycle, scaled to +/-1000. */
static const int16_t sin_table[WAVE_TABLE_LEN] = {
	0, 25, 50, 74, 98, 122, 146, 169, 191, 213, 233, 253, 271,
	288, 304, 320, 333, 346, 357, 367, 375, 383, 388, 393, 396,
	398, 398, 398, 396, 392, 387, 381, 374, 366, 356, 346, 334,
	321, 308, 294, 279, 264, 248, 231, 215, 198, 180, 163, 145,
	127, 110, 92, 75, 58, 42, 26, 11, -4, -18, -31, -44, -57,
	-69, -80, -90, -100, -108, -116, -123, -129, -134, -138, -141,
	-143, -144, -145, -144, -143, -141, -138, -134, -129, -124,
	-118, -112, -105, -97, -89, -81, -72, -63, -54, -45, -36, -27,
	-18, -9, 0, 9, 18, 27, 36, 45, 54, 63, 72, 81, 89, 97, 105,
	112, 118, 124, 129, 134, 138, 141, 143, 144, 145, 144, 143,
	141, 138, 134, 129, 123, 116, 108, 100, 90, 80, 69, 57, 44,
	31, 18, 4, -11, -26, -42, -58, -75, -92, -110, -127, -145,
	-163, -180, -198, -215, -231, -248, -264, -279, -294, -308,
	-321, -334, -346, -356, -366, -374, -381, -387, -392, -396,
	-398, -398, -398, -396, -393, -388, -383, -375, -367, -357,
	-346, -333, -320, -304, -288, -271, -253, -233, -213, -191,
	-169, -146, -122, -98, -74, -50, -25, 0
};

static const struct device *const dac_dev = DEVICE_DT_GET(DAC_NODE);

static const struct dac_channel_cfg dac_ch_cfg = {
	.channel_id = DAC_CHANNEL_ID,
	.resolution = DAC_RESOLUTION,
	.buffered = true,
};

/* Precomputed single cycle of a sine wave covering the full DAC range. */
static uint16_t wave_table[WAVE_TABLE_LEN];

static void build_wave_table(void)
{
	for (uint32_t i = 0; i < WAVE_TABLE_LEN; i++) {
		/* Full-scale offset plus half-scale sine. */
		int32_t center = (DAC_MAX_VAL - 1) / 2;

		wave_table[i] = (uint16_t)(center
					   + (center * sin_table[i]) / 1000);
	}
}

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

	build_wave_table();

	printk("silly: k_busy_wait-paced DAC tone on channel %d, ~20us/sample\n",
	       DAC_CHANNEL_ID);

	peak_loop_us = 0;
	samples_since_report = 0;
	sample_index = 0;
	last_report_us = k_uptime_get() * 1000U;

	while (1) {
		uint64_t loop_start_us = k_cyc_to_us_near64(k_cycle_get_32());
		uint16_t value = wave_table[sample_index % WAVE_TABLE_LEN];

		ret = dac_write_value(dac_dev, DAC_CHANNEL_ID, value);
		if (ret != 0) {
			printk("silly: dac_write_value failed %d\n", ret);
			return 0;
		}

		/* Custom trace event at every loop iteration. */
		sys_port_trace_silly_audio_sample(sample_index, value);

		k_busy_wait(SAMPLE_PERIOD_USEC);

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
