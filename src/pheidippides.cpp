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

#include "pheidippides.hpp"
#include "rng.hpp"

network_metrics::network_metrics() {
    for (auto& status : statuses) {
        status.store(0, std::memory_order_relaxed);
    }
}

pheidippides::pheidippides() {
    session.SetUrl(cpr::Url { opt.url });
    session.SetHeader(
        { { "User-Agent", opt.user_agent }, { "Accept", opt.accept } }
    );
    session.SetTimeout(cpr::Timeout { opt.timeout_ms });
    session.SetConnectTimeout(cpr::ConnectTimeout { opt.connect_timeout_ms });
}

nlohmann::json pheidippides::fetch_json(
    std::unordered_set<std::string>& batch, const entity_kind kind
) {
    if (batch.empty()) {
        return nlohmann::json::object();
    }

    if (kind != entity_kind::item && kind != entity_kind::property
        && kind != entity_kind::any) {
        throw std::invalid_argument("fetch_json: unsupported entity kind");
    }

    std::vector<std::string> ids;
    ids.reserve(batch.size());
    for (const auto& id : batch) {
        const auto detected = arachne::identify(id);
        if (detected == entity_kind::unknown) {
            throw std::invalid_argument(
                "fetch_json: invalid entity identifier: " + id
            );
        }
        if (kind != entity_kind::any && detected != kind) {
            continue;
        }
        ids.push_back(id);
    }

    if (ids.empty()) {
        return nlohmann::json::object();
    }

    nlohmann::json combined = nlohmann::json::object();
    combined["entities"] = nlohmann::json::object();

    for (auto&& chunk : ids | std::views::chunk(opt.batch_threshold)) {
        auto chunk_vec = std::ranges::to<std::vector<std::string>>(chunk);
        const auto size = chunk_vec.size();
        if (size == 0) {
            continue;
        }
        const std::span<const std::string> batch_info { chunk_vec.data(),
                                                        size };
        nlohmann::json data = wbgetentities_batch(batch_info);
        if (data.contains("entities") && data["entities"].is_object()) {
            for (auto& [key, value] : data["entities"].items()) {
                combined["entities"][key] = std::move(value);
            }
        }
    }

    return combined;
}

const network_metrics& pheidippides::metrics_info() { return metrics; }

cpr::Parameters
pheidippides::build_params(const std::string& ids_joined) const {
    return cpr::Parameters { {
        { "action", opt.action },
        { "props", opt.props },
        { "languages", opt.languages },
        { "languagefallback", opt.languagefallback ? "1" : "0" },
        { "normalize", opt.normalize ? "1" : "0" },
        { "format", opt.format },
        { "formatversion", std::to_string(opt.formatversion) },
        { "ids", ids_joined },
    } };
}

nlohmann::json
pheidippides::wbgetentities_batch(const std::span<const std::string> ids) {
    if (ids.empty()) {
        return nlohmann::json::object();
    }
    const std::string ids_joined = join_ids(ids);
    const auto params = build_params(ids_joined);
    auto r = get_with_retries(params);
    return nlohmann::json::parse(r.text, nullptr, true);
}

using sys_seconds = std::chrono::sys_time<std::chrono::seconds>;

static bool parse_http_date_gmt(
    const std::string_view v, std::chrono::sys_time<std::chrono::seconds>& out
) {
    char wday[4] {}, mon[4] {}, tz[4] {};
    int y = 0, d = 0, H = 0, M = 0, S = 0;
    if (std::sscanf(
            v.data(), "%3s, %d %3s %d %d:%d:%d %3s", wday, &d, mon, &y, &H, &M,
            &S, tz
        )
        != 8) {
        return false;
    }
    if (std::strncmp(tz, "GMT", 3) != 0) {
        return false;
    }
    static constexpr const char* months[]
        = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
            "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int m_idx = -1;
    for (int i = 0; i < 12; ++i) {
        if (std::strncmp(mon, months[i], 3) == 0) {
            m_idx = i + 1;
            break;
        }
    }
    if (m_idx == -1) {
        return false;
    }
    using namespace std::chrono;
    const year_month_day ymd { year { y },
                               month { static_cast<unsigned>(m_idx) },
                               day { static_cast<unsigned>(d) } };
    if (!ymd.ok()) {
        return false;
    }
    const sys_days sd { ymd };
    out = sys_seconds { sd }
        + seconds { static_cast<long long>(H) * 3600
                    + static_cast<long long>(M) * 60
                    + static_cast<long long>(S) };
    return true;
}

long long parse_retry(const cpr::Response& r) {
    const auto it = r.header.find("Retry-After");
    if (it == r.header.end()) {
        return -1;
    }
    const std::string& v = it->second;
    try {
        long long s = std::stoll(v);
        if (s >= 0)
            return s * 1000LL;
    } catch (...) { }
    sys_seconds target {};
    if (!parse_http_date_gmt(v, target)) {
        return -1;
    }
    const auto now_s = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::system_clock::now()
    );
    const auto diff
        = std::chrono::duration_cast<std::chrono::milliseconds>(target - now_s);
    return diff.count() > 0 ? diff.count() : 0;
}

int jitter_ms(const int base, const int cap) {
    std::uniform_int_distribution d(0, base);
    return std::min(base + d(rng()), cap);
}

cpr::Response pheidippides::get_with_retries(const cpr::Parameters& params) {
    using namespace std::chrono;
    for (int attempt = 1;; ++attempt) {
        session.SetParameters(params);

        const auto t0 = steady_clock::now();
        auto r = session.Get();
        const auto t1 = steady_clock::now();

        const auto dt = duration_cast<milliseconds>(t1 - t0).count();
        ++metrics.requests;
        metrics.network_ns += static_cast<size_t>(dt);

        if (r.status_code >= 0
            && r.status_code < static_cast<long>(metrics.statuses.size())) {
            ++metrics.statuses[static_cast<std::size_t>(r.status_code)];
        }
        const std::size_t rx = r.text.size();
        metrics.bytes_received += rx;

        const bool net_ok = (r.error.code == cpr::ErrorCode::OK);
        const bool http_ok = (r.status_code >= 200 && r.status_code < 300);
        if (net_ok && http_ok) {
            return r;
        }

        const bool retryable_http = (r.status_code == 429)
            || (r.status_code == 408)
            || (r.status_code >= 500 && r.status_code < 600);

        if (attempt <= opt.max_retries && (!net_ok || retryable_http)) {
            ++metrics.retries;
            long long sleep_ms = jitter_ms(
                opt.retry_base_ms * (1 << (attempt - 1)), opt.retry_max_ms
            );
            long long ra = parse_retry(r);
            if (ra >= 0) {
                sleep_ms = std::min<long long>(sleep_ms, ra);
            }
            std::this_thread::sleep_for(milliseconds(sleep_ms));
            continue;
        }

        if (!net_ok) {
            throw std::runtime_error(
                std::string("cpr error: ") + r.error.message
            );
        }
        throw std::runtime_error(
            "http error: " + std::to_string(r.status_code)
        );
    }
}

std::string pheidippides::join_ids(std::span<const std::string> ids) {
    if (ids.empty()) {
        return {};
    }
    const auto joined = ids | std::views::join_with(std::string_view { "|" });
    return joined | std::ranges::to<std::string>();
}
