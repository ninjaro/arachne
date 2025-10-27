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

bool arachne::flush(entity_kind kind) { return false; }

int arachne::queue_size(const entity_kind kind) const noexcept {
    if (kind == entity_kind::any) {
        std::size_t sum = 0;
        for (const auto& batch : batches) {
            sum += batch.size();
        }
        return static_cast<int>(sum);
    }
    const auto idx = static_cast<std::size_t>(kind);
    if (idx >= batches.size()) {
        return 0;
    }
    return static_cast<int>(batches[idx].size());
}

entity_kind arachne::identify(const std::string& entity) noexcept {
    if (entity.size() < 2)
        return entity_kind::unknown;

    size_t i = 0;
    size_t k = prefixes.find(entity[i++]);
    if (k == std::string::npos)
        return entity_kind::unknown;

    int id = 0;
    size_t n = 0;
    try {
        id = std::stoi(entity.substr(i), &n);
    } catch (...) {
        return entity_kind::unknown;
    }
    if (id < 0 || n == 0)
        return entity_kind::unknown;
    if (std::to_string(id).size() != n)
        return entity_kind::unknown;
    i += n;

    if (i == entity.size())
        return static_cast<entity_kind>(k);

    if (k != 2 || i >= entity.size() || entity[i++] != '-')
        return entity_kind::unknown;
    if (i >= entity.size())
        return entity_kind::unknown;

    char tag = entity[i++];
    if (tag != 'F' && tag != 'S')
        return entity_kind::unknown;

    int id2 = 0;
    size_t n2 = 0;
    try {
        id2 = std::stoi(entity.substr(i), &n2);
    } catch (...) {
        return entity_kind::unknown;
    }
    if (id2 < 0 || n2 == 0)
        return entity_kind::unknown;
    if (std::to_string(id2).size() != n2)
        return entity_kind::unknown;
    i += n2;

    if (i != entity.size())
        return entity_kind::unknown;

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
    if (idx > 4) {
        // Numeric Form/Sense are not representable; map to lexeme.
        // TODO: emit warning via logging sink.
        idx = 2;
    }
    return prefixes[idx] + std::to_string(id);
}

bool arachne::touch_entity(std::string_view id_with_prefix) noexcept {
    return false;
}

int arachne::touch_ids(std::span<const int> ids, entity_kind kind) noexcept {
    return 0;
}
