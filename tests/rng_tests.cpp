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

#include "rng.hpp"
#include <gtest/gtest.h>

TEST(Rng, SingletonInstance) {
    auto* p1 = &rng();
    auto* p2 = &rng();
    EXPECT_EQ(p1, p2);
}

TEST(RandomHex, LengthAndCharset) {
    for (std::size_t n : { 0u, 1u, 8u, 16u, 31u }) {
        std::string s = random_hex(n);
        ASSERT_EQ(s.size(), n);
        for (const char c : s) {
            bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            ASSERT_TRUE(ok) << c;
        }
    }
}

TEST(RandomHex, LikelyDifferentOnSuccessiveCalls) {
    std::string a = random_hex(16);
    std::string b = random_hex(16);
    EXPECT_NE(a, b);
}
