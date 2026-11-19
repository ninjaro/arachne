#ifndef ARACHNE_PATHS_HPP
#define ARACHNE_PATHS_HPP

#include <filesystem>

namespace arachne::paths {

/**
 * True when `path` is a symbolic link owned by root.
 *
 * Only an administrator can create or replace such a link, so it is part of
 * the host's directory layout rather than something an unprivileged process
 * can redirect. Examples: macOS `/var -> /private/var`, `/tmp`, and cluster
 * mount aliases such as `/home` or `/hpcwork` pointing into a parallel file
 * system. A missing path or a regular file or directory returns false.
 */
[[nodiscard]] bool is_system_symlink(const std::filesystem::path& path);

/**
 * Absolute, lexically normal `path` whose root-owned symbolic link components
 * are resolved.
 *
 * Symbolic links owned by any other user are left in place, so callers that
 * reject symlink traversal (including SQLite's `SQLITE_OPEN_NOFOLLOW`) still
 * reject them. Components that do not exist yet are appended unchanged.
 */
[[nodiscard]] std::filesystem::path
resolve_system_symlinks(const std::filesystem::path& path);

} // namespace arachne::paths

#endif // ARACHNE_PATHS_HPP
