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

#include <stdexcept>

static constexpr std::string prefixes = "QPLME";

bool arachne::new_group(std::string name) {
    if (name.empty()) {
        do {
            name = "g_" + random_hex(8);
        } while (groups.contains(name));
    }
    auto [it, inserted] = groups.try_emplace(name);
    current_group = it->first;
    return inserted;
}

int arachne::queue_size(const entity_kind kind) const noexcept {
    if (kind == entity_kind::any) {
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

entity_kind arachne::identify(const std::string& entity) noexcept {
    if (entity.size() < 2) {
        return entity_kind::unknown;
    }
    size_t pos = 0;
    size_t kind = prefixes.find(entity[pos++]);
    int id {};
    if (kind == std::string::npos || !parse_id(entity, pos, id)) {
        return entity_kind::unknown;
    }
    if (pos == entity.size()) {
        return static_cast<entity_kind>(kind);
    }
    if (kind != static_cast<size_t>(entity_kind::lexeme) || pos >= entity.size()
        || entity[pos++] != '-' || pos >= entity.size()) {
        return entity_kind::unknown;
    }
    const char tag = entity[pos++];
    if (tag != 'F' && tag != 'S' || !parse_id(entity, pos, id)
        || pos != entity.size()) {
        return entity_kind::unknown;
    }
    return tag == 'F' ? entity_kind::form : entity_kind::sense;
}

std::string arachne::normalize(const int id, const entity_kind kind) {
    if (id < 0) {
        throw std::invalid_argument("normalize: id must be non-negative");
    }
    if (kind == entity_kind::any || kind == entity_kind::unknown) {
        throw std::invalid_argument(
            "normalize: kind must be a concrete, known entity kind"
        );
    }
    auto idx = static_cast<std::size_t>(kind);
    if (idx >= static_cast<size_t>(entity_kind::form)) {
        // Numeric Form/Sense are not representable; map to lexeme.
        // TODO: emit warning via logging sink.
        idx = static_cast<size_t>(entity_kind::lexeme);
    }
    return prefixes[idx] + std::to_string(id);
}

int arachne::add_ids(
    std::span<const int> ids, entity_kind kind, std::string name
) {
    return 0;
}

int arachne::add_entity(
    std::string id_with_prefix, bool force, std::string name
) {
    return 0;
}

bool arachne::touch_entity(std::string_view id_with_prefix) noexcept {
    return false;
}

int arachne::touch_ids(std::span<const int> ids, entity_kind kind) noexcept {
    return 0;
}

bool arachne::flush(entity_kind kind) { return false; }
