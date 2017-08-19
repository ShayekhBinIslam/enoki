/*
 * Tiny self-contained version of the PCG Random Number Generation for C++ put
 * together from pieces of the much larger C/C++ codebase with vectorization
 * using Enoki.
 *
 * Wenzel Jakob, February 2017
 *
 * The PCG random number generator was developed by Melissa O'Neill
 * <oneill@pcg-random.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For additional information about the PCG random number generation scheme,
 * including its license and other licensing options, visit
 *
 *     http://www.pcg-random.org
 */

#pragma once

#include <enoki/array.h>

#define PCG32_DEFAULT_STATE  0x853c49e6748fea9bULL
#define PCG32_DEFAULT_STREAM 0xda3e39cb94b95bdbULL
#define PCG32_MULT           0x5851f42d4c957f2dULL

NAMESPACE_BEGIN(enoki)

/// PCG32 pseudorandom number generator proposed by Melissa O'Neill
template <typename T> struct PCG32 {
    static constexpr size_t Size = array_size<T>::value;

    /* Some convenient type aliases for vectorization */
    using  Int64     = int64_array_t<T>;
    using UInt64     = uint64_array_t<T>;
    using UInt32     = uint32_array_t<T>;
    using Float64    = float64_array_t<T>;
    using Float32    = float32_array_t<T>;
    using UInt32Mask = mask_t<UInt32>;
    using UInt64Mask = mask_t<UInt64>;

    /// Initialize the pseudorandom number generator with the \ref seed() function
    PCG32(UInt64 initstate = PCG32_DEFAULT_STATE,
          UInt64 initseq = index_sequence<UInt64>() + PCG32_DEFAULT_STREAM) {
        seed(initstate, initseq);
    }

    /**
     * \brief Seed the pseudorandom number generator
     *
     * Specified in two parts: a state initializer and a sequence selection
     * constant (a.k.a. stream id)
     */
    void seed(UInt64 initstate, UInt64 initseq) {
        state = zero<UInt64>();
        inc = sli<1>(initseq) | 1u;
        next_uint32();
        state += initstate;
        next_uint32();
    }

    /// Generate a uniformly distributed unsigned 32-bit random number
    ENOKI_INLINE UInt32 next_uint32() {
        UInt64 oldstate = state;
        state = oldstate * uint64_t(PCG32_MULT) + inc;
        UInt32 xorshifted = UInt32(sri<27>(sri<18>(oldstate) ^ oldstate));
        UInt32 rot_offset = UInt32(sri<59>(oldstate));
        return ror(xorshifted, rot_offset);
    }

    /// Masked version of \ref next_uint32
    ENOKI_INLINE UInt32 next_uint32(const UInt64Mask &mask) {
        UInt64 oldstate = state;
        masked(state, mask) = oldstate * uint64_t(PCG32_MULT) + inc;
        UInt32 xorshifted = UInt32(sri<27>(sri<18>(oldstate) ^ oldstate));
        UInt32 rot_offset = UInt32(sri<59>(oldstate));
        return ror(xorshifted, rot_offset);
    }

    /// Generate a uniformly distributed unsigned 64-bit random number
    ENOKI_INLINE UInt64 next_uint64() {
        return UInt64(next_uint32()) | sli<32>(UInt64(next_uint32()));
    }

    /// Masked version of \ref next_uint64
    ENOKI_INLINE UInt64 next_uint64(const UInt64Mask &mask) {
        return UInt64(next_uint32(mask)) | sli<32>(UInt64(next_uint32(mask)));
    }

    /// Generate a single precision floating point value on the interval [0, 1)
    ENOKI_INLINE Float32 next_float32() {
        return reinterpret_array<Float32>(sri<9>(next_uint32()) | 0x3f800000u) - 1.f;
    }

    /// Masked version of \ref next_float32
    ENOKI_INLINE Float32 next_float32(const UInt64Mask &mask) {
        return reinterpret_array<Float32>(sri<9>(next_uint32(mask)) | 0x3f800000u) - 1.f;
    }

    /**
     * \brief Generate a double precision floating point value on the interval [0, 1)
     *
     * \remark Since the underlying random number generator produces 32 bit output,
     * only the first 32 mantissa bits will be filled (however, the resolution is still
     * finer than in \ref next_float(), which only uses 23 mantissa bits)
     */
    ENOKI_INLINE Float64 next_float64() {
        /* Trick from MTGP: generate an uniformly distributed
           double precision number in [1,2) and subtract 1. */
        return reinterpret_array<Float64>(sli<20>(UInt64(next_uint32())) |
                                          0x3ff0000000000000ull) - 1.0;
    }

    /**
     * \brief Generate a double precision floating point value on the interval [0, 1)
     *
     * \remark Since the underlying random number generator produces 32 bit output,
     * only the first 32 mantissa bits will be filled (however, the resolution is still
     * finer than in \ref next_float(), which only uses 23 mantissa bits)
     */
    ENOKI_INLINE Float64 next_float64(const UInt64Mask &mask) {
        /* Trick from MTGP: generate an uniformly distributed
           double precision number in [1,2) and subtract 1. */
        return reinterpret_array<Float64>(sli<20>(UInt64(next_uint32(mask))) |
                                          0x3ff0000000000000ull) - 1.0;
    }

    /// Generate a uniformly distributed number, r, where 0 <= r < bound
    UInt32 next_uint32_bounded(uint32_t bound) {
        if (is_array<UInt32>::value)
            return next_uint32_bounded(bound, true);

        // To avoid bias, we need to make the range of the RNG a multiple of
        // bound, which we do by dropping output less than a threshold.
        // A naive scheme to calculate the threshold would be to do
        //
        //     UInt32 threshold = 0x1'0000'0000ull % bound;
        //
        // but 64-bit div/mod is slower than 32-bit div/mod (especially on
        // 32-bit platforms).  In essence, we do
        //
        //     UInt32 threshold = (0x1'0000'0000ull-bound) % bound;
        //
        // because this version will calculate the same modulus, but the LHS
        // value is less than 2^32.

        const UInt32 threshold = (~bound + 1u) % bound;

        // Uniformity guarantees that this loop will terminate.  In practice, it
        // should usually terminate quickly; on average (assuming all bounds are
        // equally likely), 82.25% of the time, we can expect it to require just
        // one iteration.  In the worst case, someone passes a bound of 2^31 + 1
        // (i.e., 2147483649), which invalidates almost 50% of the range.  In
        // practice, bounds are typically small and only a tiny amount of the range
        // is eliminated.

        while (true) {
            UInt32 result = next_uint32();

            if (all(result >= threshold))
                return result % bound;
        }
    }

    /**
     * \brief Vectorized version of next_uint32_bounded
     *
     * This function keeps track of which SIMD lanes have already
     * finished and stops advancing the associated PRNGs
     */
    UInt32 next_uint32_bounded(uint32_t bound, const UInt64Mask &mask_) {
        const divisor_ext<uint32_t> div(bound);
        UInt64Mask mask(mask_);
        const UInt32 threshold = (~bound + 1u) % div;

        UInt32 result = zero<UInt32>();
        do {
            masked(result, mask) = next_uint32(mask);
            mask &= result < threshold;
        } while (any(mask));

        return result % div;
    }

    /// Generate a uniformly distributed number, r, where 0 <= r < bound
    UInt64 next_uint64_bounded(uint64_t bound) {
        if (is_array<UInt64>::value)
            return next_uint64_bounded(bound, true);

        const UInt64 threshold = (~bound + 1u) % bound;

        while (true) {
            UInt64 result = next_uint64();

            if (all(result >= threshold))
                return result % bound;
        }
    }

    /**
     * \brief Vectorized version of next_uint64_bounded
     *
     * This function keeps track of which SIMD lanes have already
     * finished and stops advancing the associated PRNGs
     */
    UInt64 next_uint64_bounded(uint64_t bound, const UInt64Mask &mask_) {
        const divisor_ext<uint64_t> div(bound);
        UInt64Mask mask(mask_);
        const UInt64 threshold = (~bound + 1ull) % div;

        UInt64 result = zero<UInt64>();
        do {
            masked(result, mask) = next_uint64(mask);
            mask &= result < threshold;
        } while (any(mask));

        return result % div;
    }

    /**
     * \brief Multi-step advance function (jump-ahead, jump-back)
     *
     * The method used here is based on Brown, "Random Number Generation
     * with Arbitrary Stride", Transactions of the American Nuclear
     * Society (Nov. 1994). The algorithm is very similar to fast
     * exponentiation.
     */
    void advance(const Int64 &delta_) {
        UInt64 cur_mult = PCG32_MULT,
               cur_plus = inc,
               acc_mult = 1ull,
               acc_plus = 0ull;

        /* Even though delta is an unsigned integer, we can pass a signed
           integer to go backwards, it just goes "the long way round". */
        UInt64 delta(delta_);

        while (delta != zero<UInt64>()) {
            auto mask = neq(delta & UInt64(1), zero<UInt64>());
            acc_mult = select(mask, acc_mult * cur_mult, acc_mult);
            acc_plus = select(mask, acc_plus * cur_mult + cur_plus, acc_plus);
            cur_plus = (cur_mult + UInt64(1)) * cur_plus;
            cur_mult *= cur_mult;
            delta = sri<1>(delta);
        }

        state = acc_mult * state + acc_plus;
    }

    /// Compute the distance between two PCG32 pseudorandom number generators
    Int64 operator-(const PCG32 &other) const {
        assert(inc == other.inc);

        UInt64 cur_mult = PCG32_MULT,
               cur_plus = inc,
               cur_state = other.state,
               the_bit = 1ull,
               distance = 0ull;

        while (state != cur_state) {
            auto mask = neq(state & the_bit, cur_state & the_bit);
            cur_state = select(mask, cur_state * cur_mult + cur_plus, cur_state);
            distance = select(mask, distance | the_bit, distance);
            assert((state & the_bit) == (cur_state & the_bit));
            the_bit = sli<1>(the_bit);
            cur_plus = (cur_mult + UInt64(1)) * cur_plus;
            cur_mult *= cur_mult;
        }

        return Int64(distance);
    }

    /**
     * \brief Draw uniformly distributed permutation and permute the
     * given container
     *
     * From: Knuth, TAoCP Vol. 2 (3rd 3d), Section 3.4.2
     */
    template <typename Iterator, typename T2 = T,
              std::enable_if_t<std::is_arithmetic<T2>::value, int> = 0>
    void shuffle(Iterator begin, Iterator end) {
        for (Iterator it = end - 1; it > begin; --it)
            std::iter_swap(it, begin + next_uint32_bounded((uint32_t) (it - begin + 1)));
    }

    /// Equality operator
    bool operator==(const PCG32 &other) const { return state == other.state && inc == other.inc; }

    /// Inequality operator
    bool operator!=(const PCG32 &other) const { return state != other.state || inc != other.inc; }

    UInt64 state;  // RNG state.  All values are possible.
    UInt64 inc;    // Controls which RNG sequence (stream) is selected. Must *always* be odd.

    ENOKI_ALIGNED_OPERATOR_NEW()
};

NAMESPACE_END(enoki)
