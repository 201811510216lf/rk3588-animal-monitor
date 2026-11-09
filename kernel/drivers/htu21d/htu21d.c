// SPDX-License-Identifier: GPL-2.0
/*
 * ============================================================================
 * 文件：kernel/drivers/htu21d/htu21d.c
 * 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED
 *       —— 未经编译、未在目标内核验证；本机 Windows 不编译本文件。
 * 依据：任务书 CODEX_MASTER_TASK.md §7.2；kernel/drivers/htu21d/README.md（IIO 路线）
 * 重要提示（注册前必查）：
 *   - 主线内核已有 hwmon htu21 驱动（drivers/hwmon/htu21.c，CONFIG_SENSORS_HTU21，
 *     compatible "meas,htu21d"）。本驱动与其绑定名相同，二者不可同时启用，
 *     否则重复设备绑定（README 已记录；目标内核检查后再编译启用）。
 * 设计要点：
 *   - IIO：2 通道（IIO_TEMP / IIO_HUMIDITYRELATIVE），INDIO_DIRECT_MODE，
 *     read_raw 发起单次测量（0xE3 温度 / 0xE5 湿度，no-hold），等待保守转换
 *     时间（T 85ms / RH 50ms），读 3 字节并 CRC 校验；校验失败返回 -EIO 并计数。
 *   - RAW = 16 位原始值（IIO_VAL_INT）；SCALE = 175.72/65536、125/65536
 *     （INT_PLUS_NANO）；PROCESSED 提供毫摄氏度/毫 %RH 便捷路径（任务书简化要求）。
 *     常数项（-46.85/-6）并入公式，不提供 IIO_CHAN_INFO_OFFSET 通道。
 *   - 分辨率寄存器（0xE6）在 probe 写默认 0x02（RH12/T14）；动态修改分辨率
 *     不在初稿范围（任务书允许 PROCESSED 简单路径即可）。
 *   - devm 资源管理；无 IIO trigger/buffer（周期采样需求确认后再设计，避免过度设计）。
 *   - 不包含 DTS；器件型号（HTU21D/Si7021 兼容）与分辨率待板卡确认（UNVERIFIED）。
 * ============================================================================
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "htu21d.h"

/* CRC-8：多项式 0x131（x^8+x^5+x^4+1）、初值 0x00、MSB 先。
 * 与用户态 htu21d_math.cpp 的 Htu21dCrc8 算法一致（跨端校验向量见单元测试）。 */
u8 htu21d_crc8(u8 msb, u8 lsb)
{
	u8 crc = 0x00;
	u8 bytes[2] = { msb, lsb };
	int byte;
	int bit;

	for (byte = 0; byte < 2; byte++)
	{
		crc ^= bytes[byte];
		for (bit = 0; bit < 8; bit++)
		{
			if (crc & 0x80)
			{
				crc = (u8)((crc << 1) ^ 0x31);
			}
			else
			{
				crc = (u8)(crc << 1);
			}
		}
	}
	return crc;
}

/* 单次测量：no-hold 触发 → 保守等待 → 读 3 字节 + CRC 校验。
 * 失败计数并返回 -EIO（重试策略见 README 遗留问题）。 */
static int htu21d_measure(struct htu21d_priv *priv, u8 cmd, unsigned int conv_ms, u16 *raw)
{
	u8 buf[3];
	int ret;

	ret = i2c_master_send(priv->client, &cmd, 1);
	if (ret != 1)
	{
		priv->io_errors++;
		return ret < 0 ? ret : -EIO;
	}

	/* no-hold 模式：主器件须等待转换完成（时钟不拉伸） */
	msleep(conv_ms);

	ret = i2c_master_recv(priv->client, buf, sizeof(buf));
	if (ret != sizeof(buf))
	{
		priv->io_errors++;
		return ret < 0 ? ret : -EIO;
	}

	if (htu21d_crc8(buf[0], buf[1]) != buf[2])
	{
		priv->crc_errors++;
		dev_warn(&priv->client->dev, "htu21d: CRC 校验失败（累计 %u）\n",
			 priv->crc_errors);
		return -EIO;
	}

	*raw = (u16)((buf[0] << 8) | buf[1]);
	return 0;
}

/* 毫摄氏度（SI 毫单位，PROCESSED 路径用；四舍五入）。
 * T(°C) = -46.85 + 175.72*st/65536 → milli = -46850 + 175720*st/65536 */
static int htu21d_temp_milli(u16 st)
{
	s64 milli = HTU21D_TEMP_OFFSET_MILLI +
		    DIV_ROUND_CLOSEST_ULL((u64)HTU21D_TEMP_SCALE_NUM * st,
					  HTU21D_SCALE_DENOM);

	return (int)milli;
}

/* 毫 %RH（SI 毫单位，PROCESSED 路径用）。
 * RH(%) = -6 + 125*srh/65536 → milli = -6000 + 125000*srh/65536 */
static int htu21d_rh_milli(u16 srh)
{
	s64 milli = HTU21D_HUMID_OFFSET_MILLI +
		    DIV_ROUND_CLOSEST_ULL((u64)HTU21D_HUMID_SCALE_NUM * srh,
					  HTU21D_SCALE_DENOM);

	return (int)milli;
}

static int htu21d_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct htu21d_priv *priv = iio_priv(indio_dev);
	bool is_temp = (chan->type == IIO_TEMP);
	u8 cmd = is_temp ? HTU21D_CMD_TRIG_TEMP : HTU21D_CMD_TRIG_HUMID;
	unsigned int conv_ms = is_temp ? HTU21D_TEMP_CONV_MS : HTU21D_HUMID_CONV_MS;
	u16 raw;
	int ret;

	switch (mask)
	{
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&priv->lock);
		ret = htu21d_measure(priv, cmd, conv_ms, &raw);
		mutex_unlock(&priv->lock);
		if (ret < 0)
		{
			return ret;
		}
		*val = raw;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		/* 斜率：温度 175.72/65536、湿度 125/65536（任务书规定；
		 * 常数项并入公式，不提供 OFFSET 通道） */
		if (is_temp)
		{
			*val = 0;
			*val2 = 2681274; /* 175.72/65536 ≈ 0.002681274… */
		}
		else
		{
			*val = 0;
			*val2 = 1907349; /* 125/65536 ≈ 0.001907349… */
		}
		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_PROCESSED:
		/* 便捷路径：SI 毫单位（毫摄氏度/毫 %RH）。精确换算仍建议
		 * 用户态用 RAW + htu21d_math 公式（任务书“processed 由用户态换算”） */
		mutex_lock(&priv->lock);
		ret = htu21d_measure(priv, cmd, conv_ms, &raw);
		mutex_unlock(&priv->lock);
		if (ret < 0)
		{
			return ret;
		}
		*val = is_temp ? htu21d_temp_milli(raw) : htu21d_rh_milli(raw);
		*val2 = 0;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_info htu21d_info = {
	.read_raw = htu21d_read_raw,
};

static const struct iio_chan_spec htu21d_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_PROCESSED),
	},
	{
		.type = IIO_HUMIDITYRELATIVE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static int htu21d_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct htu21d_priv *priv;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*priv));
	if (!indio_dev)
	{
		return -ENOMEM;
	}
	priv = iio_priv(indio_dev);
	priv->client = client;
	mutex_init(&priv->lock);
	i2c_set_clientdata(client, indio_dev);

	indio_dev->name = HTU21D_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = htu21d_channels;
	indio_dev->num_channels = ARRAY_SIZE(htu21d_channels);
	indio_dev->info = &htu21d_info;

	/* 分辨率寄存器：写默认 0x02（RH 12 位 / T 14 位；失败仅告警，
	 * 器件默认值即 0x02，UNVERIFIED） */
	ret = i2c_smbus_write_byte_data(client, HTU21D_CMD_WRITE_USER,
					HTU21D_USER_RESOLUTION);
	if (ret < 0)
	{
		dev_warn(&client->dev,
			 "htu21d: 分辨率寄存器写入失败 (%d)，沿用器件默认值\n", ret);
	}

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct i2c_device_id htu21d_id[] = {
	{ "htu21d", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, htu21d_id);

/*
 * 注意：compatible 与主线 hwmon htu21.c 相同（"meas,htu21d"），
 * 二者不可共存（重复设备绑定）——注册前须检查目标内核配置（README 已记）。
 */
static const struct of_device_id htu21d_of_match[] = {
	{ .compatible = "meas,htu21d" },
	{ }
};
MODULE_DEVICE_TABLE(of, htu21d_of_match);

static struct i2c_driver htu21d_driver = {
	.driver = {
		.name = HTU21D_NAME,
		.of_match_table = htu21d_of_match,
	},
	.probe = htu21d_probe,
	.id_table = htu21d_id,
};
module_i2c_driver(htu21d_driver);

MODULE_AUTHOR("animal-monitor project");
MODULE_DESCRIPTION("HTU21D humidity/temperature custom IIO driver (draft, UNVERIFIED)");
MODULE_LICENSE("GPL");
