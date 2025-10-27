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
#include <gtest/gtest.h>

TEST(Identify, ValidSimpleKinds) {
    using K = entity_kind;
    EXPECT_EQ(arachne::identify("Q123"), K::item);
    EXPECT_EQ(arachne::identify("P45"), K::property);
    EXPECT_EQ(arachne::identify("L7"), K::lexeme);
    EXPECT_EQ(arachne::identify("M9"), K::mediainfo);
    EXPECT_EQ(arachne::identify("E2"), K::entity_schema);
}

TEST(Identify, ValidFormAndSense) {
    using K = entity_kind;
    EXPECT_EQ(arachne::identify("L77-F2"), K::form);
    EXPECT_EQ(arachne::identify("L77-S2"), K::sense);
}

TEST(Identify, InvalidInputs) {
    using K = entity_kind;
    EXPECT_EQ(arachne::identify(""), K::unknown);
    EXPECT_EQ(arachne::identify("X123"), K::unknown);
    EXPECT_EQ(arachne::identify("Q"), K::unknown);
    EXPECT_EQ(arachne::identify("Q-1"), K::unknown);
    EXPECT_EQ(arachne::identify("Qabc"), K::unknown);
    EXPECT_EQ(arachne::identify("L1-"), K::unknown);
    EXPECT_EQ(arachne::identify("L7-T1"), K::unknown);
    EXPECT_EQ(arachne::identify("L-F1"), K::unknown);
}

TEST(Normalize, BasicPrefixes) {
    EXPECT_EQ(arachne::normalize(123, entity_kind::item), "Q123");
    EXPECT_EQ(arachne::normalize(45, entity_kind::property), "P45");
    EXPECT_EQ(arachne::normalize(7, entity_kind::lexeme), "L7");
    EXPECT_EQ(arachne::normalize(9, entity_kind::mediainfo), "M9");
    EXPECT_EQ(arachne::normalize(2, entity_kind::entity_schema), "E2");
}

TEST(Normalize, FormAndSenseMapToLexeme) {
    EXPECT_EQ(arachne::normalize(7, entity_kind::form), "L7");
    EXPECT_EQ(arachne::normalize(7, entity_kind::sense), "L7");
}

TEST(Normalize, ThrowsOnAnyUnknownAndNegative) {
    EXPECT_THROW(
        (void)arachne::normalize(1, entity_kind::any), std::invalid_argument
    );
    EXPECT_THROW(
        (void)arachne::normalize(1, entity_kind::unknown), std::invalid_argument
    );
    EXPECT_THROW(
        (void)arachne::normalize(-1, entity_kind::item), std::invalid_argument
    );
}

TEST(Groups, NewGroupExplicitName) {
    arachne a;
    EXPECT_TRUE(a.new_group("alpha"));
    EXPECT_FALSE(a.new_group("alpha"));
}

TEST(Groups, NewAnonymousGroupAlwaysCreatesNew) {
    arachne a;
    EXPECT_TRUE(a.new_group("alpha"));
    EXPECT_TRUE(a.new_group(""));
    EXPECT_TRUE(a.new_group(""));
}

TEST(Queue, InitiallyEmpty) {
    arachne a;
    EXPECT_EQ(a.queue_size(entity_kind::any), 0);
    EXPECT_EQ(a.queue_size(entity_kind::item), 0);
    EXPECT_EQ(a.queue_size(entity_kind::sense), 0);
}

TEST(AddEntity, AddsToCurrentGroupAndQueues) {
    arachne a;
    a.new_group("g1");
    int size1 = a.add_entity("Q1");
    EXPECT_EQ(size1, 1);
    EXPECT_EQ(a.queue_size(entity_kind::item), 1);
    EXPECT_EQ(a.queue_size(entity_kind::any), 1);
    int size2 = a.add_entity("Q1");
    EXPECT_EQ(size2, 1);
    EXPECT_EQ(a.queue_size(entity_kind::item), 1);
    int size3 = a.add_entity("L77-F2");
    EXPECT_EQ(size3, 2);
    EXPECT_EQ(a.queue_size(entity_kind::form), 1);
    int size4 = a.add_entity("L77-S3");
    EXPECT_EQ(size4, 3);
    EXPECT_EQ(a.queue_size(entity_kind::sense), 1);
}

TEST(AddEntity, ThrowsOnInvalidId) {
    arachne a;
    a.new_group("gX");
    EXPECT_THROW((void)a.add_entity("Q"), std::invalid_argument);
    EXPECT_THROW((void)a.add_entity("X123"), std::invalid_argument);
    EXPECT_THROW((void)a.add_entity("L77-T1"), std::invalid_argument);
}

TEST(AddIds, NumericIdsNormalizeAndDedup) {
    arachne a;
    a.new_group("gnums");
    std::array<int, 5> ids { 1, 2, 2, 3, 1 };
    int sz = a.add_ids(ids, entity_kind::item);
    EXPECT_EQ(sz, 3);
    EXPECT_EQ(a.queue_size(entity_kind::item), 3);
    std::vector<int> fs { 7, 7 };
    int sz2 = a.add_ids(fs, entity_kind::form);
    EXPECT_EQ(sz2, 4);
    EXPECT_EQ(a.queue_size(entity_kind::lexeme), 1);
    int sz3 = a.add_ids(fs, entity_kind::sense);
    EXPECT_EQ(sz3, 4);
    EXPECT_EQ(a.queue_size(entity_kind::lexeme), 1);
}

TEST(AddIds, ThrowsOnAnyUnknown) {
    arachne a;
    a.new_group("gerr");
    std::vector<int> ids { 1, 2, 3 };
    EXPECT_THROW((void)a.add_ids(ids, entity_kind::any), std::invalid_argument);
    EXPECT_THROW(
        (void)a.add_ids(ids, entity_kind::unknown), std::invalid_argument
    );
}

TEST(Touch, PromotesOnThreshold) {
    arachne a;
    for (int i = 0; i < 49; ++i) {
        EXPECT_TRUE(a.touch_entity("Q42"));
        EXPECT_EQ(a.queue_size(entity_kind::item), 0);
    }
    EXPECT_TRUE(a.touch_entity("Q42"));
    EXPECT_EQ(a.queue_size(entity_kind::item), 1);
    EXPECT_FALSE(a.touch_entity("Q42"));
    EXPECT_EQ(a.queue_size(entity_kind::item), 1);
}

TEST(Touch, NumericTouchNormalizes) {
    arachne a;
    std::array<int, 3> ids { 1, 1, 1 };
    int n = a.touch_ids(ids, entity_kind::form);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(a.queue_size(entity_kind::lexeme), 0);
}

TEST(Touch, RejectsInvalidIds) {
    arachne a;
    EXPECT_FALSE(a.touch_entity("Q"));
    EXPECT_FALSE(a.touch_entity("X123"));
    EXPECT_EQ(a.queue_size(entity_kind::any), 0);
}

TEST(Flush, SpecificKindFlushesUpToThreshold) {
    arachne a;
    a.new_group("gf");
    std::vector<int> ids;
    for (int i = 1; i <= 10; ++i) {
        ids.push_back(i);
    }
    a.add_ids(ids, entity_kind::item);
    EXPECT_EQ(a.queue_size(entity_kind::item), 10);
    EXPECT_TRUE(a.flush(entity_kind::item));
    EXPECT_EQ(a.queue_size(entity_kind::item), 0);
}

TEST(Flush, AnyUsesRoundRobinAcrossKinds) {
    arachne a;
    a.new_group("gar");
    a.add_entity("Q1");
    a.add_entity("Q2");
    a.add_entity("Q3");
    a.add_entity("P1");
    a.add_entity("P2");
    a.add_entity("P3");
    a.add_entity("L1");
    a.add_entity("L2");
    a.add_entity("L3");
    EXPECT_EQ(a.queue_size(entity_kind::item), 3);
    EXPECT_EQ(a.queue_size(entity_kind::property), 3);
    EXPECT_EQ(a.queue_size(entity_kind::lexeme), 3);
    EXPECT_TRUE(a.flush(entity_kind::any));
    for (int i = 0; i < 5; ++i) {
        a.flush(entity_kind::any);
    }
    EXPECT_EQ(a.queue_size(entity_kind::any), 0);
}
