#ifndef ARACHNE_COORDINATOR_HPP
#define ARACHNE_COORDINATOR_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arachne::coordination {

enum class cocoon_status {
    received,
    needs_format_fix,
    waiting_approval,
    accepted,
    waiting_processing,
    processing,
    integrated,
    failed,
    rejected,
    superseded,
};

[[nodiscard]] std::string_view to_string(cocoon_status status) noexcept;
[[nodiscard]] cocoon_status cocoon_status_from_string(std::string_view value);
[[nodiscard]] bool
can_transition(cocoon_status from, cocoon_status to) noexcept;

struct intake_request {
    std::filesystem::path source_path;
    std::filesystem::path inbox_root;
    std::string submission_ref;
    std::string title;
    std::optional<std::string> supersedes;
    std::uintmax_t max_payload_bytes = 64U * 1024U * 1024U;
};

struct envelope_record {
    std::string envelope_id;
    std::filesystem::path payload_ref;
    std::string payload_sha256;
    int format_version = 0;
    std::string submission_ref;
    std::string title;
    cocoon_status status = cocoon_status::received;
    std::optional<std::string> accepted_by;
    std::optional<std::string> supersedes;
    std::uintmax_t byte_length = 0;
};

struct state_event {
    std::int64_t sequence = 0;
    std::string envelope_id;
    cocoon_status from = cocoon_status::received;
    cocoon_status to = cocoon_status::received;
    std::string actor_ref;
    std::string reason;
    std::string occurred_at;
};

struct accumulation_policy {
    std::size_t accepted_count = 15;
    std::uintmax_t accepted_bytes = 0;
    std::chrono::seconds oldest_age { 0 };
};

struct accumulation_state {
    std::size_t accepted_count = 0;
    std::uintmax_t accepted_bytes = 0;
    std::chrono::seconds oldest_age { 0 };
};

struct verification_issue {
    std::filesystem::path path;
    std::string message;
};

class operational_ledger {
public:
    explicit operational_ledger(
        std::filesystem::path database_path,
        std::optional<std::filesystem::path> legacy_inbox_root = std::nullopt
    );
    ~operational_ledger();

    operational_ledger(const operational_ledger&) = delete;
    operational_ledger& operator=(const operational_ledger&) = delete;
    operational_ledger(operational_ledger&&) = delete;
    operational_ledger& operator=(operational_ledger&&) = delete;

    [[nodiscard]] envelope_record intake(const intake_request& request);
    [[nodiscard]] envelope_record transition(
        std::string_view envelope_id, cocoon_status next,
        std::string_view actor_ref, std::string_view reason = {}
    );
    [[nodiscard]] envelope_record get(std::string_view envelope_id) const;
    [[nodiscard]] std::vector<envelope_record> list(cocoon_status status) const;
    [[nodiscard]] std::vector<state_event>
    history(std::string_view envelope_id) const;

    void capture_inbox_baseline(const std::filesystem::path& inbox_root);
    [[nodiscard]] std::vector<verification_issue>
    verify_inbox(const std::filesystem::path& inbox_root) const;

    [[nodiscard]] bool retire_queued_payload(
        std::string_view envelope_id,
        const std::filesystem::path& internal_queue_root,
        std::optional<std::filesystem::path> legacy_inbox_root = std::nullopt
    );

    [[nodiscard]] accumulation_state accumulation() const;
    [[nodiscard]] bool
    should_integrate(const accumulation_policy& policy) const;

    [[nodiscard]] bool claim_logical_run(
        std::string_view run_id, std::string_view graph_domain,
        std::string_view logical_date, std::string_view configuration_sha256,
        bool retry_failed = false, bool resume_running = false
    );
    void bind_product_run_inputs(
        std::string_view run_id,
        const std::vector<std::string>& envelope_ids
    );
    [[nodiscard]] std::vector<envelope_record>
    product_run_inputs(std::string_view run_id) const;
    void finish_integrated_product_run(
        std::string_view run_id, std::string_view manifest_ref
    );
    void finish_run(
        std::string_view run_id, std::string_view status,
        std::string_view manifest_ref = {}
    );

    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    struct implementation;
    implementation* impl_;
    std::filesystem::path path_;
    std::optional<std::filesystem::path> legacy_inbox_root_;
};

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& path,
    const std::filesystem::path& possible_parent
);

void reject_inbox_deletion_target(
    const std::filesystem::path& target, const std::filesystem::path& inbox_root
);

class domain_lock {
public:
    domain_lock(
        const std::filesystem::path& lock_root, std::string_view graph_domain,
        std::string_view run_id,
        std::chrono::seconds stale_after = std::chrono::hours(6)
    );
    ~domain_lock();

    domain_lock(const domain_lock&) = delete;
    domain_lock& operator=(const domain_lock&) = delete;
    domain_lock(domain_lock&& other) noexcept;
    domain_lock& operator=(domain_lock&& other) noexcept;

    [[nodiscard]] bool owns_lock() const noexcept;
    void release();

private:
    std::filesystem::path directory_;
    std::string ownership_token_;
    bool owns_ = false;
};

} // namespace arachne::coordination

#endif
