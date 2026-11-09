// tests/unit/test_sha256.cpp
// 状态：implemented（host）
// 说明：SHA-256 权威向量验证（FIPS 180-4 附录样例）与文件哈希路径。

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "test_harness.h"

#include "animal_monitor/common/sha256.h"

using animal_monitor::Sha256File;
using animal_monitor::Sha256Hex;
using animal_monitor::Status;
using animal_monitor::StatusCode;

namespace {

AM_TEST(empty_string_vector)
{
    AM_EXPECT_EQ(Sha256Hex(nullptr, 0),
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

AM_TEST(abc_vector)
{
    const std::uint8_t data[] = {'a', 'b', 'c'};
    AM_EXPECT_EQ(Sha256Hex(data, sizeof(data)),
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

AM_TEST(multi_block_vector)
{
    // 超过一个 512-bit 块，验证块间链接逻辑。
    const char* text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    AM_EXPECT_EQ(Sha256Hex(reinterpret_cast<const std::uint8_t*>(text),
                           std::char_traits<char>::length(text)),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

AM_TEST(file_sha256_matches_content)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "am_sha256_test_abc.bin";
    {
        std::ofstream file(path, std::ios::binary);
        AM_EXPECT(file.is_open());
        file << "abc";
    }
    std::string  hex;
    const Status status = Sha256File(path.string(), &hex);
    AM_EXPECT(status.Ok());
    AM_EXPECT_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    std::error_code error;
    std::filesystem::remove(path, error);
}

AM_TEST(missing_file_returns_not_found)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "am_sha256_missing_file_xyz.bin";
    std::string  hex;
    const Status status = Sha256File(path.string(), &hex);
    AM_EXPECT(!status.Ok());
    AM_EXPECT_EQ(static_cast<int>(status.Code()), static_cast<int>(StatusCode::kNotFound));
}

AM_TEST(null_output_rejected)
{
    const Status status = Sha256File("whatever.bin", nullptr);
    AM_EXPECT(!status.Ok());
    AM_EXPECT_EQ(static_cast<int>(status.Code()), static_cast<int>(StatusCode::kInvalidArgument));
}

}  // namespace

int main() { return am_test::RunAll(); }
