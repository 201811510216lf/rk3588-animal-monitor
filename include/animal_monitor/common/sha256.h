#pragma once

// animal_monitor/common/sha256.h
// 状态：implemented（纯 C++17，无外部依赖，FIPS 180-4）
// 说明：模型 manifest 完整性校验用 SHA-256。输出一律小写十六进制。

#include <cstddef>
#include <cstdint>
#include <string>

#include "animal_monitor/common/status.h"

namespace animal_monitor {

// 计算字节序列的 SHA-256，返回 64 字符小写十六进制。
// data 为 nullptr 时 size 必须为 0（空数据哈希）。
std::string Sha256Hex(const std::uint8_t* data, std::size_t size);

// 计算文件 SHA-256 并写入 *hex（64 字符小写十六进制）。
// 文件不存在返回 kNotFound；读取失败返回 kUnavailable。
Status Sha256File(const std::string& path, std::string* hex);

}  // namespace animal_monitor
