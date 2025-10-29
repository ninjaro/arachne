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

#ifndef ARACHNE_PHEIDIPPIDES_HPP
#define ARACHNE_PHEIDIPPIDES_HPP
#include "arachne.hpp"

#include <array>
#include <atomic>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

struct network_metrics final {
    std::atomic<unsigned> requests { 0 };
    std::atomic<unsigned> retries { 0 };
    std::atomic<size_t> sleep_ns { 0 };
    std::atomic<size_t> network_ns { 0 };
    std::atomic<size_t> bytes_received { 0 };
    std::array<std::atomic<unsigned>, 600> statuses { 0 };

    network_metrics();
};

struct options {
    int timeout_ms = 10000;
    int connect_timeout_ms = 3000;
    int max_retries = 3;
    int retry_base_ms = 200;
    int retry_max_ms = 3000;

    std::size_t batch_threshold = 50;

    std::string user_agent = "arachne/pheidippides";
    std::string accept = "application/json";

    cpr::Parameters params {
        { "languages", "en" },    { "languagefallback", "1" },
        { "normalize", "1" },     { "format", "json" },
        { "formatversion", "2" }, { "rvslots", "main" },
        { "rvprop", "content" }
    };
};

class pheidippides {
public:
    pheidippides();

    nlohmann::json fetch_json(
        const std::unordered_set<std::string>& batch,
        entity_kind kind = entity_kind::any
    );

    const network_metrics& metrics_info();

    static std::string join_str(
        std::span<const std::string> ids, std::string_view separator = "|"
    );

private:
    cpr::Response get_with_retries(const cpr::Parameters& params);

    network_metrics metrics;
    cpr::Session session;

    options opt {};
};

#endif // ARACHNE_PHEIDIPPIDES_HPP
