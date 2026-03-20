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

#include "arachne.hpp"
#include "rng.hpp"

namespace arachnespace {
static constexpr std::string prefixes = "QPLME";

bool arachne::new_group(std::string name) {
    if (name.empty()) {
        do {
            name = "g_" + corespace::random_hex(8);
        } while (groups.contains(name));
    }
    auto [it, inserted] = groups.try_emplace(name);
    current_group = it->first;
    return inserted;
}

size_t arachne::add_ids(
    const std::span<const int> ids, const corespace::entity_kind kind,
    std::string name
) {
    if (kind == corespace::entity_kind::any
        || kind == corespace::entity_kind::unknown) {
        throw std::invalid_argument("unknown kind of numeric IDs");
    }
    select_group(std::move(name));
    size_t last_size = groups[current_group].size();
    for (const int id : ids) {
        std::string id_with_prefix = normalize(id, kind);
        last_size = add_entity(id_with_prefix, false, current_group);
    }
    return last_size;
}

int arachne::touch_ids(
    const std::span<const int> ids, const corespace::entity_kind kind
) {
    if (kind == corespace::entity_kind::any
        || kind == corespace::entity_kind::unknown) {
        throw std::invalid_argument("unknown kind of numeric IDs");
    }
    int added = 0;
    for (const int id : ids) {
        std::string id_with_prefix = normalize(id, kind);
        added += touch_entity(id_with_prefix);
    }
    return added;
}

std::string arachne::entity_root(const std::string& id) {
    const corespace::entity_kind kind = identify(id);
    if (kind == corespace::entity_kind::any
        || kind == corespace::entity_kind::unknown) {
        throw std::invalid_argument("invalid or unknown entity kind");
    }

    if (kind == corespace::entity_kind::form
        || kind == corespace::entity_kind::sense) {
        if (id.size() < 2 || id.front() != 'L') {
            throw std::invalid_argument(
                "bad root-lexeme prefix of the entity: " + id
            );
        }
        int val {};
        if (size_t pos = 1; !parse_id(id, pos, val)) {
            throw std::invalid_argument(
                "bad numeric identifier of the entity: " + id
            );
        }
        return "L" + std::to_string(val);
    }
    return id;
}

bool arachne::flush(corespace::entity_kind kind) {
    const auto& batch = main_batches[static_cast<size_t>(kind)];
    const size_t size = batch.size();
    auto data = phe_client.fetch_json(batch, kind);
    // ariadne.store(data);
    return size > batch.size();
}

int arachne::queue_size(const corespace::entity_kind kind) const noexcept {
    if (kind == corespace::entity_kind::any) {
        std::size_t sum = 0;
        for (const auto& batch : main_batches) {
            sum += batch.size();
        }
        return static_cast<int>(sum);
    }
    const auto idx = static_cast<std::size_t>(kind);
    if (idx >= main_batches.size()) {
        return 0;
    }
    return static_cast<int>(main_batches[idx].size());
}

corespace::entity_kind arachne::identify(const std::string& entity) noexcept {
    if (entity.size() < 2) {
        return corespace::entity_kind::unknown;
    }
    size_t pos = 0;
    size_t kind = prefixes.find(entity[pos++]);
    int id {};
    if (kind == std::string::npos || !parse_id(entity, pos, id)) {
        return corespace::entity_kind::unknown;
    }
    if (pos == entity.size()) {
        return static_cast<corespace::entity_kind>(kind);
    }
    if (kind != static_cast<size_t>(corespace::entity_kind::lexeme)
        || pos >= entity.size() || entity[pos++] != '-'
        || pos >= entity.size()) {
        return corespace::entity_kind::unknown;
    }
    const char tag = entity[pos++];
    if (tag != 'F' && tag != 'S' || !parse_id(entity, pos, id)
        || pos != entity.size()) {
        return corespace::entity_kind::unknown;
    }
    return tag == 'F' ? corespace::entity_kind::form
                      : corespace::entity_kind::sense;
}

bool arachne::parse_id(const std::string& entity, size_t& pos, int& id) {
    id = 0;
    size_t len = 0;
    try {
        id = std::stoi(entity.substr(pos), &len);
    } catch (...) {
        return false;
    }
    if (id < 0 || len == 0 || std::to_string(id).size() != len) {
        return false;
    }
    pos += len;
    return true;
}

std::string
arachne::normalize(const int id, const corespace::entity_kind kind) {
    if (id < 0) {
        throw std::invalid_argument("normalize: id must be non-negative");
    }
    if (kind == corespace::entity_kind::any
        || kind == corespace::entity_kind::unknown) {
        throw std::invalid_argument(
            "normalize: kind must be a concrete, known entity kind"
        );
    }
    auto idx = static_cast<std::size_t>(kind);
    if (idx >= static_cast<size_t>(corespace::entity_kind::form)) {
        // Numeric Form/Sense are not representable; map to lexeme.
        // TODO: emit warning via logging sink.
        idx = static_cast<size_t>(corespace::entity_kind::lexeme);
    }
    return prefixes[idx] + std::to_string(id);
}

void arachne::select_group(std::string name) {
    if (name.empty()) {
        if (current_group.empty()) {
            new_group();
        }
        return;
    }
    new_group(std::move(name));
}

bool arachne::ask_update(
    std::string_view, corespace::entity_kind, const std::chrono::milliseconds
) {
    // UI/UX: todo: ask user if update is needed
    return false;
}

bool arachne::enqueue(
    const std::string_view id, const corespace::entity_kind kind,
    const bool interactive
) const {
    // ariadne.entity_status(id)
    auto [exist, last] = std::pair<bool, long long>(false, -1);
    if (!exist || last < 0) {
        return true;
    }
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()
    )
                            .count();
    const auto age = std::chrono::milliseconds { now_ms - last };
    if (age > staleness_threshold) {
        return true;
    }
    if (interactive) {
        return ask_update(id, kind, age);
    }
    return false;
}

bool arachne::touch_entity(const std::string& id_with_prefix) noexcept {
    candidates[id_with_prefix]++;
    if (candidates[id_with_prefix] >= candidates_threshold) {
        const std::string canonical = entity_root(id_with_prefix);
        corespace::entity_kind kind = identify(canonical);
        extra_batches[static_cast<size_t>(kind)].insert(canonical);
        return true;
    }
    return false;
}

size_t arachne::add_entity(
    const std::string& id_with_prefix, const bool force, std::string name
) {
    const std::string canonical = entity_root(id_with_prefix);
    select_group(std::move(name));
    auto& group = groups[current_group];
    group.insert(id_with_prefix);
    if (corespace::entity_kind kind = identify(canonical); force
        || enqueue(canonical, kind, ui == corespace::interface::command_line)) {
        auto& pool = main_batches[static_cast<size_t>(kind)];
        pool.insert(canonical);
        if (pool.size() >= batch_threshold) {
            flush(kind);
        }
    }
    return group.size();
}
}