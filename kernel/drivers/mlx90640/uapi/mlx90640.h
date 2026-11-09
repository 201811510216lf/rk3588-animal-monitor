/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ============================================================================
 * 文件：kernel/drivers/mlx90640/uapi/mlx90640.h
 * 状态：implemented（初稿）/ hardware-blocked / UNVERIFIED（未经编译、未在目标内核验证）
 * 依据：任务书 CODEX_MASTER_TASK.md §7.1；kernel/drivers/mlx90640/README.md 接口决策
 *       （标准 cdev + 固定宽度 UAPI + 每 read 主动采集）
 * 说明：
 *   - 32/64 位兼容：全部固定宽度类型（__u32/__s64/__u16），结构无指针、无隐式对齐差异；
 *     结构末尾均预留保留字段，未来扩展只追加不重排（ABI 版本保护）。
 *   - ioctl 编号：_IOR('M', 0x01, ...) / _IOR('M', 0x02, ...)。魔数 'M'(0x4D) 与 mtd-abi
 *     相同，但 ioctl 作用域按字符设备文件隔离，互不冲突；若需全局唯一可换 'X'（预留说明）。
 *   - 温度换算全部在用户态完成（内核只采集与搬运，见 mlx90640_math.*）。
 * ============================================================================
 */
#ifndef _UAPI_MLX90640_H
#define _UAPI_MLX90640_H

#include <linux/types.h>

/* 像素数：32 × 24 = 768。 */
#define MLX90640_PIXELS 768

/* EEPROM 镜像大小：0x2400 起 832 个 16 位大端 word = 1664 字节。 */
#define MLX90640_EEPROM_SIZE 1664

/* ABI 版本：结构布局变更时必须递增（只追加/重命名不破坏，破坏性变更需新版本协商）。 */
#define MLX90640_ABI_VERSION 1

/*
 * 帧状态位（与用户态 include/animal_monitor/thermal/thermal_frame.h 的
 * thermal_status_bits 保持同值，禁止单侧改动）。
 */
#define MLX90640_STATUS_SUBPAGE_OK      (1U << 0) /* 两个 subpage 均有效并完成交错 */
#define MLX90640_STATUS_EEPROM_OK       (1U << 1) /* 探测期 EEPROM 读取/基本校验通过 */
#define MLX90640_STATUS_OUT_OF_RANGE    (1U << 2) /* 本帧存在 0x7FFF 等异常读数（预留） */
#define MLX90640_STATUS_BROKEN_MASKED   (1U << 3) /* 坏像素已按规则剔除/标记 */
#define MLX90640_STATUS_NOT_CALIBRATED  (1U << 4) /* 输出为未标定估算（恒置位） */
#define MLX90640_STATUS_FRAME_OVERFLOW  (1U << 5) /* 帧号溢出/丢帧 */
#define MLX90640_STATUS_UNSUPPORTED_PATTERN (1U << 6) /* 非交错模式（未支持，预留） */

/*
 * 每帧数据：每次 read() 返回一个完整帧（sizeof 为基准，禁止部分读）。
 * raw[] 为内核已按交错模式合并的 32×24 原始读数（行优先）。
 * 初稿扩展字段（gain_raw/cp_pixel/ptat_art_raw/subpage_id）供用户态做完整
 * 温度换算（官方库 GetImage/CalculateTo 需要帧尾随区 CP/Gain/PTAT_ART），
 * ABI v1 尚未冻结，见 README「遗留问题」。
 */
struct mlx90640_frame
{
	__u32 abi_version;              /* MLX90640_ABI_VERSION */
	__u32 frame_id;                 /* 每次 read 递增，溢出回绕并置 FRAME_OVERFLOW */
	__u32 status_bits;              /* MLX90640_STATUS_* 组合 */
	__u32 reserved;                 /* 对齐/扩展预留，置 0 */
	__s64 timestamp_ns;             /* ktime_get_ns()（CLOCK_MONOTONIC） */
	__u16 raw[MLX90640_PIXELS];     /* 已交错合并的原始读数，大端转主机序 */
	__u16 ta_raw;                   /* RAM word 800（0x0A40）PTAT 原始值 */
	__u16 vdd_raw;                  /* RAM word 810（0x0A54）VDD 原始值 */
	/* —— 初稿扩展（温度换算所需帧尾随区，ABI v1 未冻结）—— */
	__u16 gain_raw;                 /* RAM word 778（0x0A14）帧内增益 */
	__u16 cp_pixel[2];              /* RAM word 776/808（0x0A10/0x0A50）CP 像素 */
	__u16 ptat_art_raw;             /* RAM word 768（0x0A00）PTAT_ART */
	__u16 subpage_id;               /* 本帧最后采到的 subpage 号（0/1） */
};

/*
 * 标定参数拷贝（MLX_IOC_GET_PARAMS）：完整 EEPROM 镜像 + 常用全局标定 word 的
 * 主机序原始值。用户态用 ParseEeprom()（mlx90640_math）解析，内核不实现温度算法。
 * cal_words 顺序固定（word 索引 → 字节地址 0x2400 + 2×索引）：
 *   16=CTRL1、17=offsetRef、32=alpha 尺度、33=alphaRef、48=增益(gainEE)、
 *   49=vPTAT25、50=Kt1/Kt2(KtPTAT/KvPTAT)、51=kVdd/vdd25、52=KvT、53=棋盘系数、
 *   54/55=KtaRC、56=分辨率/尺度、57=cpAlpha、58=cpOffset、59=cpKta/cpKv、
 *   60=KsTa/tgc、61/62=KsTo、63=ct/step/KsToScale。
 */
#define MLX90640_PARAMS_WORD_COUNT 20

struct mlx90640_params
{
	__u32 abi_version;
	__u32 eeprom_size;                              /* 应为 MLX90640_EEPROM_SIZE */
	__u8  eeprom[MLX90640_EEPROM_SIZE];             /* 完整 EEPROM 镜像（大端原样） */
	__u16 cal_words[MLX90640_PARAMS_WORD_COUNT];    /* 常用全局标定 word（主机序） */
	__u8  reserved[6];                              /* 对齐/扩展预留，置 0 */
};

/* 获取 ABI 版本号（open 后必须先校验）。 */
#define MLX_IOC_GET_ABI     _IOR('M', 0x01, __u32)

/* 获取标定参数拷贝（EEPROM 镜像 + 常用 word）。 */
#define MLX_IOC_GET_PARAMS  _IOR('M', 0x02, struct mlx90640_params)

#endif /* _UAPI_MLX90640_H */
