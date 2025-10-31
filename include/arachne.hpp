/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Yaroslav Riabtsev <yaroslav.riabtsev@rwth-aachen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef ARACHNE_ARACHNE_HPP
#define ARACHNE_ARACHNE_HPP
#include "pheidippides.hpp"

namespace arachnespace {
using std::chrono_literals::operator""h;

/** @brief Number of batchable kinds (Q, P, L, M, E, form, sense). */
inline constexpr std::size_t batched_kind_count = 7;

/**
 * @class arachne
 * @brief Accumulates entity IDs into per-kind batches and organizes groups.
 *
 * Invariants:
 *  - Queues store normalized ID strings per kind ("Q123", "P45", "L7", "M9",
 * "E2", "L7-F1", "L7-S2").
 *  - For numeric add/touch with kind = form or sense, normalization produces
 * "L<id>" (warning), because numeric IDs for forms/senses are not
 * representable; string APIs keep the exact ID.
 *  - Deduplication is by string identity in the respective containers.
 */
class arachne {
public:
    /// @name Public API
    /// @{

    /**
     * @brief Create or select a group and make it current.
     *
     * If @p name is empty, creates a new anonymous group with a random name and
     * makes it current. If @p name exists, it becomes current but is NOT
     * cleared. If it doesn't exist, the group is created and then selected.
     *
     * @param name Group name or empty for an anonymous group.
     * @return true if a new group was created; false if the group already
     * existed.
     *
     * @note The current group's name is intentionally not exposed; anonymous
     * groups cannot be addressed explicitly.
     */
    bool new_group(std::string name = "");
    /**
     * @brief Select an existing group or create it on demand.
     *
     * An empty @p name selects/creates the anonymous group. A non-empty name is
     * delegated to `new_group`, which creates the group if necessary.
     *
     * @param name Group name to activate; empty targets the anonymous group.
     */
    void select_group(std::string name);

    /**
     * @brief Enqueue numeric IDs with a given kind and add them to a group.
     *
     * Numeric IDs are normalized by adding the kind prefix.
     * - If @p kind is form or sense, normalization maps to the lexeme prefix
     *   ("L<id>"); no warning is emitted yet (logging TODO).
     * - Freshness checks are stubbed; the helper `enqueue` always asks for a
     *   fetch, and the underlying sets deduplicate repeated IDs automatically.
     *
     * @param ids  Span of numeric IDs.
     * @param kind Entity kind (must NOT be any/unknown).
     * @param name Group name; empty targets the current/anonymous group
     * (auto-created if needed).
     * @return The resulting size of the target group after insertions.
     * @throws std::invalid_argument if @p kind is any/unknown.
     */
    size_t add_ids(
        std::span<const int> ids, corespace::entity_kind kind,
        std::string name = ""
    );
    /**
     * @brief Extract the lexeme root from a full ID string.
     *
     * For IDs beginning with "L" followed by digits, returns "L<digits>". For
     * other prefixes or malformed strings, returns an empty string.
     *
     * @param id Identifier to inspect (e.g., "L7-F1").
     * @return Lexeme root ("L7") or empty on failure.
     */
    static std::string entity_root(const std::string& id);
    /**
     * @brief Placeholder for interactive staleness confirmation.
     *
     * The current implementation is non-interactive and always returns false.
     * A future version is expected to prompt the user when cached data is
     * stale and return the user's decision.
     *
     * @param id  Entity identifier under consideration.
     * @param kind Detected kind of the entity.
     * @param age  Age of the cached entry.
     * @return Currently always false; future behavior should reflect user
     *         confirmation.
     */
    bool ask_update(
        std::string_view id, corespace::entity_kind kind,
        const std::chrono::milliseconds age
    );
    /**
     * @brief Decide whether an entity should be enqueued for fetching.
     *
     * This placeholder implementation always returns true, effectively
     * requesting a fetch for every entity. The expected behavior is to consult
     * storage state (`exist`, `last`) and return true only when an update is
     * required.
     *
     * @param id   Canonical identifier (e.g., "Q123" or "L7").
     * @param kind Entity kind (lexeme for forms/senses).
     * @return true if the caller should enqueue the entity; placeholder always
     *         true.
     */
    bool enqueue(std::string_view id, corespace::entity_kind kind);

    /**
     * @brief Enqueue a full (prefixed) ID string and add it to a group.
     *
     * The ID must include its prefix (e.g., "Q123", "L77-F2").
     * Validation is performed via `identify()`. Invalid IDs cause an exception.
     * For "L...-F..."/"L...-S...", the group receives the verbatim string while
     * the batch queue stores the lexeme root ("L...") so fetches target the
     * parent lexeme.
     *
     * @param id_with_prefix Full ID with prefix.
     * @param force If true, bypass freshness/existence checks and enqueue
     * anyway.
     * @param name Group name; empty targets the current/anonymous group
     * (auto-created if needed).
     * @return The resulting size of the target group after insertion.
     * @throws std::invalid_argument if the ID is invalid or has an unknown
     * prefix.
     */
    size_t add_entity(
        const std::string& id_with_prefix, bool force = false,
        std::string name = ""
    );

    /**
     * @brief Flush (send) up to `batch_threshold` entities of a specific kind.
     *
     * For `kind != any`, attempts a single-batch flush for that kind (up to the
     * threshold). For `kind == any`, a round-robin strategy over batchable
     * kinds is used.
     *
     * @param kind Entity kind selector or entity_kind::any.
     * @return true if at least one entity was flushed; false otherwise.
     */
    bool flush(corespace::entity_kind kind = corespace::entity_kind::any);

    /**
     * @brief Get the number of queued (pending) entities tracked in the main
     *        batch containers.
     *
     * @param kind Specific kind, or entity_kind::any to return the sum across
     *             all batchable kinds.
     * @return Count of queued entities for the requested kind. Values are
     *         narrowed to `int` via `static_cast` and therefore inherit the
     *         implementation-defined behavior of signed narrowing conversions
     *         when the count exceeds `INT_MAX`.
     */
    int queue_size(corespace::entity_kind kind) const noexcept;

    /**
     * @brief Determine the kind of a full ID string.
     *
     * Accepts prefixed IDs (e.g., "Q123", "L77-F2"). Returns `unknown` if the
     * string is not a valid ID. The function does not throw.
     *
     * @param entity Full ID with prefix.
     * @return Detected kind (may be `unknown`).
     */
    static corespace::entity_kind identify(const std::string& entity) noexcept;

    /**
     * @brief Normalize a numeric ID with the given kind to a prefixed string.
     *
     * Examples:
     *  - (123, item)      -> "Q123"
     *  - (45,  property)  -> "P45"
     *  - (7,   lexeme)    -> "L7"
     *  - (9,   mediainfo) -> "M9"
     *  - (2,   entity_schema) -> "E2"
     *  - (7,   form)      -> "L7" (mapped to lexeme)
     *  - (7,   sense)     -> "L7" (mapped to lexeme)
     *
     * @param id   Numeric identifier.
     * @param kind Kind to prefix with (must not be any/unknown).
     * @return Prefixed ID string.
     * @throws std::invalid_argument if @p id is negative or @p kind is
     *         any/unknown.
     *
     * @note Form and sense identifiers are currently coerced to the lexeme
     *       prefix without emitting diagnostics; logging is a planned
     *       enhancement.
     */
    static std::string normalize(int id, corespace::entity_kind kind);

    /**
     * @brief Increment the touch counter for a single full ID (prefix
     * REQUIRED).
     *
     * If the entity is already queued or already has data, returns false (no
     * increment). If the counter reaches `candidates_threshold` and the entity
     * is not queued, it is moved into the queue. For "L…-F…"/"L…-S…", the exact
     * ID is enqueued (no mapping).
     *
     * @param id_with_prefix Full ID with prefix.
     * @return true if the counter was incremented; false otherwise.
     */
    bool touch_entity(std::string_view id_with_prefix) noexcept;

    /**
     * @brief Batch variant of touch for numeric IDs.
     *
     * Each numeric ID is normalized using @p kind.
     * If @p kind is form/sense, a warning is recorded and normalization yields
     * "L<id>" (lexeme).
     *
     * @param ids  Span of numeric IDs.
     * @param kind Normalization kind (must not be any/unknown).
     * @return The number of entities for which `touch_entity()` returned true.
     * @throws std::invalid_argument if @p kind is any/unknown.
     */
    int
    touch_ids(std::span<const int> ids, corespace::entity_kind kind) noexcept;

    /// @}

private:
    /**
     * @brief Parse a full ID string and extract the numeric portion.
     *
     * @param entity Full ID (e.g., "Q123", "L7-F1", "L7-S2").
     * @param pos    In/out index of the first digit within @p entity. On
     *               success the index is advanced past the number.
     * @param id     Out parameter for the parsed integer portion.
     * @return true on successful parse; false otherwise. Never throws.
     *
     * @internal Helper used by ID validation and normalization routines.
     */
    static bool parse_id(const std::string& entity, size_t& pos, int& id);

    // Queues (batches) per batchable kind; elements are expected to be
    // normalized IDs such as "Q123", "P45", "L7", "M9", or "E2". Forms and
    // senses contribute their lexeme root ("L<id>").
    std::array<std::unordered_set<std::string>, batched_kind_count>
        main_batches;
    std::array<std::unordered_set<std::string>, batched_kind_count>
        extra_batches;

    // Groups: group name -> set of entity IDs as provided by callers (verbatim;
    // includes "L...-F..." and "L...-S...").
    std::unordered_map<std::string, std::unordered_set<std::string>> groups;

    // Touch candidates: full ID string -> touch count.
    std::unordered_map<std::string, int> candidates;

    // Thresholds (kept constant for now; make configurable later if needed).
    const size_t batch_threshold
        = 50; ///< Typical unauthenticated entity-per-request cap.
    const size_t candidates_threshold
        = 50; ///< Intentional high bar for curiosity-driven candidates.

    // Current group name (private by design; anonymous groups cannot be
    // addressed explicitly).
    std::string current_group;
    std::chrono::milliseconds staleness_threshold = 24h;
    bool interactive = false;

    pheidippides phe_client;
};
}
#endif // ARACHNE_ARACHNE_HPP
