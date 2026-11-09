# MLX90640 自定义 I2C 驱动

> 状态：implemented（初稿）/ hardware-blocked（板卡/内核/BSP 未定；代码未经编译、未在目标内核验证，UNVERIFIED）。语言：**内核 C**。禁止 miscdevice。

## 接口决策（已定，2026-11-09）

- **采用标准字符设备（cdev）**：`cdev_init/cdev_add + device_create`（**不用 miscdevice**），
  固定宽度 UAPI（`uapi/mlx90640.h`，ABI 版本 1）+ **每 read 主动采集**（驱动内无后台线程，
  read 阻塞在 waitqueue、poll 支持）+ 多设备（动态主设备号 + IDA minor + /dev/mlx90640%d）。
- 选择理由：本项目消费方为固定客户端（mlx90640_client），无需 V4L2 生态兼容；
  UAPI 结构、流控、多 reader 语义均可控（缺点可接受）。
- **V4L2 备选评估结论保留**：主线 video-i2c 存在 MLX90640 支持讨论/补丁
  （2021 年 lore.kernel.org 上提出的支持补丁，合入状态未确认）；若后续需要与视频链
  统一（QBUF/DQBUF/流控复用）或需要通用软件生态，可再评估迁移 V4L2 thermal capture。
  迁移成本集中在 UAPI 层，驱动采集逻辑（EEPROM/双子页/交错合并）可复用。

## 待评估的接口选择（实现前必须先决策并记录 ADR）

| 方案 | 优点 | 缺点 |
|---|---|---|
| V4L2 热成像捕获（video-i2c 路线） | 与视频链统一（QBUF/DQBUF/流控）；生态兼容 | 需评估 video-i2c 对 32×24 RAW 的支持与维护成本 |
| 标准字符设备（cdev） | 简单直接；固定宽度 UAPI 可控 | 需要自定义 UAPI/流控/多 reader 语义 |

## 功能要求（源自任务书 §7.1）

1. 读取 EEPROM 校准参数并验证提取过程（禁止硬编码校准）。
2. 按协议读两个 subpage，检查 subpage/frame 状态，组成 32×24 完整热场。
3. I2C 分块、传输长度、字节序、错误重试、超时、设备移除处理。
4. 输出原始数据与校准后温度数据，单位定义明确。
5. UAPI（若 cdev）：固定宽度结构 + ABI 版本 + frame_id + timestamp + pixel_count +
   温度单位 + 状态位；ioctl 编号防冲突；read/poll/nonblock 行为明确；
   多 reader 语义明确；remove 时唤醒阻塞 reader，防 UAF。
6. 复杂温度算法放用户态（内核只做采集与搬运）。

## 验证脚本

- scripts/test_i2c.sh（总线探测与分块读测试，板卡到位后执行）。

## 交付内容（Phase 4 初稿）

| 文件 | 说明 |
|---|---|
| `uapi/mlx90640.h` | 固定宽度 UAPI：帧结构、GET_ABI/GET_PARAMS ioctl、状态位（与用户态 thermal_frame.h 同值） |
| `mlx90640.c` / `mlx90640.h` | i2c_driver + cdev；EEPROM 1664 字节分块读（重试 3 次/超时 100ms）；每 read 采双子页 + 交错合并；remove 唤醒 + -ENODEV + get_device 防 UAF |
| `Kconfig` / `Makefile` | CONFIG_MLX90640；注意与主线 video-i2c 的 MLX90640 支持二选一 |
| 用户态 | `include/animal_monitor/thermal/mlx90640_math.h`（ParseEeprom/InterleaveSubpages/TgcCompensation/CalcTa/CalcTo，host 可测）、`mlx90640_client.{h,cpp}`（cdev 客户端，仅目标构建编译） |

## 验证方法（板卡到位后）

1. `scripts/test_i2c.sh` 扩展：总线探测（0x33 应答）→ 0x2400 起 832 word 分块读，
   比对 EEPROM 版本 word 与标定常数（0x2405 = 0x10）。
2. 驱动装载后 `i2cdump` 抽查 + dmesg 观察探测日志；`cat` 不可用（二进制帧），
   用 `mlx90640_client` 或 `test_i2c.sh` 的 read 分支读帧并检查
   status_bits（kSubpageOk|kEepromOk|kNotCalibrated）。
3. 用户态 host 单元测试：`tests/unit/test_mlx90640_math.cpp`（合成 EEPROM →
   解析/交错/TGC/CalcTa/CalcTo 数值自洽 + 越界拒绝；MSVC /W4 已验证 21/21 通过）。
4. 板卡实测温度前必须做参考测温标定；未标定前输出一律标记 NOT_CALIBRATED，
   **禁止宣称准确体温**（AGENTS.md 规则 5 / 任务书 §7.1）。

## 遗留问题（板卡到位后处理）

- 寄存器/尾随区偏移（0x8000/0x800D/0x0400/0x0700、aux word 768/776/778/800/808/810）
  依据数据手册与 Melexis 官方库整理，需用真实 EEPROM 镜像复核（UNVERIFIED）。
- `struct mlx90640_frame` 含初稿扩展字段（gain_raw/cp_pixel/ptat_art_raw/subpage_id），
  ABI v1 未冻结；冻结前可裁剪或正式化。
- 棋盘模式（CTRL1 位 12 = 1）未支持：probe 强制交错模式，若需棋盘模式须扩展
  UAPI 与交错规则。
- adapter->timeout 100ms 为总线级设置，影响同总线其他器件，板卡验证时确认。
- 设备无中断线：poll 仅在“当前就绪/设备移除”时立即返回，其余依赖超时；
  如需高响应可后续引入内核线程 + 定时采集（当前设计为“每 read 主动采集”）。
- 分辨率/刷新率动态配置（0x800D）未做，初稿沿用器件默认（2Hz 附近）。
