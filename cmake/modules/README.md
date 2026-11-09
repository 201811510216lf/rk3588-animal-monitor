# cmake/modules/

本目录预留 CMake 辅助模块（缓存文件、Find 模块扩展、函数库）。
当前 Find 模块放在 `cmake/` 根（FindRockchipMPP.cmake），按需补充：

- FindRga.cmake / FindRknn.cmake / FindZlm.cmake —— Phase 2~3 随对应链路迁移补充。
- 探测失败必须给出清晰报错（路径提示 + 版本提示），禁止静默降级为假实现。
