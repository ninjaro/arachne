#include "arachne/paths.hpp"

#include <sys/stat.h>

#include <system_error>

namespace arachne::paths {

namespace fs = std::filesystem;

namespace {

    // Matches the usual SYMLOOP_MAX; deeper chains are left unresolved.
    constexpr int max_link_depth = 40;

    fs::path resolve(const fs::path& absolute, const int depth) {
        std::error_code error;
        fs::path current;
        bool missing = false;
        for (const auto& component : absolute.lexically_normal()) {
            current /= component;
            if (missing || !component.has_filename()) {
                continue;
            }
            const auto status = fs::symlink_status(current, error);
            if (error || !fs::exists(status)) {
                missing = true;
                continue;
            }
            if (depth < max_link_depth && is_system_symlink(current)) {
                const fs::path target = fs::read_symlink(current, error);
                if (error) {
                    continue;
                }
                // Only the root-owned link itself is replaced; a user-owned
                // link inside its target stays visible to callers' checks.
                current = resolve(
                    target.is_absolute() ? target : current.parent_path() / target,
                    depth + 1
                );
            }
        }
        return current;
    }

} // namespace

bool is_system_symlink(const fs::path& path) {
    struct stat status {};
    if (::lstat(path.c_str(), &status) != 0) {
        return false;
    }
    return S_ISLNK(status.st_mode) && status.st_uid == 0;
}

fs::path resolve_system_symlinks(const fs::path& path) {
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error);
    if (error) {
        return path.lexically_normal();
    }
    return resolve(absolute, 0);
}

} // namespace arachne::paths
