// SPDX-License-Identifier: GPL-2.0
/*
 * ============================================================================
 * 文件：kernel/drivers/mlx90640/mlx90640.c
 * 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED
 *       —— 未经编译、未在目标内核验证；本机 Windows 不编译本文件。
 * 依据：任务书 CODEX_MASTER_TASK.md §7.1；kernel/drivers/mlx90640/README.md 接口决策
 * 设计要点：
 *   - 标准 i2c_driver + 标准字符设备（cdev_init/cdev_add + device_create），
 *     禁止 miscdevice（AGENTS.md 规则 5）。
 *   - 每 read 主动采集：状态寄存器轮询就绪 → 冻结 RAM → 读两个 subpage
 *     （每个 subpage：0x0400 起 768 word + 0x0700 起 64 word 尾随区，分块读）
 *     → 内核仅按交错模式合并/搬运，温度算法全部在用户态（mlx90640_math）。
 *   - read 阻塞：采集间隙在 waitqueue 上等待（无后台线程）；remove 置
 *     shutting_down 并 wake_up_all，被唤醒的 reader 返回 -ENODEV；
 *     多 reader 语义：每次 read 独立采集（mutex 串行），各 reader 拿到最新完整帧。
 *   - UAF 防护：open() 持 get_device(&client->dev)，release() 归还；
 *     devm 分配的 priv 在最后一个设备引用释放后才回收。
 *   - 多设备：模块级动态主设备号 + IDA 分配 minor + device_create 命名
 *     mlx90640%d；静态上限 MLX90640_MAX_DEVICES。
 *   - 不包含 DTS（DTS 待板卡确定后单独提交）；compatible 字符串仅作驱动内绑定名。
 *   - 未用真实器件验证：寄存器地址/尾随区偏移/交错规则均为数据手册公开知识
 *     （UNVERIFIED），禁止宣称输出为准确体温（NOT_CALIBRATED）。
 * ============================================================================
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "mlx90640.h"

/* ============================ 模块级资源 ============================ */

static dev_t mlx90640_devt;
static struct class *mlx90640_class;
static DEFINE_IDA(mlx90640_ida);

/* 常用全局标定 word 索引（与 UAPI 头注释一致；字节地址 = 0x2400 + 2*idx） */
static const u16 mlx90640_cal_word_idx[MLX90640_PARAMS_WORD_COUNT] = {
	16, 17, 32, 33, 48, 49, 50, 51, 52, 53,
	54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
};

/* ============================ I2C 基础传输 ============================ */

/* 分块读：2 字节大端地址 + count 个 16 位大端 word；每块 ≤ 32 word；
 * 错误重试 3 次；超时由 adapter->timeout（100ms，probe 时设置）保证。 */
int mlx90640_read_words(struct i2c_client *client, u16 addr, u16 *words, u16 count)
{
	u16 offset = 0;

	while (offset < count)
	{
		u16 chunk = min_t(u16, MLX90640_I2C_CHUNK_WORDS, count - offset);
		u16 chunk_addr = addr + offset;
		u8 cmd[2] = { (chunk_addr >> 8) & 0xff, chunk_addr & 0xff };
		u8 buf[2 * MLX90640_I2C_CHUNK_WORDS];
		struct i2c_msg msgs[2] = {
			{
				.addr = client->addr,
				.flags = 0,
				.len = sizeof(cmd),
				.buf = cmd,
			},
			{
				.addr = client->addr,
				.flags = I2C_M_RD,
				.len = 2 * chunk,
				.buf = buf,
			},
		};
		int ret = -EIO;
		int attempt;
		int i;

		for (attempt = 0; attempt < MLX90640_I2C_RETRIES; attempt++)
		{
			ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
			if (ret == ARRAY_SIZE(msgs))
			{
				break;
			}
			dev_warn(&client->dev,
				 "mlx90640: 读 0x%04x 重试 %d/%d (ret=%d)\n",
				 chunk_addr, attempt + 1, MLX90640_I2C_RETRIES, ret);
			if (attempt + 1 < MLX90640_I2C_RETRIES)
			{
				msleep(MLX90640_RETRY_DELAY_MS);
			}
		}
		if (ret != ARRAY_SIZE(msgs))
		{
			return ret < 0 ? ret : -EIO;
		}

		/* 16 位大端 word → 主机序 */
		for (i = 0; i < chunk; i++)
		{
			words[offset + i] = (u16)((buf[2 * i] << 8) | buf[2 * i + 1]);
		}
		offset += chunk;
	}
	return 0;
}

/* 单 word 写（2 字节地址 + 2 字节大端数据）。 */
int mlx90640_write_word(struct i2c_client *client, u16 addr, u16 value)
{
	u8 buf[4] = { (addr >> 8) & 0xff, addr & 0xff, (value >> 8) & 0xff, value & 0xff };
	int ret = i2c_master_send(client, buf, sizeof(buf));

	return ret == sizeof(buf) ? 0 : (ret < 0 ? ret : -EIO);
}

/* ============================ 采集辅助 ============================ */

/* 轮询状态寄存器等待新子页数据（超时 -ETIMEDOUT）。 */
static int mlx90640_wait_data_ready(struct mlx90640_priv *priv, unsigned long timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

	for (;;)
	{
		u16 status;
		int ret = mlx90640_read_words(priv->client, MLX90640_REG_STATUS, &status, 1);

		if (ret < 0)
		{
			priv->i2c_errors++;
			return ret;
		}
		if (status & MLX90640_STATUS_NEW_DATA)
		{
			return 0;
		}
		if (time_after_eq(jiffies, end))
		{
			return -ETIMEDOUT;
		}
		usleep_range(2000, 5000);
	}
}

/* 等待与当前不同的子页完成（交错模式需要两个子页）。 */
static int mlx90640_wait_next_subpage(struct mlx90640_priv *priv, u8 current,
				      unsigned long timeout_ms)
{
	unsigned long end = jiffies + msecs_to_jiffies(timeout_ms);

	for (;;)
	{
		u16 status;
		int ret = mlx90640_read_words(priv->client, MLX90640_REG_STATUS, &status, 1);

		if (ret < 0)
		{
			priv->i2c_errors++;
			return ret;
		}
		if ((status & MLX90640_STATUS_NEW_DATA) &&
		    (u8)(status & MLX90640_STATUS_SUBPAGE) != current)
		{
			return 0;
		}
		if (time_after_eq(jiffies, end))
		{
			return -ETIMEDOUT;
		}
		usleep_range(2000, 5000);
	}
}

/* 读取一个子页：冻结 RAM → 读 768 像素 word + 64 尾随 word；返回子页号。 */
static int mlx90640_read_subpage(struct mlx90640_priv *priv, u16 *pixels, u16 *aux,
				 u8 *subpage)
{
	u16 status;
	int ret;

	/* 冻结 RAM 更新，避免读取过程中被半帧覆盖（官方库 SynchFrame 做法） */
	ret = mlx90640_write_word(priv->client, MLX90640_REG_STATUS, MLX90640_FREEZE_RAM);
	if (ret < 0)
	{
		return ret;
	}

	ret = mlx90640_read_words(priv->client, MLX90640_REG_STATUS, &status, 1);
	if (ret < 0)
	{
		return ret;
	}
	*subpage = (u8)(status & MLX90640_STATUS_SUBPAGE);

	ret = mlx90640_read_words(priv->client, MLX90640_RAM_BASE, pixels,
				  MLX90640_RAM_PIXEL_WORDS);
	if (ret < 0)
	{
		return ret;
	}
	return mlx90640_read_words(priv->client, MLX90640_RAM_AUX_BASE, aux,
				   MLX90640_RAM_AUX_WORDS);
}

/* 坏帧检测（官方库 ValidateFrameData/ValidateAuxData 简化；UNVERIFIED）：
 * 与子页同奇偶的行的首列像素为 0x7FFF，或关键尾随字为 0x7FFF → 坏帧。 */
static bool mlx90640_frame_bad(const u16 *pixels, const u16 *aux, u8 subpage)
{
	int line;

	for (line = 0; line < 24; line++)
	{
		u32 p = line * 32;

		if (pixels[p] == 0x7FFF && (u8)(line % 2) == subpage)
		{
			return true;
		}
	}
	return aux[MLX90640_AUX_PTAT_ART] == 0x7FFF ||
	       aux[MLX90640_AUX_PTAT] == 0x7FFF ||
	       aux[MLX90640_AUX_VDD] == 0x7FFF;
}

/* 交错合并：subpage 0 = 偶数行、subpage 1 = 奇数行（数据手册交错模式）。 */
static void mlx90640_interleave(const u16 *sp0, const u16 *sp1, u8 sub0, u16 *out)
{
	const u16 *even = (sub0 == 0) ? sp0 : sp1;
	const u16 *odd = (sub0 == 0) ? sp1 : sp0;
	int row;
	int col;

	for (row = 0; row < 24; row++)
	{
		for (col = 0; col < 32; col++)
		{
			u32 p = row * 32 + col;

			out[p] = (row & 1) ? odd[p] : even[p];
		}
	}
}

/* 采集一帧：等待就绪 → 双子页读取（坏帧内部重试）；返回 0 或负错误码。
 * 调用者须持有 priv->lock。 */
static int mlx90640_acquire_frame(struct mlx90640_priv *priv, u16 pixels[2][MLX90640_PIXELS],
				  u16 aux[2][MLX90640_RAM_AUX_WORDS], u8 subpage[2])
{
	int attempt;

	for (attempt = 0; attempt < MLX90640_ACQUIRE_RETRIES; attempt++)
	{
		int ret;

		ret = mlx90640_wait_data_ready(priv, MLX90640_DATA_READY_TIMEOUT_MS);
		if (ret < 0)
		{
			return ret;
		}
		ret = mlx90640_read_subpage(priv, pixels[0], aux[0], &subpage[0]);
		if (ret < 0)
		{
			return ret;
		}
		if (mlx90640_frame_bad(pixels[0], aux[0], subpage[0]))
		{
			priv->bad_frames++;
			continue;
		}

		ret = mlx90640_wait_next_subpage(priv, subpage[0],
						 MLX90640_DATA_READY_TIMEOUT_MS);
		if (ret < 0)
		{
			return ret;
		}
		ret = mlx90640_read_subpage(priv, pixels[1], aux[1], &subpage[1]);
		if (ret < 0)
		{
			return ret;
		}
		if (subpage[0] == subpage[1] || mlx90640_frame_bad(pixels[1], aux[1], subpage[1]))
		{
			priv->bad_frames++;
			continue;
		}
		return 0;
	}
	return -EBADMSG;
}

/* ============================ file_operations ============================ */

static int mlx90640_open(struct inode *inode, struct file *file)
{
	struct mlx90640_priv *priv = container_of(inode->i_cdev, struct mlx90640_priv, cdev);

	mutex_lock(&priv->lock);
	if (priv->shutting_down)
	{
		mutex_unlock(&priv->lock);
		return -ENODEV;
	}
	/* 持有设备引用：remove 后 devm 资源（priv）直到本文件关闭才释放（防 UAF） */
	get_device(&priv->client->dev);
	mutex_unlock(&priv->lock);

	file->private_data = priv;
	return 0;
}

static int mlx90640_release(struct inode *inode, struct file *file)
{
	struct mlx90640_priv *priv = file->private_data;

	if (priv)
	{
		put_device(&priv->client->dev);
	}
	return 0;
}

static ssize_t mlx90640_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct mlx90640_priv *priv = file->private_data;
	struct mlx90640_frame frame;
	u16 pixels[2][MLX90640_PIXELS];
	u16 aux[2][MLX90640_RAM_AUX_WORDS];
	u8 subpage[2];
	u32 status_bits = 0;
	u32 frame_id;
	int ret;
	int attempt;

	if (count < sizeof(frame))
	{
		return -EINVAL; /* 仅支持整帧读，禁止部分读 */
	}

	if (file->f_flags & O_NONBLOCK)
	{
		u16 status;

		ret = mlx90640_read_words(priv->client, MLX90640_REG_STATUS, &status, 1);
		if (ret < 0)
		{
			return ret;
		}
		if (!(status & MLX90640_STATUS_NEW_DATA))
		{
			return -EAGAIN;
		}
	}

	if (mutex_lock_interruptible(&priv->lock))
	{
		return -ERESTARTSYS;
	}

	/* 每 read 主动采集（无后台线程）；失败退避在 waitqueue 上等待，
	 * remove 时置 shutting_down 并唤醒，此处返回 -ENODEV。 */
	ret = -EBADMSG;
	for (attempt = 0; attempt < MLX90640_ACQUIRE_RETRIES; attempt++)
	{
		if (priv->shutting_down)
		{
			ret = -ENODEV;
			break;
		}
		ret = mlx90640_acquire_frame(priv, pixels, aux, subpage);
		if (ret == 0)
		{
			break;
		}
		if (ret == -ETIMEDOUT)
		{
			wait_event_interruptible_timeout(priv->waitq, priv->shutting_down,
							 msecs_to_jiffies(MLX90640_POLL_PERIOD_MS));
			continue;
		}
		if (ret == -EBADMSG)
		{
			continue; /* 坏帧重试 */
		}
		break; /* I2C 等硬错误直接返回 */
	}
	if (priv->shutting_down)
	{
		ret = -ENODEV;
	}
	frame_id = priv->frame_id++;
	if (priv->frame_id == 0)
	{
		status_bits |= MLX90640_STATUS_FRAME_OVERFLOW; /* 帧号回绕 */
	}
	mutex_unlock(&priv->lock);

	if (ret < 0)
	{
		return ret;
	}

	/* 组装帧（内核仅搬运：交错合并 + 原始值拷贝，不做温度换算） */
	memset(&frame, 0, sizeof(frame));
	frame.abi_version = MLX90640_ABI_VERSION;
	frame.frame_id = frame_id;
	frame.timestamp_ns = ktime_get_ns();
	if (priv->eeprom_valid)
	{
		status_bits |= MLX90640_STATUS_EEPROM_OK;
	}
	status_bits |= MLX90640_STATUS_SUBPAGE_OK | MLX90640_STATUS_NOT_CALIBRATED;
	frame.status_bits = status_bits;

	mlx90640_interleave(pixels[0], pixels[1], subpage[0], frame.raw);

	/* 帧尾随区值：Ta/Vdd/Gain/PTAT_ART 取后采到的子页；CP 像素按子页各取一份
	 * （官方库约定；UNVERIFIED） */
	frame.ta_raw = aux[1][MLX90640_AUX_PTAT];
	frame.vdd_raw = aux[1][MLX90640_AUX_VDD];
	frame.gain_raw = aux[1][MLX90640_AUX_GAIN];
	frame.ptat_art_raw = aux[1][MLX90640_AUX_PTAT_ART];
	frame.cp_pixel[0] = aux[0][MLX90640_AUX_CP0];
	frame.cp_pixel[1] = aux[1][MLX90640_AUX_CP1];
	frame.subpage_id = subpage[1];

	if (copy_to_user(buf, &frame, sizeof(frame)))
	{
		return -EFAULT;
	}
	return sizeof(frame);
}

static __poll_t mlx90640_poll(struct file *file, poll_table *wait)
{
	struct mlx90640_priv *priv = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &priv->waitq, wait);

	/* 器件无中断线：帧就绪无法主动唤醒 waitq，poll 仅在“当前就绪”或
	 * “设备移除”时立即返回，其余依赖调用方超时（UNVERIFIED 设计说明）。 */
	mutex_lock(&priv->lock);
	if (priv->shutting_down)
	{
		mask |= POLLHUP;
	}
	else
	{
		u16 status;
		int ret = mlx90640_read_words(priv->client, MLX90640_REG_STATUS, &status, 1);

		if (ret == 0 && (status & MLX90640_STATUS_NEW_DATA))
		{
			mask |= POLLIN | POLLRDNORM;
		}
	}
	mutex_unlock(&priv->lock);
	return mask;
}

/* 从缓存的 EEPROM 镜像提取常用全局标定 word（大端 → 主机序）。 */
static void mlx90640_fill_cal_words(struct mlx90640_priv *priv, u16 *cal_words)
{
	int i;

	for (i = 0; i < MLX90640_PARAMS_WORD_COUNT; i++)
	{
		u16 idx = mlx90640_cal_word_idx[i];

		cal_words[i] = (u16)((priv->eeprom[2 * idx] << 8) | priv->eeprom[2 * idx + 1]);
	}
}

static long mlx90640_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct mlx90640_priv *priv = file->private_data;

	switch (cmd)
	{
	case MLX_IOC_GET_ABI:
	{
		__u32 abi = MLX90640_ABI_VERSION;

		if (copy_to_user((void __user *)arg, &abi, sizeof(abi)))
		{
			return -EFAULT;
		}
		return 0;
	}
	case MLX_IOC_GET_PARAMS:
	{
		struct mlx90640_params params;

		/* 显式尺寸校验（ioctl 框架已按 _IOC_SIZE 校验，此处双保险） */
		if (_IOC_SIZE(cmd) != sizeof(params))
		{
			return -EINVAL;
		}
		memset(&params, 0, sizeof(params));
		if (!priv->eeprom_valid)
		{
			return -EIO; /* 探测期 EEPROM 读取失败 */
		}
		params.abi_version = MLX90640_ABI_VERSION;
		params.eeprom_size = MLX90640_EEPROM_SIZE;
		memcpy(params.eeprom, priv->eeprom, MLX90640_EEPROM_SIZE);
		mlx90640_fill_cal_words(priv, params.cal_words);
		if (copy_to_user((void __user *)arg, &params, sizeof(params)))
		{
			return -EFAULT;
		}
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations mlx90640_fops = {
	.owner = THIS_MODULE,
	.open = mlx90640_open,
	.release = mlx90640_release,
	.read = mlx90640_read,
	.poll = mlx90640_poll,
	.unlocked_ioctl = mlx90640_ioctl,
	.llseek = no_llseek,
};

/* ============================ i2c_driver ============================ */

/* 探测期读取 EEPROM（1664 字节，分块读）并做基本校验。 */
static int mlx90640_read_eeprom(struct mlx90640_priv *priv)
{
	u16 *words;
	int ret;
	int i;

	words = kcalloc(MLX90640_EEPROM_WORDS, sizeof(*words), GFP_KERNEL);
	if (!words)
	{
		return -ENOMEM;
	}
	ret = mlx90640_read_words(priv->client, MLX90640_EEPROM_BASE, words,
				  MLX90640_EEPROM_WORDS);
	if (ret < 0)
	{
		kfree(words);
		return ret;
	}

	/* 基本校验：版本 word 非全 0/全 0xFF（详细参数范围校验在用户态 ParseEeprom） */
	if (words[0] == 0x0000 || words[0] == 0xFFFF)
	{
		dev_err(&priv->client->dev, "mlx90640: EEPROM 版本字段非法 (0x%04x)\n", words[0]);
		kfree(words);
		return -EINVAL;
	}

	/* 缓存大端原样镜像（GET_PARAMS 原样下发，用户态解析） */
	for (i = 0; i < MLX90640_EEPROM_WORDS; i++)
	{
		priv->eeprom[2 * i] = (u8)(words[i] >> 8);
		priv->eeprom[2 * i + 1] = (u8)(words[i] & 0xff);
	}
	kfree(words);
	return 0;
}

/* 强制交错模式（CTRL1 位 12 = 0）：官方库仅保证该模式精度（UNVERIFIED）。 */
static int mlx90640_force_interleaved(struct mlx90640_priv *priv)
{
	u16 ctrl;
	int ret = mlx90640_read_words(priv->client, MLX90640_REG_CTRL1, &ctrl, 1);

	if (ret < 0)
	{
		return ret;
	}
	ctrl &= ~MLX90640_CTRL1_INTERLEAVED;
	return mlx90640_write_word(priv->client, MLX90640_REG_CTRL1, ctrl);
}

static int mlx90640_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct mlx90640_priv *priv;
	int err;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
	{
		return -ENOMEM;
	}
	priv->client = client;
	mutex_init(&priv->lock);
	init_waitqueue_head(&priv->waitq);
	i2c_set_clientdata(client, priv);

	/* 超时 100ms（适配器级；影响同总线其他器件，板卡验证时确认无冲突） */
	client->adapter->timeout = msecs_to_jiffies(MLX90640_I2C_TIMEOUT_MS);

	err = mlx90640_read_eeprom(priv);
	if (err < 0)
	{
		dev_err(&client->dev, "mlx90640: EEPROM 读取失败 (%d)\n", err);
		return err;
	}
	priv->eeprom_valid = true;

	err = mlx90640_force_interleaved(priv);
	if (err < 0)
	{
		dev_err(&client->dev, "mlx90640: 设置交错模式失败 (%d)\n", err);
		return err;
	}

	/* cdev 注册（多设备：动态主设备号 + IDA minor） */
	priv->minor = ida_simple_get(&mlx90640_ida, 0, MLX90640_MAX_DEVICES, GFP_KERNEL);
	if (priv->minor < 0)
	{
		return priv->minor;
	}
	priv->devt = MKDEV(MAJOR(mlx90640_devt), priv->minor);

	cdev_init(&priv->cdev, &mlx90640_fops);
	priv->cdev.owner = THIS_MODULE;
	err = cdev_add(&priv->cdev, priv->devt, 1);
	if (err < 0)
	{
		goto err_ida;
	}

	priv->dev = device_create(mlx90640_class, &client->dev, priv->devt, priv,
				  "mlx90640%d", priv->minor);
	if (IS_ERR(priv->dev))
	{
		err = PTR_ERR(priv->dev);
		goto err_cdev;
	}

	dev_info(&client->dev, "mlx90640: 已注册 /dev/mlx90640%d (eeprom=ok)\n",
		 priv->minor);
	return 0;

err_cdev:
	cdev_del(&priv->cdev);
err_ida:
	ida_simple_remove(&mlx90640_ida, priv->minor);
	return err;
}

static void mlx90640_remove(struct i2c_client *client)
{
	struct mlx90640_priv *priv = i2c_get_clientdata(client);

	/* 1) 拒绝新 open 并唤醒所有阻塞 reader（被唤醒者返回 -ENODEV） */
	mutex_lock(&priv->lock);
	priv->shutting_down = true;
	mutex_unlock(&priv->lock);
	wake_up_all(&priv->waitq);

	/* 2) 摘除字符设备（此刻在途 read 仍受 get_device 引用保护，见 open） */
	device_destroy(mlx90640_class, priv->devt);
	cdev_del(&priv->cdev);
	ida_simple_remove(&mlx90640_ida, priv->minor);

	/* 3) priv 由 devm 随 client->dev 释放；最后一个打开的 fd 关闭时归还引用 */
}

static const struct i2c_device_id mlx90640_id[] = {
	{ "mlx90640", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, mlx90640_id);

static const struct of_device_id mlx90640_of_match[] = {
	{ .compatible = "melexis,mlx90640" },
	{ }
};
MODULE_DEVICE_TABLE(of, mlx90640_of_match);

static struct i2c_driver mlx90640_driver = {
	.driver = {
		.name = MLX90640_NAME,
		.of_match_table = mlx90640_of_match,
	},
	.probe = mlx90640_probe,
	.remove = mlx90640_remove,
	.id_table = mlx90640_id,
};

static int __init mlx90640_init(void)
{
	int err;

	err = alloc_chrdev_region(&mlx90640_devt, 0, MLX90640_MAX_DEVICES, MLX90640_NAME);
	if (err < 0)
	{
		return err;
	}
	mlx90640_class = class_create(THIS_MODULE, MLX90640_NAME);
	if (IS_ERR(mlx90640_class))
	{
		err = PTR_ERR(mlx90640_class);
		unregister_chrdev_region(mlx90640_devt, MLX90640_MAX_DEVICES);
		return err;
	}
	err = i2c_add_driver(&mlx90640_driver);
	if (err < 0)
	{
		class_destroy(mlx90640_class);
		unregister_chrdev_region(mlx90640_devt, MLX90640_MAX_DEVICES);
	}
	return err;
}
module_init(mlx90640_init);

static void __exit mlx90640_exit(void)
{
	i2c_del_driver(&mlx90640_driver);
	class_destroy(mlx90640_class);
	unregister_chrdev_region(mlx90640_devt, MLX90640_MAX_DEVICES);
}
module_exit(mlx90640_exit);

MODULE_AUTHOR("animal-monitor project");
MODULE_DESCRIPTION("MLX90640 32x24 thermal camera custom cdev driver (draft, UNVERIFIED)");
MODULE_LICENSE("GPL");
