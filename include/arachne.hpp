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

#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>

/**
 * @brief Wikidata entity kind.
 *
 * Names include the prefix letter for clarity:
 *  - item (Q…), property (P…), lexeme (L…), mediainfo (M…), entity_schema (E…),
 *    form (L…-F…), sense (L…-S…).
 * `any` is an API selector; `unknown` denotes invalid/unrecognized IDs.
 */
enum class entity_kind {
    item, ///< Q…
    property, ///< P…
    lexeme, ///< L…
    mediainfo, ///< M…
    entity_schema, ///< E…
    form, ///< L…-F…
    sense, ///< L…-S…
    any, ///< API selector (e.g., flush(any)); not directly batchable.
    unknown ///< Unrecognized/invalid identifier.
};

/** @brief Number of batchable kinds (Q, P, L, M, E, Form, Sense). */
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
     * @brief Create/select a group.
     *
     * If @p name is empty, creates a new anonymous group with a random name and
     * makes it current. If @p name exists, it becomes current but is NOT
     * cleared. If it doesn't exist, it is created.
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
     * @brief Enqueue numeric IDs with a given kind and add them to a group.
     *
     * Numeric IDs are normalized by adding the kind prefix.
     * - If @p kind is form or sense, a warning is recorded and normalization
     * yields "L<id>" (lexeme), since forms/senses are not representable
     * numerically.
     * - Freshness checks are stubbed; duplicates in the queue are ignored.
     *
     * @param ids  Span of numeric IDs.
     * @param kind Entity kind (must NOT be any/unknown).
     * @param name Group name; empty targets the current/anonymous group
     * (auto-created if needed).
     * @return The resulting size of the target group after insertions.
     * @throws std::invalid_argument if @p kind is any/unknown.
     */
    int
    add_ids(std::span<const int> ids, entity_kind kind, std::string name = "");

    /**
     * @brief Enqueue a full (prefixed) ID string and add it to a group.
     *
     * The ID must include its prefix (e.g., "Q123", "L77-F2").
     * Validation is performed via `identify()`. Invalid IDs cause an exception.
     * For "L…-F…"/"L…-S…" exact IDs are queued (no mapping).
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
    int add_entity(
        std::string id_with_prefix, bool force = false, std::string name = ""
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
    bool flush(entity_kind kind = entity_kind::any);

    /**
     * @brief Get the number of queued (pending) entities.
     *
     * @param kind Specific kind, or entity_kind::any to return the sum across
     * all batchable kinds.
     * @return Count of queued entities.
     */
    int queue_size(entity_kind kind) const noexcept;

    /**
     * @brief Determine the kind of a full ID string.
     *
     * Accepts prefixed IDs (e.g., "Q123", "L77-F2"). Returns `unknown` if the
     * string is not a valid ID. The function does not throw.
     *
     * @param entity Full ID with prefix.
     * @return Detected kind (may be `unknown`).
     */
    static entity_kind identify(const std::string& entity) noexcept;

    /**
     * @brief Normalize a numeric ID with the given kind to a prefixed string.
     *
     * Examples:
     *  - (123, item)      -> "Q123"
     *  - (45,  property)  -> "P45"
     *  - (7,   lexeme)    -> "L7"
     *  - (9,   mediainfo) -> "M9"
     *  - (2,   entity_schema)    -> "E2"
     *  - (7,   form)      -> "L7"   (warning: mapped to lexeme)
     *  - (7,   sense)     -> "L7"   (warning: mapped to lexeme)
     *
     * @param id   Numeric identifier.
     * @param kind Kind to prefix with (must not be any/unknown).
     * @return Prefixed ID string.
     * @throws std::invalid_argument if @p kind is any/unknown.
     */
    static std::string normalize(int id, entity_kind kind);

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
    int touch_ids(std::span<const int> ids, entity_kind kind) noexcept;

    /// @}

private:
    /**
     * @brief Parse a full ID string and extract the numeric part
     * position/value.
     * @param entity Full ID (e.g., "Q123", "L7-F1", "L7-S2").
     * @param pos    Out: index of the first digit within @p entity
     * (implementation-defined if absent).
     * @param id     Out: parsed integer portion when present.
     * @return true on successful parse; false otherwise. Never throws.
     * @internal Helper used by ID validation/normalization routines.
     */

    static bool parse_id(const std::string& entity, size_t& pos, int& id);

    // Queues (batches) per batchable kind; elements are normalized IDs
    // ("Q123", "P45", "L7", "M9", "E2", "L7-F1", "L7-S2").
    std::array<std::unordered_set<std::string>, batched_kind_count>
        main_batches;
    std::array<std::unordered_set<std::string>, batched_kind_count>
        extra_batches;

    // Groups: group name -> set of entity IDs as added (verbatim; includes
    // "L…-F…"/"L…-S…").
    std::unordered_map<std::string, std::unordered_set<std::string>> groups;

    // Touch candidates: full ID string -> touch count.
    std::unordered_map<std::string, int> candidates;

    // Thresholds (kept constant for now; make configurable later if needed).
    const int batch_threshold
        = 50; ///< Typical unauthenticated entity-per-request cap.
    const int candidates_threshold
        = 50; ///< Intentional high bar for curiosity-driven candidates.

    // Current group name (private by design; anonymous groups cannot be
    // addressed explicitly).
    std::string current_group;
};

#endif // ARACHNE_ARACHNE_HPP
