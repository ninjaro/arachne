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

#include "http_client.hpp"

#include <gtest/gtest.h>

using namespace corespace;

http_client& http_shared_client() {
    static http_client client;
    return client;
}

TEST(HttpClientSmoke, OK) {
    auto& client = http_shared_client();
    const auto response = client.get("https://httpbingo.org/get?ping=ok");

    EXPECT_EQ(response.status_code, 200);
    EXPECT_FALSE(response.text.empty());
    EXPECT_EQ(response.error_code, CURLE_OK);
    EXPECT_TRUE(response.error_message.empty());
}

TEST(HttpClientSmoke, RedirectFollow) {
    auto& client = http_shared_client();
    const auto response
        = client.get("https://httpbingo.org/redirect-to?url=/status/204");

    EXPECT_EQ(response.status_code, 204);
    EXPECT_TRUE(response.text.empty());
}

TEST(NetworkMetrics, DefaultInitialization) {
    // auto& client = http_shared_client();
    // const network_metrics& metrics = client.metrics_info();
    // EXPECT_EQ(metrics.requests.load(), 2u);
    // EXPECT_EQ(metrics.retries.load(), 707u);
    // EXPECT_EQ(metrics.sleep_ms.load(), 751u);
    // EXPECT_EQ(metrics.network_ms.load(), 1u);
    // EXPECT_EQ(metrics.bytes_received.load(), 1u);
}