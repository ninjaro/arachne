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
#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(NetworkMetrics, InitializesToZero) {
    const network_metrics metrics;
    EXPECT_EQ(metrics.requests.load(), 0u);
    EXPECT_EQ(metrics.retries.load(), 0u);
    EXPECT_EQ(metrics.sleep_ns.load(), 0ull);
    EXPECT_EQ(metrics.network_ns.load(), 0ull);
    EXPECT_EQ(metrics.bytes_received.load(), 0ull);

    for (const auto& status : metrics.statuses) {
        EXPECT_EQ(status.load(), 0u);
    }
}

TEST(Pheidippides, FetchJsonItems) {
    pheidippides client;
    std::unordered_set<std::string> ids
        = { "Q190082", "Q165769", "Q184874", "Q313728" };
    const std::unordered_map<std::string, std::string> expected_labels {
        { "Q190082", "Arachne" },
        { "Q165769", "Penelope" },
        { "Q184874", "Ariadne" },
        { "Q313728", "Pheidippides" },
    };
    const auto json = client.fetch_json(ids, entity_kind::item);
    ASSERT_TRUE(json.contains("entities"));
    const auto& entities = json.at("entities");
    EXPECT_EQ(entities.size(), expected_labels.size());
    for (const auto& [id, expected_label] : expected_labels) {
        ASSERT_TRUE(entities.contains(id));
        const auto& entity = entities.at(id);
        ASSERT_TRUE(entity.contains("labels"));
        ASSERT_TRUE(entity["labels"].contains("en"));
        ASSERT_TRUE(entity["labels"]["en"].contains("value"));
        EXPECT_EQ(entity["labels"]["en"]["value"], expected_label);
    }
}

TEST(Pheidippides, FetchJsonProperty) {
    pheidippides client;
    std::unordered_set<std::string> ids = { "P1049", "P2925", "P4185" };
    const std::unordered_map<std::string, std::string> expected_labels {
        { "P1049", "worshipped by" },
        { "P2925", "domain of saint or deity" },
        { "P4185", "iconographic symbol" },
    };
    const auto json = client.fetch_json(ids, entity_kind::property);
    ASSERT_TRUE(json.contains("entities"));
    const auto& entities = json.at("entities");
    EXPECT_EQ(entities.size(), expected_labels.size());
    for (const auto& [id, expected_label] : expected_labels) {
        ASSERT_TRUE(entities.contains(id));
        const auto& entity = entities.at(id);
        ASSERT_TRUE(entity.contains("labels"));
        ASSERT_TRUE(entity["labels"].contains("en"));
        ASSERT_TRUE(entity["labels"]["en"].contains("value"));
        EXPECT_EQ(entity["labels"]["en"]["value"], expected_label);
    }
}