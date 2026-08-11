#ifndef ARACHNE_PENELOPE_MERGE_HINT_STORE_HPP
#define ARACHNE_PENELOPE_MERGE_HINT_STORE_HPP

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace arachne::penelope {

class merge_hint_store_error final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * Recreate the disposable hint database, attach the canonical product database
 * read-only, and return the versioned in-memory input consumed by Ariadne.
 */
[[nodiscard]] nlohmann::json prepare_merge_hint_rebuild(
    const std::filesystem::path& repository_root,
    std::string_view generator_version
);

/** Persist Ariadne's derived projection in the disposable hint database. */
void store_merge_hint_projection(
    const std::filesystem::path& repository_root,
    const nlohmann::json& projection
);

/**
 * Validate freshness and return only the selected identity rows, enriched with
 * current canonical labels, plus their bounded review metadata. Structural
 * observations remain queryable in the disposable SQLite store.
 */
[[nodiscard]] nlohmann::json load_merge_hint_export(
    const std::filesystem::path& repository_root,
    std::string_view expected_generator_version
);

/** Explicitly remove the disposable database and its SQLite sidecars. */
void discard_merge_hint_store(
    const std::filesystem::path& repository_root
);

[[nodiscard]] std::filesystem::path merge_hint_store_path(
    const std::filesystem::path& repository_root
);

[[nodiscard]] std::filesystem::path merge_hint_review_path(
    const std::filesystem::path& repository_root
);

[[nodiscard]] std::filesystem::path merge_hint_decisions_path(
    const std::filesystem::path& repository_root
);

} // namespace arachne::penelope

#endif // ARACHNE_PENELOPE_MERGE_HINT_STORE_HPP
