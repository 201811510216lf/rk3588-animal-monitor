/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ============================================================================
 * 文件：kernel/drivers/htu21d/htu21d.h（驱动内部头）
 * 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未经编译、未在目标内核验证）
 * 依据：任务书 CODEX_MASTER_TASK.md §7.2；README（IIO 路线）
 * ============================================================================
 */
#ifndef _HTU21D_DRV_H
#define _HTU21D_DRV_H

#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define HTU21D_NAME "htu21d"

/* —— 命令（数据手册公开知识；UNVERIFIED 以板卡实测为准）—— */
#define HTU21D_CMD_TRIG_TEMP    0xE3  /* 温度测量，no-hold */
#define HTU21D_CMD_TRIG_HUMID   0xE5  /* 湿度测量，no-hold */
#define HTU21D_CMD_WRITE_USER   0xE6  /* 写用户寄存器（分辨率配置） */
#define HTU21D_CMD_SOFT_RESET   0xFE  /* 软复位（预留） */

/* 用户寄存器默认 0x02：RH 12 位 / T 14 位（数据手册默认；UNVERIFIED） */
#define HTU21D_USER_RESOLUTION  0x02

/* 保守转换等待时间（数据手册最大转换时间：T 14 位 ≤ 85ms、RH 14 位 ≤ 50ms） */
#define HTU21D_TEMP_CONV_MS     85
#define HTU21D_HUMID_CONV_MS    50

/* 换算公式（数据手册）：T = -46.85 + 175.72*St/2^16；RH = -6 + 125*Srh/2^16。
 * IIO SCALE 只提供斜率（175.72/65536、125/65536，INT_PLUS_NANO），
 * 常数项并入用户态公式（htu21d_math / PROCESSED 路径），不提供 OFFSET 通道。 */
#define HTU21D_TEMP_SCALE_NUM   175720   /* 175.72 × 1000（毫单位换算） */
#define HTU21D_TEMP_OFFSET_MILLI (-46850) /* -46.85°C 的毫单位 */
#define HTU21D_HUMID_SCALE_NUM  125000   /* 125 × 1000 */
#define HTU21D_HUMID_OFFSET_MILLI (-6000) /* -6%RH 的毫单位 */
#define HTU21D_SCALE_DENOM      65536

struct htu21d_priv
{
	struct i2c_client *client;
	struct mutex lock;      /* 串行化单次测量（器件同时只能有一个转换） */
	u32 crc_errors;         /* CRC 校验失败计数（诊断） */
	u32 io_errors;          /* I2C 错误计数（诊断） */
};

/* CRC-8（多项式 0x131、初值 0x00、MSB 先），覆盖两个数据字节。 */
u8 htu21d_crc8(u8 msb, u8 lsb);

#endif /* _HTU21D_DRV_H */
