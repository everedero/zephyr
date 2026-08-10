/*
 * Copyright (c) 2026 Draeger
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdint.h>

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(codec_tone);

/* M_PI is not exposed by picolibc under -std=c17. */
#define TWO_PI 6.28318530717958647692f

/*
 * The codec API has no capability query, so the only way to learn what a codec
 * accepts is to offer it every combination and look at the return code.
 */
static const audio_pcm_rate_t rates[] = {
	AUDIO_PCM_RATE_8K,  AUDIO_PCM_RATE_11P025K, AUDIO_PCM_RATE_16K, AUDIO_PCM_RATE_22P05K,
	AUDIO_PCM_RATE_24K, AUDIO_PCM_RATE_32K,     AUDIO_PCM_RATE_44P1K, AUDIO_PCM_RATE_48K,
	AUDIO_PCM_RATE_96K, AUDIO_PCM_RATE_192K,
};

static const audio_pcm_width_t widths[] = {
	AUDIO_PCM_WIDTH_16_BITS,
	AUDIO_PCM_WIDTH_20_BITS,
	AUDIO_PCM_WIDTH_24_BITS,
	AUDIO_PCM_WIDTH_32_BITS,
};

/* Word-aligned so the codec or its DMA can read it efficiently. */
static uint8_t __aligned(4) block[CONFIG_SAMPLE_BLOCK_SIZE];

static K_SEM_DEFINE(block_done, 0, 1);

/* Radians, carried across blocks so the tone has no discontinuity at the seam. */
static float phase;

/* Bytes the codec spends on one sample of the given width. */
static size_t container_size(audio_pcm_width_t width)
{
	return DIV_ROUND_UP((size_t)width, 8U);
}

/*
 * Largest whole number of samples that fits in the configured block. Only
 * matters for the 3-byte containers, where CONFIG_SAMPLE_BLOCK_SIZE is
 * unlikely to divide evenly: the codec is configured with this reduced size so
 * that every write is a full block. A short write would leave the driver to
 * pad the tail, which is an audible click.
 */
static size_t block_bytes(audio_pcm_width_t width)
{
	size_t bytes = container_size(width);

	return (CONFIG_SAMPLE_BLOCK_SIZE / bytes) * bytes;
}

/*
 * Fill one block with the tone. The sample is a signed width-bit value stored
 * little-endian and sign-extended over the whole container, so a 20-bit sample
 * sits sign-extended in three bytes.
 */
static void fill_block(audio_pcm_rate_t rate, audio_pcm_width_t width)
{
	size_t bytes = container_size(width);
	size_t samples = block_bytes(width) / bytes;
	int32_t full_scale = (int32_t)(BIT64((unsigned int)width - 1U) - 1U);
	float step = TWO_PI * (float)CONFIG_SAMPLE_TONE_FREQ_HZ / (float)rate;

	for (size_t i = 0; i < samples; i++) {
		int32_t value = (int32_t)(sinf(phase) * (float)full_scale);

		for (size_t b = 0; b < bytes; b++) {
			block[(i * bytes) + b] = (uint8_t)(value >> (8U * b));
		}

		phase += step;
		if (phase >= TWO_PI) {
			phase -= TWO_PI;
		}
	}
}

static void tx_done(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	/* Keep the callback empty: it runs in the codec's interrupt, and this
	 * sample is also used to trace that interrupt. Generating the tone here
	 * would put an FPU user inside every measurement.
	 */
	k_sem_give(&block_done);
}

static int configure(const struct device *dev, audio_pcm_rate_t rate, audio_pcm_width_t width)
{
	struct audio_codec_cfg cfg = {
		.dai_type = AUDIO_DAI_TYPE_PCM,
		.dai_cfg.pcm.dir = AUDIO_DAI_DIR_TX,
		.dai_cfg.pcm.pcm_width = width,
		.dai_cfg.pcm.channels = 1,
		.dai_cfg.pcm.block_size = block_bytes(width),
		.dai_cfg.pcm.samplerate = rate,
	};

	return audio_codec_configure(dev, &cfg);
}

static void play(const struct device *dev, audio_pcm_rate_t rate, audio_pcm_width_t width)
{
	int ret = configure(dev, rate, width);

	if (ret < 0) {
		LOG_ERR("configure %u Hz / %u bit failed (%d)", (unsigned int)rate,
			(unsigned int)width, ret);
		return;
	}

	/* TX only: no RX callback, this sample never captures. */
	if (audio_codec_register_done_callback(dev, tx_done, NULL, NULL, NULL) < 0) {
		LOG_ERR("could not register tx callback");
		return;
	}

	if ((unsigned int)rate < 2U * CONFIG_SAMPLE_TONE_FREQ_HZ) {
		LOG_WRN("%u Hz is below the Nyquist rate for a %d Hz tone, expect aliasing",
			(unsigned int)rate, CONFIG_SAMPLE_TONE_FREQ_HZ);
	}

	LOG_INF("playing %d Hz at %u Hz / %u bit (%zu bytes per block)",
		CONFIG_SAMPLE_TONE_FREQ_HZ, (unsigned int)rate, (unsigned int)width,
		block_bytes(width));

	phase = 0.0f;
	k_sem_reset(&block_done);
	audio_codec_start(dev, AUDIO_DAI_DIR_TX);

	int64_t deadline = k_uptime_get() + CONFIG_SAMPLE_STEP_MS;

	while (k_uptime_get() < deadline) {
		if (k_sem_take(&block_done, K_MSEC(500)) < 0) {
			LOG_ERR("codec stopped asking for data");
			break;
		}

		fill_block(rate, width);

		ret = audio_codec_write(dev, block, block_bytes(width));
		if (ret < 0) {
			LOG_ERR("write failed (%d)", ret);
			break;
		}
	}

	audio_codec_stop(dev, AUDIO_DAI_DIR_TX);
}

static void probe(const struct device *dev, bool supported[ARRAY_SIZE(rates)][ARRAY_SIZE(widths)])
{
	LOG_INF("probing codec capabilities");
	LOG_INF("   rate    16   20   24   32");

	for (size_t r = 0; r < ARRAY_SIZE(rates); r++) {
		char row[32];
		size_t pos = 0;

		for (size_t w = 0; w < ARRAY_SIZE(widths); w++) {
			supported[r][w] = configure(dev, rates[r], widths[w]) == 0;
			pos += snprintk(&row[pos], sizeof(row) - pos, "%5s",
					supported[r][w] ? "ok" : "-");
		}

		LOG_INF("%7u %s", (unsigned int)rates[r], row);
	}
}

int main(void)
{
#if !DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(codec0))
	LOG_ERR("no codec0 alias on this board");
	return 0;
#else
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(codec0));
	static bool supported[ARRAY_SIZE(rates)][ARRAY_SIZE(widths)];

	LOG_INF("Audio codec tone sample");

	if (!device_is_ready(dev)) {
		LOG_ERR("codec device is not ready");
		return -EBUSY;
	}

	probe(dev, supported);

	unsigned int played = 0;

	for (size_t r = 0; r < ARRAY_SIZE(rates); r++) {
		for (size_t w = 0; w < ARRAY_SIZE(widths); w++) {
			if (supported[r][w]) {
				play(dev, rates[r], widths[w]);
				played++;
			}
		}
	}

	if (played == 0U) {
		LOG_ERR("codec accepted no rate/width combination");
	}

	LOG_INF("Exiting");
	return 0;
#endif
}
