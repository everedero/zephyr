/*
 * Copyright 2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/audio/codec.h>
#include <zephyr/logging/log.h>
#include "sine.h"
LOG_MODULE_REGISTER(codec_sample);

#define AUDIO_BLOCK_SIZE 218
#define SPEAKER_VOL      15

static bool loopback;
static uint8_t *audio_data_p;

/* The playback loop only emits whole AUDIO_BLOCK_SIZE chunks, so the buffer must
 * be an exact multiple of the block or its tail would never be played.
 */
BUILD_ASSERT(sizeof(wave_table) % AUDIO_BLOCK_SIZE == 0,
	     "PCM buffer must be a whole number of AUDIO_BLOCK_SIZE blocks");

static void tx_done(const struct device *dev, void *user_data)
{
	if (!loopback) {
		size_t offset = (size_t)(audio_data_p - wave_table);
		size_t remaining = sizeof(wave_table) - offset;

		if (remaining < AUDIO_BLOCK_SIZE) {
			audio_data_p = wave_table;
		}
		audio_codec_write(dev, audio_data_p, AUDIO_BLOCK_SIZE);
		audio_data_p += AUDIO_BLOCK_SIZE;
	}
}
static void rx_done(const struct device *dev, uint8_t *buf, uint32_t len, void *user_data)
{
	if (loopback) {
		audio_codec_write(dev, buf, AUDIO_BLOCK_SIZE);
	}
}

int main(void)
{
	static const struct device *dev;
	audio_property_value_t val;
	struct audio_codec_cfg cfg = {
		.dai_type = AUDIO_DAI_TYPE_PCM,
		.dai_cfg.pcm.dir = AUDIO_DAI_DIR_TX,
		.dai_cfg.pcm.pcm_width = AUDIO_PCM_WIDTH_16_BITS,
		.dai_cfg.pcm.channels = 1,
		.dai_cfg.pcm.block_size = AUDIO_BLOCK_SIZE,
		.dai_cfg.pcm.samplerate = AUDIO_PCM_RATE_48K,
	};

	LOG_INF("Audio codec sample");
#if DT_NODE_HAS_STATUS_OKAY(DT_ALIAS(codec0))
	dev = DEVICE_DT_GET(DT_ALIAS(codec0));
#else
	LOG_ERR("No audio codec on this board. Skipping audio test.\n");
	return 0;
#endif
	if (!device_is_ready(dev)) {
		LOG_ERR("codec device is not ready\n");
		return -EBUSY;
	}
	LOG_INF("codec device is ready");
	k_sleep(K_MSEC(1000));

	LOG_INF("codec playback example");
	loopback = false;
	audio_data_p = wave_table;
	if (audio_codec_configure(dev, &cfg) < 0) {
		LOG_ERR("configure codec error\n");
		return -EIO;
	}
	if (audio_codec_register_done_callback(dev, tx_done, NULL, rx_done, NULL) < 0) {
		LOG_ERR("could not register codec callbacks\n");
		return -EIO;
	}
	audio_codec_start(dev, AUDIO_DAI_DIR_TX);
	LOG_INF("playback started");
	val.vol = SPEAKER_VOL;
	if (audio_codec_set_property(dev, AUDIO_PROPERTY_OUTPUT_VOLUME, 0, val) < 0) {
		LOG_ERR("could not set volume\n");
		return -EIO;
	}
	k_sleep(K_MSEC(15000));
	audio_codec_stop(dev, AUDIO_DAI_DIR_TX);
	LOG_INF("codec transfer stopped");

	LOG_INF("Exiting");
	return 0;
}
