#include "arachne/crypto.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace arachne::crypto {
namespace {

    constexpr std::array<std::uint32_t, 64> round_constants {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
    };

    constexpr char hexadecimal[] = "0123456789abcdef";

    [[nodiscard]] std::uint32_t load_be32(const std::byte* bytes) noexcept {
        return static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[0]))
            << 24U
            | static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[1]))
            << 16U
            | static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[2]))
            << 8U
            | static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[3]));
    }

    void store_be32(std::byte* output, const std::uint32_t value) noexcept {
        output[0] = static_cast<std::byte>(value >> 24U);
        output[1] = static_cast<std::byte>(value >> 16U);
        output[2] = static_cast<std::byte>(value >> 8U);
        output[3] = static_cast<std::byte>(value);
    }

    [[nodiscard]] std::string to_hex(const sha256_digest& digest) {
        std::string result;
        result.resize(digest.size() * 2U);
        std::size_t index = 0;
        for (const std::byte value : digest) {
            const auto byte = std::to_integer<unsigned>(value);
            result[index++] = hexadecimal[byte >> 4U];
            result[index++] = hexadecimal[byte & 0x0fU];
        }
        return result;
    }

}

sha256_hasher::sha256_hasher() noexcept { reset(); }

void sha256_hasher::reset() noexcept {
    state_ = { 0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
               0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U };
    buffer_.fill(std::byte { 0 });
    digest_.fill(std::byte { 0 });
    bit_count_ = 0;
    buffered_ = 0;
    finished_ = false;
}

void sha256_hasher::update(const std::span<const std::byte> bytes) {
    if (finished_) {
        throw std::logic_error("cannot update a finalized SHA-256 digest");
    }
    constexpr std::uint64_t max_bytes
        = std::numeric_limits<std::uint64_t>::max() / 8U;
    if (bytes.size() > max_bytes
        || static_cast<std::uint64_t>(bytes.size())
            > max_bytes - bit_count_ / 8U) {
        throw std::length_error("SHA-256 input exceeds its length encoding");
    }
    bit_count_ += static_cast<std::uint64_t>(bytes.size()) * 8U;

    std::size_t offset = 0;
    if (buffered_ != 0) {
        const std::size_t copied
            = std::min(buffer_.size() - buffered_, bytes.size());
        std::copy_n(bytes.data(), copied, buffer_.data() + buffered_);
        buffered_ += copied;
        offset += copied;
        if (buffered_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_ = 0;
        }
    }

    while (bytes.size() - offset >= buffer_.size()) {
        transform(bytes.data() + offset);
        offset += buffer_.size();
    }

    if (offset != bytes.size()) {
        buffered_ = bytes.size() - offset;
        std::copy_n(bytes.data() + offset, buffered_, buffer_.data());
    }
}

void sha256_hasher::update(const std::string_view bytes) {
    update(std::as_bytes(std::span { bytes.data(), bytes.size() }));
}

sha256_digest sha256_hasher::finish() {
    if (finished_) {
        return digest_;
    }

    const std::uint64_t original_bit_count = bit_count_;
    buffer_[buffered_++] = std::byte { 0x80 };
    if (buffered_ > 56U) {
        std::fill(
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
            buffer_.end(), std::byte { 0 }
        );
        transform(buffer_.data());
        buffered_ = 0;
    }
    std::fill(
        buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
        buffer_.begin() + 56, std::byte { 0 }
    );
    for (std::size_t index = 0; index < 8U; ++index) {
        const unsigned shift = static_cast<unsigned>((7U - index) * 8U);
        buffer_[56U + index]
            = static_cast<std::byte>(original_bit_count >> shift);
    }
    transform(buffer_.data());

    for (std::size_t index = 0; index < state_.size(); ++index) {
        store_be32(digest_.data() + index * 4U, state_[index]);
    }
    finished_ = true;
    buffered_ = 0;
    return digest_;
}

std::string sha256_hasher::finish_hex() { return to_hex(finish()); }

void sha256_hasher::transform(const std::byte* const block) noexcept {
    std::array<std::uint32_t, 64> schedule {};
    for (std::size_t index = 0; index < 16U; ++index) {
        schedule[index] = load_be32(block + index * 4U);
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
        const std::uint32_t s0 = std::rotr(schedule[index - 15U], 7)
            ^ std::rotr(schedule[index - 15U], 18)
            ^ (schedule[index - 15U] >> 3U);
        const std::uint32_t s1 = std::rotr(schedule[index - 2U], 17)
            ^ std::rotr(schedule[index - 2U], 19)
            ^ (schedule[index - 2U] >> 10U);
        schedule[index]
            = schedule[index - 16U] + s0 + schedule[index - 7U] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0; index < schedule.size(); ++index) {
        const std::uint32_t sum1
            = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temporary1
            = h + sum1 + choose + round_constants[index] + schedule[index];
        const std::uint32_t sum0
            = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
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

std::string sha256(const std::span<const std::byte> bytes) {
    sha256_hasher hasher;
    hasher.update(bytes);
    return hasher.finish_hex();
}

std::string sha256(const std::string_view bytes) {
    sha256_hasher hasher;
    hasher.update(bytes);
    return hasher.finish_hex();
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::system_error(
            errno == 0 ? EIO : errno, std::generic_category(),
            "cannot open file for SHA-256"
        );
    }

    sha256_hasher hasher;
    std::array<char, 64U * 1024U> buffer {};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            hasher.update(
                std::as_bytes(
                    std::span { buffer.data(), static_cast<std::size_t>(count) }
                )
            );
        }
    }
    if (!input.eof()) {
        throw std::runtime_error("I/O error while calculating file SHA-256");
    }
    return hasher.finish_hex();
}

bool is_safe_relative_artifact_ref(
    const std::string_view artifact_ref
) noexcept {
    if (artifact_ref.empty() || artifact_ref.size() > 4096U
        || artifact_ref.front() == '/' || artifact_ref.front() == '\\'
        || artifact_ref.back() == '/' || artifact_ref.back() == '\\'
        || artifact_ref.find('\0') != std::string_view::npos
        || artifact_ref.find('\\') != std::string_view::npos) {
        return false;
    }

    std::size_t start = 0;
    while (start < artifact_ref.size()) {
        const std::size_t slash = artifact_ref.find('/', start);
        const std::size_t end
            = slash == std::string_view::npos ? artifact_ref.size() : slash;
        const std::string_view component
            = artifact_ref.substr(start, end - start);
        if (component.empty() || component == "." || component == ".."
            || component.find(':') != std::string_view::npos
            || component.back() == ' ' || component.back() == '.') {
            return false;
        }
        if (std::ranges::any_of(component, [](const char character) {
                const auto value = static_cast<unsigned char>(character);
                return value < 0x20U || value == 0x7fU;
            })) {
            return false;
        }
        std::string device_name(component.substr(0, component.find('.')));
        std::ranges::transform(
            device_name, device_name.begin(), [](const char character) {
                return static_cast<char>(
                    std::toupper(static_cast<unsigned char>(character))
                );
            }
        );
        const bool numbered_device = device_name.size() == 4U
            && (device_name.starts_with("COM")
                || device_name.starts_with("LPT"))
            && device_name.back() >= '1' && device_name.back() <= '9';
        if (device_name == "CON" || device_name == "PRN" || device_name == "AUX"
            || device_name == "NUL" || numbered_device) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1U;
    }
    return true;
}

std::filesystem::path safe_artifact_path(
    const std::filesystem::path& root, const std::string_view artifact_ref
) {
    if (!is_safe_relative_artifact_ref(artifact_ref)) {
        throw std::invalid_argument("unsafe relative artifact reference");
    }
    return root / std::filesystem::path { artifact_ref };
}

}
