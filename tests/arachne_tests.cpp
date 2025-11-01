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

using namespace arachnespace;
using namespace corespace;

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

TEST(Identify, StrictSyntax) {
    EXPECT_EQ(arachne::identify("L1-"), entity_kind::unknown);
    EXPECT_EQ(arachne::identify("L1-X2"), entity_kind::unknown);
    EXPECT_EQ(arachne::identify("Q1-2"), entity_kind::unknown);
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

TEST(Identify, RejectsLeadingZeros) {
    EXPECT_EQ(arachne::identify("Q01"), entity_kind::unknown);
    EXPECT_EQ(arachne::identify("L01-F1"), entity_kind::unknown);
    EXPECT_EQ(arachne::identify("L1-F01"), entity_kind::unknown);
}

TEST(Identify, Bounds) {
    EXPECT_EQ(arachne::identify("Q2147483647"), entity_kind::item);
    EXPECT_EQ(arachne::identify("Q2147483648"), entity_kind::unknown);
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
