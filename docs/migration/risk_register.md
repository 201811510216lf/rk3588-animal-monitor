# 风险登记册（risk_register）

> 风险编号沿用；状态：`open` / `mitigated` / `accepted` / `watch`。所有硬件结论 UNVERIFIED。

## 高风险（必须处理才能进入对应 Phase）

| 编号 | 风险 | 影响 | 缓解措施 | 状态 |
|---|---|---|---|---|
| R-01 | A/B 许可证为学习/研究限定（禁商用/再分发/衍生再分发），版权方 HeXiaotian | 迁移代码进入新项目即构成衍生；商用/交付即违约 | 商用前取得版权方书面授权；迁移代码保留原许可与出处；新写代码 Apache-2.0 且与迁移代码分离（NOTICE 已声明）；迁移矩阵经用户确认后才动手 | open（阻塞 Phase 2 代码迁移的许可面） |
| R-02 | 板卡/Sensor/MIPI/GPIO/I2C/SPI/UART/DTS 全部未定 | 硬件绑定实现（采集/驱动/部署）无法落地 | 一律 Board Profile + UNVERIFIED 标记；先接口+mock；板卡确定后逐项填依据 | open |
| R-03 | Grooming/Feeding/Rearing 三行为模型文件不存在 | 核心业务功能不可实现 | 只建 IInferenceModel 接口与 manifest 模板；禁止编造模型结构与结果；先以现有 person_relu 契约验证推理链 | open |
| R-04 | B 当前源码不可编译（stream_loader.cpp:148 语法错误），且存在 prop 未赋值、mpp_ctx 遮蔽等严重缺陷 | 直接迁移即带病上线 | 迁移 = 重构而非复制；每文件按审计缺陷清单修复并有对应 host/目标测试 | mitigated（通过 module_mapping 的重构策略） |
| R-05 | 源项目含明文 RTSP 密码（`admin:******`，已脱敏）等凭据 | 泄露风险、坏习惯扩散 | 新工程禁止任何凭据入库；配置用占位符，运行时从受保护环境注入 | mitigated（规范+config 模板已落实） |
| R-06 | host 与目标 SDK 版本不齐（MPP/RGA/RKNN 头文件无版本标注） | 目标板链接/运行失败 | 依赖矩阵记录实际版本；Find 模块探测+清晰报错；禁止混用不同 SDK 版本 | watch |

## 中风险

| 编号 | 风险 | 影响 | 缓解措施 | 状态 |
|---|---|---|---|---|
| R-07 | 零拷贝链路（DMABUF/fence/cache）在目标 BSP 上的行为未经实测 | 花屏/丢帧/稳定性问题 | buffer_lifecycle.md 明确所有权协议；目标板逐环节硬件测试（Phase 2 测试清单）；不臆造 fence/cache 规则 | open |
| R-08 | NPU 三模型并发策略未实测（serial/shared/multi-context） | 推理延迟或 NPU 抢占 | 策略配置化；以 RK3588 实测为准；scheduler 支持 round-robin/priority/per-camera | open |
| R-09 | 多路 CPU 预处理（NV12→BGR→RGB 三重拷贝）压力线性放大 | 多路掉帧 | 链路改 RGA im2d（cvtcolor/resize）；输入直连 NV12 或 DMABUF 路径；队列丢帧策略兜底 | open |
| R-10 | ZLMediaKit 内嵌发布与 WebRTC 的 SPS/PPS/首屏问题（Ref 自造 JSON/URL 解析） | 首屏延迟/兼容性 | 保留 SPS/PPS/IDR 正确性；时间戳单调化；JSON 库替代手写解析 | open |
| R-11 | 断网缓存/补传/OTA 涉及磁盘与签名策略 | 数据丢失/安全事件 | 有界离线队列+ACK/去重/指数退避；SHA256+签名验证；A/B 分区设计以真实 bootloader 为准 | open（Phase 7） |
| R-12 | 融合模块（IoU/IoM/类别无关 NMS）语义缺陷 | 误报/漏报 | 迁移时修类别无关 NMS；融合输出接入画框链路；host 合成框单元测试 | open |

## 低风险 / 已接受

| 编号 | 风险 | 影响 | 缓解措施 | 状态 |
|---|---|---|---|---|
| R-13 | Ref 仓库为浅克隆，无历史审计 | 参考信息不完整 | 仅作风格/架构参考，不复制实现 | accepted |
| R-14 | 流程图.png 无法读取内容 | 设计意图信息缺失 | 不影响审计结论；需要时向用户索取原图说明 | accepted |
| R-15 | Phase 1 采用自研 YAML 子集解析器 | 与标准 YAML 的兼容边界 | 已文档化子集语义并单测；Phase 2+ 可替换为 yaml-cpp 且接口不变 | accepted（记录） |
| R-16 | 本会话 DSH 沙箱无工作区写权限（环境问题，非项目风险） | 构建需每次申请授权 | 已按诊断流程修复工作区根目录权限并保留回滚记录；正常环境不受影响 | accepted（记录） |
