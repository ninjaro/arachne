#include "arachne/coordinator.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {

class temporary_directory {
public:
    temporary_directory() {
        static std::atomic<unsigned> sequence { 0 };
        std::random_device entropy;
        const std::string nonce
            = std::to_string(entropy()) + "-"
            + std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()
            )
            + "-" + std::to_string(++sequence);
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("arachne-coordinator-tests-" + nonce + "-"
                   + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = candidate;
                return;
            }
            if (error) {
                throw std::runtime_error(
                    "cannot create coordinator test directory: "
                    + error.message()
                );
            }
        }
        throw std::runtime_error("cannot allocate coordinator test directory");
    }

    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output);
    output << bytes;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(input),
             std::istreambuf_iterator<char>() };
}

std::int64_t
sqlite_integer(const std::filesystem::path& path, const char* sql) {
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("cannot open coordinator test ledger");
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("cannot query coordinator test ledger");
    }
    const int result = sqlite3_step(statement);
    const std::int64_t value
        = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

arachne::coordination::envelope_record approve_for_processing(
    arachne::coordination::operational_ledger& ledger,
    const arachne::coordination::envelope_record& envelope
) {
    const auto accepted = ledger.transition(
        envelope.envelope_id, arachne::coordination::cocoon_status::accepted,
        "maintainer:test", "explicit test approval"
    );
    return ledger.transition(
        accepted.envelope_id,
        arachne::coordination::cocoon_status::waiting_processing,
        "workflow:test", "approved cocoon entered processing queue"
    );
}

} // namespace

TEST(Coordinator, IntakePreservesBytesAndRecordsAuditableLifecycle) {
    temporary_directory temporary;
    const auto source = temporary.path() / "upload.json";
    const std::string bytes = R"JSON({
  "format_version": 1,
  "batch_id": "example-001",
  "batch_type": "mining",
  "scope": {"label": "Example"},
  "works": [{"local_id": "work-1"}]
})JSON";
    write(source, bytes);
    const auto ledger_path = temporary.path() / "state" / "ledger.sqlite";
    const auto inbox = temporary.path() / "inbox";

    arachne::coordination::operational_ledger ledger(ledger_path);
    const auto envelope = ledger.intake(
        {
            .source_path = source,
            .inbox_root = inbox,
            .submission_ref = "github-issue:12",
            .title = "Example batch",
            .supersedes = std::nullopt,
        }
    );
    EXPECT_EQ(
        envelope.status, arachne::coordination::cocoon_status::waiting_approval
    );
    EXPECT_TRUE(
        arachne::coordination::path_is_within(envelope.payload_ref, inbox)
    );
    EXPECT_EQ(read(envelope.payload_ref), bytes);
    EXPECT_EQ(read(source), bytes);
    EXPECT_EQ(envelope.payload_sha256.size(), 64U);
    ASSERT_EQ(ledger.history(envelope.envelope_id).size(), 1U);

    const auto accepted = ledger.transition(
        envelope.envelope_id, arachne::coordination::cocoon_status::accepted,
        "maintainer:reviewer", "reviewed and approved"
    );
    EXPECT_EQ(accepted.status, arachne::coordination::cocoon_status::accepted);
    EXPECT_EQ(accepted.accepted_by, "maintainer:reviewer");
    const auto waiting = ledger.transition(
        envelope.envelope_id,
        arachne::coordination::cocoon_status::waiting_processing,
        "workflow:run-1"
    );
    EXPECT_EQ(
        waiting.status, arachne::coordination::cocoon_status::waiting_processing
    );

    const auto processing = ledger.transition(
        envelope.envelope_id, arachne::coordination::cocoon_status::processing,
        "workflow:run-1"
    );
    EXPECT_EQ(
        processing.status, arachne::coordination::cocoon_status::processing
    );
    EXPECT_EQ(ledger.history(envelope.envelope_id).size(), 4U);
    EXPECT_TRUE(ledger.history(envelope.envelope_id).back().reason.empty());
}

TEST(Coordinator, OpaquePayloadIsQueuedWithoutSpeculativeValidation) {
    temporary_directory temporary;
    const auto source = temporary.path() / "broken.json";
    write(source, "{not-json");
    arachne::coordination::operational_ledger ledger(
        temporary.path() / "ledger.sqlite"
    );
    const auto envelope = ledger.intake(
        {
            .source_path = source,
            .inbox_root = temporary.path() / "inbox",
            .submission_ref = "github-issue:13",
            .title = "Broken",
            .supersedes = std::nullopt,
        }
    );
    EXPECT_EQ(
        envelope.status, arachne::coordination::cocoon_status::waiting_approval
    );
    EXPECT_TRUE(std::filesystem::is_regular_file(envelope.payload_ref));
    EXPECT_EQ(read(envelope.payload_ref), "{not-json");
}

TEST(Coordinator, InvalidTransitionRollsBackAtomically) {
    temporary_directory temporary;
    const auto source = temporary.path() / "upload.json";
    write(
        source, R"({"format_version":1,"batch_id":"b","batch_type":"mining"})"
    );
    arachne::coordination::operational_ledger ledger(
        temporary.path() / "ledger.sqlite"
    );
    const auto envelope = ledger.intake(
        {
            .source_path = source,
            .inbox_root = temporary.path() / "inbox",
            .submission_ref = "local:1",
            .title = "Batch",
            .supersedes = std::nullopt,
        }
    );
    EXPECT_THROW(
        static_cast<void>(ledger.transition(
            envelope.envelope_id,
            arachne::coordination::cocoon_status::integrated, "maintainer:test"
        )),
        std::logic_error
    );
    EXPECT_EQ(ledger.get(envelope.envelope_id).status, envelope.status);
    EXPECT_EQ(ledger.history(envelope.envelope_id).size(), 1U);
}

TEST(Coordinator, InboxBaselineDetectsChangesAndWorkflowsDoNotChangeIt) {
    temporary_directory temporary;
    const auto legacy_inbox = temporary.path() / "legacy_inbox";
    const auto existing = legacy_inbox / "existing.json";
    write(existing, "unchanged");
    arachne::coordination::operational_ledger ledger(
        temporary.path() / "ledger.sqlite", legacy_inbox
    );
    ledger.capture_inbox_baseline(legacy_inbox);
    EXPECT_TRUE(ledger.verify_inbox(legacy_inbox).empty());

    const auto source = temporary.path() / "new.json";
    write(
        source, R"({"format_version":1,"batch_id":"b","batch_type":"mining"})"
    );
    static_cast<void>(ledger.intake(
        {
            .source_path = source,
            .inbox_root = temporary.path() / "queue",
            .submission_ref = "local:2",
            .title = "Second batch",
            .supersedes = std::nullopt,
        }
    ));
    EXPECT_TRUE(ledger.verify_inbox(legacy_inbox).empty());

    write(existing, "changed");
    const auto issues = ledger.verify_inbox(legacy_inbox);
    ASSERT_EQ(issues.size(), 1U);
    EXPECT_EQ(issues.front().path, existing);
}

TEST(Coordinator, AccumulationAndLogicalDateGuardsAreDeterministic) {
    temporary_directory temporary;
    const auto source = temporary.path() / "batch.json";
    write(
        source, R"({"format_version":1,"batch_id":"b","batch_type":"mining"})"
    );
    arachne::coordination::operational_ledger ledger(
        temporary.path() / "ledger.sqlite"
    );
    const auto envelope = ledger.intake(
        {
            .source_path = source,
            .inbox_root = temporary.path() / "inbox",
            .submission_ref = "local:3",
            .title = "Batch",
            .supersedes = std::nullopt,
        }
    );
    EXPECT_EQ(
        envelope.status, arachne::coordination::cocoon_status::waiting_approval
    );
    EXPECT_FALSE(ledger.should_integrate({}));
    const auto accepted = ledger.transition(
        envelope.envelope_id, arachne::coordination::cocoon_status::accepted,
        "maintainer:test", "explicit test approval"
    );
    EXPECT_EQ(accepted.status, arachne::coordination::cocoon_status::accepted);
    EXPECT_TRUE(ledger.should_integrate({ .accepted_count = 1 }));
    EXPECT_FALSE(ledger.should_integrate({ .accepted_count = 2 }));

    const std::string hash(64, 'a');
    EXPECT_TRUE(
        ledger.claim_logical_run("run-1", "product_graph", "2026-07-18", hash)
    );
    EXPECT_FALSE(
        ledger.claim_logical_run("run-2", "product_graph", "2026-07-18", hash)
    );
    ledger.finish_run("run-1", "succeeded", "runs/run-1.json");
    EXPECT_FALSE(ledger.claim_logical_run(
        "run-2", "product_graph", "2026-07-18", hash, true
    ));
    EXPECT_THROW(
        static_cast<void>(ledger.claim_logical_run(
            "bad-date", "product_graph", "2026-02-30", hash
        )),
        std::invalid_argument
    );
}

TEST(Coordinator, FailedLogicalRunsCanBeRetriedWithAuditableAttempts) {
    temporary_directory temporary;
    const auto ledger_path = temporary.path() / "ledger.sqlite";
    arachne::coordination::operational_ledger ledger(ledger_path);
    const std::string hash(64, 'b');
    ASSERT_TRUE(ledger.claim_logical_run(
        "run-failed", "product_graph", "2026-07-19", hash
    ));
    ledger.finish_run("run-failed", "failed", "reports/failed.json");
    EXPECT_FALSE(ledger.claim_logical_run(
        "run-retry", "product_graph", "2026-07-19", hash
    ));
    ASSERT_TRUE(ledger.claim_logical_run(
        "run-retry", "product_graph", "2026-07-19", hash, true
    ));
    ledger.finish_run("run-retry", "failed", "reports/retry-failed.json");
    ASSERT_TRUE(ledger.claim_logical_run(
        "run-retry", "product_graph", "2026-07-19", hash, true
    ));
    ledger.finish_run("run-retry", "succeeded", "reports/succeeded.json");
    EXPECT_EQ(
        sqlite_integer(
            ledger_path,
            "SELECT count(*) FROM run_attempts WHERE status='failed'"
        ),
        2
    );
    EXPECT_EQ(
        sqlite_integer(
            ledger_path,
            "SELECT count(*) FROM run_attempts WHERE status='succeeded'"
        ),
        1
    );
    EXPECT_THROW(
        static_cast<void>(ledger.claim_logical_run(
            "run-retry", "product_graph", "2026-07-19", std::string(64, 'c'),
            true
        )),
        std::logic_error
    );
}

TEST(Coordinator, ProductActivationReconciliationIsAtomicAndIdempotent) {
    temporary_directory temporary;
    const auto ledger_path = temporary.path() / "ledger.sqlite";
    const auto queue = temporary.path() / "queue";
    arachne::coordination::operational_ledger ledger(ledger_path);
    std::vector<std::string> envelope_ids;
    for (int index = 0; index < 2; ++index) {
        const auto source = temporary.path()
            / ("reconcile-" + std::to_string(index) + ".bin");
        write(source, "reconciliation payload " + std::to_string(index));
        const auto envelope = approve_for_processing(
            ledger,
            ledger.intake(
                {
                    .source_path = source,
                    .inbox_root = queue,
                    .submission_ref = "reconcile:" + std::to_string(index),
                    .title = "Reconciliation payload",
                    .supersedes = std::nullopt,
                }
            )
        );
        envelope_ids.push_back(envelope.envelope_id);
    }
    const std::string hash(64, 'd');
    ASSERT_TRUE(ledger.claim_logical_run(
        "run-reconcile", "product_graph", "2026-07-20", hash
    ));
    ledger.bind_product_run_inputs("run-reconcile", envelope_ids);
    std::ranges::reverse(envelope_ids);
    EXPECT_NO_THROW(
        ledger.bind_product_run_inputs("run-reconcile", envelope_ids)
    );
    for (const std::string& envelope_id : envelope_ids) {
        static_cast<void>(ledger.transition(
            envelope_id, arachne::coordination::cocoon_status::processing,
            "workflow:reconcile"
        ));
    }
    static_cast<void>(ledger.transition(
        envelope_ids.front(), arachne::coordination::cocoon_status::integrated,
        "workflow:legacy-partial-finish"
    ));

    ledger.finish_integrated_product_run(
        "run-reconcile", "runs/run-reconcile.json"
    );
    EXPECT_NO_THROW(ledger.finish_integrated_product_run(
        "run-reconcile", "runs/run-reconcile.json"
    ));
    for (const auto& envelope : ledger.product_run_inputs("run-reconcile")) {
        EXPECT_EQ(
            envelope.status, arachne::coordination::cocoon_status::integrated
        );
    }
    EXPECT_EQ(
        sqlite_integer(
            ledger_path,
            "SELECT count(*) FROM runs WHERE run_id='run-reconcile' AND "
            "status='succeeded' AND manifest_ref='runs/run-reconcile.json'"
        ),
        1
    );
    EXPECT_EQ(
        sqlite_integer(
            ledger_path,
            "SELECT count(*) FROM run_attempts WHERE run_id='run-reconcile' "
            "AND status='succeeded'"
        ),
        1
    );
}

TEST(Coordinator, InterruptedProductRunCanResumeAndCleanupConverges) {
    temporary_directory temporary;
    const auto ledger_path = temporary.path() / "ledger.sqlite";
    const auto queue = temporary.path() / "queue";
    arachne::coordination::operational_ledger ledger(ledger_path);
    std::vector<std::string> envelope_ids;
    for (int index = 0; index < 2; ++index) {
        const auto source
            = temporary.path() / ("resume-" + std::to_string(index) + ".bin");
        write(source, "resume payload " + std::to_string(index));
        const auto envelope = approve_for_processing(
            ledger,
            ledger.intake(
                {
                    .source_path = source,
                    .inbox_root = queue,
                    .submission_ref = "resume:" + std::to_string(index),
                    .title = "Resume payload",
                    .supersedes = std::nullopt,
                }
            )
        );
        envelope_ids.push_back(envelope.envelope_id);
    }
    const std::string hash(64, 'e');
    ASSERT_TRUE(ledger.claim_logical_run(
        "run-resume", "product_graph", "2026-07-21", hash
    ));
    ledger.bind_product_run_inputs("run-resume", envelope_ids);
    for (const std::string& envelope_id : envelope_ids) {
        static_cast<void>(ledger.transition(
            envelope_id, arachne::coordination::cocoon_status::processing,
            "workflow:resume"
        ));
    }
    static_cast<void>(ledger.transition(
        envelope_ids.back(), arachne::coordination::cocoon_status::failed,
        "workflow:pre-activation-failure"
    ));

    EXPECT_THROW(
        ledger.finish_integrated_product_run(
            "run-resume", "runs/run-resume.json"
        ),
        std::logic_error
    );
    EXPECT_EQ(
        ledger.get(envelope_ids.front()).status,
        arachne::coordination::cocoon_status::processing
    );
    EXPECT_EQ(
        sqlite_integer(
            ledger_path,
            "SELECT count(*) FROM runs WHERE run_id='run-resume' AND "
            "status='running'"
        ),
        1
    );
    EXPECT_FALSE(ledger.claim_logical_run(
        "run-resume", "product_graph", "2026-07-21", hash
    ));
    EXPECT_TRUE(ledger.claim_logical_run(
        "run-resume", "product_graph", "2026-07-21", hash, false, true
    ));
    EXPECT_EQ(
        sqlite_integer(
            ledger_path,
            "SELECT count(*) FROM run_attempts WHERE run_id='run-resume'"
        ),
        1
    );
    static_cast<void>(ledger.transition(
        envelope_ids.back(),
        arachne::coordination::cocoon_status::waiting_processing,
        "workflow:resume"
    ));
    static_cast<void>(ledger.transition(
        envelope_ids.back(), arachne::coordination::cocoon_status::processing,
        "workflow:resume"
    ));
    ledger.finish_integrated_product_run("run-resume", "runs/run-resume.json");
    for (const auto& envelope :
         ledger.list(arachne::coordination::cocoon_status::integrated)) {
        EXPECT_TRUE(ledger.retire_queued_payload(envelope.envelope_id, queue));
    }
    for (const auto& envelope : ledger.product_run_inputs("run-resume")) {
        EXPECT_EQ(
            envelope.status, arachne::coordination::cocoon_status::integrated
        );
        EXPECT_FALSE(std::filesystem::exists(envelope.payload_ref));
        EXPECT_FALSE(ledger.retire_queued_payload(envelope.envelope_id, queue));
    }
}

TEST(Coordinator, FailedQueuedPayloadsStillReachTheRetryThreshold) {
    temporary_directory temporary;
    arachne::coordination::operational_ledger ledger(
        temporary.path() / "ledger.sqlite"
    );
    const auto queue = temporary.path() / "queue";
    for (int index = 0; index < 15; ++index) {
        const auto source
            = temporary.path() / ("failed-" + std::to_string(index) + ".bin");
        write(source, "opaque payload " + std::to_string(index));
        const auto envelope = approve_for_processing(
            ledger,
            ledger.intake(
                {
                    .source_path = source,
                    .inbox_root = queue,
                    .submission_ref = "retry:" + std::to_string(index),
                    .title = "Retry payload",
                    .supersedes = std::nullopt,
                }
            )
        );
        static_cast<void>(ledger.transition(
            envelope.envelope_id,
            arachne::coordination::cocoon_status::processing,
            "workflow:failed-run"
        ));
        static_cast<void>(ledger.transition(
            envelope.envelope_id, arachne::coordination::cocoon_status::failed,
            "workflow:failed-run", "materialization failed"
        ));
    }
    EXPECT_EQ(ledger.accumulation().accepted_count, 15U);
    EXPECT_TRUE(ledger.should_integrate({}));
}

TEST(Coordinator, InboxDeletionTargetsAreAlwaysRejected) {
    temporary_directory temporary;
    const auto inbox = temporary.path() / "inbox";
    std::filesystem::create_directories(inbox);
    EXPECT_THROW(
        arachne::coordination::reject_inbox_deletion_target(
            inbox / "anything", inbox
        ),
        std::invalid_argument
    );
    EXPECT_NO_THROW(
        arachne::coordination::reject_inbox_deletion_target(
            temporary.path() / "generated", inbox
        )
    );
    EXPECT_THROW(
        arachne::coordination::reject_inbox_deletion_target(
            temporary.path(), inbox
        ),
        std::invalid_argument
    );
}

TEST(Coordinator, IntegratedInternalPayloadCanRetireWithoutTouchingLegacy) {
    temporary_directory temporary;
    const auto legacy = temporary.path() / "legacy_inbox";
    const auto queue = temporary.path() / "queue";
    const auto source = legacy / "legacy-batch.bin";
    write(source, "opaque legacy bytes");
    const auto ledger_path = temporary.path() / "ledger.sqlite";
    arachne::coordination::operational_ledger ledger(ledger_path, legacy);
    ledger.capture_inbox_baseline(legacy);
    const arachne::coordination::intake_request request {
        .source_path = source,
        .inbox_root = queue,
        .submission_ref = "legacy-import:1",
        .title = "Legacy opaque batch",
        .supersedes = std::nullopt,
    };
    const auto envelope
        = approve_for_processing(ledger, ledger.intake(request));
    ASSERT_TRUE(std::filesystem::is_regular_file(envelope.payload_ref));
    EXPECT_THROW(
        static_cast<void>(
            ledger.retire_queued_payload(envelope.envelope_id, queue)
        ),
        std::logic_error
    );
    static_cast<void>(ledger.transition(
        envelope.envelope_id, arachne::coordination::cocoon_status::processing,
        "workflow:run-retire"
    ));
    static_cast<void>(ledger.transition(
        envelope.envelope_id, arachne::coordination::cocoon_status::integrated,
        "workflow:run-retire"
    ));
    EXPECT_TRUE(ledger.retire_queued_payload(envelope.envelope_id, queue));
    EXPECT_FALSE(std::filesystem::exists(envelope.payload_ref));
    EXPECT_FALSE(ledger.retire_queued_payload(envelope.envelope_id, queue));
    EXPECT_EQ(read(source), "opaque legacy bytes");
    EXPECT_TRUE(ledger.verify_inbox(legacy).empty());

    const auto duplicate = ledger.intake(request);
    EXPECT_EQ(duplicate.envelope_id, envelope.envelope_id);
    EXPECT_EQ(
        duplicate.status, arachne::coordination::cocoon_status::integrated
    );
    EXPECT_FALSE(std::filesystem::exists(duplicate.payload_ref));
    EXPECT_TRUE(ledger.verify_inbox(legacy).empty());
}

TEST(Coordinator, QueueReferencesRebaseWhenTheStateCheckoutMoves) {
    temporary_directory temporary;
    const auto checkout = temporary.path() / "checkout";
    const auto source = checkout / "upload.bin";
    write(source, "portable queued bytes");
    std::string envelope_id;
    {
        arachne::coordination::operational_ledger ledger(
            checkout / "state" / "ledger.sqlite"
        );
        const auto envelope = ledger.intake(
            {
                .source_path = source,
                .inbox_root = checkout / "queue",
                .submission_ref = "move:test",
                .title = "Movable state",
                .supersedes = std::nullopt,
            }
        );
        envelope_id = envelope.envelope_id;
    }

    const auto moved = temporary.path() / "moved-checkout";
    std::filesystem::rename(checkout, moved);
    arachne::coordination::operational_ledger moved_ledger(
        moved / "state" / "ledger.sqlite"
    );
    const auto rebased = moved_ledger.get(envelope_id);
    EXPECT_TRUE(
        arachne::coordination::path_is_within(
            rebased.payload_ref, moved / "queue"
        )
    );
    EXPECT_EQ(read(rebased.payload_ref), "portable queued bytes");
    const auto duplicate = moved_ledger.intake(
        {
            .source_path = moved / "upload.bin",
            .inbox_root = moved / "queue",
            .submission_ref = "move:test",
            .title = "Movable state",
            .supersedes = std::nullopt,
        }
    );
    EXPECT_EQ(duplicate.envelope_id, envelope_id);
    EXPECT_EQ(duplicate.payload_ref, rebased.payload_ref);
}

TEST(Coordinator, GraphDomainLockAllowsOnlyOneWriter) {
    temporary_directory temporary;
    const auto locks = temporary.path() / "locks";
    arachne::coordination::domain_lock first(locks, "product_graph", "run-1");
    EXPECT_TRUE(first.owns_lock());
    EXPECT_THROW(
        arachne::coordination::domain_lock(locks, "product_graph", "run-2"),
        std::runtime_error
    );
    first.release();
    arachne::coordination::domain_lock second(locks, "product_graph", "run-2");
    EXPECT_TRUE(second.owns_lock());
}

TEST(Coordinator, StaleOrIncompleteLocksRequireNonDestructiveRecovery) {
    temporary_directory temporary;
    const auto locks = temporary.path() / "locks";
    const auto stale = locks / "product_graph.lock";
    std::filesystem::create_directories(stale);
    const auto old = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()
                     )
                         .count()
        - 3600;
    write(
        stale / "lease.json",
        "{\"format_version\":1,\"run_id\":\"old-run\","
        "\"graph_domain\":\"product_graph\",\"acquired_unix\":"
            + std::to_string(old) + "}"
    );
    EXPECT_THROW(
        arachne::coordination::domain_lock(
            locks, "product_graph", "new-run", std::chrono::seconds(10)
        ),
        std::runtime_error
    );
    EXPECT_TRUE(std::filesystem::is_regular_file(stale / "lease.json"));

    const auto incomplete_root = temporary.path() / "incomplete-locks";
    std::filesystem::create_directories(
        incomplete_root / "research_candidate_graph.lock"
    );
    EXPECT_THROW(
        arachne::coordination::domain_lock(
            incomplete_root, "research_candidate_graph", "candidate-run"
        ),
        std::runtime_error
    );
    EXPECT_TRUE(
        std::filesystem::is_directory(
            incomplete_root / "research_candidate_graph.lock"
        )
    );
}

TEST(Coordinator, ReplacedLockOwnershipCannotDeleteTheNewLease) {
    temporary_directory temporary;
    const auto locks = temporary.path() / "locks";
    arachne::coordination::domain_lock first(
        locks, "product_graph", "run-first"
    );
    std::filesystem::remove_all(locks / "product_graph.lock");
    arachne::coordination::domain_lock replacement(
        locks, "product_graph", "run-replacement"
    );
    EXPECT_THROW(first.release(), std::runtime_error);
    EXPECT_TRUE(replacement.owns_lock());
    EXPECT_TRUE(
        std::filesystem::is_regular_file(
            locks / "product_graph.lock" / "lease.json"
        )
    );
    replacement.release();
}

TEST(Coordinator, LockRootsMayNotTraverseSymlinks) {
    temporary_directory temporary;
    const auto real = temporary.path() / "real-locks";
    const auto link = temporary.path() / "linked-locks";
    std::filesystem::create_directories(real);
    std::filesystem::create_directory_symlink(real, link);
    EXPECT_THROW(
        arachne::coordination::domain_lock(
            link, "product_graph", "symlink-run"
        ),
        std::invalid_argument
    );
    EXPECT_TRUE(std::filesystem::is_empty(real));
}
