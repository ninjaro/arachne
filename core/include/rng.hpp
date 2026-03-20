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

#ifndef ARACHNE_RNG_HPP
#define ARACHNE_RNG_HPP

#include <random>
#include <string>

namespace corespace {
/**
 * @brief Shared PRNG seeded on first use.
 *
 * The generator is a process-wide `std::mt19937_64` seeded from
 * `std::random_device`. Calls from multiple threads share the same engine and
 * therefore require external synchronization if deterministic ordering is
 * important.
 */
std::mt19937_64& rng();

/**
 * @brief Return exactly @p n random hexadecimal characters (lowercase).
 *
 * The function draws 4-bit nibbles from the shared PRNG. Characters are not
 * zero-padded beyond the requested length; each position is an independent,
 * uniformly distributed hex digit.
 */
std::string random_hex(std::size_t n);
}
#endif // ARACHNE_RNG_HPP
