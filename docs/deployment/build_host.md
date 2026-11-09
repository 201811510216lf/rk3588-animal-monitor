# Host 构建说明（build_host）

## 环境要求

- Windows 10/11 + Visual Studio 2022（含 C++ 工具集）或 Linux（g++/clang++ ≥ C++17）。
- CMake ≥ 3.16；本机使用 VS2022 自带的 CMake/Ninja。
- **不依赖** Rockchip SDK / OpenCV / FFmpeg（`ANIMAL_MONITOR_ENABLE_TARGET_DEPS=OFF` 默认）。

## 命令

```powershell
# 方式一：脚本
powershell -ExecutionPolicy Bypass -File scripts\build_host.ps1

# 方式二：手动（本机 VS2022 路径示例）
& "D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
& "D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build
& "D:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir build --output-on-failure
```

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## 实际验证结果（2026-11-06，Windows + MSVC 19.44 /W4 /permissive-）

```text
[1/8] … [8/8] Linking 全部目标成功（animal_monitor_core.lib、animal_monitor_app.exe、5 个测试 exe）
ctest：100% tests passed, 0 tests failed out of 5
  test_bounded_queue / test_timestamp_aligner / test_modbus_crc / test_config / test_metrics
运行示例：
  build\bin\animal_monitor_app.exe --config configs\default.yaml
  → [SERVICE] mock_pipeline (MOCK：…)；2 秒后自动退出，退出码 0；
  → mock_pipeline stats: produced=33 consumed=33 dropped=0 queue_left=0
```

编译告警：0（MSVC /W4、GNU -Wall -Wextra -Wpedantic 均开启）。

## 产物

- `build/bin/animal_monitor_app.exe`
- `build/bin/test_*.exe`（注册于 CTest）
- `build/lib/animal_monitor_core.lib`
