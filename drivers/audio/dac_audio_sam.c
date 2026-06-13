/*
 * Copyright (c) 2026 Eve Redero
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT atmel_sam_dacc_audio

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/codec.h>
#include <zephyr/drivers/clock_control/atmel_sam_pmc.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <soc.h>

LOG_MODULE_REGISTER(dac_audio_sam, CONFIG_AUDIO_CODEC_LOG_LEVEL);

struct dac_audio_sam_cfg {
	Dacc *dacc;
	Tc   *tc;
	uint32_t tc_ch;
	uint32_t trig_sel;
	struct atmel_sam_pmc_config dacc_clock_cfg;
	/* TC has 3 channels, each with its own PMC clock entry */
	struct atmel_sam_pmc_config tc_clock_cfgs[3];
	const struct pinctrl_dev_config *pcfg;
	void (*irq_config)(void);
};

struct dac_audio_sam_data {
	audio_codec_tx_done_callback_t tx_cb;
	void *tx_cb_user_data;
	bool configured;
	uint8_t active_half;
	size_t half_samples;
	uint16_t dbl_buf[2 * CONFIG_DAC_AUDIO_SAM_MAX_BLOCK_SAMPLES];
};

static void dac_audio_sam_isr(const struct device *dev)
{
	const struct dac_audio_sam_cfg *cfg = dev->config;
	struct dac_audio_sam_data *data = dev->data;
	Dacc *dacc = cfg->dacc;

	uint32_t sr = dacc->DACC_ISR & dacc->DACC_IMR;

	if (sr & DACC_ISR_TXBUFE) {
		/* Buffer underrun: PDC exhausted all data, stop cleanly */
		TcChannel *ch = &cfg->tc->TcChannel[cfg->tc_ch];

		ch->TC_CCR = TC_CCR_CLKDIS;
		dacc->DACC_PTCR = DACC_PTCR_TXTDIS;
		dacc->DACC_IDR = DACC_IDR_ENDTX | DACC_IDR_TXBUFE;
		LOG_ERR("DAC PDC underrun, stopped");
		return;
	}

	if (sr & DACC_ISR_ENDTX) {
		/* Current half done; promote next→current, expose new next */
		data->active_half ^= 1;
		uint16_t *idle = data->dbl_buf +
				 (size_t)(1U - data->active_half) * data->half_samples;

		dacc->DACC_TNPR = (uint32_t)idle;
		dacc->DACC_TNCR = (uint32_t)data->half_samples;

		if (data->tx_cb != NULL) {
			data->tx_cb(dev, data->tx_cb_user_data);
		}
	}
}

static int dac_audio_sam_configure(const struct device *dev,
				   struct audio_codec_cfg *audio_cfg)
{
	const struct dac_audio_sam_cfg *cfg = dev->config;
	struct dac_audio_sam_data *data = dev->data;
	Dacc *dacc = cfg->dacc;
	TcChannel *ch = &cfg->tc->TcChannel[cfg->tc_ch];
	uint32_t sample_rate = (uint32_t)audio_cfg->dai_cfg.pcm.samplerate;
	int ret;

	/* Enable DACC and TC clocks */
	clock_control_on(SAM_DT_PMC_CONTROLLER,
			 (clock_control_subsys_t)&cfg->dacc_clock_cfg);
	clock_control_on(SAM_DT_PMC_CONTROLLER,
			 (clock_control_subsys_t)&cfg->tc_clock_cfgs[cfg->tc_ch]);

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("pinctrl_apply_state failed: %d", ret);
		return ret;
	}

	/* Reset DACC */
	dacc->DACC_CR = DACC_CR_SWRST;

	/* Triggered mode, TC trigger, channel 1, startup=8 periods */
	dacc->DACC_MR = DACC_MR_TRGEN_EN |
			DACC_MR_TRGSEL(cfg->trig_sel) |
			DACC_MR_USER_SEL_CHANNEL1 |
			DACC_MR_STARTUP_8;

	/* Enable DACC channel 1 (PB16 / DAC1 header pin) */
	dacc->DACC_CHER = DACC_CHER_CH1;

	/* Set analog current for better drive strength */
	dacc->DACC_ACR = DACC_ACR_IBCTLCH1(0x2) | DACC_ACR_IBCTLDACCORE(0x1);

	/*
	 * TC waveform mode: UP with auto-trigger on RC compare.
	 * TCCLKS=0 → TCLK1 = MCK/2 = 42 MHz on Arduino Due (84 MHz MCK).
	 * RC = 42000000 / sample_rate − 1 gives the period.
	 */
	if (sample_rate == 0) {
		LOG_ERR("invalid sample rate 0");
		return -EINVAL;
	}

	uint32_t rc = 42000000U / sample_rate;

	if (rc == 0 || (42000000U % sample_rate) != 0) {
		LOG_ERR("sample rate %u not exactly achievable at 42 MHz", sample_rate);
		return -EINVAL;
	}
	rc -= 1;

	ch->TC_CMR = TC_CMR_WAVE |
		     TC_CMR_WAVSEL_UP_RC |
		     TC_CMR_TCCLKS_TIMER_CLOCK1 |
		     TC_CMR_ACPA_CLEAR |
		     TC_CMR_ACPC_SET;
	ch->TC_RA = 1;
	ch->TC_RC = rc;

	data->configured = false;
	data->active_half = 0;

	LOG_INF("configured: %u Hz, RC=%u", sample_rate, rc);
	return 0;
}

static int dac_audio_sam_write(const struct device *dev, uint8_t *buf, size_t size)
{
	const struct dac_audio_sam_cfg *cfg = dev->config;
	struct dac_audio_sam_data *data = dev->data;
	Dacc *dacc = cfg->dacc;
	size_t samples = size / sizeof(uint16_t);

	if (!data->configured) {
		/* First write: fill both halves identically and prime PDC */
		data->half_samples = samples;
		memcpy(data->dbl_buf, buf, size);
		memcpy(data->dbl_buf + samples, buf, size);

		dacc->DACC_TPR  = (uint32_t)data->dbl_buf;
		dacc->DACC_TCR  = (uint32_t)data->half_samples;
		dacc->DACC_TNPR = (uint32_t)(data->dbl_buf + data->half_samples);
		dacc->DACC_TNCR = (uint32_t)data->half_samples;
		dacc->DACC_PTCR = DACC_PTCR_TXTEN;

		data->configured = true;
	} else {
		/* Subsequent writes: copy into the idle half */
		uint8_t idle = 1U - data->active_half;
		uint16_t *dst = data->dbl_buf + (size_t)idle * data->half_samples;

		memcpy(dst, buf, data->half_samples * sizeof(uint16_t));
	}

	return 0;
}

static void dac_audio_sam_start_output(const struct device *dev)
{
	const struct dac_audio_sam_cfg *cfg = dev->config;
	Dacc *dacc = cfg->dacc;
	TcChannel *ch = &cfg->tc->TcChannel[cfg->tc_ch];

	/* Enable ENDTX and buffer-empty interrupts */
	dacc->DACC_IER = DACC_IER_ENDTX | DACC_IER_TXBUFE;

	/* Start TC: enable clock and software-trigger first conversion */
	ch->TC_CCR = TC_CCR_CLKEN | TC_CCR_SWTRG;

	LOG_INF("DAC audio output started");
}

static void dac_audio_sam_stop_output(const struct device *dev)
{
	const struct dac_audio_sam_cfg *cfg = dev->config;
	Dacc *dacc = cfg->dacc;
	TcChannel *ch = &cfg->tc->TcChannel[cfg->tc_ch];

	ch->TC_CCR = TC_CCR_CLKDIS;
	dacc->DACC_PTCR = DACC_PTCR_TXTDIS;
	dacc->DACC_IDR = DACC_IDR_ENDTX | DACC_IDR_TXBUFE;

	LOG_INF("DAC audio output stopped");
}

static int dac_audio_sam_set_property(const struct device *dev,
				      audio_property_t property,
				      audio_channel_t channel,
				      audio_property_value_t val)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(property);
	ARG_UNUSED(channel);
	ARG_UNUSED(val);
	return -ENOTSUP;
}

static int dac_audio_sam_apply_properties(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int dac_audio_sam_register_done_callback(const struct device *dev,
						audio_codec_tx_done_callback_t tx_cb,
						void *tx_user_data,
						audio_codec_rx_done_callback_t rx_cb,
						void *rx_user_data)
{
	struct dac_audio_sam_data *data = dev->data;

	ARG_UNUSED(rx_cb);
	ARG_UNUSED(rx_user_data);

	data->tx_cb = tx_cb;
	data->tx_cb_user_data = tx_user_data;
	return 0;
}

static int dac_audio_sam_start(const struct device *dev, audio_dai_dir_t dir)
{
	if ((dir & AUDIO_DAI_DIR_TX) == 0) {
		return -ENOTSUP;
	}
	dac_audio_sam_start_output(dev);
	return 0;
}

static int dac_audio_sam_stop(const struct device *dev, audio_dai_dir_t dir)
{
	if ((dir & AUDIO_DAI_DIR_TX) == 0) {
		return -ENOTSUP;
	}
	dac_audio_sam_stop_output(dev);
	return 0;
}

static const struct audio_codec_api dac_audio_sam_codec_api = {
	.configure          = dac_audio_sam_configure,
	.start_output       = dac_audio_sam_start_output,
	.stop_output        = dac_audio_sam_stop_output,
	.set_property       = dac_audio_sam_set_property,
	.apply_properties   = dac_audio_sam_apply_properties,
	.start              = dac_audio_sam_start,
	.stop               = dac_audio_sam_stop,
	.write              = dac_audio_sam_write,
	.register_done_callback = dac_audio_sam_register_done_callback,
};

static int dac_audio_sam_init(const struct device *dev)
{
	const struct dac_audio_sam_cfg *cfg = dev->config;

	cfg->irq_config();
	return 0;
}

#define DAC_AUDIO_SAM_INIT(n)                                                            \
	PINCTRL_DT_INST_DEFINE(n);                                                       \
                                                                                         \
	static void dac_audio_sam_irq_config_##n(void)                                   \
	{                                                                                \
		IRQ_CONNECT(DT_IRQ_BY_IDX(DT_PARENT(DT_DRV_INST(n)), 0, irq),           \
			    DT_IRQ_BY_IDX(DT_PARENT(DT_DRV_INST(n)), 0, priority),      \
			    dac_audio_sam_isr, DEVICE_DT_INST_GET(n), 0);               \
		irq_enable(DT_IRQ_BY_IDX(DT_PARENT(DT_DRV_INST(n)), 0, irq));          \
	}                                                                                \
                                                                                         \
	static struct dac_audio_sam_data dac_audio_sam_data_##n;                         \
                                                                                         \
	static const struct dac_audio_sam_cfg dac_audio_sam_cfg_##n = {                  \
		.dacc = (Dacc *)DT_REG_ADDR(DT_PARENT(DT_DRV_INST(n))),                 \
		.tc   = (Tc *)DT_REG_ADDR(DT_INST_PHANDLE(n, atmel_timer)),             \
		.tc_ch    = DT_INST_PROP(n, atmel_tc_channel),                          \
		.trig_sel = DT_INST_PROP(n, atmel_trig_sel),                            \
		.dacc_clock_cfg = SAM_DT_CLOCK_PMC_CFG(0,                               \
				    DT_PARENT(DT_DRV_INST(n))),                          \
		.tc_clock_cfgs = SAM_DT_CLOCKS_PMC_CFG(                                 \
				    DT_INST_PHANDLE(n, atmel_timer)),                    \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),                              \
		.irq_config = dac_audio_sam_irq_config_##n,                             \
	};                                                                               \
                                                                                         \
	DEVICE_DT_INST_DEFINE(n, dac_audio_sam_init, NULL,                               \
			      &dac_audio_sam_data_##n,                                   \
			      &dac_audio_sam_cfg_##n,                                    \
			      POST_KERNEL, CONFIG_AUDIO_CODEC_INIT_PRIORITY,             \
			      &dac_audio_sam_codec_api);

DT_INST_FOREACH_STATUS_OKAY(DAC_AUDIO_SAM_INIT)
