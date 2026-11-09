#include "animal_monitor/common/sha256.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace animal_monitor {

namespace {

// SHA-256 轮常量（FIPS 180-4 §4.2.2）。
constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<std::uint32_t, 8> kSha256InitialState = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr std::uint32_t RotateRight(std::uint32_t value, unsigned int bits)
{
    return (value >> bits) | (value << (32u - bits));
}

// FIPS 180-4 流式实现：按字节流处理，避免对齐/端序/别名陷阱。
class Sha256Context
{
  public:
    void Update(const std::uint8_t* data, std::size_t size)
    {
        total_bytes_ += size;
        while (size > 0)
        {
            const std::size_t take = std::min(size, kBlockSize - buffer_used_);
            std::memcpy(buffer_.data() + buffer_used_, data, take);
            buffer_used_ += take;
            data += take;
            size -= take;
            if (buffer_used_ == kBlockSize)
            {
                ProcessBlock(buffer_.data());
                buffer_used_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> Final()
    {
        const std::uint64_t bit_length = total_bytes_ * 8u;
        const std::uint8_t  pad_byte   = 0x80;
        Update(&pad_byte, 1);
        const std::uint8_t zero = 0;
        while (buffer_used_ != 56)
        {
            Update(&zero, 1);
        }
        std::array<std::uint8_t, 8> length_bytes{};
        for (std::size_t i = 0; i < length_bytes.size(); ++i)
        {
            length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (56u - 8u * i));
        }
        Update(length_bytes.data(), length_bytes.size());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i)
        {
            digest[4 * i + 0] = static_cast<std::uint8_t>(state_[i] >> 24);
            digest[4 * i + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            digest[4 * i + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
            digest[4 * i + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return digest;
    }

  private:
    static constexpr std::size_t kBlockSize = 64;

    void ProcessBlock(const std::uint8_t* block)
    {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<std::uint32_t>(block[4 * i + 0]) << 24) |
                   (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) |
                   (static_cast<std::uint32_t>(block[4 * i + 3]));
        }
        for (std::size_t i = 16; i < w.size(); ++i)
        {
            const std::uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (std::size_t i = 0; i < w.size(); ++i)
        {
            const std::uint32_t big_sigma1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const std::uint32_t choose     = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1      = h + big_sigma1 + choose + kSha256RoundConstants[i] + w[i];
            const std::uint32_t big_sigma0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const std::uint32_t majority   = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2      = big_sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = kSha256InitialState;
    std::uint64_t                 total_bytes_ = 0;
    std::array<std::uint8_t, 64>  buffer_{};
    std::size_t                   buffer_used_ = 0;
};

std::string ToHexLower(const std::array<std::uint8_t, 32>& digest)
{
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string            out;
    out.reserve(64);
    for (const std::uint8_t byte : digest)
    {
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0f]);
    }
    return out;
}

}  // namespace

std::string Sha256Hex(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr && size > 0)
    {
        return "";
    }
    Sha256Context context;
    context.Update(data, size);
    return ToHexLower(context.Final());
}

Status Sha256File(const std::string& path, std::string* hex)
{
    if (hex == nullptr)
    {
        return Status(StatusCode::kInvalidArgument, "Sha256File: hex 输出参数为空");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return Status(StatusCode::kNotFound, "cannot open file: " + path);
    }

    constexpr std::size_t            kChunkSize = 64 * 1024;
    std::array<std::uint8_t, kChunkSize> chunk{};
    Sha256Context                    context;
    while (file)
    {
        file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = file.gcount();
        if (got > 0)
        {
            context.Update(chunk.data(), static_cast<std::size_t>(got));
        }
    }
    if (file.bad())
    {
        return Status(StatusCode::kUnavailable, "read error on file: " + path);
    }
    *hex = ToHexLower(context.Final());
    return Status::MakeOk();
}

}  // namespace animal_monitor
