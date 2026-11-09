# 依赖矩阵（dependency_matrix）

> 版本信息来自源项目审计；未标注版本的一律 UNVERIFIED，目标板以实际 SDK 为准。

## 1. Rockchip SDK

| 组件 | 用途 | 审计来源/版本 | 本工程获取方式 | 状态 |
|---|---|---|---|---|
| MPP (librockchip_mpp) | 硬解/硬编码 | A\mpp\lib（版本未标注） | 板卡 BSP/系统包；FindRockchipMPP.cmake 探测 | UNVERIFIED |
| RGA (librga, im2d) | 缩放/颜色转换/合成 | A\rga\lib（im2d API） | 板卡 BSP/系统包 | UNVERIFIED |
| RKNN (librknnrt/librknn_api) | NPU 推理 | A\rknn（rknn_context=uint64_t；API 1.4.x~2.x 系） | 板上 rknn_server 配套版本 | UNVERIFIED |
| ZLMediaKit C API | RTSP/WebRTC 发布 | Ref lib（libmk_api.so + libZLToolKit.so） | 按目标板架构自建或厂商提供 | UNVERIFIED |

## 2. 第三方库

| 库 | 用途 | 审计来源/版本 | 策略 |
|---|---|---|---|
| OpenCV 4 | 颜色转换/缩放/显示（审计链路大量使用） | A\opencv4-1（lib.tar.gz，版本未标注） | 目标链逐步替换为 RGA；仅保留必要时使用；不入库 |
| FFmpeg 6.x / 7.1 | RTSP 拉流 demux/BSF、RTMP muxing | A\ffmpeg-lite(6.x)/ffmpeg7-1(7.1) | 网络/文件输入源与 RTMP 推流用；不入库 |
| ZLToolKit | ZLM 运行时依赖 | Ref lib | 与 ZLM 同源提供 |
| yaml-cpp（计划） | 全量 YAML（替代 Phase 1 自有子集解析器） | — | third_party/ 固定版本或 FetchContent（离线备选：vendor） |
| SQLite（计划） | 本地结构化存储 | — | 系统包/自建 |

## 3. 工具链

| 工具 | 版本（已实测/审计） | 备注 |
|---|---|---|
| CMake | 本机 3.29+（VS2022 自带）；A/B 用 3.16.3 | 要求 ≥ 3.16 |
| MSVC | 19.44（VS2022 Professional，/W4 /permissive-） | host 构建已实测 |
| g++（目标板） | B 的 build 记录为板端原生（CMakeCache：/usr/bin/c++） | 目标工具链 UNVERIFIED |
| clang-format | VS2022 自带 LLVM 工具（.clang-format 已就位） | 提交前格式化 |

## 4. 禁止事项

- 不把 SDK 动态库/模型大文件/FFmpeg/OpenCV 拷入本仓库（体积与许可）。
- 不混用不同 SDK 版本的头文件与动态库。
- 引入 third_party 依赖前核对许可并记录（见 third_party/README.md）。
