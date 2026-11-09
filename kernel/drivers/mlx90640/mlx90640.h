/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ============================================================================
 * 文件：kernel/drivers/mlx90640/mlx90640.h（驱动内部头，非 UAPI）
 * 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未经编译、未在目标内核验证）
 * 依据：任务书 CODEX_MASTER_TASK.md §7.1；README 接口决策（标准 cdev，禁止 miscdevice）
 * ============================================================================
 */
#ifndef _MLX90640_DRV_H
#define _MLX90640_DRV_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "uapi/mlx90640.h"

#define MLX90640_NAME          "mlx90640"
#define MLX90640_MAX_DEVICES   8          /* 静态上限（多设备经 IDA 分配 minor） */

/* —— 器件 I2C/寄存器地址（数据手册公开知识；UNVERIFIED 以板卡实测为准）—— */
#define MLX90640_I2C_ADDR      0x33      /* 7 位地址 */
#define MLX90640_EEPROM_BASE   0x2400    /* 832 word = 1664 字节 */
#define MLX90640_EEPROM_WORDS  832
#define MLX90640_RAM_BASE      0x0400    /* 像素区 768 word */
#define MLX90640_RAM_PIXEL_WORDS 768
#define MLX90640_RAM_AUX_BASE  0x0700    /* 尾随计算因子区 64 word */
#define MLX90640_RAM_AUX_WORDS 64
#define MLX90640_REG_STATUS    0x8000    /* 状态寄存器（可读可写） */
#define MLX90640_REG_CTRL1     0x800D    /* 控制寄存器 1 */

#define MLX90640_STATUS_NEW_DATA   0x0008  /* 位 3：子页测量完成 */
#define MLX90640_STATUS_SUBPAGE    0x0001  /* 位 0：刚完成的子页号 */
#define MLX90640_FREEZE_RAM        0x0030  /* 写状态寄存器：冻结 RAM 更新（官方库 SynchFrame） */
#define MLX90640_CTRL1_INTERLEAVED 0x1000  /* CTRL1 位 12：0 = 交错模式（1 = 棋盘模式） */

/* —— 帧尾随区字偏移（0x0700 起，官方库约定；UNVERIFIED 以数据手册 §10.2.2 复核）—— */
#define MLX90640_AUX_PTAT_ART  0         /* word 768 */
#define MLX90640_AUX_CP0       8         /* word 776 */
#define MLX90640_AUX_GAIN      10        /* word 778 */
#define MLX90640_AUX_PTAT      32        /* word 800 */
#define MLX90640_AUX_CP1       40        /* word 808 */
#define MLX90640_AUX_VDD       42        /* word 810 */

/* —— I2C 传输参数（任务书要求：分块读、重试 3 次、超时 100ms）—— */
#define MLX90640_I2C_RETRIES      3
#define MLX90640_I2C_CHUNK_WORDS  32   /* 每块 32 word（64 字节） */
#define MLX90640_I2C_TIMEOUT_MS   100  /* 适配器级超时（probe 时设置，总线级生效） */
#define MLX90640_RETRY_DELAY_MS   10   /* 重试间隔 */

/* —— 帧采集参数 —— */
#define MLX90640_DATA_READY_TIMEOUT_MS 600 /* 数据就绪等待（2Hz 帧率子页间隔 250ms 兜底） */
#define MLX90640_ACQUIRE_RETRIES  3        /* 坏帧/超时重试次数 */
#define MLX90640_POLL_PERIOD_MS   100      /* read 等待队列上的单次等待周期 */

/* 每设备私有数据（devm 分配，随 i2c 设备释放；UAF 防护见 open/release 的 get_device） */
struct mlx90640_priv
{
	struct i2c_client *client;
	struct cdev cdev;
	struct device *dev;             /* device_create 结果 */
	dev_t devt;
	int minor;

	struct mutex lock;              /* 串行化 read 采集（每次 read 独立采集一帧） */
	wait_queue_head_t waitq;        /* remove 唤醒阻塞 reader；帧就绪靠 read 内轮询 */
	bool shutting_down;

	u8 eeprom[MLX90640_EEPROM_SIZE]; /* 探测期 EEPROM 镜像（大端原样缓存） */
	bool eeprom_valid;               /* 基本校验通过（版本 word 非 0x0000/0xFFFF） */

	u32 frame_id;                    /* 每次 read 递增（锁内自增） */
	u32 bad_frames;                  /* 坏帧计数（诊断） */
	u32 i2c_errors;                  /* I2C 错误计数（诊断） */
};

/* I2C 分块读：2 字节大端地址 + count 个 16 位大端 word；重试 3 次。 */
int mlx90640_read_words(struct i2c_client *client, u16 addr, u16 *words, u16 count);

/* I2C 单 word 写（大端）。 */
int mlx90640_write_word(struct i2c_client *client, u16 addr, u16 value);

#endif /* _MLX90640_DRV_H */
