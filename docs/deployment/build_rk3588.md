# 目标板构建说明（build_rk3588）

> 状态：UNVERIFIED —— 板卡/BSP/交叉工具链未确定。以下为通用流程模板，
> 实际编译器三元组、sysroot、SDK 路径以板卡 BSP 为准，禁止照抄。

## 前置条件

1. 确定板卡型号并取得 BSP（内核源码、aarch64 工具链、sysroot）。
2. SDK 组件：Rockchip MPP（头文件 rk_mpi.h + librockchip_mpp）、RGA（im2d.h + librga）、
   RKNN（librknnrt，与板上 rknn_server 版本匹配）、（Phase 2 起）ZLMediaKit C API。
3. 按 `configs/boards/board-template.yaml` 建立真实板卡 profile（逐项标依据）。

## 构建（模板）

```bash
cmake -S . -B build-rk -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/rk3588-aarch64-linux-gnu.cmake \
      -DANIMAL_MONITOR_ENABLE_TARGET_DEPS=ON \
      -DRK_TOOLCHAIN_PREFIX=aarch64-<vendor>-linux-gnu \
      -DRK_SYSROOT=/path/to/sysroot \
      -DRK_SDK_ROOT=/path/to/bsp/sdk
cmake --build build-rk -j$(nproc)
```

或使用 CMakePresets 的 target-rk3588 preset（同名缓存变量覆盖）。

## 部署（模板，非破坏性）

```bash
# 手工部署验证（脚本：scripts/deploy_target.sh）
scp build-rk/bin/animal_monitor_app  root@<设备IP>:/usr/local/bin/
scp configs/default.yaml             root@<设备IP>:/etc/animal_monitor/
ssh  root@<设备IP> "systemctl restart animal-monitor"   # 仅当已安装 systemd unit
```

`deploy/systemd/` 提供 unit 模板；**未确认分区表/bootloader 前禁止任何刷写、OTA、重启
目标设备操作**（任务书第 15 节第 6 条）。

## 验证顺序（Phase 2 硬件测试，全部待板卡到位）

scripts/test_camera.sh（V4L2 枚举/格式/fps）→ 单路链路 → 双路链路 → 合成推流 →
多模型推理 → 传感器 I2C/IIO → 外设逐个（CAN/RS485/GPIO/PWM/ADC/RTC/EEPROM/Watchdog）。

## 已知风险

- 工具链三元组因厂商而异（aarch64-rockchip-linux-gnu / aarch64-none-linux-gnu /
  aarch64-buildroot-linux-gnu 等），必须以 BSP 为准。
- MPP/RGA/RKNN 头文件版本须与板上库一致，禁止混用不同 SDK 版本。
