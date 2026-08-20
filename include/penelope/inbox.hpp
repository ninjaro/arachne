/*
 * Copyright (c) 2026 Yaroslav Riabtsev
 * SPDX-License-Identifier: MIT
 */

#ifndef ARACHNE_PENELOPE_INBOX_HPP
#define ARACHNE_PENELOPE_INBOX_HPP

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace arachne::penelope {

struct inbox_issue final {
    std::string batch_id;
    std::string code;
    std::string json_path;
    std::string message;
    std::string value_json;
};

enum class inbox_batch_status {
    valid,
    applied,
    already_applied,
    rejected,
};

struct inbox_batch_report final {
    std::filesystem::path path;
    std::string batch_id;
    inbox_batch_status status { inbox_batch_status::valid };
    std::vector<inbox_issue> issues;
};

struct inbox_result final {
    bool ok { false };
    bool applied { false };
    std::size_t valid_count { 0 };
    std::size_t applied_count { 0 };
    std::size_t already_applied_count { 0 };
    std::size_t rejected_count { 0 };
    std::vector<inbox_batch_report> batches;
};

class inbox_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Validate every plain-JSON batch under `<repository_root>/inbox`.
 *
 * The database is opened read-only. No issue rows, hints, files, or product
 * records are modified.
 */
[[nodiscard]] inbox_result
check_product_inbox(const std::filesystem::path& repository_root);

/**
 * Validate every pending batch first, then apply valid batches directly to the
 * current product database, one BEGIN IMMEDIATE transaction per batch.
 */
[[nodiscard]] inbox_result
apply_product_inbox(const std::filesystem::path& repository_root);

[[nodiscard]] const char* to_string(inbox_batch_status status) noexcept;

} // namespace arachne::penelope

#endif // ARACHNE_PENELOPE_INBOX_HPP
