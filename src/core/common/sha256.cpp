// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT
//
// SHA-256 as specified in FIPS 180-4, written for this project.

#include "ds/common/sha256.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <vector>

namespace ds::common {
namespace {

constexpr std::array<std::uint32_t, 64> kRound{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::uint32_t loadBigEndian(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24U) | (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) | static_cast<std::uint32_t>(p[3]);
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const std::uint8_t* data) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = loadBigEndian(data + (4 * i));
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const std::uint32_t s1 =
            std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
        const std::uint32_t s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
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

void Sha256::update(std::span<const std::byte> data) noexcept {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    std::size_t n = data.size();
    length_ += n;
    if (buffered_ > 0) {
        const std::size_t take = std::min(n, buffer_.size() - buffered_);
        std::memcpy(buffer_.data() + buffered_, p, take);
        buffered_ += take;
        p += take;
        n -= take;
        if (buffered_ == buffer_.size()) {
            block(buffer_.data());
            buffered_ = 0;
        }
    }
    while (n >= 64) {
        block(p);
        p += 64;
        n -= 64;
    }
    if (n > 0) {
        std::memcpy(buffer_.data(), p, n);
        buffered_ = n;
    }
}

void Sha256::update(std::string_view text) noexcept { update(std::as_bytes(std::span(text))); }

Sha256::Digest Sha256::finish() noexcept {
    const std::uint64_t bits = length_ * 8U;
    buffer_[buffered_++] = 0x80;
    if (buffered_ > 56) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0);
        block(buffer_.data());
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + 56, 0);
    for (std::size_t i = 0; i < 8; ++i) {
        buffer_[56 + i] = static_cast<std::uint8_t>(bits >> (56U - (8U * i)));
    }
    block(buffer_.data());
    Digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            digest[(4 * i) + j] = static_cast<std::uint8_t>(state_[i] >> (24U - (8U * j)));
        }
    }
    return digest;
}

std::string Sha256::toHex(const Digest& digest) {
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : digest) {
        out += std::format("{:02x}", byte);
    }
    return out;
}

std::string sha256Hex(std::string_view text) {
    Sha256 hash;
    hash.update(text);
    return Sha256::toHex(hash.finish());
}

Result<std::string> sha256File(const std::filesystem::path& file, std::stop_token stop) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return makeError(std::format("cannot open \"{}\"", file.string()));
    }
    Sha256 hash;
    std::vector<char> piece(std::size_t{1} << 20U);
    while (in) {
        if (stop.stop_requested()) {
            return makeError("cancelled");
        }
        in.read(piece.data(), static_cast<std::streamsize>(piece.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        hash.update(std::as_bytes(std::span(piece.data(), got)));
    }
    if (in.bad()) {
        return makeError(std::format("error reading \"{}\"", file.string()));
    }
    return Sha256::toHex(hash.finish());
}

bool isSha256Hex(std::string_view text) noexcept {
    return text.size() == 64 && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}

}  // namespace ds::common
