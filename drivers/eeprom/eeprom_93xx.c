/*
 * Copyright (c) 2026 Eve Redero
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Microchip 93xx Microwire serial EEPROM, x16 organisation.
 *
 * Microwire is SPI mode 0 / MSB first with two quirks:
 *
 *   1. CS is active HIGH. Handled in devicetree: "spi-cs-high" on this node and
 *      GPIO_ACTIVE_HIGH on the controller's matching "cs-gpios" entry.
 *
 *   2. Command headers are 3 + addr_bits long (one start bit, two opcode bits,
 *      then the address field), so they are not byte-aligned. Handled by padding
 *      with leading zeros: the chip ignores clocks on DI until it sees the start
 *      bit, so a command always fits in exactly two bytes for this family.
 *
 *      addr_bits is the width of the address *field*, taken from devicetree. It
 *      is not derivable from the size: some parts clock a leading don't-care bit
 *      ahead of the address, making the field one bit wider than the address
 *      needs. See the binding for the per-part values.
 *
 * One non-obvious property, established on a bench rather than from the data
 * sheet: read data comes back BYTE-ALIGNED. The chip drives its leading dummy 0
 * onto DO on the *same* clock that shifts in the last address bit, not on the
 * clock after it. The dummy is therefore consumed inside the two padded command
 * bytes, and D15..D0 begins on a byte boundary with no realignment. Reading the
 * timing diagram as "dummy bit, then data" instead yields a 7-bit shift.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/eeprom.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#define DT_DRV_COMPAT microchip_93xx

LOG_MODULE_REGISTER(eeprom_93xx, CONFIG_EEPROM_LOG_LEVEL);

/* Two-bit opcodes, shifted into place under the start bit by eeprom_93xx_frame() */
#define EEPROM_93XX_OP_EXTENDED 0x0U
#define EEPROM_93XX_OP_WRITE    0x1U
#define EEPROM_93XX_OP_READ     0x2U

/*
 * The extended opcode takes its function from the top two address bits:
 * 0b11 enables writes (EWEN), 0b00 disables them (EWDS). ERAL (0b10) and WRAL
 * (0b01) are not used: the EEPROM API has no erase operation, and a 93xx WRITE
 * is self-erasing.
 */
#define EEPROM_93XX_EXT_EWEN 0x3U
#define EEPROM_93XX_EXT_EWDS 0x0U

#define EEPROM_93XX_WORD_SIZE 2U

struct eeprom_93xx_config {
	struct spi_dt_spec spi;
	size_t size;
	uint8_t addr_bits;
	uint16_t timeout_ms;
	bool readonly;
};

struct eeprom_93xx_data {
	struct k_sem lock;
};

/*
 * Pack "start bit, opcode, address" into two zero-padded bytes. Valid while
 * 3 + addr_bits <= 16, i.e. addr_bits <= 13, which the whole 93xx family is
 * comfortably inside.
 */
static void eeprom_93xx_frame(const struct eeprom_93xx_config *config, uint8_t op, uint16_t addr,
			      uint8_t buf[2])
{
	const uint16_t frame = BIT(config->addr_bits + 2) | (op << config->addr_bits) | addr;

	sys_put_be16(frame, buf);
}

/* Issue a command with no data phase, used for EWEN and EWDS */
static int eeprom_93xx_cmd(const struct device *dev, uint8_t op, uint16_t addr)
{
	const struct eeprom_93xx_config *config = dev->config;
	uint8_t frame[2];

	eeprom_93xx_frame(config, op, addr, frame);

	const struct spi_buf tx_buf = {
		.buf = frame,
		.len = sizeof(frame),
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};

	return spi_write_dt(&config->spi, &tx);
}

static int eeprom_93xx_set_write_enable(const struct device *dev, bool enable)
{
	const struct eeprom_93xx_config *config = dev->config;
	const uint8_t func = enable ? EEPROM_93XX_EXT_EWEN : EEPROM_93XX_EXT_EWDS;
	int ret;

	ret = eeprom_93xx_cmd(dev, EEPROM_93XX_OP_EXTENDED, func << (config->addr_bits - 2));
	if (ret < 0) {
		LOG_ERR("failed to %s writes: %d", enable ? "enable" : "disable", ret);
	}

	return ret;
}

/*
 * Command and data phases go in a single transceive so CS stays asserted across
 * both; a 93xx aborts the operation if CS drops mid-command.
 */
static int eeprom_93xx_read_word(const struct device *dev, uint16_t word, uint16_t *out)
{
	const struct eeprom_93xx_config *config = dev->config;
	uint8_t frame[2];
	uint8_t raw[EEPROM_93XX_WORD_SIZE];
	int ret;

	eeprom_93xx_frame(config, EEPROM_93XX_OP_READ, word, frame);

	const struct spi_buf tx_bufs[] = {
		{ .buf = frame, .len = sizeof(frame) },
		{ .buf = NULL, .len = sizeof(raw) },
	};
	const struct spi_buf rx_bufs[] = {
		{ .buf = NULL, .len = sizeof(frame) },
		{ .buf = raw, .len = sizeof(raw) },
	};
	const struct spi_buf_set tx = {
		.buffers = tx_bufs,
		.count = ARRAY_SIZE(tx_bufs),
	};
	const struct spi_buf_set rx = {
		.buffers = rx_bufs,
		.count = ARRAY_SIZE(rx_bufs),
	};

	ret = spi_transceive_dt(&config->spi, &tx, &rx);
	if (ret < 0) {
		LOG_ERR("failed to read word %u: %d", word, ret);
		return ret;
	}

	*out = sys_get_be16(raw);

	return 0;
}

/* Caller must hold write-enable and the device lock */
static int eeprom_93xx_write_word(const struct device *dev, uint16_t word, uint16_t val)
{
	const struct eeprom_93xx_config *config = dev->config;
	uint8_t frame[2 + EEPROM_93XX_WORD_SIZE];
	int ret;

	eeprom_93xx_frame(config, EEPROM_93XX_OP_WRITE, word, frame);
	sys_put_be16(val, &frame[2]);

	const struct spi_buf tx_buf = {
		.buf = frame,
		.len = sizeof(frame),
	};
	const struct spi_buf_set tx = {
		.buffers = &tx_buf,
		.count = 1,
	};

	ret = spi_write_dt(&config->spi, &tx);
	if (ret < 0) {
		LOG_ERR("failed to write word %u: %d", word, ret);
		return ret;
	}

	/*
	 * The write cycle is self-timed. Polling DO for ready is the data sheet
	 * method, but a fixed wait keeps this driver free of a second protocol
	 * path for no practical gain at these sizes.
	 */
	k_msleep(config->timeout_ms);

	return 0;
}

/*
 * Walks one word at a time rather than exploiting the chip's sequential-read
 * auto-increment. A single-transaction fast path would only apply to aligned
 * reads, and the loop costs ~2.5 ms for a whole 128-byte part at 500 kHz, so the
 * second code path is not worth its weight.
 */
static int eeprom_93xx_read(const struct device *dev, off_t offset, void *data, size_t len)
{
	const struct eeprom_93xx_config *config = dev->config;
	uint8_t *dst = data;
	int ret;

	if (offset < 0 || (size_t)offset + len > config->size) {
		LOG_ERR("read of %zu bytes at offset %ld exceeds size %zu", len, (long)offset,
			config->size);
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	while (len > 0) {
		const uint16_t word = (uint16_t)(offset / EEPROM_93XX_WORD_SIZE);
		const size_t skip = (size_t)offset % EEPROM_93XX_WORD_SIZE;
		const size_t chunk = MIN(len, EEPROM_93XX_WORD_SIZE - skip);
		uint8_t raw[EEPROM_93XX_WORD_SIZE];
		uint16_t val;

		ret = eeprom_93xx_read_word(dev, word, &val);
		if (ret < 0) {
			return ret;
		}

		sys_put_be16(val, raw);
		memcpy(dst, &raw[skip], chunk);

		dst += chunk;
		offset += chunk;
		len -= chunk;
	}

	return 0;
}

static int eeprom_93xx_write(const struct device *dev, off_t offset, const void *data, size_t len)
{
	const struct eeprom_93xx_config *config = dev->config;
	struct eeprom_93xx_data *dev_data = dev->data;
	const uint8_t *src = data;
	int disable_ret;
	int ret;

	if (config->readonly) {
		LOG_ERR("device is read-only");
		return -EACCES;
	}

	if (offset < 0 || (size_t)offset + len > config->size) {
		LOG_ERR("write of %zu bytes at offset %ld exceeds size %zu", len, (long)offset,
			config->size);
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	k_sem_take(&dev_data->lock, K_FOREVER);

	/* Latched once for the whole operation rather than per word */
	ret = eeprom_93xx_set_write_enable(dev, true);
	if (ret < 0) {
		goto out;
	}

	while (len > 0) {
		const uint16_t word = (uint16_t)(offset / EEPROM_93XX_WORD_SIZE);
		const size_t skip = (size_t)offset % EEPROM_93XX_WORD_SIZE;
		const size_t chunk = MIN(len, EEPROM_93XX_WORD_SIZE - skip);
		uint8_t raw[EEPROM_93XX_WORD_SIZE];

		if (chunk < EEPROM_93XX_WORD_SIZE) {
			/*
			 * The API is byte addressed but the device is not, so a
			 * partially covered word has to keep the bytes the caller
			 * did not ask to change.
			 */
			uint16_t val;

			ret = eeprom_93xx_read_word(dev, word, &val);
			if (ret < 0) {
				goto out_disable;
			}

			sys_put_be16(val, raw);
		}

		memcpy(&raw[skip], src, chunk);

		ret = eeprom_93xx_write_word(dev, word, sys_get_be16(raw));
		if (ret < 0) {
			goto out_disable;
		}

		src += chunk;
		offset += chunk;
		len -= chunk;
	}

out_disable:
	/* Re-protect the device even if the loop failed partway */
	disable_ret = eeprom_93xx_set_write_enable(dev, false);
	if (ret == 0) {
		ret = disable_ret;
	}

out:
	k_sem_give(&dev_data->lock);

	return ret;
}

static size_t eeprom_93xx_size(const struct device *dev)
{
	const struct eeprom_93xx_config *config = dev->config;

	return config->size;
}

static int eeprom_93xx_init(const struct device *dev)
{
	const struct eeprom_93xx_config *config = dev->config;
	struct eeprom_93xx_data *data = dev->data;

	k_sem_init(&data->lock, 1, 1);

	if (!spi_is_ready_dt(&config->spi)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(eeprom, eeprom_93xx_api) = {
	.read = &eeprom_93xx_read,
	.write = &eeprom_93xx_write,
	.size = &eeprom_93xx_size,
};

#define EEPROM_93XX_ADDR_BITS(inst) DT_INST_PROP(inst, address_width)

/* Address bits needed to reach every word. The field is this wide or one wider. */
#define EEPROM_93XX_ADDR_BITS_MIN(inst) LOG2(DT_INST_PROP(inst, size) / EEPROM_93XX_WORD_SIZE)

#define EEPROM_93XX_INIT(inst)                                                                     \
	BUILD_ASSERT(IS_POWER_OF_TWO(DT_INST_PROP(inst, size)),                                    \
		     "93xx size must be a power of two");                                          \
	BUILD_ASSERT(DT_INST_PROP(inst, size) >= 2 * EEPROM_93XX_WORD_SIZE,                        \
		     "93xx size must cover at least two words");                                   \
	BUILD_ASSERT(EEPROM_93XX_ADDR_BITS(inst) == EEPROM_93XX_ADDR_BITS_MIN(inst) ||             \
		     EEPROM_93XX_ADDR_BITS(inst) == EEPROM_93XX_ADDR_BITS_MIN(inst) + 1,           \
		     "93xx address-width must match size, or exceed it by the single "             \
		     "don't-care bit that some parts clock ahead of the address");                 \
	BUILD_ASSERT(EEPROM_93XX_ADDR_BITS(inst) + 3 <= 16,                                        \
		     "93xx command must fit the two-byte frame");                                  \
	BUILD_ASSERT(DT_INST_PROP(inst, spi_cs_high),                                              \
		     "93xx chip select is active high: set spi-cs-high on the node, and "          \
		     "GPIO_ACTIVE_HIGH on its cs-gpios entry");                                    \
                                                                                                   \
	static struct eeprom_93xx_data eeprom_93xx_data_##inst;                                    \
                                                                                                   \
	static const struct eeprom_93xx_config eeprom_93xx_config_##inst = {                       \
		.spi = SPI_DT_SPEC_INST_GET(inst, SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) |       \
							  SPI_TRANSFER_MSB),                       \
		.size = DT_INST_PROP(inst, size),                                                  \
		.addr_bits = EEPROM_93XX_ADDR_BITS(inst),                                          \
		.timeout_ms = DT_INST_PROP(inst, timeout),                                         \
		.readonly = DT_INST_PROP(inst, read_only),                                         \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, eeprom_93xx_init, NULL, &eeprom_93xx_data_##inst,              \
			      &eeprom_93xx_config_##inst, POST_KERNEL,                             \
			      CONFIG_EEPROM_INIT_PRIORITY, &eeprom_93xx_api);

DT_INST_FOREACH_STATUS_OKAY(EEPROM_93XX_INIT)
