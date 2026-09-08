/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <sample_usbd.h>

#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(uac2_dac_sample, LOG_LEVEL_INF);

#define SPEAKER_OUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(out_terminal))

#define SAMPLE_RATE            48000
#define SAMPLE_BIT_WIDTH       16
#define NUMBER_OF_CHANNELS     1
#define BYTES_PER_SAMPLE       (SAMPLE_BIT_WIDTH / 8)
#define FS_SAMPLES_PER_SOF     (SAMPLE_RATE / 1000)
#define MAX_SAMPLES_PER_SOF    FS_SAMPLES_PER_SOF
#define MAX_BLOCK_SIZE         (MAX_SAMPLES_PER_SOF * BYTES_PER_SAMPLE)

/* The UAC2 driver needs buffers that are safe for UDC DMA. Use a small
 * number of blocks, enough to cover the isochronous receive pipeline.
 */
#define USB_BUFFERS_COUNT     7
K_MEM_SLAB_DEFINE_STATIC(usb_rx_slab, ROUND_UP(MAX_BLOCK_SIZE, UDC_BUF_GRANULARITY),
			 USB_BUFFERS_COUNT, UDC_BUF_ALIGN);

/* Ring buffer stores samples from USB host before they are written to the DAC.
 * It decouples the bursty isochronous USB reception from the steady sample
 * clock of the DAC output.
 *
 * 48000 Hz / 1000 = 48 samples per SOF, 2 bytes each = 96 bytes per SOF.
 * A 512 byte buffer holds ~5 SOF frames worth of audio.
 */
#define DAC_RING_BUFFER_SIZE   512
RING_BUF_DECLARE(dac_ring, DAC_RING_BUFFER_SIZE);

#define DAC_CHANNEL_ID      1
#define DAC_RESOLUTION      12

static const struct device *const dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac1));

static const struct dac_channel_cfg dac_channel_cfg = {
	.channel_id = DAC_CHANNEL_ID,
	.resolution = DAC_RESOLUTION,
	.buffered = true,
};

static struct k_timer dac_timer;

static bool terminal_enabled;

/* Explicit feedback value in Q10.14 format. For 48 kHz there are nominally
 * 48 samples per Full-Speed SOF frame.
 */
#define FEEDBACK_VALUE (FS_SAMPLES_PER_SOF << 14)

static uint32_t uac2_feedback_cb(const struct device *dev, uint8_t terminal,
				 void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	return FEEDBACK_VALUE;
}

static void uac2_sof_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
}

static void uac2_terminal_update_cb(const struct device *dev, uint8_t terminal,
				    bool enabled, bool microframes,
				    void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(microframes);
	ARG_UNUSED(user_data);

	__ASSERT_NO_MSG(terminal == SPEAKER_OUT_TERMINAL_ID);

	terminal_enabled = enabled;
	LOG_INF("Output terminal %s", enabled ? "enabled" : "disabled");
}

static void *uac2_get_recv_buf(const struct device *dev, uint8_t terminal,
			       uint16_t size, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);
	void *buf = NULL;
	int ret;

	ret = k_mem_slab_alloc(&usb_rx_slab, &buf, K_NO_WAIT);
	if (ret != 0) {
		buf = NULL;
	}

	return buf;
}

static void uac2_data_recv_cb(const struct device *dev, uint8_t terminal,
			      void *buf, uint16_t size, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (!terminal_enabled) {
		k_mem_slab_free(&usb_rx_slab, buf);
		return;
	}

	if (!size) {
		k_mem_slab_free(&usb_rx_slab, buf);
		return;
	}

	/* Copy received PCM samples into the ring buffer. If the ring buffer
	 * overflows, drop the excess samples (simplest possible behavior for a
	 * minimal sample). The DAC keeps emitting the last valid samples.
	 */
	(void)ring_buf_put(&dac_ring, buf, size);

	k_mem_slab_free(&usb_rx_slab, buf);
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal,
				void *buf, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(buf);
	ARG_UNUSED(user_data);
}

static struct uac2_ops usb_audio_ops = {
	.sof_cb = uac2_sof_cb,
	.terminal_update_cb = uac2_terminal_update_cb,
	.get_recv_buf = uac2_get_recv_buf,
	.data_recv_cb = uac2_data_recv_cb,
	.buf_release_cb = uac2_buf_release_cb,
	.feedback_cb = uac2_feedback_cb,
};

static void dac_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	uint8_t sample[BYTES_PER_SAMPLE];
	uint32_t sample_value;
	int ret;
	size_t got;

	/* Pull one sample from the ring buffer. On underrun (empty buffer),
	 * output mid-scale voltage, which corresponds to digital silence in
	 * the unsigned DAC output domain.
	 */
	got = ring_buf_get(&dac_ring, sample, BYTES_PER_SAMPLE);
	if (got < BYTES_PER_SAMPLE) {
		sample_value = 1U << (DAC_RESOLUTION - 1);
	} else {
		/* 16-bit USB PCM samples are signed (two's complement),
		 * little-endian. Convert to unsigned and scale down to the
		 * 12-bit DAC resolution so that a silent sample (0x0000,
		 * signed 0) ends up at mid-scale voltage.
		 */
		sample_value = (uint32_t)((uint16_t)(sample[1] << 8 | sample[0]));
		sample_value ^= 0x8000;
		sample_value >>= (SAMPLE_BIT_WIDTH - DAC_RESOLUTION);
	}

	ret = dac_write_value(dac_dev, DAC_CHANNEL_ID, sample_value);
	if (ret < 0) {
		LOG_ERR("dac_write_value failed: %d", ret);
	}
}

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(uac2_speaker));
	struct usbd_context *sample_usbd;
	int ret;

	if (!device_is_ready(dac_dev)) {
		printk("DAC device %s is not ready\n", dac_dev->name);
		return -ENODEV;
	}

	ret = dac_channel_setup(dac_dev, &dac_channel_cfg);
	if (ret != 0) {
		printk("DAC channel setup failed: %d\n", ret);
		return ret;
	}

	usbd_uac2_set_ops(dev, &usb_audio_ops, NULL);

	sample_usbd = sample_usbd_init_device(NULL);
	if (sample_usbd == NULL) {
		return -ENODEV;
	}

	ret = usbd_enable(sample_usbd);
	if (ret) {
		return ret;
	}

	/* Schedule DAC output at the fixed sample rate. With the system tick set
	 * to 96000 Hz, a period of 2 ticks is exactly 20.83 us, i.e. 48 kHz.
	 */
	k_timer_init(&dac_timer, dac_timer_handler, NULL);
	k_timer_start(&dac_timer, K_NO_WAIT, K_TICKS(2));

	printk("UAC2 DAC sample started\n");
	printk("Sample rate: %d Hz, %d-bit, %d channel(s)\n",
	       SAMPLE_RATE, SAMPLE_BIT_WIDTH, NUMBER_OF_CHANNELS);
	printk("DAC output: PA4 (DAC_OUT1), %d-bit, channel %d\n",
	       DAC_RESOLUTION, DAC_CHANNEL_ID);

	return 0;
}
