#ifndef ARACHNE_CRYPTO_HPP
#define ARACHNE_CRYPTO_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace arachne::crypto {

using sha256_digest = std::array<std::byte, 32>;

/** Incremental SHA-256 calculator. */
class sha256_hasher final {
public:
    sha256_hasher() noexcept;

    void reset() noexcept;
    void update(std::span<const std::byte> bytes);
    void update(std::string_view bytes);

    /** Finalize the digest. Repeated calls return the same value. */
    [[nodiscard]] sha256_digest finish();
    [[nodiscard]] std::string finish_hex();

private:
    void transform(const std::byte* block) noexcept;

    std::array<std::uint32_t, 8> state_ {};
    std::array<std::byte, 64> buffer_ {};
    std::uint64_t bit_count_ = 0;
    std::size_t buffered_ = 0;
    bool finished_ = false;
    sha256_digest digest_ {};
};

/** Return the lowercase hexadecimal SHA-256 digest of arbitrary bytes. */
[[nodiscard]] std::string sha256(std::span<const std::byte> bytes);

/** Return the lowercase hexadecimal SHA-256 digest of a byte string. */
[[nodiscard]] std::string sha256(std::string_view bytes);

/** Stream a file in binary mode and return its lowercase SHA-256 digest. */
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

/** Compatibility spelling used by early Penelope code. */
[[nodiscard]] inline std::string sha256_string(std::string_view bytes) {
    return sha256(bytes);
}

/**
 * Return true only for a portable, non-empty relative artifact reference.
 * Backslashes, absolute paths, empty components, and dot components are
 * rejected so a reference has the same meaning on POSIX and Windows.
 */
[[nodiscard]] bool
is_safe_relative_artifact_ref(std::string_view artifact_ref) noexcept;

/**
 * Join a validated artifact reference to a root. This is a lexical safety
 * primitive; callers that write files must additionally reject symlinked
 * directory components.
 */
[[nodiscard]] std::filesystem::path safe_artifact_path(
    const std::filesystem::path& root, std::string_view artifact_ref
);

}

#endif // ARACHNE_CRYPTO_HPP
