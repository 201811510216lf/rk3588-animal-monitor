# Board Profiles

板卡、Sensor、GPIO、I2C/SPI/UART 总线、MIPI lane、clock/regulator、DTS 节点等
一切硬件绑定信息，**当前均为 UNVERIFIED**：目标板卡尚未确定，禁止臆造。

## 规则

1. 所有硬件绑定值一律写在 `configs/boards/<board>.yaml`，代码内禁止硬编码。
2. 每项取值必须标注依据（原理图页码 / BSP 配置 / 实测），无依据一律 `UNVERIFIED`。
3. 新板卡：复制 `board-template.yaml` 填写，并在 `configs/default.yaml` 的
   `board_profile` 指向新文件。
4. 真实 DTS/内核配置的落地路径见 `kernel/dts/` 与 `kernel/configs/`（Phase 4+）。

## 状态

| 文件 | 状态 |
|---|---|
| generic-rk3588.yaml | stub（结构占位，全部 UNVERIFIED） |
| board-template.yaml | 模板 |
