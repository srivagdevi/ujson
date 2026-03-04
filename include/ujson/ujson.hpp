/*
 * ujson
 * Copyright (c) 2026 h8
 * Licensed under the MIT License.
 * See LICENSE file in the project root for full license information.
 */

// ReSharper disable CppClangTidyCppcoreguidelinesAvoidConstOrRefDataMembers
// ReSharper disable CppClangTidyCppcoreguidelinesAvoidGoto
// ReSharper disable CppClangTidyBugproneMultiLevelImplicitPointerConversion
// ReSharper disable CppClangTidyBugproneBitwisePointerCast

#ifndef UJSON_HPP
#define UJSON_HPP

#pragma once
#include <algorithm>
#include <bit>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
    #include <malloc.h>
#endif

#ifdef _MSC_VER
    #define UJSON_FORCEINLINE __forceinline
#else
    #define UJSON_FORCEINLINE __attribute__((always_inline)) inline
#endif

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
    #include <immintrin.h>
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))
    #include <emmintrin.h>
#endif

namespace ujson {
    class Arena;
    struct DomContext;
} // namespace ujson

namespace ujson::concepts {
    template <class T>
    using remove_cvref_t = std::remove_cvref_t<T>;

    template <class C>
    concept text_char = std::same_as<C, char> ||
#ifdef __cpp_char8_t
                        std::same_as<C, char8_t> ||
#endif
                        std::same_as<C, char16_t> || std::same_as<C, char32_t> || std::same_as<C, wchar_t>;

    template <class T> concept text_array = std::is_array_v<remove_cvref_t<T>> && text_char<std::remove_cv_t<std::remove_extent_t<remove_cvref_t<T>>>>;

    template <class T>
    concept text_buffer = requires(const remove_cvref_t<T>& v) {
        typename remove_cvref_t<T>::value_type;
        requires text_char<typename remove_cvref_t<T>::value_type>;
        { v.data() } -> std::convertible_to<const remove_cvref_t<T>::value_type*>;
        { v.size() } -> std::convertible_to<std::size_t>;
    };

    template <class T> concept text_ptr = std::is_pointer_v<remove_cvref_t<T>> && text_char<std::remove_cv_t<std::remove_pointer_t<remove_cvref_t<T>>>>;

    template <class T> concept string_like = text_array<T> || text_buffer<T> || text_ptr<T>;
} // namespace ujson::concepts

namespace ujson::detail {
    enum class StringEscapePolicy : std::uint8_t;
    template <class Sink, StringEscapePolicy Policy>
    class JsonWriterCoreImpl;

    struct CharMask256 {
        std::uint64_t w[4] {};

        static consteval CharMask256 make_ws() {
            CharMask256 m {};
            auto set = [&](const unsigned c) {
                m.w[c >> 6] |= 1ull << (c & 63);
            };
            set(' ');
            set('\n');
            set('\r');
            set('\t');
            return m;
        }

        static consteval CharMask256 make_digit() {
            CharMask256 m {};
            for (unsigned c = '0'; c <= '9'; ++c)
                m.w[c >> 6] |= 1ull << (c & 63);
            return m;
        }

        static consteval CharMask256 make_hex() {
            CharMask256 m {};
            auto set = [&](const unsigned c) {
                m.w[c >> 6] |= 1ull << (c & 63);
            };
            for (unsigned c = '0'; c <= '9'; ++c)
                set(c);
            for (unsigned c = 'a'; c <= 'f'; ++c)
                set(c);
            for (unsigned c = 'A'; c <= 'F'; ++c)
                set(c);
            return m;
        }

        static consteval CharMask256 make_escape() {
            CharMask256 m {};

            auto set = [&](const unsigned c) {
                m.w[c >> 6] |= 1ull << (c & 63);
            };

            set('"');
            set('\\');
            set('\b');
            set('\f');
            set('\n');
            set('\r');
            set('\t');

            for (unsigned c = 0; c < 0x20; ++c)
                set(c);

            return m;
        }

        static consteval CharMask256 make_escape_follow() {
            CharMask256 m {};

            auto set = [&](const unsigned c) {
                m.w[c >> 6] |= 1ull << (c & 63);
            };

            set('"');
            set('\\');
            set('/');
            set('b');
            set('f');
            set('n');
            set('r');
            set('t');
            set('u');

            return m;
        }

        static consteval CharMask256 make_structural_end() {
            CharMask256 m {};

            auto set = [&](const unsigned c) {
                m.w[c >> 6] |= 1ull << (c & 63);
            };

            set(',');
            set(']');
            set('}');
            set(' ');
            set('\n');
            set('\r');
            set('\t');

            return m;
        }

        [[nodiscard]] static constexpr bool test(const CharMask256& m, const unsigned char c) noexcept {
            return m.w[c >> 6] >> (c & 63) & 1ull;
        }
    };

    inline constexpr CharMask256 kWsMask = CharMask256::make_ws();
    inline constexpr CharMask256 kDigitMask = CharMask256::make_digit();
    inline constexpr CharMask256 kHexMask = CharMask256::make_hex();
    inline constexpr CharMask256 kEscapeMask = CharMask256::make_escape();
    inline constexpr CharMask256 kEscapeFollowMask = CharMask256::make_escape_follow();
    inline constexpr CharMask256 kStructuralEndMask = CharMask256::make_structural_end();

    inline constexpr char kHexDigits[] = "0123456789ABCDEF";

    struct StructuralIndex {
        std::uint32_t* positions {};
        std::uint32_t count {};
        std::uint32_t capacity {};
        bool oom {};

        UJSON_FORCEINLINE bool push_back(const std::uint32_t pos) {
            assert(count + 1 <= capacity && "out of bounds");
            positions[++count - 1] = pos;
            return true;
        }
    };

    [[nodiscard]] UJSON_FORCEINLINE constexpr bool is_digit(const char c) noexcept {
        return CharMask256::test(kDigitMask, static_cast<unsigned char>(c));
    }

    [[nodiscard]] UJSON_FORCEINLINE constexpr bool is_structural_char(const char c) noexcept {
        switch (c) {
        case '{':
        case '}':
        case '[':
        case ']':
        case ',':
        case ':':
        case '"':
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t prefix_xor_mask(std::uint32_t mask) noexcept {
        mask ^= mask << 1;
        mask ^= mask << 2;
        mask ^= mask << 4;
        mask ^= mask << 8;
        mask ^= mask << 16;
        return mask;
    }

    UJSON_FORCEINLINE std::uint32_t ctz32(std::uint32_t x) noexcept {
#if defined(_MSC_VER)
        unsigned long i;
        _BitScanForward(&i, x);
        return static_cast<std::uint32_t>(i);
#else
        return static_cast<std::uint32_t>(__builtin_ctz(x));
#endif
    }

    UJSON_FORCEINLINE std::uint32_t run_len_ones_from(const std::uint32_t r) noexcept {
#if defined(_MSC_VER)
        unsigned long i;
        _BitScanForward(&i, ~r);
        return static_cast<std::uint32_t>(i);
#else
        return static_cast<std::uint32_t>(__builtin_ctz(~r));
#endif
    }

    UJSON_FORCEINLINE std::uint32_t make_mask_upto_32(const unsigned n) noexcept {
        return (n >= 32u) ? 0xFFFFFFFFu : ((1u << n) - 1u);
    }

    UJSON_FORCEINLINE std::uint32_t escaped_from_run(const std::uint32_t s, const std::uint32_t run_len, const bool start_parity, const unsigned W) noexcept {
        const unsigned span = static_cast<unsigned>(run_len) + 1u;
        const unsigned max_span = (s >= W) ? 0u : ((span > (W - s)) ? (W - s) : span);
        if (max_span == 0u)
            return 0u;

        const std::uint32_t range = make_mask_upto_32(max_span) << s;

        const std::uint32_t base_alt = (s & 1u) ? 0x55555555u : 0xAAAAAAAAu; // bit(s)=0, bit(s+1)=1
        const std::uint32_t alt = start_parity ? ~base_alt : base_alt;

        return (alt & range) & ~(1u << s);
    }

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]]
    UJSON_FORCEINLINE std::uint32_t compute_escaped_mask(const std::uint32_t backslash_mask, const bool prev_escaped, bool& out_prev_escaped) noexcept {
        constexpr std::uint32_t ODD = 0xAAAAAAAAu; // bits 1,3,5...
        const std::uint32_t bs = backslash_mask;
        const std::uint32_t carry = static_cast<std::uint32_t>(prev_escaped);

        // run starts: bit i set when bs[i]=1 and bs[i-1]=0 (for i=0 -> start if bs[0]=1)
        const std::uint32_t run_start = bs & ~(bs << 1);

        // runs that start at odd index; plus special-case: if run starts at bit0 and carry-in is set,
        // parity inside that leading run flips (virtual backslash before the chunk).
        std::uint32_t odd_start = (run_start & ODD) | (carry & (run_start & 1u));

        // Contiguity ladders: allow power-of-two expansion only across truly contiguous 1-runs.
        const std::uint32_t m2 = bs & (bs << 1);
        const std::uint32_t m4 = m2 & (m2 << 2);
        const std::uint32_t m8 = m4 & (m4 << 4);
        const std::uint32_t m16 = m8 & (m8 << 8);

        // Propagate odd_start within each contiguous run of backslashes (no data-dependent loops).
        std::uint32_t odd_run = odd_start;
        odd_run |= (odd_run << 1) & bs;
        odd_run |= (odd_run << 2) & m2;
        odd_run |= (odd_run << 4) & m4;
        odd_run |= (odd_run << 8) & m8;
        odd_run |= (odd_run << 16) & m16;

        // Backslashes that are "escaped" are at (ODD ^ odd_run) positions within bs.
        const std::uint32_t escaped_bs = bs & (ODD ^ odd_run);
        const std::uint32_t unescaped_bs = bs & ~escaped_bs;

        // Characters escaped by an unescaped backslash are the next positions.
        const std::uint32_t escaped_mask = (unescaped_bs << 1) | carry;

        // Carry-out: if the last char is an unescaped backslash, it escapes the first char of next chunk.
        out_prev_escaped = (unescaped_bs >> 31) != 0u;
        return escaped_mask;
    }

#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))

    [[nodiscard]]
    UJSON_FORCEINLINE std::uint32_t compute_escaped_mask(const std::uint32_t backslash_mask, const bool prev_escaped, bool& out_prev_escaped) noexcept {
        constexpr auto ALL = 0xFFFFu;
        constexpr auto ODD = 0xAAAAu; // bits 1,3,5... within 16 bits

        const std::uint32_t bs = backslash_mask & ALL;
        const auto carry = static_cast<std::uint32_t>(prev_escaped);

        const std::uint32_t run_start = bs & ~(bs << 1);

        const std::uint32_t odd_start = (run_start & ODD) | (carry & (run_start & 1u));

        const std::uint32_t m2 = bs & (bs << 1);
        const std::uint32_t m4 = m2 & (m2 << 2);
        const std::uint32_t m8 = m4 & (m4 << 4);

        std::uint32_t odd_run = odd_start;
        odd_run |= (odd_run << 1) & bs;
        odd_run |= (odd_run << 2) & m2;
        odd_run |= (odd_run << 4) & m4;
        odd_run |= (odd_run << 8) & m8;

        const std::uint32_t escaped_bs = bs & (ODD ^ odd_run);
        const std::uint32_t unescaped_bs = bs & ~escaped_bs;

        const std::uint32_t escaped_mask = ((unescaped_bs << 1) | carry) & ALL;

        out_prev_escaped = ((unescaped_bs >> 15) & 1u) != 0u;
        return escaped_mask;
    }

#endif

    [[nodiscard]] UJSON_FORCEINLINE StructuralIndex build_structural_index(const char* input, const std::size_t len, Arena& arena) noexcept;

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]] UJSON_FORCEINLINE const char* skip_ws_simd(const char* p, const char* e) noexcept {
        while (p + 32 <= e) {
            const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            const __m256i s = _mm256_cmpeq_epi8(x, _mm256_set1_epi8(' '));
            const __m256i n = _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\n'));
            const __m256i r = _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\r'));
            const __m256i t = _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\t'));
            const __m256i ws = _mm256_or_si256(_mm256_or_si256(s, n), _mm256_or_si256(r, t));

            const unsigned m = static_cast<unsigned>(_mm256_movemask_epi8(ws));
            if (m != 0xFFFFFFFFu) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, ~m);
                return p + idx;
    #else
                return p + __builtin_ctz(~m);
    #endif
            }
            p += 32;
        }
        return p;
    }

#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))

    [[nodiscard]] UJSON_FORCEINLINE const char* skip_ws_simd(const char* p, const char* e) noexcept {
        while (p + 16 <= e) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i s = _mm_cmpeq_epi8(x, _mm_set1_epi8(' '));
            const __m128i n = _mm_cmpeq_epi8(x, _mm_set1_epi8('\n'));
            const __m128i r = _mm_cmpeq_epi8(x, _mm_set1_epi8('\r'));
            const __m128i t = _mm_cmpeq_epi8(x, _mm_set1_epi8('\t'));
            const __m128i ws = _mm_or_si128(_mm_or_si128(s, n), _mm_or_si128(r, t));

            const auto m = static_cast<unsigned>(_mm_movemask_epi8(ws));
            if (m != 0xFFFFu) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, ~m);
                return p + idx;
    #else
                return p + __builtin_ctz(~m);
    #endif
            }
            p += 16;
        }

        return p;
    }

#endif

    [[nodiscard]] UJSON_FORCEINLINE constexpr bool is_ws_u8(const unsigned char c) noexcept {
        return CharMask256::test(kWsMask, c);
    }

    [[nodiscard]] UJSON_FORCEINLINE const char* skip_ws(const char* p, const char* e) noexcept {
        p = skip_ws_simd(p, e);

        while (p < e && is_ws_u8(static_cast<unsigned char>(*p)))
            ++p;

        return p;
    }

    [[nodiscard]] UJSON_FORCEINLINE bool hex4_to_u16(const char* p, std::uint16_t& out) noexcept {
        out = 0;
        for (auto i = 0; i < 4; ++i) {
            const auto x = static_cast<std::uint8_t>(p[i]);

            const auto d = static_cast<std::uint8_t>(x - '0');
            const auto l = static_cast<std::uint8_t>((x | 0x20u) - 'a');

            const std::uint16_t v = d <= 9 ? d : l <= 5 ? static_cast<std::uint16_t>(l + 10) : 0xFFFFu;

            if (v == 0xFFFFu)
                return false;

            out = static_cast<std::uint16_t>(out << 4 | v);
        }
        return true;
    }

    [[nodiscard]] UJSON_FORCEINLINE constexpr bool is_structural_end(const char c) noexcept {
        return CharMask256::test(kStructuralEndMask, static_cast<unsigned char>(c));
    }

    [[nodiscard]] UJSON_FORCEINLINE std::size_t utf8_encode(char* out, const std::uint32_t cp) noexcept {
        if (cp > 0x10FFFFu)
            return 0;
        if (cp >= 0xD800u && cp <= 0xDFFFu)
            return 0;

        if (cp <= 0x7F) {
            out[0] = static_cast<char>(cp);
            return 1;
        }
        if (cp <= 0x7FF) {
            out[0] = static_cast<char>(0xC0 | cp >> 6);
            out[1] = static_cast<char>(0x80 | (cp & 0x3F));
            return 2;
        }
        if (cp <= 0xFFFF) {
            out[0] = static_cast<char>(0xE0 | cp >> 12);
            out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out[2] = static_cast<char>(0x80 | (cp & 0x3F));
            return 3;
        }
        out[0] = static_cast<char>(0xF0 | cp >> 18);
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        return 4;
    }

    [[nodiscard]] UJSON_FORCEINLINE constexpr bool has_zero_byte_u64(const std::uint64_t v) noexcept {
        return (v - 0x0101010101010101ull & ~v & 0x8080808080808080ull) != 0;
    }

    [[nodiscard]] UJSON_FORCEINLINE const char* find_escape_in_string(const char* p, const char* e) noexcept {
        const char* it = p;

        while (it + 8 <= e) {
            constexpr auto kQuote = 0x2222222222222222ull;
            constexpr auto kBSlash = 0x5C5C5C5C5C5C5C5Cull;
            constexpr auto kE0 = 0xE0E0E0E0E0E0E0E0ull;

            std::uint64_t x;
            std::memcpy(&x, it, 8);

            const std::uint64_t xq = x ^ kQuote;
            const std::uint64_t xb = x ^ kBSlash;

            const std::uint64_t ctrl = x & kE0;
            if (has_zero_byte_u64(xq) || has_zero_byte_u64(xb) || has_zero_byte_u64(ctrl)) {
                for (auto i = 0; i < 8; ++i) {
                    const auto c = static_cast<unsigned char>(it[i]);
                    if (CharMask256::test(kEscapeMask, c))
                        return it + i;
                }
            }
            it += 8;
        }

        while (it < e) {
            const auto c = static_cast<unsigned char>(*it);
            if (CharMask256::test(kEscapeMask, c))
                return it;
            ++it;
        }

        return e;
    }

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]] UJSON_FORCEINLINE const char* find_string_special_simd(const char* it, const char* e) noexcept {
        const __m256i v_quote = _mm256_set1_epi8('"');
        const __m256i v_bsl = _mm256_set1_epi8('\\');
        const __m256i v_e0 = _mm256_set1_epi8(static_cast<char>(0xE0));
        const __m256i v_zero = _mm256_setzero_si256();

        while (it + 32 <= e) {
            const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(it));

            const __m256i is_quote = _mm256_cmpeq_epi8(x, v_quote);
            const __m256i is_bsl = _mm256_cmpeq_epi8(x, v_bsl);

            // control (<0x20): (x & 0xE0) == 0
            const __m256i ctrl = _mm256_cmpeq_epi8(_mm256_and_si256(x, v_e0), v_zero);

            const __m256i special = _mm256_or_si256(_mm256_or_si256(is_quote, is_bsl), ctrl);

            if (auto mask = static_cast<unsigned>(_mm256_movemask_epi8(special))) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                return it + idx;
    #else
                return it + __builtin_ctz(mask);
    #endif
            }

            it += 32;
        }

        return it;
    }

#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)

    [[nodiscard]] UJSON_FORCEINLINE const char* find_string_special_simd(const char* it, const char* e) noexcept {
        const __m128i v_quote = _mm_set1_epi8('"');
        const __m128i v_bsl = _mm_set1_epi8('\\');
        const __m128i v_e0 = _mm_set1_epi8(static_cast<char>(0xE0));
        const __m128i v_zero = _mm_setzero_si128();

        while (it + 16 <= e) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(it));

            const __m128i is_quote = _mm_cmpeq_epi8(x, v_quote);
            const __m128i is_bsl = _mm_cmpeq_epi8(x, v_bsl);

            // control (<0x20): (x & 0xE0) == 0
            const __m128i ctrl = _mm_cmpeq_epi8(_mm_and_si128(x, v_e0), v_zero);

            const __m128i special = _mm_or_si128(_mm_or_si128(is_quote, is_bsl), ctrl);
            auto mask = static_cast<unsigned>(_mm_movemask_epi8(special));

            if (mask) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                return it + idx;
    #else
                return it + __builtin_ctz(mask);
    #endif
            }

            it += 16;
        }

        return it;
    }

#endif

    [[nodiscard]] UJSON_FORCEINLINE const char* find_string_special(const char* it, const char* e) noexcept {
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__)) || defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        const char* p = find_string_special_simd(it, e);
        // tail
        it = p;
#endif
        while (it < e) {
            const auto c = static_cast<unsigned char>(*it);
            if (c == '"' || c == '\\' || c < 0x20)
                return it;
            ++it;
        }
        return e;
    }

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]] UJSON_FORCEINLINE const char* find_closing_quote_unescaped_simd(const char* it, const char* e) noexcept {
        const __m256i v_quote = _mm256_set1_epi8('"');
        const __m256i v_e0 = _mm256_set1_epi8(static_cast<char>(0xE0));
        const __m256i v_zero = _mm256_setzero_si256();

        while (it + 32 <= e) {
            const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(it));

            const __m256i is_quote = _mm256_cmpeq_epi8(x, v_quote);
            const __m256i ctrl = _mm256_cmpeq_epi8(_mm256_and_si256(x, v_e0), v_zero);

            const __m256i special = _mm256_or_si256(is_quote, ctrl);
            unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(special));

            while (mask) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
    #else
                unsigned idx = static_cast<unsigned>(__builtin_ctz(mask));
    #endif
                const char* q = it + idx;
                const unsigned char c = static_cast<unsigned char>(*q);

                if (c < 0x20)
                    return q;

                // c == '"'
                // check if escaped
                const char* b = q;
                unsigned backslashes = 0;
                while (b > it && *(--b) == '\\')
                    ++backslashes;

                if ((backslashes & 1u) == 0u)
                    return q; // not escaped

                mask &= mask - 1; // clear lowest bit
            }

            it += 32;
        }

        return it;
    }

#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)

    [[nodiscard]] UJSON_FORCEINLINE const char* find_closing_quote_unescaped_simd(const char* it, const char* e) noexcept {
        const __m128i v_quote = _mm_set1_epi8('"');
        const __m128i v_e0 = _mm_set1_epi8(static_cast<char>(0xE0));
        const __m128i v_zero = _mm_setzero_si128();

        while (it + 16 <= e) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(it));

            const __m128i is_quote = _mm_cmpeq_epi8(x, v_quote);
            const __m128i ctrl = _mm_cmpeq_epi8(_mm_and_si128(x, v_e0), v_zero);

            const __m128i special = _mm_or_si128(is_quote, ctrl);
            auto mask = static_cast<unsigned>(_mm_movemask_epi8(special));

            while (mask) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
    #else
                unsigned idx = static_cast<unsigned>(__builtin_ctz(mask));
    #endif
                const char* q = it + idx;

                if (const auto c = static_cast<unsigned char>(*q); c < 0x20)
                    return q;

                const char* b = q;
                unsigned backslashes = 0;

                while (b > it) {
                    --b;
                    if (*b != '\\')
                        break;
                    ++backslashes;
                }

                if ((backslashes & 1u) == 0u)
                    return q;

                mask &= mask - 1;
            }

            it += 16;
        }

        return it;
    }

#endif

    [[nodiscard]] UJSON_FORCEINLINE const char* find_closing_quote_unescaped(const char* it, const char* e) noexcept {
#if defined(__AVX2__) || defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        const char* p = find_closing_quote_unescaped_simd(it, e);
        it = p;
#endif

        while (it < e) {
            const char* q = find_string_special(it, e);
            if (q >= e)
                return e;

            const auto c = static_cast<unsigned char>(*q);
            if (c < 0x20)
                return q;

            if (c == '"') {
                const char* b = q;
                unsigned backslashes = 0;

                while (b > it) {
                    --b;
                    if (*b != '\\')
                        break;
                    ++backslashes;
                }

                if ((backslashes & 1u) == 0u)
                    return q;
            }

            // skip escaped char
            it = q + 2;
        }

        return e;
    }

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]] UJSON_FORCEINLINE const char* scan_digits_avx2(const char* p, const char* e) noexcept {
        const __m256i v0 = _mm256_set1_epi8('0');
        const __m256i v9 = _mm256_set1_epi8('9');

        while (p + 32 <= e) {
            const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            const __m256i ge0 = _mm256_cmpgt_epi8(x, _mm256_sub_epi8(v0, _mm256_set1_epi8(1)));
            const __m256i le9 = _mm256_cmpgt_epi8(_mm256_add_epi8(v9, _mm256_set1_epi8(1)), x);
            const __m256i isd = _mm256_and_si256(ge0, le9);

            const unsigned m = static_cast<unsigned>(_mm256_movemask_epi8(isd));
            if (m != 0xFFFFFFFFu) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, ~m);
                return p + idx;
    #else
                return p + __builtin_ctz(~m);
    #endif
            }
            p += 32;
        }
        return p;
    }

#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))

    [[nodiscard]] UJSON_FORCEINLINE const char* scan_digits_sse2(const char* p, const char* e) noexcept {
        const __m128i v0 = _mm_set1_epi8('0');
        const __m128i v9 = _mm_set1_epi8('9');

        while (p + 16 <= e) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i ge0 = _mm_cmpgt_epi8(x, _mm_sub_epi8(v0, _mm_set1_epi8(1)));
            const __m128i le9 = _mm_cmpgt_epi8(_mm_add_epi8(v9, _mm_set1_epi8(1)), x);
            const __m128i isd = _mm_and_si128(ge0, le9);

            if (const auto m = static_cast<unsigned>(_mm_movemask_epi8(isd)); m != 0xFFFFu) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, ~m);
                return p + idx;
    #else
                return p + __builtin_ctz(~m);
    #endif
            }
            p += 16;
        }
        return p;
    }

#endif

    [[nodiscard]] UJSON_FORCEINLINE const char* scan_digits(const char* p, const char* e) noexcept {
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
        p = scan_digits_avx2(p, e);
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))
        p = scan_digits_sse2(p, e);
#endif
        while (p < e) {
            if (const auto d = static_cast<unsigned>(*p - '0'); d > 9)
                break;
            ++p;
        }
        return p;
    }

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]]
    UJSON_FORCEINLINE const char* scan_ascii_avx2(const char* p, const char* e) noexcept {
        const __m256i v_quote = _mm256_set1_epi8('"');
        const __m256i v_bsl = _mm256_set1_epi8('\\');
        const __m256i v_1f = _mm256_set1_epi8(0x1F);

        while (p + 32 <= e) {
            const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
            const __m256i ctrl = _mm256_cmpgt_epi8(v_1f, x);
            const __m256i is_quote = _mm256_cmpeq_epi8(x, v_quote);
            const __m256i is_bsl = _mm256_cmpeq_epi8(x, v_bsl);
            const __m256i bad = _mm256_or_si256(_mm256_or_si256(ctrl, is_quote), is_bsl);

            if (const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(bad)); mask) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                return p + idx;
    #else
                return p + __builtin_ctz(mask);
    #endif
            }

            p += 32;
        }

        return p;
    }

#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))

    [[nodiscard]]
    UJSON_FORCEINLINE const char* scan_ascii_sse2(const char* p, const char* e) noexcept {
        const __m128i v_quote = _mm_set1_epi8('"');
        const __m128i v_bsl = _mm_set1_epi8('\\');
        const __m128i v_1f = _mm_set1_epi8(0x1F);

        while (p + 16 <= e) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i ctrl = _mm_cmpgt_epi8(v_1f, x);
            const __m128i is_quote = _mm_cmpeq_epi8(x, v_quote);
            const __m128i is_bsl = _mm_cmpeq_epi8(x, v_bsl);
            const __m128i bad = _mm_or_si128(_mm_or_si128(ctrl, is_quote), is_bsl);

            if (const auto mask = static_cast<unsigned>(_mm_movemask_epi8(bad))) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                return p + idx;
    #else
                return p + __builtin_ctz(mask);
    #endif
            }

            p += 16;
        }

        return p;
    }

#endif

    [[nodiscard]]
    UJSON_FORCEINLINE const char* scan_ascii(const char* p, const char* e) noexcept {
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
        p = scan_ascii_avx2(p, e);
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))
        p = scan_ascii_sse2(p, e);
#endif

        while (p < e) {
            if (const auto c = static_cast<unsigned char>(*p); c == '"' || c == '\\' || c < 0x20u)
                break;
            ++p;
        }

        return p;
    }

    [[nodiscard]] UJSON_FORCEINLINE constexpr double pow10_u32(const unsigned k) noexcept {
        // up to 22 is enough for fast fraction; bigger handled via std::pow fallback or from_chars path
        constexpr double t[] = {1.0,
                                10.0,
                                100.0,
                                1'000.0,
                                10'000.0,
                                100'000.0,
                                1'000'000.0,
                                10'000'000.0,
                                100'000'000.0,
                                1'000'000'000.0,
                                10'000'000'000.0,
                                100'000'000'000.0,
                                1'000'000'000'000.0,
                                10'000'000'000'000.0,
                                100'000'000'000'000.0,
                                1'000'000'000'000'000.0,
                                10'000'000'000'000'000.0,
                                100'000'000'000'000'000.0,
                                1e18,
                                1e19,
                                1e20,
                                1e21,
                                1e22};
        return (k < std::size(t)) ? t[k] : 0.0;
    }

    [[nodiscard]] UJSON_FORCEINLINE bool parse_double_fast(const char* s, const char* e, double& out, const char*& out_end) noexcept {
        const char* p = s;

        auto neg = false;
        if (p < e && *p == '-') {
            neg = true;
            ++p;
        }
        if (p >= e)
            return false;

        // integer digits
        const char* int_beg = p;
        p = scan_digits(p, e);
        const auto int_len = static_cast<std::size_t>(p - int_beg);
        if (int_len == 0)
            return false;

        // leading zero rules are checked outside if you want; here just parse
        std::uint64_t int_val = 0;
        // limit to 19 digits for exact uint64 accumulation
        if (int_len > 19)
            return false;
        for (const char* it = int_beg; it < int_beg + int_len; ++it)
            int_val = int_val * 10u + static_cast<unsigned>(*it - '0');

        std::uint64_t frac_val = 0;
        unsigned frac_len = 0;

        if (p < e && *p == '.') {
            ++p;
            const char* frac_beg = p;
            p = scan_digits(p, e);
            const auto fl = static_cast<std::size_t>(p - frac_beg);
            if (fl == 0)
                return false;

            // keep up to 18 digits in fraction; rest -> give up to from_chars
            if (fl > 18)
                return false;

            frac_len = static_cast<unsigned>(fl);
            for (const char* it = frac_beg; it < frac_beg + fl; ++it)
                frac_val = frac_val * 10u + static_cast<unsigned>(*it - '0');
        }

        auto exp10 = 0;
        if (p < e && (*p == 'e' || *p == 'E')) {
            ++p;
            auto exp_neg = false;
            if (p < e && (*p == '+' || *p == '-')) {
                exp_neg = (*p == '-');
                ++p;
            }
            const char* exp_beg = p;
            p = scan_digits(p, e);
            const auto el = static_cast<std::size_t>(p - exp_beg);
            if (el == 0 || el > 3)
                return false; // fast-path cap
            auto v = 0;
            for (const char* it = exp_beg; it < exp_beg + el; ++it)
                v = v * 10 + static_cast<int>(*it - '0');
            exp10 = exp_neg ? -v : v;

            if (exp10 < -308 || exp10 > 308)
                return false;
        }

        auto x = static_cast<double>(int_val);
        if (frac_len) {
            const double d = pow10_u32(frac_len);
            if (d == 0.0)
                return false;
            x += static_cast<double>(frac_val) / d;
        }

        if (exp10) {
            // keep it cheap; if you want, replace by table for +/-308 later
            x *= std::pow(10.0, static_cast<double>(exp10));
            if (!std::isfinite(x))
                return false;
        }

        out = neg ? -x : x;
        out_end = p;
        return true;
    }

    [[nodiscard]] UJSON_FORCEINLINE static bool is_cont(const unsigned char c) noexcept {
        return (c & 0xC0u) == 0x80u;
    }

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]]
    UJSON_FORCEINLINE const char* skip_ascii_avx2(const char* p, const char* e) noexcept {
        const __m256i zero = _mm256_setzero_si256();

        while (p + 32 <= e) {
            const __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));

            // signed compare: negative = high bit set
            const __m256i high = _mm256_cmpgt_epi8(zero, x);

            if (const unsigned mask = static_cast<unsigned>(_mm256_movemask_epi8(high)); mask) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                return p + idx;
    #else
                return p + __builtin_ctz(mask);
    #endif
            }

            p += 32;
        }

        return p;
    }

#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))

    [[nodiscard]]
    UJSON_FORCEINLINE const char* skip_ascii_sse2(const char* p, const char* e) noexcept {
        const __m128i zero = _mm_setzero_si128();

        while (p + 16 <= e) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
            const __m128i high = _mm_cmpgt_epi8(zero, x);

            if (const auto mask = static_cast<unsigned>(_mm_movemask_epi8(high))) {
    #if defined(_MSC_VER)
                unsigned long idx;
                _BitScanForward(&idx, mask);
                return p + idx;
    #else
                return p + __builtin_ctz(mask);
    #endif
            }

            p += 16;
        }

        return p;
    }

#endif

    [[nodiscard]]
    UJSON_FORCEINLINE const char* skip_ascii(const char* p, const char* e) noexcept {
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
        p = skip_ascii_avx2(p, e);
#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))
        p = skip_ascii_sse2(p, e);
#endif

        while (p < e) {
            if (static_cast<unsigned char>(*p) & 0x80u)
                break;
            ++p;
        }

        return p;
    }

    [[nodiscard]]
    UJSON_FORCEINLINE static bool validate_utf8_segment(const char* p, const char* e) noexcept {
        while (p < e) {

            // SIMD ASCII skip
            p = skip_ascii(p, e);

            if (p >= e)
                return true;

            const auto c = static_cast<unsigned char>(*p++);

            if (c < 0x80u)
                continue;

            // 2-byte
            if ((c >> 5) == 0x6) {
                if (p >= e)
                    return false;

                const auto c1 = static_cast<unsigned char>(*p++);
                if (!is_cont(c1))
                    return false;

                if (((c & 0x1Fu) << 6 | (c1 & 0x3Fu)) < 0x80u)
                    return false;

                continue;
            }

            // 3-byte
            if ((c >> 4) == 0xE) {
                if (e - p < 2)
                    return false;

                const auto c1 = static_cast<unsigned char>(*p++);
                const auto c2 = static_cast<unsigned char>(*p++);

                if (!is_cont(c1) || !is_cont(c2))
                    return false;

                const std::uint32_t cp = (c & 0x0Fu) << 12 | (c1 & 0x3Fu) << 6 | (c2 & 0x3Fu);

                if (cp < 0x800u)
                    return false;

                if (cp >= 0xD800u && cp <= 0xDFFFu)
                    return false;

                continue;
            }

            // 4-byte
            if ((c >> 3) == 0x1E) {
                if (e - p < 3)
                    return false;

                const auto c1 = static_cast<unsigned char>(*p++);
                const auto c2 = static_cast<unsigned char>(*p++);
                const auto c3 = static_cast<unsigned char>(*p++);

                if (!is_cont(c1) || !is_cont(c2) || !is_cont(c3))
                    return false;

                const std::uint32_t cp = (c & 0x07u) << 18 | (c1 & 0x3Fu) << 12 | (c2 & 0x3Fu) << 6 | (c3 & 0x3Fu);

                if (cp < 0x10000u)
                    return false;

                if (cp > 0x10FFFFu)
                    return false;

                continue;
            }

            return false;
        }

        return true;
    }

} // namespace ujson::detail

namespace ujson::detail {
    UJSON_FORCEINLINE bool append_cp(std::string& out, const std::uint32_t cp) {
        char buf[4];
        const std::size_t n = utf8_encode(buf, cp);
        if (!n)
            return false;
        out.append(buf, n);
        return true;
    }

    [[nodiscard]] UJSON_FORCEINLINE bool utf8_next_cp(const char* p, const char* e, const char*& out_p, std::uint32_t& out_cp) noexcept {
        if (p >= e)
            return false;

        const auto b0 = static_cast<unsigned char>(*p++);

        if (b0 < 0x80u) {
            out_cp = b0;
            out_p = p;
            return true;
        }

        std::uint32_t cp;
        if ((b0 >> 5) == 0x6) { // 2-byte
            if (p >= e)
                return false;
            const auto b1 = static_cast<unsigned char>(*p++);
            if (!is_cont(b1))
                return false;
            cp = (b0 & 0x1Fu) << 6 | (b1 & 0x3Fu);
            if (cp < 0x80u)
                return false;
        } else if ((b0 >> 4) == 0xE) { // 3-byte
            if (e - p < 2)
                return false;
            const auto b1 = static_cast<unsigned char>(*p++);
            const auto b2 = static_cast<unsigned char>(*p++);
            if (!is_cont(b1) || !is_cont(b2))
                return false;
            cp = (b0 & 0x0Fu) << 12 | (b1 & 0x3Fu) << 6 | (b2 & 0x3Fu);
            if (cp < 0x800u)
                return false;
            if (cp >= 0xD800u && cp <= 0xDFFFu)
                return false;
        } else if ((b0 >> 3) == 0x1E) { // 4-byte
            if (e - p < 3)
                return false;
            const auto b1 = static_cast<unsigned char>(*p++);
            const auto b2 = static_cast<unsigned char>(*p++);
            const auto b3 = static_cast<unsigned char>(*p++);
            if (!is_cont(b1) || !is_cont(b2) || !is_cont(b3))
                return false;
            cp = (b0 & 0x07u) << 18 | (b1 & 0x3Fu) << 12 | (b2 & 0x3Fu) << 6 | (b3 & 0x3Fu);
            if (cp < 0x10000u || cp > 0x10FFFFu)
                return false;
        } else {
            return false;
        }

        out_cp = cp;
        out_p = p;
        return true;
    }

    UJSON_FORCEINLINE void append_hex4(char* dst, const std::uint16_t x) noexcept {
        dst[0] = kHexDigits[(x >> 12) & 0xF];
        dst[1] = kHexDigits[(x >> 8) & 0xF];
        dst[2] = kHexDigits[(x >> 4) & 0xF];
        dst[3] = kHexDigits[(x >> 0) & 0xF];
    }

    UJSON_FORCEINLINE char* append_u16_escape(char* dst, const std::uint16_t u) noexcept {
        *dst++ = '\\';
        *dst++ = 'u';
        append_hex4(dst, u);
        return dst + 4;
    }
    UJSON_FORCEINLINE char* append_ascii_escaped(char* dst, const unsigned char c) noexcept {
        switch (c) {
        case '\"':
            *dst++ = '\\';
            *dst++ = '\"';
            return dst;
        case '\\':
            *dst++ = '\\';
            *dst++ = '\\';
            return dst;
        case '\b':
            *dst++ = '\\';
            *dst++ = 'b';
            return dst;
        case '\f':
            *dst++ = '\\';
            *dst++ = 'f';
            return dst;
        case '\n':
            *dst++ = '\\';
            *dst++ = 'n';
            return dst;
        case '\r':
            *dst++ = '\\';
            *dst++ = 'r';
            return dst;
        case '\t':
            *dst++ = '\\';
            *dst++ = 't';
            return dst;
        default:
            break;
        }

        if (c < 0x20u) {
            return append_u16_escape(dst, c);
        }

        *dst++ = static_cast<char>(c);
        return dst;
    }

    [[nodiscard]] UJSON_FORCEINLINE bool json_escape_utf8_control_chars(const char* p, const char* e, char* out, std::size_t& out_len) noexcept {
        char* dst = out;

        while (p < e) {
            if (const char* q = scan_ascii(p, e); q > p) {
                const auto n = static_cast<std::size_t>(q - p);
                std::memcpy(dst, p, n);
                dst += n;
                p = q;
                if (p >= e)
                    break;
            }

            const auto c = static_cast<unsigned char>(*p++);
            if (c < 0x80u) {
                dst = append_ascii_escaped(dst, c);
            } else {
                *dst++ = static_cast<char>(c);
            }
        }

        out_len = static_cast<std::size_t>(dst - out);
        return true;
    }

    [[nodiscard]] UJSON_FORCEINLINE bool json_escape_utf8_to_ascii(const char* p, const char* e, char* out, std::size_t& out_len) noexcept {
        char* dst = out;

        while (p < e) {
            std::uint32_t cp = 0;
            const char* np = nullptr;
            if (!utf8_next_cp(p, e, np, cp))
                return false;
            p = np;

            if (cp < 0x80u) {
                dst = append_ascii_escaped(dst, static_cast<unsigned char>(cp));
                continue;
            }

            if (cp <= 0xFFFFu) {
                dst = append_u16_escape(dst, static_cast<std::uint16_t>(cp));
            } else {
                cp -= 0x10000u;
                const auto hi = static_cast<std::uint16_t>(0xD800u + (cp >> 10));
                const auto lo = static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu));
                dst = append_u16_escape(dst, hi);
                dst = append_u16_escape(dst, lo);
            }
        }

        out_len = static_cast<std::size_t>(dst - out);
        return true;
    }

} // namespace ujson::detail

namespace ujson::traits {
    template <class CharT>
    struct utf8_encoder;

    template <>
    struct utf8_encoder<char> {
        static bool append(std::string& out, const std::string_view in) {
            out.append(in.data(), in.size());
            return true;
        }
    };

#ifdef __cpp_char8_t
    template <>
    struct utf8_encoder<char8_t> {
        UJSON_FORCEINLINE static bool append(std::string& out, const std::basic_string_view<char8_t> in) {
            out.append(reinterpret_cast<const char*>(in.data()), in.size());
            return true;
        }
    };
#endif

    template <>
    struct utf8_encoder<char32_t> {
        UJSON_FORCEINLINE static bool append(std::string& out, const std::basic_string_view<char32_t> in) {
            out.reserve(out.size() + in.size() * 4);
            for (const char32_t ch : in)
                if (!detail::append_cp(out, ch))
                    return false;
            return true;
        }
    };

    template <>
    struct utf8_encoder<char16_t> {
        UJSON_FORCEINLINE static bool append(std::string& out, const std::basic_string_view<char16_t> in) {
            out.reserve(out.size() + in.size() * 4);

            for (std::size_t i = 0; i < in.size(); ++i) {
                const std::uint32_t w1 = in[i];

                if (w1 >= 0xD800u && w1 <= 0xDBFFu) { // lead surrogate
                    if (i + 1 >= in.size())
                        return false;
                    const std::uint32_t w2 = in[i + 1];
                    if (w2 < 0xDC00u || w2 > 0xDFFFu)
                        return false;

                    const std::uint32_t cp = 0x10000u + (((w1 - 0xD800u) << 10) | (w2 - 0xDC00u));
                    ++i;
                    if (!detail::append_cp(out, cp))
                        return false;
                    continue;
                }

                if (w1 >= 0xDC00u && w1 <= 0xDFFFu) // stray trail surrogate
                    return false;

                if (!detail::append_cp(out, w1))
                    return false;
            }
            return true;
        }
    };

    template <>
    struct utf8_encoder<wchar_t> {
        UJSON_FORCEINLINE static bool append(std::string& out, const std::basic_string_view<wchar_t> in) {
            if constexpr (sizeof(wchar_t) == 2) {
                const auto v = std::basic_string_view {reinterpret_cast<const char16_t*>(in.data()), in.size()};
                return utf8_encoder<char16_t>::append(out, v);
            } else {
                auto v = std::basic_string_view {reinterpret_cast<const char32_t*>(in.data()), in.size()};
                return utf8_encoder<char32_t>::append(out, v);
            }
        }
    };

    template <class CharT>
    struct utf8_encoded {
        std::string storage;

        [[nodiscard]] UJSON_FORCEINLINE std::string_view view() const noexcept {
            return storage;
        }
    };

    template <>
    struct utf8_encoded<char> {
        std::string_view storage;

        [[nodiscard]] UJSON_FORCEINLINE std::string_view view() const noexcept {
            return storage;
        }
    };

#ifdef __cpp_char8_t
    template <>
    struct utf8_encoded<char8_t> {
        std::string_view storage;

        [[nodiscard]] UJSON_FORCEINLINE std::string_view view() const noexcept {
            return storage;
        }
    };
#endif

    template <concepts::text_char CharT>
    struct text_view {
        using value_type = CharT;
        using view_type = std::basic_string_view<CharT>;

        constexpr text_view() noexcept = default;

        constexpr explicit text_view(view_type v) noexcept: p_(v.data()), n_(v.size()) { }

        constexpr explicit text_view(const CharT* s) noexcept: p_(s), n_(cstrlen(s)) { }

        template <std::size_t N>
        constexpr explicit text_view(const CharT (&s)[N]) noexcept: p_(s), n_(trim_nul(s, N)) { }

        template <concepts::text_buffer Buf>
            requires std::same_as<typename std::remove_cvref_t<Buf>::value_type, CharT>
        constexpr explicit text_view(Buf&& b) noexcept: p_(static_cast<const CharT*>(std::as_const(b).data())), n_(static_cast<std::size_t>(std::as_const(b).size())) { }

        [[nodiscard]] constexpr const CharT* data() const noexcept {
            return p_;
        }
        [[nodiscard]] constexpr std::size_t size() const noexcept {
            return n_;
        }
        [[nodiscard]] constexpr view_type view() const noexcept {
            return view_type {p_, n_};
        }

    private:
        static constexpr std::size_t cstrlen(const CharT* s) noexcept {
            if (!s)
                return 0;
            std::size_t n = 0;
            while (s[n] != CharT {})
                ++n;
            return n;
        }

        template <std::size_t N>
        static constexpr std::size_t trim_nul(const CharT (&s)[N], std::size_t n) noexcept {
            if (n && s[n - 1] == CharT {})
                --n;
            return n;
        }

        const CharT* p_ = nullptr;
        std::size_t n_ = 0;
    };

    template <concepts::text_char CharT>
    text_view(std::basic_string_view<CharT>) -> text_view<CharT>;

    template <concepts::text_char CharT>
    text_view(const CharT*) -> text_view<CharT>;

    template <concepts::text_char CharT, std::size_t N>
    text_view(const CharT (&)[N]) -> text_view<CharT>;

    template <concepts::text_buffer Buf>
    text_view(Buf&&) -> text_view<typename std::remove_cvref_t<Buf>::value_type>;

} // namespace ujson::traits

namespace ujson::detail {
    template <class CharT>
    UJSON_FORCEINLINE auto to_utf8(std::basic_string_view<CharT> in) {
        if constexpr (std::same_as<CharT, char>) {
            return traits::utf8_encoded<char> {std::string_view {in.data(), in.size()}};
#ifdef __cpp_char8_t
        } else if constexpr (std::same_as<CharT, char8_t>) {
            return traits::utf8_encoded<char8_t> {std::string_view {reinterpret_cast<const char*>(in.data()), in.size()}};
#endif
        } else {
            traits::utf8_encoded<CharT> r;
            r.storage.reserve(in.size() * 4);
            if (!traits::utf8_encoder<CharT>::append(r.storage, in))
                r.storage.clear();
            return r;
        }
    }

    template <class T>
    constexpr auto as_text(T&& v) {
        using U = std::remove_cvref_t<T>;

        if constexpr (concepts::text_array<U>) {
            using C = std::remove_cv_t<std::remove_extent_t<U>>;
            static_assert(concepts::text_char<C>);
            return traits::text_view<C> {v};
        } else if constexpr (concepts::text_buffer<U>) {
            using C = typename U::value_type;
            static_assert(concepts::text_char<C>);
            return traits::text_view<C> {std::forward<T>(v)};
        } else if constexpr (concepts::text_ptr<U>) {
            using C = std::remove_cv_t<std::remove_pointer_t<U>>;
            static_assert(concepts::text_char<C>);
            return traits::text_view<C> {v};
        } else {
            static_assert(!sizeof(T), "as_text: unsupported type");
        }
    }
} // namespace ujson::detail

namespace ujson::detail {
    enum class key_format : std::uint8_t {
        None,
        SnakeCase,
        CamelCase,
        PascalCase
    };

    [[nodiscard]] UJSON_FORCEINLINE constexpr char to_lower_ascii(const char c) noexcept {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    [[nodiscard]] UJSON_FORCEINLINE constexpr char to_upper_ascii(const char c) noexcept {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }

    [[nodiscard]] UJSON_FORCEINLINE constexpr bool is_ascii_char(const unsigned char c) noexcept {
        return (c & 0x80u) == 0u;
    }

    [[nodiscard]] UJSON_FORCEINLINE constexpr bool is_ascii_sep(const char c) noexcept {
        return c == '_' || c == '-' || c == ' ';
    }

    struct KeyScratch;

    [[nodiscard]] UJSON_FORCEINLINE bool normalize_ascii(const std::string_view key, const key_format format, Arena* arena, KeyScratch& scratch, std::string_view& out) noexcept;

    inline constexpr auto snake_case = key_format::SnakeCase;
    inline constexpr auto camel_case = key_format::CamelCase;
    inline constexpr auto pascal_case = key_format::PascalCase;
} // namespace ujson::detail

namespace ujson {
    struct DomContext {
        Arena* arena {};
        detail::key_format fmt {detail::key_format::None};
    };

    [[nodiscard]] UJSON_FORCEINLINE detail::key_format init_key_format(Arena& a, detail::key_format requested) noexcept;

    struct TapeStorage;
    [[nodiscard]] UJSON_FORCEINLINE TapeStorage* tape_storage(const Arena* arena) noexcept;
    UJSON_FORCEINLINE void mark_normalized_keys(Arena* arena) noexcept;

    constexpr auto kDefaultBlockSize = 64ull * 1024ull;
    constexpr auto kDefaultMaxDepth = 512;

    template <uint64_t BlockSize = kDefaultBlockSize>
    struct NewAllocator {
        static constexpr auto kBlockSize = BlockSize;

        void* allocate(std::size_t sz, std::size_t al) {
            if (al == 0 || !std::has_single_bit(al))
                return nullptr;

#if defined(_MSC_VER)
            return _aligned_malloc(sz, al);
#else
            void* p {};
            if (posix_memalign(&p, al, sz) != 0)
                return nullptr;
            return p;
#endif
        }

        // ReSharper disable once CppMemberFunctionMayBeStatic
        void deallocate(void* p, std::size_t, std::size_t) noexcept {
#if defined(_MSC_VER)
            _aligned_free(p);
#else
            std::free(p);
#endif
        }
    };

    template <std::size_t N, uint64_t BlockSize = kDefaultBlockSize>
    struct StaticBufferAllocator {
        static constexpr auto kBlockSize = BlockSize;
        static constexpr auto kMaxSize = N;

        alignas(std::max_align_t) std::byte buffer[N];
        std::size_t offset = 0;

        void* allocate(const std::size_t sz, const std::size_t al) {
            if (al == 0 || !std::has_single_bit(al))
                return nullptr;

            const auto base = reinterpret_cast<std::uintptr_t>(buffer);
            const auto mask = ~(static_cast<std::uintptr_t>(al) - 1u);
            const auto aligned_ptr = (base + offset + (static_cast<std::uintptr_t>(al) - 1u)) & mask;
            const auto aligned = aligned_ptr - base;

            if (aligned > kMaxSize || sz > kMaxSize - aligned)
                return nullptr;

            void* p = buffer + aligned;
            offset = aligned + sz;
            return p;
        }

        // ReSharper disable once CppMemberFunctionMayBeStatic
        void deallocate(void*, std::size_t, std::size_t) noexcept {
            // bump allocator: deallocation handled by reset()
        }

        void reset() noexcept {
            offset = 0;
        }
    };

    template <uint64_t BlockSize = kDefaultBlockSize>
    struct PmrAllocator {
        static constexpr auto kBlockSize = BlockSize;

        std::pmr::monotonic_buffer_resource* resource {};

        PmrAllocator() = default;

        explicit PmrAllocator(std::pmr::monotonic_buffer_resource* r): resource(r) { }

        [[nodiscard]] void* allocate(const std::size_t sz, const std::size_t al) const {
            return resource ? resource->allocate(sz, al) : nullptr;
        }

        void deallocate(void* p, const std::size_t sz, const std::size_t al) const noexcept {
            if (resource)
                resource->deallocate(p, sz, al);
        }

        void reset() const noexcept {
            if (!resource)
                return;

            resource->release();
        }
    };

    template <class T>
    concept AllocatorLike = requires(T a, std::size_t sz, std::size_t align) {
        { T::kBlockSize } -> std::convertible_to<uint64_t>;
        { a.allocate(sz, align) } -> std::same_as<void*>;
        { a.deallocate(static_cast<void*>(nullptr), sz, align) } -> std::same_as<void>;
    };

    class Arena {
        struct Block {
            Block* next;
            std::size_t cap;
            std::size_t used;
        };

    public:
        template <AllocatorLike Alloc>
        explicit Arena(Alloc& alloc);

        ~Arena();

        Arena(const Arena&) = delete;
        Arena& operator=(const Arena&) = delete;

        Arena(Arena&& other) noexcept;
        Arena& operator=(Arena&& other) noexcept;

        void* alloc(std::size_t sz, std::size_t al = alignof(std::max_align_t));

        UJSON_FORCEINLINE void set_key_format(const detail::key_format fmt) noexcept {
            key_format_ = fmt;
        }

        [[nodiscard]] UJSON_FORCEINLINE detail::key_format get_key_format() const noexcept {
            return key_format_;
        }

        [[nodiscard]] UJSON_FORCEINLINE detail::key_format key_format() const noexcept {
            return key_format_;
        }

        template <class T, class... Args>
        UJSON_FORCEINLINE T* make(Args&&... args) noexcept;

        template <class T>
        UJSON_FORCEINLINE T* make_array(std::size_t n);

        void reset();

    private:
        template <class Sink>
        friend class JsonWriterCore;
        template <class Sink, detail::StringEscapePolicy Policy>
        friend class detail::JsonWriterCoreImpl;
        friend class ValueRef;
        friend class SaxDomHandler;
        friend class ValueBuilder;
        friend class NodeRef;
        template <bool, bool, bool>
        friend class TDocument;
        friend TapeStorage* tape_storage(const Arena*) noexcept;

        static constexpr bool valid_align(const std::size_t a) noexcept {
            return a != 0 && std::has_single_bit(a);
        }

        static constexpr std::size_t align_up(std::size_t v, std::size_t a) noexcept;
        static char* payload(Block* b) noexcept;

        bool add_block(std::size_t min_payload);

        void* allocator_ {};
        void* (*alloc_fn_)(void*, std::size_t, std::size_t) {};
        void (*dealloc_fn_)(void*, void*, std::size_t, std::size_t) {};

        Block* head_ {};
        Block* cur_ {};
        std::size_t block_size_ {kDefaultBlockSize};
        std::size_t block_align_ {alignof(std::max_align_t)};
        void* tape_storage_ {};
        detail::key_format key_format_ {detail::key_format::None};
    };

    template <AllocatorLike Alloc>
    Arena::Arena(Alloc& alloc)
        : allocator_(&alloc), alloc_fn_([](void* self, std::size_t sz, std::size_t al) { return static_cast<Alloc*>(self)->allocate(sz, al); }),
          dealloc_fn_([](void* self, void* p, std::size_t sz, std::size_t al) { static_cast<Alloc*>(self)->deallocate(p, sz, al); }), block_size_(Alloc::kBlockSize) { }

    inline Arena::~Arena() {
        Block* b = head_;
        while (b) {
            Block* next = b->next;
            dealloc_fn_(allocator_, b, sizeof(Block) + b->cap, block_align_);
            b = next;
        }
    }

    inline Arena::Arena(Arena&& other) noexcept
        : allocator_(other.allocator_), alloc_fn_(other.alloc_fn_), dealloc_fn_(other.dealloc_fn_), head_(other.head_), cur_(other.cur_), block_size_(other.block_size_),
          tape_storage_(other.tape_storage_), key_format_(other.key_format_) {
        other.head_ = nullptr;
        other.cur_ = nullptr;
        other.tape_storage_ = nullptr;
    }

    inline Arena& Arena::operator=(Arena&& other) noexcept {
        if (this != &other) {
            this->~Arena();
            allocator_ = other.allocator_;
            alloc_fn_ = other.alloc_fn_;
            dealloc_fn_ = other.dealloc_fn_;
            head_ = other.head_;
            cur_ = other.cur_;
            block_size_ = other.block_size_;
            tape_storage_ = other.tape_storage_;
            key_format_ = other.key_format_;
            other.head_ = nullptr;
            other.cur_ = nullptr;
            other.tape_storage_ = nullptr;
        }
        return *this;
    }

    inline void* Arena::alloc(const std::size_t sz, const std::size_t al) {
        if (!valid_align(al))
            return nullptr;

        if (sz > std::numeric_limits<std::size_t>::max() - al)
            return nullptr;

        if (!cur_ && !add_block(sz + al))
            return nullptr;

        for (;;) {
            const auto base = reinterpret_cast<std::uintptr_t>(payload(cur_));

            const auto mask = ~(static_cast<std::uintptr_t>(al) - 1u);
            const std::uintptr_t aligned = (base + cur_->used + (static_cast<std::uintptr_t>(al) - 1u)) & mask;

            if (const std::size_t off = aligned - base; off + sz <= cur_->cap) {
                const auto ptr = std::bit_cast<void*>(aligned);
                cur_->used = off + sz;
                return ptr;
            }

            if (!add_block(sz + al))
                return nullptr;
        }
    }

    template <class T, class... Args>
    UJSON_FORCEINLINE T* Arena::make(Args&&... args) noexcept {
        void* mem = alloc(sizeof(T), alignof(T));
        if (!mem)
            return nullptr;

        T* ptr = static_cast<T*>(mem);

        static_assert(std::is_constructible_v<T, Args...>, "Type not constructible");
        return std::construct_at(ptr, std::forward<Args>(args)...);
    }

    template <class T>
    UJSON_FORCEINLINE T* Arena::make_array(const std::size_t n) {
        return static_cast<T*>(alloc(sizeof(T) * n, alignof(T))); // NOLINT(bugprone-sizeof-expression)
    }

    inline void Arena::reset() {
        Block* b = head_;
        while (b) {
            Block* next = b->next;
            dealloc_fn_(allocator_, b, sizeof(Block) + b->cap, block_align_);
            b = next;
        }
        head_ = nullptr;
        cur_ = nullptr;
        tape_storage_ = nullptr;
    }

    constexpr std::size_t Arena::align_up(const std::size_t v, const std::size_t a) noexcept {
        if (!valid_align(a))
            return 0;
        const std::size_t add = a - 1;
        if (v > (std::numeric_limits<std::size_t>::max)() - add)
            return 0;
        return (v + add) & ~add;
    }

    inline char* Arena::payload(Block* b) noexcept {
        return reinterpret_cast<char*>(b + 1);
    }

    inline bool Arena::add_block(const std::size_t min_payload) {
        const std::size_t cap = std::max(block_size_, min_payload);
        const std::size_t total = sizeof(Block) + cap;

        block_align_ = std::max<std::size_t>(alignof(std::max_align_t), alignof(Block));
        void* mem = alloc_fn_(allocator_, total, block_align_);
        if (!mem)
            return false;

        auto* b = static_cast<Block*>(mem);
        b->next = nullptr;
        b->cap = cap;
        b->used = 0;

        if (!head_)
            head_ = b;
        else
            cur_->next = b;

        cur_ = b;
        return true;
    }

    [[nodiscard]] UJSON_FORCEINLINE detail::key_format init_key_format(Arena& a, const detail::key_format requested) noexcept {
        auto cur = a.get_key_format();
        if (cur == detail::key_format::None) {
            a.set_key_format(requested);
            cur = requested;
        }
        return cur;
    }

} // namespace ujson

namespace ujson::detail {
    struct KeyScratch {
        char buf[128];
        char* heap = nullptr;
        std::size_t cap = sizeof(buf);
        std::size_t size = 0;

        [[nodiscard]] UJSON_FORCEINLINE char* data() noexcept {
            return heap ? heap : buf;
        }

        [[nodiscard]] UJSON_FORCEINLINE const char* data() const noexcept {
            return heap ? heap : buf;
        }

        UJSON_FORCEINLINE void reset() noexcept {
            size = 0;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool ensure(const std::size_t need, Arena* arena) noexcept {
            if (need <= cap)
                return true;
            if (!arena)
                return false;

            std::size_t new_cap = cap ? cap : sizeof(buf);
            while (new_cap < need)
                new_cap *= 2u;

            auto* nb = static_cast<char*>(arena->alloc(new_cap, 1u));
            if (!nb)
                return false;

            if (size)
                std::memcpy(nb, data(), size);

            heap = nb;
            cap = new_cap;
            return true;
        }
    };

    [[nodiscard]] UJSON_FORCEINLINE bool normalize_ascii(const std::string_view key, const key_format format, Arena* arena, KeyScratch& scratch, std::string_view& out) noexcept {
        out = key;

        if (format == key_format::None)
            return true;

        for (const unsigned char c : key)
            if (!is_ascii_char(c))
                return true;

        scratch.reset();
        if (!scratch.ensure(key.size() + 4u, arena))
            return false;

        auto start_word = true;
        auto first_word = true;
        auto prev_lower = false;
        auto prev_upper = false;

        for (std::size_t i = 0; i < key.size(); ++i) {
            const char c = key[i];
            if (is_ascii_sep(c)) {
                start_word = true;
                prev_lower = false;
                prev_upper = false;
                continue;
            }

            const bool is_upper = c >= 'A' && c <= 'Z';
            const bool is_lower = c >= 'a' && c <= 'z';
            const bool is_alpha = is_upper || is_lower;
            const bool is_digit = c >= '0' && c <= '9';

            auto boundary = false;
            if (!start_word && is_upper) {
                if (prev_lower) {
                    boundary = true;
                } else if (prev_upper && i + 1 < key.size()) {
                    const char next = key[i + 1];
                    if (next >= 'a' && next <= 'z')
                        boundary = true;
                }
            }

            if (boundary)
                start_word = true;

            if (start_word && format == key_format::SnakeCase && scratch.size) {
                if (!scratch.ensure(scratch.size + 1u, arena))
                    return false;
                scratch.data()[scratch.size++] = '_';
            }

            char out_c = c;
            if (is_alpha) {
                switch (format) {
                case key_format::SnakeCase:
                    out_c = to_lower_ascii(c);
                    break;
                case key_format::CamelCase:
                    out_c = start_word ? (first_word ? to_lower_ascii(c) : to_upper_ascii(c)) : to_lower_ascii(c);
                    break;
                case key_format::PascalCase:
                    out_c = start_word ? to_upper_ascii(c) : to_lower_ascii(c);
                    break;
                case key_format::None:
                    break;
                }
            } else if (!is_digit) {
                start_word = true;
                prev_lower = false;
                prev_upper = false;
                if (!scratch.ensure(scratch.size + 1u, arena))
                    return false;
                scratch.data()[scratch.size++] = out_c;
                continue;
            }

            if (!scratch.ensure(scratch.size + 1u, arena))
                return false;
            scratch.data()[scratch.size++] = out_c;

            if (start_word) {
                start_word = false;
                first_word = false;
            }

            prev_lower = is_lower;
            prev_upper = is_upper;
        }

        if (scratch.size == key.size() && std::memcmp(scratch.data(), key.data(), scratch.size) == 0)
            return true;

        out = std::string_view {scratch.data(), scratch.size};
        if (scratch.heap && scratch.cap > scratch.size)
            scratch.data()[scratch.size] = '\0';
        if (arena)
            ujson::mark_normalized_keys(arena);
        return true;
    }

    UJSON_FORCEINLINE void emit_mask_32(std::uint32_t mask, const std::uint32_t base, std::uint32_t*& out) noexcept {
        while (mask) {
            const std::uint32_t bit = std::countr_zero(mask);
            *out++ = base + bit;
            mask &= mask - 1u;
        }
    }

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t mask_eq_avx2(__m256i v, const char c) noexcept {
        const __m256i k = _mm256_set1_epi8(c);
        return static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, k)));
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t structural_noquote_avx2(__m256i v) noexcept {
        // { } [ ] , :
        std::uint32_t m = 0;
        m |= mask_eq_avx2(v, '{');
        m |= mask_eq_avx2(v, '}');
        m |= mask_eq_avx2(v, '[');
        m |= mask_eq_avx2(v, ']');
        m |= mask_eq_avx2(v, ',');
        m |= mask_eq_avx2(v, ':');
        return m;
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t ws_mask_avx2(__m256i v) noexcept {
        std::uint32_t m = 0;
        m |= mask_eq_avx2(v, ' ');
        m |= mask_eq_avx2(v, '\n');
        m |= mask_eq_avx2(v, '\r');
        m |= mask_eq_avx2(v, '\t');
        return m;
    }

#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t mask_eq_sse2(const __m128i v, const char c) noexcept {
        const __m128i k = _mm_set1_epi8(c);
        return static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, k)));
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t structural_noquote_sse2(const __m128i v) noexcept {
        std::uint32_t m = 0;
        m |= mask_eq_sse2(v, '{');
        m |= mask_eq_sse2(v, '}');
        m |= mask_eq_sse2(v, '[');
        m |= mask_eq_sse2(v, ']');
        m |= mask_eq_sse2(v, ',');
        m |= mask_eq_sse2(v, ':');
        return m;
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t ws_mask_sse2(const __m128i v) noexcept {
        std::uint32_t m = 0;
        m |= mask_eq_sse2(v, ' ');
        m |= mask_eq_sse2(v, '\n');
        m |= mask_eq_sse2(v, '\r');
        m |= mask_eq_sse2(v, '\t');
        return m;
    }

#endif

    [[nodiscard]] UJSON_FORCEINLINE StructuralIndex build_structural_index(const char* input, const std::size_t len, Arena& arena) noexcept {
        StructuralIndex out {};

        if (!input || len == 0)
            return out;

        if (len > (std::numeric_limits<std::uint32_t>::max)())
            return out;

        out.capacity = static_cast<uint32_t>(len);

        out.positions = arena.make_array<std::uint32_t>(out.capacity);
        if (!out.positions) {
            out.oom = true;
            return out;
        }

        auto prev_in_string = false;
        auto prev_escaped = false;
        auto prev_is_delim_carry = 1u; // stream start = delimiter

        std::size_t i = 0;

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

        constexpr std::uint32_t W = 32u;
        constexpr std::uint32_t ALL = 0xFFFFFFFFu;

        for (; i + W <= len; i += W) {

            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + i));

            const std::uint32_t quote_mask = mask_eq_avx2(v, '"');
            const std::uint32_t backslash_mask = mask_eq_avx2(v, '\\');

            bool next_prev_escaped = false;
            const std::uint32_t escaped_mask = compute_escaped_mask(backslash_mask, prev_escaped, next_prev_escaped);

            const std::uint32_t unescaped_quotes = quote_mask & ~escaped_mask;

            std::uint32_t in_string_mask = prefix_xor_mask(unescaped_quotes);
            in_string_mask ^= unescaped_quotes;

            if (prev_in_string)
                in_string_mask = ~in_string_mask;

            in_string_mask &= ALL;
            const std::uint32_t outside_string = ~in_string_mask & ALL;

            const std::uint32_t structural_noquote = structural_noquote_avx2(v) & outside_string;
            const std::uint32_t ws = ws_mask_avx2(v) & outside_string;
            const std::uint32_t structural = structural_noquote | unescaped_quotes;

            const std::uint32_t delim = (structural | ws) & ALL;
            const std::uint32_t prev_delim = ((delim << 1) | (prev_is_delim_carry & 1u)) & ALL;

            const std::uint32_t pseudo = outside_string & prev_delim & ~ws & ~structural;
            const std::uint32_t final_mask = structural | pseudo;

            std::uint32_t mask = final_mask;
            while (mask) {
                const std::uint32_t bit = std::countr_zero(mask);
                out.push_back(static_cast<std::uint32_t>(i + bit));
                mask &= mask - 1u;
            }

            prev_in_string ^= (std::popcount(unescaped_quotes) & 1u) != 0u;
            prev_escaped = next_prev_escaped;
            prev_is_delim_carry = (delim >> (W - 1u)) & 1u;
        }

#elif defined(__SSE2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86_FP)))

        constexpr auto W = 16u;
        constexpr auto ALL = 0xFFFFu;

        for (; i + W <= len; i += W) {

            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + i));

            const std::uint32_t quote_mask = mask_eq_sse2(v, '"') & ALL;
            const std::uint32_t backslash_mask = mask_eq_sse2(v, '\\') & ALL;

            auto next_prev_escaped = false;
            const std::uint32_t escaped_mask = compute_escaped_mask(backslash_mask, prev_escaped, next_prev_escaped) & ALL;

            const std::uint32_t unescaped_quotes = quote_mask & ~escaped_mask;

            std::uint32_t in_string_mask = prefix_xor_mask(unescaped_quotes);
            in_string_mask ^= unescaped_quotes;

            if (prev_in_string)
                in_string_mask = ~in_string_mask;

            in_string_mask &= ALL;

            const std::uint32_t outside_string = ~in_string_mask & ALL;

            const std::uint32_t structural_noquote = structural_noquote_sse2(v) & outside_string;
            const std::uint32_t ws = ws_mask_sse2(v) & outside_string;
            const std::uint32_t structural = structural_noquote | unescaped_quotes;

            const std::uint32_t delim = (structural | ws) & ALL;
            const std::uint32_t prev_delim = ((delim << 1) | (prev_is_delim_carry & 1u)) & ALL;

            const std::uint32_t pseudo = outside_string & prev_delim & ~ws & ~structural;
            const std::uint32_t final_mask = structural | pseudo;

            std::uint32_t mask = final_mask;
            while (mask) {
                const std::uint32_t bit = std::countr_zero(mask);
                out.push_back(static_cast<std::uint32_t>(i + bit));
                mask &= mask - 1u;
            }

            prev_in_string ^= (std::popcount(unescaped_quotes) & 1u) != 0u;
            prev_escaped = next_prev_escaped;
            prev_is_delim_carry = (delim >> (W - 1u)) & 1u;
        }

#endif

        bool in_string = prev_in_string;
        bool escaped = prev_escaped;
        bool prev_delim = (prev_is_delim_carry & 1u) != 0u;

        for (; i < len; ++i) {

            const char c = input[i];

            if (in_string) {
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (c == '\\') {
                    escaped = true;
                    continue;
                }
                if (c == '"') {
                    in_string = false;
                    out.push_back(static_cast<std::uint32_t>(i));
                    prev_delim = true;
                }
                continue;
            }

            if (c == '"') {
                in_string = true;
                out.push_back(static_cast<std::uint32_t>(i));
                prev_delim = true;
                continue;
            }

            if (is_ws_u8(static_cast<unsigned char>(c))) {
                prev_delim = true;
                continue;
            }

            if (is_structural_char(c)) {
                out.push_back(static_cast<std::uint32_t>(i));
                prev_delim = true;
                continue;
            }

            if (prev_delim)
                out.push_back(static_cast<std::uint32_t>(i));

            prev_delim = false;
        }

        return out;
    }

} // namespace ujson::detail

namespace ujson {

    class ArenaHolder {
    public:
        constexpr ArenaHolder() noexcept = default;

        explicit ArenaHolder(bool): own_alloc_ {NewAllocator {}}, own_arena_ {std::in_place, own_alloc_}, arena_ {&own_arena_.value()}, is_owner_ {true} {
            assert(arena_);
        }

        template <AllocatorLike Allocator>
        explicit ArenaHolder(Allocator& alloc): own_arena_ {std::in_place, alloc}, arena_ {&own_arena_.value()}, is_owner_ {true} {
            assert(arena_);
        }

        explicit ArenaHolder(Arena& arena) noexcept: arena_ {&arena} { }

        ~ArenaHolder() {
            reset();
        }

        ArenaHolder(const ArenaHolder&) = delete;
        ArenaHolder& operator=(const ArenaHolder&) = delete;

        ArenaHolder(ArenaHolder&& other) noexcept: own_alloc_(other.own_alloc_), own_arena_(std::move(other.own_arena_)), arena_(other.arena_), is_owner_(other.is_owner_) {
            if (is_owner_ && own_arena_.has_value())
                arena_ = &own_arena_.value();

            other.arena_ = nullptr;
            other.is_owner_ = false;
            other.own_arena_.reset();
        }

        ArenaHolder& operator=(ArenaHolder&& other) noexcept {
            if (this == &other)
                return *this;
            reset();

            own_alloc_ = other.own_alloc_;
            own_arena_ = std::move(other.own_arena_);
            arena_ = other.arena_;
            is_owner_ = other.is_owner_;

            if (is_owner_ && own_arena_.has_value())
                arena_ = &own_arena_.value();

            other.arena_ = nullptr;
            other.is_owner_ = false;
            other.own_arena_.reset();
            return *this;
        }

        [[nodiscard]] Arena& arena() const noexcept {
            assert(arena_);
            return *arena_;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return arena_ != nullptr;
        }

        void reset() noexcept {
            if (is_owner_) {
                own_arena_.reset();
                is_owner_ = false;
            }
            arena_ = nullptr;
        }

    protected:
        [[no_unique_address]] NewAllocator<> own_alloc_ {};
        std::optional<Arena> own_arena_ {};
        Arena* arena_ = nullptr;
        bool is_owner_ = false;
    };

    enum class Type : std::uint8_t {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    enum class NumberKind : std::uint8_t {
        Integer,
        Double
    };

    struct Number {
        NumberKind kind {NumberKind::Double};
        union {
            std::int64_t i;
            double d;
        };

        constexpr Number() noexcept: d(0.0) { }

        static constexpr Number from_i64(const std::int64_t v) noexcept {
            Number n;
            n.kind = NumberKind::Integer;
            n.i = v;
            return n;
        }

        static constexpr Number from_double(const double v) noexcept {
            Number n;
            n.kind = NumberKind::Double;
            n.d = v;
            return n;
        }

        [[nodiscard]] constexpr int64_t as_i64() const noexcept {
            return kind == NumberKind::Integer ? i : static_cast<std::int64_t>(d);
        }

        [[nodiscard]] constexpr double as_double() const noexcept {
            return kind == NumberKind::Integer ? static_cast<double>(i) : d;
        }
    };

    std::uint32_t hash32(const void* data, std::size_t len) noexcept;

    static constexpr auto kInvalidHash = std::numeric_limits<std::uint32_t>::max();
    struct Node {
        Type type {Type::Null};
        std::string_view key {}; // used for object children
        std::uint32_t key_hash = kInvalidHash;

        std::uint32_t next {};

        union Data {
            bool b;
            Number num;
            std::string_view str;

            struct {
                std::uint32_t count;
                std::uint32_t capacity;
                void* index;
                Node** items;
            } kids;

            Data() { }

        } data;

        static constexpr bool is_container(const Type t) noexcept {
            return t == Type::Array || t == Type::Object;
        }

        [[nodiscard]] constexpr bool key_equals(const std::uint32_t other, const std::string_view sv) const noexcept {
            assert(key_hash != kInvalidHash && "key hash must initialized.");

            if (key.size() != sv.size())
                return false;

            return key_hash == other;
        }
    };

    static_assert(std::is_trivially_copyable_v<Node>);
    static_assert(std::is_trivially_copyable_v<Node::Data>);

    struct TapeStorage {
        Node* tape = nullptr;
        std::uint32_t size = 0;
        std::uint32_t pos = 0;
        bool normalized_keys = false;
    };

    [[nodiscard]] UJSON_FORCEINLINE TapeStorage* tape_storage(const Arena* arena) noexcept {
        return arena ? static_cast<TapeStorage*>(arena->tape_storage_) : nullptr;
    }

    UJSON_FORCEINLINE void mark_normalized_keys(Arena* arena) noexcept {
        if (!arena)
            return;
        if (auto* storage = tape_storage(arena))
            storage->normalized_keys = true;
    }

    [[nodiscard]] UJSON_FORCEINLINE bool tape_ready(const TapeStorage* t) noexcept {
        return t && t->tape && t->size;
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t tape_index_of(const TapeStorage* t, const Node* n) noexcept {
        return (t && n) ? static_cast<std::uint32_t>(n - t->tape) : 0u;
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t subtree_next_index(const TapeStorage* t, const std::uint32_t idx) noexcept {
        return t->tape[idx].next;
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t hash32(const void* data, std::size_t len) noexcept {
        constexpr auto seed = 0x9E3779B9u;

        const auto* p = static_cast<const std::uint8_t*>(data);
        std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);

        while (len >= 4) {
            std::uint32_t v;
            std::memcpy(&v, p, 4);
            h ^= v;
            h *= 0x85EBCA6Bu;
            h ^= h >> 13;
            p += 4;
            len -= 4;
        }

        std::uint32_t tail = 0;
        for (std::size_t i = 0; i < len; ++i)
            tail |= static_cast<std::uint32_t>(p[i]) << (i * 8);

        h ^= tail;
        h *= 0xC2B2AE35u;
        h ^= h >> 16;

        return h ? h : 1u;
    }

    [[nodiscard]] UJSON_FORCEINLINE std::uint32_t hash_bucket(const std::uint32_t h, const std::uint32_t mask) noexcept {
        return h & mask;
    }

    struct ArrIndex {
        std::uint32_t count {};
        std::uint32_t* pos {}; // pos[i] = tape index of i-th element
    };

    struct ObjIndex {
        static constexpr auto kEmpty = 0u;
        static constexpr auto kTomb = 0xFFFFFFFFu;

        struct Slot {
            std::uint32_t h;
            std::uint32_t pos; // tape index of child; kTomb = tombstone
        };
        std::uint32_t cap {};
        std::uint32_t size {};
        Slot* slots {};
    };

    enum class ErrorCode : std::uint8_t {
        None,
        UnexpectedEOF,
        UnexpectedToken,
        InvalidNumber,
        InvalidString,
        InvalidEscape,
        InvalidUnicode,
        DepthExceeded,
        TrailingGarbage,
        EncodeDepthExceeded,
        WriterOverflow,
        ToCharsFailed,
        BuilderInvalidState,
        BuilderMissingKey,
        BuilderPopUnderflow,
        BuilderKeyOutsideObject,
    };

    enum class ErrorFormat : std::uint8_t {
        Pretty,
        Compact
    };

    struct ParseError {
        ErrorCode code {ErrorCode::None};
        const char* at {};
        std::string_view input {};

        UJSON_FORCEINLINE void set(const ErrorCode c, const char* at_str = nullptr) {
            if (code == ErrorCode::None) {
                code = c;
                at = at_str;
            }
        }

        UJSON_FORCEINLINE void reset() {
            code = ErrorCode::None;
            at = nullptr;
            input = {};
        }

        template <ErrorFormat Fmt>
        [[nodiscard]] UJSON_FORCEINLINE std::string format() const;

        [[nodiscard]] UJSON_FORCEINLINE std::string to_string() const;

        [[nodiscard]] UJSON_FORCEINLINE constexpr bool ok() const noexcept {
            return code == ErrorCode::None;
        }

        [[nodiscard]] UJSON_FORCEINLINE constexpr explicit operator bool() const noexcept {
            return ok();
        }
    };

    [[nodiscard]] constexpr const char* error_code_name(const ErrorCode c) noexcept {
        switch (c) {
        case ErrorCode::None:
            return "None";
        case ErrorCode::UnexpectedEOF:
            return "UnexpectedEOF";
        case ErrorCode::UnexpectedToken:
            return "UnexpectedToken";
        case ErrorCode::InvalidNumber:
            return "InvalidNumber";
        case ErrorCode::InvalidString:
            return "InvalidString";
        case ErrorCode::InvalidEscape:
            return "InvalidEscape";
        case ErrorCode::InvalidUnicode:
            return "InvalidUnicode";
        case ErrorCode::DepthExceeded:
            return "DepthExceeded";
        case ErrorCode::TrailingGarbage:
            return "TrailingGarbage";
        case ErrorCode::EncodeDepthExceeded:
            return "EncodeDepthExceeded";
        case ErrorCode::WriterOverflow:
            return "WriterOverflow";
        case ErrorCode::ToCharsFailed:
            return "ToCharsFailed";
        case ErrorCode::BuilderInvalidState:
            return "BuilderInvalidState";
        case ErrorCode::BuilderMissingKey:
            return "BuilderMissingKey";
        case ErrorCode::BuilderPopUnderflow:
            return "BuilderPopUnderflow";
        case ErrorCode::BuilderKeyOutsideObject:
            return "BuilderKeyOutsideObject";
        }
        return "Unknown";
    }

    struct ErrorLocation {
        std::size_t offset {};
        std::size_t line {1};
        std::size_t column {1};
    };

    [[nodiscard]] inline ErrorLocation locate_error(const std::string_view input, const ParseError& e) noexcept {
        ErrorLocation loc {};
        if (e.code == ErrorCode::None || !e.at || input.data() == nullptr)
            return loc;

        const auto* base = input.data();
        const auto* end = base + input.size();
        if (e.at < base) {
            return loc;
        }
        const auto* p = e.at;
        p = std::min(p, end);

        loc.offset = static_cast<std::size_t>(p - base);

        std::size_t line = 1;
        std::size_t col = 1;
        for (const char* it = base; it < p; ++it) {
            if (*it == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        loc.line = line;
        loc.column = col;
        return loc;
    }

    [[nodiscard]] inline std::string format_error_compact(const std::string_view input, const ParseError& e) {
        if (e.code == ErrorCode::None)
            return {};

        const auto [offset, line, column] = locate_error(input, e);

        std::string out;
        out.reserve(96);

        out.append("ujson: ");
        out.append(error_code_name(e.code));
        out.append(" at ");
        out.append(std::to_string(line));
        out.push_back(':');
        out.append(std::to_string(column));
        out.append(" (offset ");
        out.append(std::to_string(offset));
        out.push_back(')');

        if (offset < input.size()) {
            out.append(" unexpected '");
            out.push_back(input[offset]);
            out.push_back('\'');
        }

        return out;
    }

    [[nodiscard]] inline std::string format_error(const std::string_view input, const ParseError& e) {
        if (e.code == ErrorCode::None)
            return {};

        constexpr std::size_t kMaxWidth = 100;
        constexpr std::size_t kHalfWin = 40;

        const auto [offset, line, column] = locate_error(input, e);

        std::size_t start = offset;
        while (start > 0 && input[start - 1] != '\n')
            --start;

        std::size_t end = offset;
        while (end < input.size() && input[end] != '\n')
            ++end;

        std::string_view full = input.substr(start, end - start);

        std::size_t caret = column ? column - 1 : 0;
        std::size_t trim_left = 0;

        if (full.size() > kMaxWidth) {
            std::size_t win = caret > kHalfWin ? caret - kHalfWin : 0;

            if (win + kMaxWidth > full.size())
                win = full.size() - kMaxWidth;

            trim_left = win;
            full = full.substr(win, kMaxWidth);
            caret -= trim_left;
        }

        std::string out;
        out.reserve(full.size() + 128);

        // header
        out.append("ujson: ");
        out.append(error_code_name(e.code));
        out.push_back('\n');

        out.append(" --> ");
        out.append(std::to_string(line));
        out.push_back(':');
        out.append(std::to_string(column));
        out.append(" (offset ");
        out.append(std::to_string(offset));
        out.append(")\n\n");

        // build rendered source line
        const std::string prefix = " " + std::to_string(line) + " | ";
        std::string rendered = prefix;

        if (trim_left)
            rendered += "...";

        rendered.append(full);

        if (trim_left + full.size() < end - start)
            rendered += "...";

        out.append(rendered);
        out.push_back('\n');

        // caret line
        std::string caret_line(rendered.size(), ' ');

        const std::size_t caret_pos = prefix.size() + (trim_left ? 3 : 0) + caret;

        if (caret_pos < caret_line.size())
            caret_line[caret_pos] = '^';

        out.append(caret_line);

        if (offset < input.size()) {
            out.append(" unexpected '");
            out.push_back(input[offset]);
            out.push_back('\'');
        }

        out.push_back('\n');

        return out;
    }

    template <ErrorFormat Fmt>
    UJSON_FORCEINLINE std::string ParseError::format() const {
        if (input.empty())
            return {};

        if constexpr (Fmt == ErrorFormat::Compact)
            return format_error_compact(input, *this);
        if constexpr (Fmt == ErrorFormat::Pretty)
            return format_error(input, *this);

        return {};
    }

    UJSON_FORCEINLINE std::string ParseError::to_string() const {
        return error_code_name(code);
    }

    struct DefaultStringScratch {
        Arena* arena {};
        char* buf {};
        std::size_t cap {};
        std::size_t len {}; // valid bytes in buf

        explicit DefaultStringScratch(Arena& a): arena(&a) { }

        [[nodiscard]] char* ensure(const std::size_t n) {
            constexpr std::size_t kMaxStringSize = 64ull * 1024ull * 1024ull;
            if (!arena)
                return nullptr;
            if (n > kMaxStringSize)
                return nullptr;
            if (n <= cap)
                return buf;

            std::size_t new_cap = cap ? cap : 256u;
            while (new_cap < n) {
                if (new_cap > kMaxStringSize / 2u)
                    return nullptr;
                if (new_cap > (std::numeric_limits<std::size_t>::max)() / 2u)
                    return nullptr;
                new_cap *= 2u;
            }

            auto* nb = static_cast<char*>(arena->alloc(new_cap, 1));
            if (!nb)
                return nullptr;

            if (buf && len)
                std::memcpy(nb, buf, len);

            buf = nb;
            cap = new_cap;
            return buf;
        }

        void set_len(const std::size_t n) noexcept {
            len = n <= cap ? n : cap;
        }
    };

    class ValueRef {
    public:
        ValueRef() = default;
        ValueRef(Node* n, Arena* a): n_(n) {
            if (a) {
                ctx_storage_.arena = a;
                ctx_storage_.fmt = init_key_format(*a, detail::key_format::None);
                ctx_ = &ctx_storage_;
            }
        }
        ValueRef(Node* n, const DomContext* ctx): n_(n), ctx_(ctx) { }

        [[nodiscard]] UJSON_FORCEINLINE Arena* arena() const noexcept {
            return ctx_ ? ctx_->arena : nullptr;
        }

        [[nodiscard]] UJSON_FORCEINLINE Type type() const noexcept {
            return n_ ? n_->type : Type::Null;
        }
        [[nodiscard]] UJSON_FORCEINLINE Node* raw() const noexcept {
            return n_;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool is_object() const noexcept {
            return n_ && n_->type == Type::Object;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_array() const noexcept {
            return n_ && n_->type == Type::Array;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_string() const noexcept {
            return n_ && n_->type == Type::String;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_number() const noexcept {
            return n_ && n_->type == Type::Number;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_bool() const noexcept {
            return n_ && n_->type == Type::Bool;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_null() const noexcept {
            return !n_ || n_->type == Type::Null;
        }

        [[nodiscard]] UJSON_FORCEINLINE std::optional<std::string_view> try_string() const noexcept {
            if (n_ && n_->type == Type::String)
                return n_->data.str;
            return std::nullopt;
        }
        [[nodiscard]] UJSON_FORCEINLINE std::optional<bool> try_bool() const noexcept {
            if (n_ && n_->type == Type::Bool)
                return n_->data.b;
            return std::nullopt;
        }
        [[nodiscard]] UJSON_FORCEINLINE std::optional<std::int64_t> try_i64() const noexcept {
            if (n_ && n_->type == Type::Number && n_->data.num.kind == NumberKind::Integer)
                return n_->data.num.i;
            return std::nullopt;
        }
        [[nodiscard]] UJSON_FORCEINLINE std::optional<double> try_double() const noexcept {
            if (!n_ || n_->type != Type::Number)
                return std::nullopt;
            if (n_->data.num.kind == NumberKind::Double)
                return n_->data.num.d;
            return static_cast<double>(n_->data.num.i);
        }

        [[nodiscard]] UJSON_FORCEINLINE std::string_view as_string(const std::string_view def = {}) const noexcept {
            return n_ && n_->type == Type::String ? n_->data.str : def;
        }
        [[nodiscard]] UJSON_FORCEINLINE double as_double(const double def = 0.0) const noexcept {
            if (!n_ || n_->type != Type::Number)
                return def;
            return n_->data.num.kind == NumberKind::Double ? n_->data.num.d : static_cast<double>(n_->data.num.i);
        }
        [[nodiscard]] UJSON_FORCEINLINE std::int64_t as_i64(const std::int64_t def = 0) const noexcept {
            if (!n_ || n_->type != Type::Number)
                return def;
            if (n_->data.num.kind == NumberKind::Integer)
                return n_->data.num.i;
            return def;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool as_bool(const bool def = false) const noexcept {
            return n_ && n_->type == Type::Bool ? n_->data.b : def;
        }

        [[nodiscard]] UJSON_FORCEINLINE std::uint32_t size() const noexcept {
            return n_ && Node::is_container(n_->type) ? n_->data.kids.count : 0;
        }

        [[nodiscard]] UJSON_FORCEINLINE ValueRef at(const std::uint32_t i) const noexcept {
            if (!n_ || n_->type != Type::Array)
                return {};

            const auto& k = n_->data.kids;
            if (i >= k.count)
                return {};

            const auto* t = tape_storage(arena());
            if (!tape_ready(t)) {
                if (!k.items)
                    return {};
                Node* child = k.items[i];
                return child ? make_ref(child) : ValueRef {};
            }

            // build lazily if random access becomes worth it
            if (!k.index) {
                // optional heuristic: only build if i is not tiny
                if (i >= 4u)
                    ensure_arr_index();
            }

            if (k.index) {
                if (const auto* aix = static_cast<const ArrIndex*>(k.index); i < aix->count) {
                    if (const std::uint32_t pos = aix->pos[i]; pos < t->size)
                        return make_ref(t->tape + pos);
                }
                return {};
            }

            // fallback: old O(i) path (for small arrays / if index wasn't built)
            const std::uint32_t parent_idx = tape_index_of(t, n_);
            std::uint32_t cur = parent_idx + 1u;

            for (std::uint32_t step = 0; step < i; ++step)
                cur = subtree_next_index(t, cur);

            if (cur >= t->size)
                return {};
            return make_ref(t->tape + cur);
        }

        [[nodiscard]] UJSON_FORCEINLINE ValueRef operator[](const std::uint32_t i) const noexcept {
            return at(i);
        }

        template <class Fn>
            requires std::invocable<Fn, ValueRef>
        UJSON_FORCEINLINE void for_each(Fn&& fn) const noexcept {
            if (!n_ || !Node::is_container(n_->type))
                return;

            const auto& k = n_->data.kids;

            const auto* t = tape_storage(arena());
            if (!tape_ready(t)) {
                if (!k.items)
                    return;
                for (std::uint32_t i = 0; i < k.count; ++i) {
                    ValueRef v {make_ref(k.items[i])};
                    if constexpr (std::convertible_to<std::invoke_result_t<Fn, ValueRef>, bool>) {
                        if (!std::forward<Fn>(fn)(v))
                            return;
                    } else {
                        std::forward<Fn>(fn)(v); // NOLINT(bugprone-use-after-move)
                    }
                }
                return;
            }

            const std::uint32_t parent_idx = tape_index_of(t, n_);
            std::uint32_t cur = parent_idx + 1u;

            for (std::uint32_t i = 0; i < k.count; ++i) {
                if (cur >= t->size)
                    return;

                ValueRef v {make_ref(t->tape + cur)};

                if constexpr (std::convertible_to<std::invoke_result_t<Fn, ValueRef>, bool>) {
                    if (!std::forward<Fn>(fn)(v))
                        return;
                } else {
                    std::forward<Fn>(fn)(v); // NOLINT(bugprone-use-after-move)
                }

                cur = subtree_next_index(t, cur);
            }
        }

        [[nodiscard]] UJSON_FORCEINLINE ValueRef get(const std::string_view k) const {
            if (!n_ || n_->type != Type::Object)
                return {};

            const auto* t = tape_storage(arena());
            const auto& kids = n_->data.kids;

            detail::KeyScratch scratch;
            const std::string_view nk = normalize_key(k, scratch);
            const auto h = hash32(nk.data(), nk.size());

            if (!tape_ready(t)) {
                if (!kids.items)
                    return {};

                for (std::uint32_t i = 0; i < kids.count; ++i) {
                    if (Node* cand = kids.items[i]; cand && cand->key_equals(h, nk))
                        return make_ref(cand);
                }
                return {};
            }

            if (kids.count >= 12)
                ensure_obj_index();

            if (const auto* ix = static_cast<ObjIndex*>(kids.index)) {
                const std::uint32_t mask = ix->cap - 1u;
                std::uint32_t pos = hash_bucket(h, mask);

                for (std::uint32_t step = 0; step < ix->cap; ++step) {
                    const auto s = ix->slots[pos];
                    if (s.h == ObjIndex::kEmpty)
                        break;

                    if (s.h == h && s.pos != ObjIndex::kTomb) {
                        if (s.pos < t->pos) {
                            if (Node* cand = t->tape + s.pos; cand && cand->key_equals(h, nk))
                                return make_ref(cand);
                        }
                    }

                    pos = (pos + 1u) & mask;
                }
                return {};
            }

            const std::uint32_t parent_idx = tape_index_of(t, n_);
            std::uint32_t cur = parent_idx + 1u;

            for (std::uint32_t i = 0; i < kids.count; ++i) {
                if (cur >= t->size)
                    return {};
                if (Node* cand = t->tape + cur; cand && cand->key_equals(h, nk))
                    return make_ref(cand);
                cur = subtree_next_index(t, cur);
            }

            return {};
        }

        [[nodiscard]] UJSON_FORCEINLINE ValueRef operator[](const std::string_view k) const {
            return get(k);
        }

        [[nodiscard]] UJSON_FORCEINLINE bool contains(const std::string_view k) const {
            return static_cast<bool>(get(k).raw());
        }

        bool get_ints(const std::string_view key, int* out, const std::size_t count) const {
            const ValueRef arr = key.empty() ? *this : get(key);
            if (!arr.is_array() || arr.size() < count)
                return false;
            for (std::size_t i = 0; i < count; ++i)
                out[i] = static_cast<int>(arr.at(static_cast<std::uint32_t>(i)).as_double());
            return true;
        }

        // ranges
        struct ArrayIter;
        struct ArrayRange;

        UJSON_FORCEINLINE ArrayRange items() const noexcept;

        struct ObjIter;
        struct ObjRange;

        UJSON_FORCEINLINE ObjRange members() const noexcept;

    private:
        [[nodiscard]] UJSON_FORCEINLINE ValueRef make_ref(Node* n) const noexcept {
            return ctx_ ? ValueRef {n, ctx_} : ValueRef {};
        }

        [[nodiscard]] UJSON_FORCEINLINE std::string_view normalize_key(const std::string_view key, detail::KeyScratch& scratch) const noexcept {
            const auto fmt = ctx_ ? ctx_->fmt : detail::key_format::None;
            if (const auto* arena_ptr = arena()) {
                if (const auto* storage = tape_storage(arena_ptr); storage && !storage->normalized_keys)
                    return key;
            }
            std::string_view out = key;
            if (!detail::normalize_ascii(key, fmt, arena(), scratch, out))
                return key;
            return out;
        }

        UJSON_FORCEINLINE static std::uint32_t next_pow2(const std::uint32_t v) noexcept {
            if (v <= 1)
                return 1;
            return 1u << (32 - std::countl_zero(v - 1));
        }

        void objindex_rehash(ObjIndex* ix, const std::uint32_t new_cap) const {
            auto* arena_ptr = arena();
            auto* new_slots = arena_ptr ? arena_ptr->make_array<ObjIndex::Slot>(new_cap) : nullptr;
            if (!new_slots)
                return;
            std::memset(new_slots, 0, sizeof(ObjIndex::Slot) * new_cap);

            const std::uint32_t mask = new_cap - 1;

            for (std::uint32_t i = 0; i < ix->cap; ++i) {
                const auto& s = ix->slots[i];
                if (s.h == ObjIndex::kEmpty || s.pos == ObjIndex::kTomb)
                    continue;

                std::uint32_t pos = hash_bucket(s.h, mask);
                for (;;) {
                    auto& d = new_slots[pos];
                    if (d.h == ObjIndex::kEmpty) {
                        d = s;
                        break;
                    }
                    pos = (pos + 1u) & mask;
                }
            }

            ix->slots = new_slots;
            ix->cap = new_cap;
        }

        UJSON_FORCEINLINE void ensure_arr_index() const {
            auto* arena_ptr = arena();
            if (!arena_ptr || !n_ || n_->type != Type::Array)
                return;

            auto& kids = n_->data.kids;
            if (kids.index) // already built
                return;

            const auto* t = tape_storage(arena_ptr);
            if (!tape_ready(t))
                return;

            // optional threshold: don't build for tiny arrays
            if (kids.count < 16u)
                return;

            auto* ix = arena_ptr->make<ArrIndex>();
            if (!ix)
                return;

            ix->count = kids.count;
            ix->pos = arena_ptr->make_array<std::uint32_t>(kids.count);
            if (!ix->pos)
                return;

            std::uint32_t cur = tape_index_of(t, n_) + 1u;

            for (std::uint32_t i = 0; i < kids.count; ++i) {
                if (cur >= t->size) {
                    // corrupted tape / bounds, best-effort
                    ix->count = i;
                    break;
                }
                ix->pos[i] = cur;
                cur = subtree_next_index(t, cur);
            }

            kids.index = ix;
        }

        UJSON_FORCEINLINE void ensure_obj_index() const {
            auto* arena_ptr = arena();
            if (!arena_ptr || !n_ || n_->type != Type::Object)
                return;

            auto& kids = n_->data.kids;
            if (kids.index)
                return;

            const auto* t = tape_storage(arena_ptr);
            if (!tape_ready(t))
                return;

            auto* ix = arena_ptr->make<ObjIndex>();
            if (!ix)
                return;

            ix->size = 0;
            ix->cap = std::max<std::uint32_t>(16u, next_pow2(kids.count * 2u));
            ix->slots = arena_ptr->make_array<ObjIndex::Slot>(ix->cap);
            if (!ix->slots) {
                ix->cap = 0;
                return;
            }

            std::memset(ix->slots, 0, sizeof(ObjIndex::Slot) * ix->cap);
            kids.index = ix;

            const std::uint32_t mask = ix->cap - 1u;

            const std::uint32_t parent_idx = tape_index_of(t, n_);
            std::uint32_t cur = parent_idx + 1u;

            for (std::uint32_t i = 0; i < kids.count; ++i) {
                if (cur >= t->size)
                    break;

                Node* child = t->tape + cur;
                const std::uint32_t h = child->key_hash != kInvalidHash ? child->key_hash : hash32(child->key.data(), child->key.size());
                child->key_hash = h;

                std::uint32_t pos = hash_bucket(h, mask);
                for (;;) {
                    auto& s = ix->slots[pos];
                    if (s.h == ObjIndex::kEmpty) {
                        s.h = h;
                        s.pos = cur; // tape index
                        ++ix->size;
                        break;
                    }
                    pos = (pos + 1u) & mask;
                }

                cur = subtree_next_index(t, cur);
            }
        }

        template <class Sink>
        friend class JsonWriterCore;
        template <class Sink, detail::StringEscapePolicy Policy>
        friend class detail::JsonWriterCoreImpl;

        Node* n_ {};
        const DomContext* ctx_ {};
        DomContext ctx_storage_ {};
    };

    struct ValueRef::ArrayIter {
        const TapeStorage* tape {};
        Node* const* items {};
        std::uint32_t cur_idx {};
        std::uint32_t cur_pos {};
        std::uint32_t remaining {};
        const DomContext* ctx {};

        UJSON_FORCEINLINE ValueRef operator*() const noexcept {
            if (remaining == 0)
                return {};
            if (tape && tape_ready(tape)) {
                if (cur_idx >= tape->size)
                    return {};
                return ctx ? ValueRef {tape->tape + cur_idx, ctx} : ValueRef {};
            }
            if (!items)
                return {};
            return ctx ? ValueRef {items[cur_pos], ctx} : ValueRef {};
        }

        UJSON_FORCEINLINE ArrayIter& operator++() noexcept {
            if (!remaining)
                return *this;
            if (tape && tape_ready(tape)) {
                cur_idx = subtree_next_index(tape, cur_idx);
            } else {
                ++cur_pos;
            }
            --remaining;
            return *this;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool operator!=(const ArrayIter& o) const noexcept {
            return remaining != o.remaining;
        }
    };

    struct ValueRef::ArrayRange {
        const TapeStorage* tape {};
        Node* const* items {};
        std::uint32_t start_idx {};
        std::uint32_t count {};
        const DomContext* ctx {};

        UJSON_FORCEINLINE ArrayIter begin() const noexcept {
            return {.tape = tape, .items = items, .cur_idx = start_idx, .cur_pos = 0, .remaining = count, .ctx = ctx};
        }
        UJSON_FORCEINLINE ArrayIter end() const noexcept {
            return {.tape = tape, .items = items, .cur_idx = start_idx, .cur_pos = count, .remaining = 0, .ctx = ctx};
        }
    };

    struct ValueRef::ObjIter {
        struct Member {
            std::string_view key;
            ValueRef value;
        };

        const TapeStorage* tape {};
        Node* const* items {};
        std::uint32_t cur_idx {};
        std::uint32_t cur_pos {};
        std::uint32_t remaining {};
        const DomContext* ctx {};

        UJSON_FORCEINLINE Member operator*() const noexcept {
            if (remaining == 0)
                return {};

            if (tape && tape_ready(tape)) {
                if (cur_idx >= tape->size)
                    return {};
                Node* child = tape->tape + cur_idx;
                return child ? Member {.key = child->key, .value = ctx ? ValueRef {child, ctx} : ValueRef {}} : Member {};
            }

            if (!items)
                return {};

            Node* child = items[cur_pos];
            return child ? Member {.key = child->key, .value = ctx ? ValueRef {child, ctx} : ValueRef {}} : Member {};
        }

        UJSON_FORCEINLINE ObjIter& operator++() noexcept {
            if (!remaining)
                return *this;

            if (tape && tape_ready(tape)) {
                cur_idx = subtree_next_index(tape, cur_idx);
            } else {
                ++cur_pos;
            }

            --remaining;
            return *this;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool operator!=(const ObjIter& o) const noexcept {
            return remaining != o.remaining;
        }
    };

    struct ValueRef::ObjRange {
        const TapeStorage* tape {};
        Node* const* items {}; // pointer-DOM fallback
        std::uint32_t start_idx {};
        std::uint32_t count {};
        const DomContext* ctx {};

        UJSON_FORCEINLINE ObjIter begin() const noexcept {
            return {.tape = tape, .items = items, .cur_idx = start_idx, .cur_pos = 0, .remaining = count, .ctx = ctx};
        }

        UJSON_FORCEINLINE ObjIter end() const noexcept {
            return {.tape = tape, .items = items, .cur_idx = start_idx, .cur_pos = count, .remaining = 0, .ctx = ctx};
        }
    };

    UJSON_FORCEINLINE ValueRef::ArrayRange ValueRef::items() const noexcept {
        if (!n_ || n_->type != Type::Array)
            return {};
        const auto& k = n_->data.kids;
        const auto* t = tape_storage(arena());
        if (!tape_ready(t))
            return ArrayRange {.tape = nullptr, .items = k.items, .start_idx = 0, .count = k.count, .ctx = ctx_};

        const std::uint32_t parent_idx = tape_index_of(t, n_);
        return ArrayRange {.tape = t, .items = nullptr, .start_idx = parent_idx + 1u, .count = k.count, .ctx = ctx_};
    }

    UJSON_FORCEINLINE ValueRef::ObjRange ValueRef::members() const noexcept {
        if (!n_ || n_->type != Type::Object)
            return {};

        const auto& k = n_->data.kids;
        const auto* t = tape_storage(arena());

        if (!tape_ready(t)) {
            return ObjRange {.tape = nullptr, .items = k.items, .start_idx = 0, .count = k.count, .ctx = ctx_};
        }

        const std::uint32_t parent_idx = tape_index_of(t, n_);
        return ObjRange {.tape = t, .items = nullptr, .start_idx = parent_idx + 1u, .count = k.count, .ctx = ctx_};
    }

    template <class H>
    concept ParserHandler = requires(H& h) {
        { h.on_null() } -> std::same_as<bool>;
        { h.on_bool(true) } -> std::same_as<bool>;
        { h.on_integer(std::int64_t {}) } -> std::same_as<bool>;
        { h.on_number(double {}) } -> std::same_as<bool>;
        { h.on_string(std::string_view {}) } -> std::same_as<bool>;
        { h.on_key(std::string_view {}) } -> std::same_as<bool>;
        { h.on_array_begin() } -> std::same_as<bool>;
        { h.on_array_end() } -> std::same_as<bool>;
        { h.on_object_begin() } -> std::same_as<bool>;
        { h.on_object_end() } -> std::same_as<bool>;

        { h.arena() } -> std::same_as<Arena&>;
    };

    template <bool MaterializeStrings, bool StrictUtf8, ParserHandler Handler>
    class CoreParser {
    public:
        CoreParser(Handler& h, const std::string_view in, const std::uint32_t max_depth = kDefaultMaxDepth): h_(h), input_(in.data()), end_(in.data() + in.size()), max_depth_(max_depth) {
            err_.input = in;
        }
        CoreParser(Handler& h, const std::string_view in, const detail::StructuralIndex& si, const std::uint32_t max_depth = kDefaultMaxDepth)
            : h_(h), input_(in.data()), end_(in.data() + in.size()), max_depth_(max_depth), structural_(si) {
            err_.input = in;
        }

        [[nodiscard]] ParseError parse_root() {
            if (!input_ || input_ == end_)
                return set_err(ErrorCode::UnexpectedEOF, input_);

            if (structural_.count <= 0) {
                const auto len = static_cast<std::size_t>(end_ - input_);
                structural_ = detail::build_structural_index(input_, len, arena());
                if (structural_.oom)
                    return set_err(ErrorCode::WriterOverflow, input_);
                if (!structural_.positions || structural_.count == 0)
                    return set_err(ErrorCode::UnexpectedEOF, input_);
            }

            idx_pos_ = 0;
            stack_size_ = 0;
            auto root_done = false;

            while (true) {
                if (stack_size_ == 0) {
                    if (!root_done) {
                        [[maybe_unused]] const auto prev_idx_pos = idx_pos_;
                        if (!parse_value())
                            return err_;
                        assert(idx_pos_ != prev_idx_pos || !err_.ok());
                        if (stack_size_ == 0)
                            root_done = true;
                        continue;
                    }

                    if (idx_pos_ < structural_.count)
                        return set_err(ErrorCode::TrailingGarbage, input_ + structural_.positions[idx_pos_]);
                    return err_;
                }

                [[maybe_unused]] const auto prev_idx_pos = idx_pos_;
                if (!process_frame(root_done))
                    return err_;
                assert(idx_pos_ != prev_idx_pos || !err_.ok());
            }
        }

    private:
        enum class State : std::uint8_t {
            ExpectValueOrEnd,
            ExpectValue,
            ExpectKeyOrEnd,
            ExpectKey,
            ExpectColon,
            ExpectCommaOrEnd
        };

        struct Frame {
            std::uint8_t value {};

            static constexpr std::uint8_t type_mask = 0b0000'0111;
            static constexpr std::uint8_t state_mask = 0b0011'1000;

            Frame() = default;

            constexpr Frame(const Type t, const State s) noexcept {
                type(t);
                state(s);
            }

            [[nodiscard]] constexpr Type type() const noexcept {
                return static_cast<Type>(value & type_mask);
            }

            [[nodiscard]] constexpr State state() const noexcept {
                return static_cast<State>((value & state_mask) >> 3);
            }

            constexpr void type(Type t) noexcept {
                value = (value & ~type_mask) | (static_cast<std::uint8_t>(t) & type_mask);
            }

            constexpr void state(State s) noexcept {
                value = static_cast<uint8_t>((value & ~state_mask) | ((static_cast<std::uint8_t>(s) & type_mask) << 3));
            }
        };

        static_assert(sizeof(Frame) == 1);
        static_assert(std::is_trivially_copyable_v<Frame>);

        [[nodiscard]] UJSON_FORCEINLINE ParseError set_err(const ErrorCode c, const char* at = nullptr) const {
            err_.set(c, at ? at : cur_);
            return err_;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool fail(const ErrorCode c, const char* at = nullptr) const {
            (void)set_err(c, at);
            return false;
        }

        [[nodiscard]] UJSON_FORCEINLINE Arena& arena() {
            return h_.arena();
        }

        [[nodiscard]] bool ensure_stack(const std::uint32_t need) {
            if (need <= stack_cap_)
                return true;

            std::uint32_t new_cap = stack_cap_ ? stack_cap_ * 2u : 32u;
            while (new_cap < need)
                new_cap *= 2u;

            new_cap = std::min(new_cap, max_depth_ ? max_depth_ : 1u);
            if (new_cap < need)
                return fail(ErrorCode::DepthExceeded, cur_);

            Frame* ns = arena().template make_array<Frame>(new_cap);
            if (!ns)
                return fail(ErrorCode::WriterOverflow, cur_);

            if (stack_ && stack_size_)
                std::memcpy(ns, stack_, stack_size_ * sizeof(Frame));

            stack_ = ns;
            stack_cap_ = new_cap;
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool push_frame(const Frame& frame) {
            if (!ensure_stack(stack_size_ + 1u))
                return false;
            stack_[stack_size_++] = frame;
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool advance_token(char& out_c, std::uint32_t& out_pos) {
            if (idx_pos_ >= structural_.count) {
                (void)set_err(ErrorCode::UnexpectedEOF, end_);
                return false;
            }

            out_pos = structural_.positions[idx_pos_];
            cur_ = input_ + out_pos;
            out_c = *cur_;
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE static const char* trim_end(const char* p, const char* e) noexcept {
            while (e > p && detail::is_ws_u8(static_cast<unsigned char>(e[-1])))
                --e;
            return e;
        }

        [[nodiscard]]
        bool parse_string_raw(std::string_view& out_sv, const std::uint32_t open_pos, const std::uint32_t close_pos) {
            if (open_pos >= close_pos || input_[open_pos] != '"' || input_[close_pos] != '"')
                return fail(ErrorCode::InvalidString, input_ + open_pos);

            const char* const start = input_ + open_pos + 1;
            const char* const end = input_ + close_pos;

            // fast path: ascii & unicode
            if (const char* first_bad = detail::scan_ascii(start, end); first_bad == end) {
                const auto len = static_cast<std::size_t>(end - start);

                if constexpr (StrictUtf8) {
                    if (!detail::validate_utf8_segment(start, end))
                        return fail(ErrorCode::InvalidString, input_ + open_pos);
                }

                if constexpr (MaterializeStrings) {
                    char* dst = h_.string_buffer(len + 1);
                    if (!dst)
                        return fail(ErrorCode::InvalidString, input_ + open_pos);

                    std::memcpy(dst, start, len);
                    dst[len] = '\0';
                    out_sv = {dst, len};
                } else {
                    out_sv = {start, len};
                }

                return true;
            }

            // slow path: escape

            const auto max_len = static_cast<std::size_t>(end - start);

            char* dst = h_.string_buffer(max_len + 1);
            if (!dst)
                return fail(ErrorCode::InvalidString, input_ + open_pos);

            std::size_t len = 0;
            const char* p = start;

            while (p < end) {

                // bulk ASCII copy
                if (const char* chunk_end = detail::scan_ascii(p, end); chunk_end > p) {
                    const auto n = static_cast<std::size_t>(chunk_end - p);

                    std::memcpy(dst + len, p, n);
                    len += n;
                    p = chunk_end;

                    if (p == end)
                        break;
                }

                const auto c = static_cast<unsigned char>(*p++);

                if (c == '\\') {

                    const char* slash_pos = p - 1;

                    if (p >= end)
                        return fail(ErrorCode::InvalidEscape, slash_pos);

                    const char* esc_pos = p;
                    const auto esc = static_cast<unsigned char>(*p++);

                    if (!detail::CharMask256::test(detail::kEscapeFollowMask, esc))
                        return fail(ErrorCode::InvalidEscape, esc_pos);

                    switch (esc) {

                    case '"':
                        dst[len++] = '"';
                        break;
                    case '\\':
                        dst[len++] = '\\';
                        break;
                    case '/':
                        dst[len++] = '/';
                        break;
                    case 'b':
                        dst[len++] = '\b';
                        break;
                    case 'f':
                        dst[len++] = '\f';
                        break;
                    case 'n':
                        dst[len++] = '\n';
                        break;
                    case 'r':
                        dst[len++] = '\r';
                        break;
                    case 't':
                        dst[len++] = '\t';
                        break;

                    case 'u': {

                        if (end - p < 4)
                            return fail(ErrorCode::InvalidUnicode, p);

                        const char* u1_pos = p;

                        std::uint16_t u1 {};
                        if (!detail::hex4_to_u16(p, u1))
                            return fail(ErrorCode::InvalidUnicode, u1_pos);

                        p += 4;

                        std::uint32_t cp = u1;

                        if (u1 >= 0xD800u && u1 <= 0xDBFFu) {

                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                                return fail(ErrorCode::InvalidUnicode, p);

                            p += 2;

                            const char* u2_pos = p;

                            std::uint16_t u2 {};
                            if (!detail::hex4_to_u16(p, u2))
                                return fail(ErrorCode::InvalidUnicode, u2_pos);

                            p += 4;

                            if (u2 < 0xDC00u || u2 > 0xDFFFu)
                                return fail(ErrorCode::InvalidUnicode, u2_pos);

                            cp = 0x10000u + (((u1 - 0xD800u) << 10) | (u2 - 0xDC00u));
                        } else if (u1 >= 0xDC00u && u1 <= 0xDFFFu) {
                            return fail(ErrorCode::InvalidUnicode, u1_pos);
                        }

                        const std::size_t wrote = detail::utf8_encode(dst + len, cp);

                        if (!wrote)
                            return fail(ErrorCode::InvalidUnicode, u1_pos);

                        len += wrote;
                        break;
                    }

                    default:
                        return fail(ErrorCode::InvalidEscape, esc_pos);
                    }

                    continue;
                }

                if (c == '"' || c < 0x20u)
                    return fail(ErrorCode::InvalidString, input_ + open_pos);

                dst[len++] = static_cast<char>(c);
            }

            dst[len] = '\0';

            if constexpr (StrictUtf8) {
                if (!detail::validate_utf8_segment(dst, dst + len))
                    return fail(ErrorCode::InvalidString, input_ + open_pos);
            }

            out_sv = {dst, len};
            return true;
        }

        [[nodiscard]] bool parse_null_value(const std::uint32_t start_pos, const char* end_pos) {
            const char* start = input_ + start_pos;
            const char* const end = trim_end(start, end_pos);
            if (static_cast<std::size_t>(end - start) != 4 || std::memcmp(start, "null", 4) != 0)
                return fail(ErrorCode::UnexpectedToken, input_ + start_pos);

            ++idx_pos_;
            if (!h_.on_null())
                return fail(ErrorCode::BuilderInvalidState, input_ + start_pos);
            return true;
        }

        [[nodiscard]] bool parse_bool_value(const std::uint32_t start_pos, const char* end_pos, const bool value) {
            const char* start = input_ + start_pos;
            const char* const end = trim_end(start, end_pos);
            const char* lit = value ? "true" : "false";
            const std::size_t len = value ? 4u : 5u;

            if (std::cmp_not_equal(end - start, len) || std::memcmp(start, lit, len) != 0)
                return fail(ErrorCode::UnexpectedToken, input_ + start_pos);

            ++idx_pos_;
            if (!h_.on_bool(value))
                return fail(ErrorCode::BuilderInvalidState, input_ + start_pos);
            return true;
        }

        [[nodiscard]] bool parse_number_value(const std::uint32_t start_pos, const char* end_pos) {
            const char* start = input_ + start_pos;
            const char* const end = trim_end(start, end_pos);
            if (start >= end)
                return fail(ErrorCode::InvalidNumber, input_ + start_pos);

            const char* p = start;
            auto neg = false;
            if (*p == '-') {
                neg = true;
                ++p;
                if (p >= end)
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);
            }

            if (*p == '0') {
                ++p;
                if (p < end) {
                    if (const auto d = static_cast<unsigned>(*p - '0'); d <= 9)
                        return fail(ErrorCode::InvalidNumber, input_ + start_pos);
                }
            } else {
                const auto d = static_cast<unsigned>(*p - '0');
                if (d > 9)
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);

                constexpr auto max_pos = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                constexpr std::uint64_t max_neg = max_pos + 1ull;

                const std::uint64_t limit = neg ? max_neg : max_pos;
                const std::uint64_t cutoff = limit / 10;
                const auto cutlim = static_cast<unsigned>(limit % 10);

                std::uint64_t val = d;
                ++p;

                while (p < end) {
                    const auto digit = static_cast<unsigned>(*p - '0');
                    if (digit > 9)
                        break;

                    if (val > cutoff || (val == cutoff && digit > cutlim))
                        goto fallback_double;

                    val = val * 10 + digit;
                    ++p;
                }

                if (p != end) {
                    if (const char c = *p; c == '.' || c == 'e' || c == 'E')
                        goto fallback_double;
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);
                }

                const std::int64_t iv = neg ? -static_cast<std::int64_t>(val) : static_cast<std::int64_t>(val);
                ++idx_pos_;
                if (!h_.on_integer(iv))
                    return fail(ErrorCode::BuilderInvalidState, input_ + start_pos);
                return true;
            }

            if (p != end) {
                const char c = *p;
                if (c == '.' || c == 'e' || c == 'E')
                    goto fallback_double;
                return fail(ErrorCode::InvalidNumber, input_ + start_pos);
            }

            ++idx_pos_;
            if (!h_.on_integer(0))
                return fail(ErrorCode::BuilderInvalidState, input_ + start_pos);
            return true;

fallback_double:
            p = start;
            if (*p == '-')
                ++p;
            if (p >= end)
                return fail(ErrorCode::InvalidNumber, input_ + start_pos);

            if (*p == '0') {
                ++p;
            } else {
                if (const auto digit = static_cast<unsigned>(*p - '0'); digit > 9)
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);
                while (p < end) {
                    if (const auto digit = static_cast<unsigned>(*p - '0'); digit > 9)
                        break;
                    ++p;
                }
            }

            if (p < end && *p == '.') {
                ++p;
                if (p >= end)
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);
                if (const auto digit = static_cast<unsigned>(*p - '0'); digit > 9)
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);
                while (p < end) {
                    if (const auto digit = static_cast<unsigned>(*p - '0'); digit > 9)
                        break;
                    ++p;
                }
            }

            if (p < end && (*p == 'e' || *p == 'E')) {
                ++p;
                if (p < end && (*p == '+' || *p == '-'))
                    ++p;
                if (p >= end)
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);
                if (const auto digit = static_cast<unsigned>(*p - '0'); digit > 9)
                    return fail(ErrorCode::InvalidNumber, input_ + start_pos);
                while (p < end) {
                    if (const auto digit = static_cast<unsigned>(*p - '0'); digit > 9)
                        break;
                    ++p;
                }
            }

            if (p != end)
                return fail(ErrorCode::InvalidNumber, input_ + start_pos);

            double dv {};
            if (const char* fast_end = nullptr; detail::parse_double_fast(start, end, dv, fast_end) && fast_end == end) {
                ++idx_pos_;
                if (!h_.on_number(dv))
                    return fail(ErrorCode::BuilderInvalidState, input_ + start_pos);
                return true;
            }

            if (const auto [ptr, ec] = std::from_chars(start, end, dv); ec != std::errc {} || ptr != end)
                return fail(ErrorCode::InvalidNumber, input_ + start_pos);

            ++idx_pos_;
            if (!h_.on_number(dv))
                return fail(ErrorCode::BuilderInvalidState, input_ + start_pos);
            return true;
        }

        [[nodiscard]] bool parse_value() {
            char c {};
            std::uint32_t pos {};
            if (!advance_token(c, pos))
                return false;
            return parse_value_at(c, pos);
        }

        [[nodiscard]] UJSON_FORCEINLINE bool parse_value_at(const char c, const std::uint32_t pos) {
            switch (c) {
            case '{':
                if (depth_ == max_depth_)
                    return fail(ErrorCode::DepthExceeded, input_ + pos);
                ++idx_pos_;
                if (!h_.on_object_begin())
                    return fail(ErrorCode::BuilderInvalidState, input_ + pos);
                ++depth_;
                return push_frame(Frame {Type::Object, State::ExpectKeyOrEnd});

            case '[':
                if (depth_ == max_depth_)
                    return fail(ErrorCode::DepthExceeded, input_ + pos);
                ++idx_pos_;
                if (!h_.on_array_begin())
                    return fail(ErrorCode::BuilderInvalidState, input_ + pos);
                ++depth_;
                return push_frame(Frame {Type::Array, State::ExpectValueOrEnd});

            case '"': {
                if (idx_pos_ + 1 >= structural_.count)
                    return fail(ErrorCode::InvalidString, input_ + pos);
                const auto close_pos = structural_.positions[idx_pos_ + 1];
                std::string_view sv;
                if (!parse_string_raw(sv, pos, close_pos))
                    return false;
                idx_pos_ += 2;
                if (!h_.on_string(sv))
                    return fail(ErrorCode::BuilderInvalidState, input_ + pos);
                return true;
            }

            case 'n': {
                const char* end_pos = (idx_pos_ + 1 < structural_.count) ? (input_ + structural_.positions[idx_pos_ + 1]) : end_;
                return parse_null_value(pos, end_pos);
            }
            case 't': {
                const char* end_pos = (idx_pos_ + 1 < structural_.count) ? (input_ + structural_.positions[idx_pos_ + 1]) : end_;
                return parse_bool_value(pos, end_pos, true);
            }
            case 'f': {
                const char* end_pos = (idx_pos_ + 1 < structural_.count) ? (input_ + structural_.positions[idx_pos_ + 1]) : end_;
                return parse_bool_value(pos, end_pos, false);
            }

            default:
                if (c == '}' || c == ']' || c == ':' || c == ',')
                    return fail(ErrorCode::UnexpectedToken, input_ + pos);

                if (c != '-' && static_cast<unsigned>(c - '0') > 9u)
                    return fail(ErrorCode::UnexpectedToken, input_ + pos);

                const char* end_pos = (idx_pos_ + 1 < structural_.count) ? (input_ + structural_.positions[idx_pos_ + 1]) : end_;
                return parse_number_value(pos, end_pos);
            }
        }

        [[nodiscard]] bool close_container(bool& root_done, const Type type) {
            if (type == Type::Array) {
                if (!h_.on_array_end())
                    return fail(ErrorCode::BuilderInvalidState, cur_);
            } else if (type == Type::Object) {
                if (!h_.on_object_end())
                    return fail(ErrorCode::BuilderInvalidState, cur_);
            }

            if (depth_)
                --depth_;
            if (stack_size_)
                --stack_size_;
            if (stack_size_ == 0)
                root_done = true;
            return true;
        }

        [[nodiscard]] bool process_frame(bool& root_done) {
            if (!stack_ || stack_size_ == 0)
                return fail(ErrorCode::BuilderPopUnderflow, cur_);
            auto& frame = stack_[stack_size_ - 1u];
            char c {};
            std::uint32_t pos {};
            if (!advance_token(c, pos))
                return false;

            // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
            switch (frame.state()) {
            case State::ExpectKeyOrEnd:
                if (c == '}') {
                    ++idx_pos_;
                    return close_container(root_done, frame.type());
                }
                [[fallthrough]];
            case State::ExpectKey: {
                const auto close_pos = structural_.positions[idx_pos_ + 1];
                std::string_view key;
                if (!parse_string_raw(key, pos, close_pos))
                    return false;
                idx_pos_ += 2;
                if (!h_.on_key(key))
                    return fail(ErrorCode::BuilderInvalidState, input_ + pos);
                frame.state(State::ExpectColon);
                return true;
            }
            case State::ExpectColon:
                if (c != ':')
                    return fail(ErrorCode::UnexpectedToken, input_ + pos);
                ++idx_pos_;
                frame.state(State::ExpectValue);
                return true;
            case State::ExpectValueOrEnd: {
                if (frame.type() == Type::Array && c == ']') {
                    ++idx_pos_;
                    return close_container(root_done, frame.type());
                }
                if (!parse_value_at(c, pos))
                    return false;
                frame.state(State::ExpectCommaOrEnd);
                return true;
            }
            case State::ExpectValue:
                if (!parse_value_at(c, pos))
                    return false;
                frame.state(State::ExpectCommaOrEnd);
                return true;
            case State::ExpectCommaOrEnd:
                if (frame.type() == Type::Array) {
                    if (c == ',') {
                        ++idx_pos_;
                        frame.state(State::ExpectValue);
                        return true;
                    }
                    if (c == ']') {
                        ++idx_pos_;
                        return close_container(root_done, frame.type());
                    }
                    return fail(ErrorCode::UnexpectedToken, input_ + pos);
                }

                if (frame.type() == Type::Object) {
                    if (c == ',') {
                        ++idx_pos_;
                        frame.state(State::ExpectKey);
                        return true;
                    }
                    if (c == '}') {
                        ++idx_pos_;
                        return close_container(root_done, frame.type());
                    }
                    return fail(ErrorCode::UnexpectedToken, input_ + pos);
                }
                return fail(ErrorCode::UnexpectedToken, input_ + pos);
            }

            return fail(ErrorCode::UnexpectedToken, input_ + pos);
        }

        Handler& h_; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

        const char* input_ {};
        const char* end_ {};
        const char* cur_ {};

        mutable ParseError err_ {};

        std::uint32_t depth_ {0};
        std::uint32_t max_depth_ {kDefaultMaxDepth};

        detail::StructuralIndex structural_ {};
        std::uint32_t idx_pos_ {};

        Frame inline_stack_[64];
        Frame* stack_ = inline_stack_;
        std::uint32_t stack_size_ {};
        std::uint32_t stack_cap_ {64};
    };

    class SaxDomHandler {
    public:
        explicit SaxDomHandler(DomContext ctx, const std::uint32_t max_depth = kDefaultMaxDepth): max_depth_(max_depth ? max_depth : 1u), ctx_(ctx) {
            sp_ = 0;
            root_ = nullptr;

            tape_ = tape_storage(ctx_.arena);

            stack_cap_ = 12;
            stack_ = ctx_.arena ? ctx_.arena->make_array<Frame>(stack_cap_) : nullptr;
            if (!stack_) {
                set_err(ErrorCode::WriterOverflow);
                stack_cap_ = 0;
            }
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* root() const noexcept {
            return root_;
        }

        [[nodiscard]] UJSON_FORCEINLINE Arena& arena() const noexcept {
            return *ctx_.arena;
        }

        [[nodiscard]] UJSON_FORCEINLINE char* string_buffer(const std::size_t n) const {
            return ctx_.arena ? static_cast<char*>(ctx_.arena->alloc(n, 1)) : nullptr;
        }

        UJSON_FORCEINLINE bool on_null() {
            return push_value(Type::Null);
        }

        UJSON_FORCEINLINE bool on_bool(const bool v) {
            Node* n = make_value_node(Type::Bool);
            if (!n)
                return false;
            n->data.b = v;
            return attach(n);
        }

        UJSON_FORCEINLINE bool on_integer(const std::int64_t v) {
            Node* n = make_value_node(Type::Number);
            if (!n)
                return false;
            n->data.num = Number::from_i64(v);
            return attach(n);
        }

        UJSON_FORCEINLINE bool on_number(const double v) {
            Node* n = make_value_node(Type::Number);
            if (!n)
                return false;
            n->data.num = Number::from_double(v);
            return attach(n);
        }

        UJSON_FORCEINLINE bool on_string(const std::string_view s) {
            Node* n = make_value_node(Type::String);
            if (!n)
                return false;
            n->data.str = s;
            return attach(n);
        }

        UJSON_FORCEINLINE bool on_key(const std::string_view k) {
            pending_key_ = k;
            has_pending_key_ = true;
            return true;
        }

        UJSON_FORCEINLINE bool on_array_begin() {
            return enter_container(Type::Array);
        }

        UJSON_FORCEINLINE bool on_array_end() {
            return leave_container();
        }

        UJSON_FORCEINLINE bool on_object_begin() {
            return enter_container(Type::Object);
        }

        UJSON_FORCEINLINE bool on_object_end() {
            return leave_container();
        }

        [[nodiscard]] UJSON_FORCEINLINE bool ok() const noexcept {
            return err_.ok();
        }

        [[nodiscard]] UJSON_FORCEINLINE const ParseError& error() const noexcept {
            return err_;
        }

    private:
        struct Frame {
            std::uint32_t index {};
            Node* node {};
        };

        UJSON_FORCEINLINE void set_err(const ErrorCode c) const noexcept {
            if (err_.ok())
                err_.set(c, nullptr);
        }

        [[nodiscard]] bool ensure_stack(const std::uint32_t need) {
            if (!stack_) {
                set_err(ErrorCode::WriterOverflow);
                return false;
            }

            if (need <= stack_cap_)
                return true;

            std::uint32_t new_cap = stack_cap_ ? stack_cap_ * 2u : 32u;
            while (new_cap < need)
                new_cap *= 2u;

            // stack stores frames for open containers; max frames == max_depth_
            new_cap = std::min(new_cap, max_depth_);

            if (new_cap < need) {
                set_err(ErrorCode::DepthExceeded);
                return false;
            }

            Frame* ns = ctx_.arena ? ctx_.arena->make_array<Frame>(new_cap) : nullptr;
            if (!ns) {
                set_err(ErrorCode::WriterOverflow);
                return false;
            }

            if (sp_)
                std::memcpy(ns, stack_, sp_ * sizeof(Frame));

            stack_ = ns;
            stack_cap_ = new_cap;
            return true;
        }
        [[nodiscard]] UJSON_FORCEINLINE Node* alloc_node() const {
            if (!tape_) {
                Node* n = ctx_.arena ? ctx_.arena->make<Node>() : nullptr;
                if (!n)
                    set_err(ErrorCode::WriterOverflow);
                return n;
            }

            if (tape_->pos >= tape_->size) {
                if (!grow_tape())
                    return nullptr;
            }

            const uint32_t idx = tape_->pos++;
            Node* n = tape_->tape + idx;

            n->next = idx + 1u;
            n->key = {};
            n->key_hash = 0;
            return n;
        }

        [[nodiscard]] bool grow_tape() const {
            const uint32_t new_cap = tape_->size ? tape_->size * 2u : 256u;

            Node* new_buf = ctx_.arena ? ctx_.arena->make_array<Node>(new_cap) : nullptr;
            if (!new_buf) {
                set_err(ErrorCode::WriterOverflow);
                return false;
            }

            if (tape_->tape && tape_->pos)
                std::memcpy(new_buf, tape_->tape, tape_->pos * sizeof(Node));

            tape_->tape = new_buf;
            tape_->size = new_cap;
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE std::uint32_t node_index(const Node* n) const {
            assert(tape_ && tape_->tape);
            return static_cast<std::uint32_t>(n - tape_->tape);
        }

        [[nodiscard]] UJSON_FORCEINLINE bool append_child(Node* parent, Node* child) {
            if (!parent || !Node::is_container(parent->type))
                return false;

            if (parent->type == Type::Object) {
                if (!has_pending_key_) {
                    set_err(ErrorCode::BuilderMissingKey);
                    return false;
                }
                std::string_view normalized;
                if (!detail::normalize_ascii(pending_key_, ctx_.fmt, ctx_.arena, key_scratch_, normalized)) {
                    set_err(ErrorCode::WriterOverflow);
                    return false;
                }

                if (normalized.data() >= key_scratch_.buf && normalized.data() < key_scratch_.buf + sizeof(key_scratch_.buf)) {
                    auto* dst = ctx_.arena ? static_cast<char*>(ctx_.arena->alloc(normalized.size() + 1u, 1u)) : nullptr;
                    if (!dst) {
                        set_err(ErrorCode::WriterOverflow);
                        return false;
                    }
                    std::memcpy(dst, normalized.data(), normalized.size());
                    dst[normalized.size()] = '\0';
                    normalized = std::string_view {dst, normalized.size()};
                }

                child->key = normalized;
                child->key_hash = hash32(normalized.data(), normalized.size());
                pending_key_ = {};
                has_pending_key_ = false;
            }

            auto& kids = parent->data.kids;

            if (tape_) {
                ++kids.count;
                return true;
            }

            if (!kids.items) {
                constexpr auto kInitCap = 16u;
                kids.items = ctx_.arena ? ctx_.arena->make_array<Node*>(kInitCap) : nullptr;
                if (!kids.items) {
                    set_err(ErrorCode::WriterOverflow);
                    return false;
                }
                kids.capacity = kInitCap;
            } else if (kids.count >= kids.capacity) {
                const std::uint32_t new_cap = kids.capacity ? kids.capacity * 2u : 16u;
                Node** new_items = ctx_.arena ? ctx_.arena->make_array<Node*>(new_cap) : nullptr;
                if (!new_items) {
                    set_err(ErrorCode::WriterOverflow);
                    return false;
                }
                if (kids.count)
                    std::memcpy(new_items, kids.items, kids.count * sizeof(Node*));
                kids.items = new_items;
                kids.capacity = new_cap;
            }

            kids.items[kids.count++] = child;
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* make_value_node(const Type t) const {
            if (!ok())
                return nullptr;

            Node* n = alloc_node();
            if (!n)
                return nullptr;

            n->type = t;
            n->key = {};
            n->key_hash = 0;

            return n;
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* make_container(const Type t) const {
            Node* n = alloc_node();
            if (!n)
                return nullptr;

            n->type = t;
            n->key = {};
            n->key_hash = 0;

            n->data.kids.count = 0;
            n->data.kids.capacity = 0;
            n->data.kids.index = nullptr;
            n->data.kids.items = nullptr;
            return n;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool attach(Node* n) {
            if (!ok() || !n)
                return false;

            if (sp_ == 0) {
                if (root_) {
                    set_err(ErrorCode::BuilderInvalidState);
                    return false;
                }
                root_ = n;
                return true;
            }

            const auto& parent_frame = stack_[sp_ - 1];
            Node* parent = tape_ ? tape_->tape + parent_frame.index : parent_frame.node;

            if (!parent || !Node::is_container(parent->type)) {
                set_err(ErrorCode::BuilderInvalidState);
                return false;
            }

            return append_child(parent, n);
        }

        [[nodiscard]] UJSON_FORCEINLINE bool push_value(const Type t) {
            Node* n = make_value_node(t);
            if (!n)
                return false;
            return attach(n);
        }

        [[nodiscard]] bool enter_container(const Type t) {
            if (!ok())
                return false;

            if (sp_ + 1u > max_depth_) {
                set_err(ErrorCode::DepthExceeded);
                return false;
            }

            if (!ensure_stack(sp_ + 1u))
                return false;

            Node* n = make_container(t);
            if (!n)
                return false;

            if (!attach(n))
                return false;

            stack_[sp_++] = tape_ ? Frame {.index = node_index(n), .node = nullptr} : Frame {.index = 0, .node = n};
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool leave_container() {
            if (!ok())
                return false;

            if (sp_ == 0) {
                set_err(ErrorCode::BuilderPopUnderflow);
                return false;
            }

            if (tape_) {
                if (const auto& [i, _] = stack_[sp_ - 1]; i < tape_->size) {
                    Node* node = tape_->tape + i;
                    node->next = tape_->pos;
                }
            }

            --sp_;

            pending_key_ = {};
            has_pending_key_ = false;

            return true;
        }

    private:
        mutable ParseError err_;

        std::uint32_t max_depth_ {};
        Node* root_ {};

        TapeStorage* tape_ {};
        Frame* stack_ {};
        std::uint32_t sp_ {};
        std::uint32_t stack_cap_ {};

        std::string_view pending_key_ {};
        bool has_pending_key_ {};

        mutable detail::KeyScratch key_scratch_ {};
        DomContext ctx_ {};
    };

    template <class T>
    concept HasCStringData = requires(T t) {
        { std::as_const(t).data() } -> std::convertible_to<const char*>;
        { t.size() } -> std::convertible_to<std::size_t>;
    };

    template <bool ShouldCopyInput = false, bool MaterializeStrings = false, bool StrictUtf8 = true>
    class TDocument : public ArenaHolder {
    public:
        TDocument() = default;

        [[nodiscard]] static TDocument parse(HasCStringData auto&& input, Arena& a, const std::uint32_t max_depth = kDefaultMaxDepth) {
            TDocument doc(a);

            if constexpr (ShouldCopyInput) {
                doc.input_own_.assign(input.data(), input.size());
                doc.input_view_ = std::string_view {doc.input_own_.data(), doc.input_own_.size()};
            } else {
                doc.input_view_ = std::string_view {input.data(), input.size()};
            }

            doc.parse(max_depth);
            return doc;
        }

        template <AllocatorLike Allocator>
        [[nodiscard]] static TDocument parse(HasCStringData auto&& input, Allocator& alloc, const std::uint32_t max_depth = kDefaultMaxDepth) {
            TDocument doc(alloc);

            if constexpr (ShouldCopyInput) {
                doc.input_own_.assign(input.data(), input.size());
                doc.input_view_ = std::string_view {doc.input_own_.data(), doc.input_own_.size()};
            } else {
                doc.input_view_ = std::string_view {input.data(), input.size()};
            }

            doc.parse(max_depth);
            return doc;
        }

        [[nodiscard]] static TDocument parse(HasCStringData auto&& input, const std::uint32_t max_depth = kDefaultMaxDepth) {
            TDocument doc(true);

            if constexpr (ShouldCopyInput) {
                doc.input_own_.assign(input.data(), input.size());
                doc.input_view_ = std::string_view {doc.input_own_.data(), doc.input_own_.size()};
            } else {
                doc.input_view_ = std::string_view {input.data(), input.size()};
            }

            doc.parse(max_depth);
            return doc;
        }

        template <size_t N>
        [[nodiscard]] static TDocument parse(const char (&input)[N], Arena& a, const std::uint32_t max_depth = 512) {
            TDocument doc(a);
            doc.input_view_ = std::string_view {input, (N ? N - 1 : 0)};
            doc.parse(max_depth);
            return doc;
        }

        template <AllocatorLike Allocator, size_t N>
        [[nodiscard]] static TDocument parse(const char (&input)[N], Allocator& alloc, const std::uint32_t max_depth = 512) {
            TDocument doc(alloc);
            doc.input_view_ = std::string_view {input, (N ? N - 1 : 0)};
            doc.parse(max_depth);
            return doc;
        }
        template <size_t N>
        [[nodiscard]] static TDocument parse(const char (&input)[N], const std::uint32_t max_depth = 512) {
            TDocument doc(true);
            doc.input_view_ = std::string_view {input, (N ? N - 1 : 0)};
            doc.parse(max_depth);
            return doc;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool ok() const noexcept {
            return root_ != nullptr && err_.code == ErrorCode::None;
        }

        [[nodiscard]] UJSON_FORCEINLINE const ParseError& error() const noexcept {
            return err_;
        }

        [[nodiscard]] UJSON_FORCEINLINE std::string_view input() const noexcept {
            return input_view_;
        }

        [[nodiscard]] UJSON_FORCEINLINE ValueRef root() const noexcept {
            return ValueRef {root_, &ctx_};
        }

    private:
        using ArenaHolder::ArenaHolder;

        bool parse(uint32_t max_depth) {

            const auto si = detail::build_structural_index(input_view_.data(), input_view_.size(), arena());
            if (si.oom) {
                err_.set(ErrorCode::WriterOverflow, nullptr);
                return false;
            }

            if (!si.positions || !si.count) {
                err_.set(ErrorCode::UnexpectedEOF, nullptr);
                return false;
            }

            auto* storage = arena().template make<TapeStorage>();
            if (!storage) {
                err_.set(ErrorCode::WriterOverflow, nullptr);
                return false;
            }

            storage->size = si.count;
            storage->pos = 0;
            storage->tape = arena().template make_array<Node>(storage->size);
            if (!storage->tape) {
                err_.set(ErrorCode::WriterOverflow, nullptr);
                return false;
            }

            arena().tape_storage_ = storage;

            ctx_.arena = &arena();
            ctx_.fmt = init_key_format(arena(), detail::key_format::None);

            SaxDomHandler builder(ctx_, max_depth);
            CoreParser<MaterializeStrings, StrictUtf8, SaxDomHandler> p(builder, input_view_, si, max_depth);

            err_ = p.parse_root();
            if (!builder.ok())
                err_ = builder.error();

            if (!err_.ok())
                return false;

            root_ = builder.root();
            return true;
        }

        Node* root_ {};
        ParseError err_ {};
        std::string_view input_view_ {};
        std::string input_own_ {};
        DomContext ctx_ {};
    };

    // default production document
    using Document = TDocument<>;

    // zero-copy view document
    using DocumentView = TDocument<>;

    // owned input document
    using DocumentOwnedInput = TDocument<true>;

    // fully owning (input + strings)
    using DocumentFull = TDocument<true, true>;

    // fastest possible
    using DocumentFast = TDocument<false, false, false>;

    namespace detail {
        enum class StringEscapePolicy : std::uint8_t {
            Escape,
            PreEscaped
        };

#ifndef UJSON_PRETTY_INDENT
        inline constexpr auto kPrettyIndent = 2;
#else
        inline constexpr int kPrettyIndent = UJSON_PRETTY_INDENT;
#endif
        static_assert(kPrettyIndent > 0, "UJSON_PRETTY_INDENT must be positive");

        template <class Sink, StringEscapePolicy Policy>
        class JsonWriterCoreImpl {
        public:
            JsonWriterCoreImpl(Sink sink, const bool pretty, const std::size_t max_depth = 512, ParseError* err = nullptr): sink_(std::move(sink)), pretty_(pretty), max_depth_(max_depth), err_(err) {
                if (err_)
                    *err_ = {};
            }

            [[nodiscard]] UJSON_FORCEINLINE bool write(const ValueRef v) {
                tape_ = tape_storage(v.arena());
                if (tape_ready(tape_))
                    return write_tape_nonrec(v.raw());
                return write_ptr_recursive(v.raw());
            }

            [[nodiscard]] UJSON_FORCEINLINE auto finish() {
                return sink_.finish();
            }

        private:
            UJSON_FORCEINLINE void set_err(const ErrorCode c) const {
                if (err_ && err_->ok())
                    err_->set(c, nullptr);
            }

            [[nodiscard]] UJSON_FORCEINLINE bool fail_overflow() const noexcept {
                set_err(ErrorCode::WriterOverflow);
                return false;
            }

            [[nodiscard]] UJSON_FORCEINLINE bool put(char c) {
                if (!sink_.put(c))
                    return fail_overflow();
                return true;
            }

            [[nodiscard]] UJSON_FORCEINLINE bool puts(std::string_view s) {
                if (!sink_.puts(s))
                    return fail_overflow();
                return true;
            }

            [[nodiscard]] UJSON_FORCEINLINE bool newline() {
                if (!pretty_)
                    return true;

                if (!put('\n'))
                    return false;

                for (auto i = 0; i < indent_; ++i) {
                    if (!put(' '))
                        return false;
                }
                return true;
            }

            [[nodiscard]] bool write_string(const std::string_view s) {
                if constexpr (Policy == StringEscapePolicy::PreEscaped) {
                    if (!put('"'))
                        return false;
                    if (!s.empty() && !puts(s))
                        return false;
                    return put('"');
                } else {
                    if (!put('"'))
                        return false;

                    const char* p = s.data();
                    const char* e = p + s.size();

                    while (p < e) {
                        if (const char* q = detail::find_escape_in_string(p, e); q > p) {
                            if (!puts(std::string_view {p, static_cast<std::size_t>(q - p)}))
                                return false;
                            p = q;
                            if (p >= e)
                                break;
                        }

                        const auto c = static_cast<unsigned char>(*p++);

                        if (!detail::CharMask256::test(detail::kEscapeMask, c)) {
                            if (!put(static_cast<char>(c)))
                                return false;
                            continue;
                        }

                        switch (c) {
                        case '"': // NOLINT(bugprone-branch-clone)
                            if (!puts("\\\""))
                                return false;
                            break;
                        case '\\':
                            if (!puts("\\\\"))
                                return false;
                            break;
                        case '\b':
                            if (!puts("\\b"))
                                return false;
                            break;
                        case '\f':
                            if (!puts("\\f"))
                                return false;
                            break;
                        case '\n':
                            if (!puts("\\n"))
                                return false;
                            break;
                        case '\r':
                            if (!puts("\\r"))
                                return false;
                            break;
                        case '\t':
                            if (!puts("\\t"))
                                return false;
                            break;
                        default: {
                            char tmp[6];
                            tmp[0] = '\\';
                            tmp[1] = 'u';
                            tmp[2] = '0';
                            tmp[3] = '0';
                            tmp[4] = detail::kHexDigits[(c >> 4) & 0xF];
                            tmp[5] = detail::kHexDigits[c & 0xF];
                            if (!puts(std::string_view {tmp, 6}))
                                return false;
                            break;
                        }
                        }
                    }

                    return put('"');
                }
            }

            [[nodiscard]] bool write_i64(const std::int64_t v) {
                char tmp[32];
                auto [p, ec] = std::to_chars(tmp, tmp + sizeof(tmp), v);
                if (ec != std::errc {}) {
                    set_err(ErrorCode::ToCharsFailed);
                    return false;
                }
                return puts(std::string_view {tmp, static_cast<std::size_t>(p - tmp)});
            }

            [[nodiscard]] bool write_double(const double d) {
                if (!std::isfinite(d)) {
                    set_err(ErrorCode::ToCharsFailed);
                    return false;
                }
                char tmp[32];
                auto [p, ec] = std::to_chars(tmp, tmp + sizeof(tmp), d);
                if (ec != std::errc {}) {
                    set_err(ErrorCode::ToCharsFailed);
                    return false;
                }
                return puts(std::string_view {tmp, static_cast<std::size_t>(p - tmp)});
            }

            [[nodiscard]] UJSON_FORCEINLINE bool write_number(const Number& n) {
                return (n.kind == NumberKind::Integer) ? write_i64(n.i) : write_double(n.d);
            }

            [[nodiscard]] UJSON_FORCEINLINE bool write_primitive(const Node* n) {
                switch (n ? n->type : Type::Null) {
                case Type::Null:
                    return puts("null");
                case Type::Bool:
                    return puts(n->data.b ? "true" : "false");
                case Type::Number:
                    return write_number(n->data.num);
                case Type::String:
                    return write_string(n->data.str);
                case Type::Array:
                case Type::Object:
                    break;
                }
                return true;
            }

            struct Frame {
                Type type {};
                std::uint32_t cur_idx {};
                std::uint32_t remaining {};
                std::uint32_t count {};
                bool emitted_any {};
            };

            [[nodiscard]] bool open_container(const Node* n) {
                if (depth_ + 1 > max_depth_) {
                    set_err(ErrorCode::EncodeDepthExceeded);
                    return false;
                }
                ++depth_;
                indent_ += kPrettyIndent;

                if (n->type == Type::Array)
                    return put('[');
                return put('{');
            }

            [[nodiscard]] bool close_container(const Type t, const std::uint32_t count) {
                indent_ -= kPrettyIndent;
                --depth_;

                if (pretty_ && count)
                    if (!newline())
                        return false;

                return put(t == Type::Array ? ']' : '}');
            }

            [[nodiscard]] bool write_tape_nonrec(const Node* root) {
                if (!root)
                    return puts("null");

                if (!Node::is_container(root->type))
                    return write_primitive(root);

                const std::uint32_t root_idx = tape_index_of(tape_, root);

                if (depth_ + 1 > max_depth_) {
                    set_err(ErrorCode::EncodeDepthExceeded);
                    return false;
                }

                constexpr std::size_t kInlineDepth = 64;

                Frame inline_stack[kInlineDepth];
                Frame* stack = inline_stack;
                std::size_t capacity = kInlineDepth;
                std::size_t sp = 0;

                std::unique_ptr<Frame[]> heap_stack;

                auto ensure_capacity = [&](const std::size_t need) -> bool {
                    if (need <= capacity)
                        return true;

                    if (need > max_depth_) {
                        set_err(ErrorCode::EncodeDepthExceeded);
                        return false;
                    }

                    std::size_t new_cap = capacity * 2;
                    while (new_cap < need)
                        new_cap *= 2;

                    new_cap = std::min(new_cap, max_depth_);

                    heap_stack.reset(new (std::nothrow) Frame[new_cap]);
                    if (!heap_stack)
                        return fail_overflow();

                    std::memcpy(heap_stack.get(), stack, sp * sizeof(Frame));

                    stack = heap_stack.get();
                    capacity = new_cap;
                    return true;
                };

                if (!open_container(root))
                    return false;

                stack[sp++] = Frame {
                    .type = root->type,
                    .cur_idx = root_idx + 1u,
                    .remaining = root->data.kids.count,
                    .count = root->data.kids.count,
                    .emitted_any = false,
                };

                while (sp) {
                    Frame& f = stack[sp - 1];

                    if (f.remaining == 0) {
                        const Type t = f.type;
                        const std::uint32_t cnt = f.count;

                        --sp;

                        if (!close_container(t, cnt))
                            return false;

                        continue;
                    }

                    if (f.emitted_any) {
                        if (!put(','))
                            return false;
                    }

                    if (!newline())
                        return false;

                    if (f.cur_idx >= tape_->size)
                        return fail_overflow();

                    const Node* child = tape_->tape + f.cur_idx;

                    if (f.type == Type::Object) {
                        if (!write_string(child->key))
                            return false;

                        if (!put(':'))
                            return false;

                        if (pretty_ && !put(' '))
                            return false;
                    }

                    if (!Node::is_container(child->type)) {
                        if (!write_primitive(child))
                            return false;

                        f.cur_idx = subtree_next_index(tape_, f.cur_idx);
                        --f.remaining;
                        f.emitted_any = true;
                        continue;
                    }

                    const std::uint32_t child_idx = f.cur_idx;

                    f.cur_idx = subtree_next_index(tape_, f.cur_idx);
                    --f.remaining;
                    f.emitted_any = true;

                    if (!ensure_capacity(sp + 1))
                        return false;

                    if (!open_container(child))
                        return false;

                    stack[sp++] = Frame {
                        .type = child->type,
                        .cur_idx = child_idx + 1u,
                        .remaining = child->data.kids.count,
                        .count = child->data.kids.count,
                        .emitted_any = false,
                    };
                }

                return true;
            }

            [[nodiscard]] bool write_ptr_recursive(const Node* n) {
                if (!n)
                    return puts("null");

                if (!Node::is_container(n->type))
                    return write_primitive(n);

                if (depth_ + 1 > max_depth_) {
                    set_err(ErrorCode::EncodeDepthExceeded);
                    return false;
                }

                ++depth_;
                indent_ += kPrettyIndent;

                auto epilog = [this]() noexcept {
                    indent_ -= kPrettyIndent;
                    --depth_;
                };

                if (n->type == Type::Array) {
                    if (!put('[')) {
                        epilog();
                        return false;
                    }

                    const auto& k = n->data.kids;
                    if (!k.items && k.count) {
                        epilog();
                        return fail_overflow();
                    }

                    for (std::uint32_t i = 0; i < k.count; ++i) {
                        if (i && !put(',')) {
                            epilog();
                            return false;
                        }
                        if (!newline()) {
                            epilog();
                            return false;
                        }
                        if (!write_ptr_recursive(k.items[i])) {
                            epilog();
                            return false;
                        }
                    }

                    if (k.count && !newline()) {
                        epilog();
                        return false;
                    }
                    if (!put(']')) {
                        epilog();
                        return false;
                    }
                    epilog();
                    return true;
                }

                if (!put('{')) {
                    epilog();
                    return false;
                }

                const auto& k = n->data.kids;
                if (!k.items && k.count) {
                    epilog();
                    return fail_overflow();
                }

                for (std::uint32_t i = 0; i < k.count; ++i) {
                    if (i && !put(',')) {
                        epilog();
                        return false;
                    }
                    if (!newline()) {
                        epilog();
                        return false;
                    }

                    const Node* child = k.items[i];
                    if (!child) {
                        epilog();
                        return fail_overflow();
                    }

                    if (!write_string(child->key)) {
                        epilog();
                        return false;
                    }
                    if (!put(':')) {
                        epilog();
                        return false;
                    }
                    if (pretty_ && !put(' ')) {
                        epilog();
                        return false;
                    }

                    if (!write_ptr_recursive(child)) {
                        epilog();
                        return false;
                    }
                }

                if (k.count && !newline()) {
                    epilog();
                    return false;
                }
                if (!put('}')) {
                    epilog();
                    return false;
                }
                epilog();
                return true;
            }

            Sink sink_;
            bool pretty_ {};
            int indent_ {};

            std::size_t max_depth_ {};
            std::size_t depth_ {};

            TapeStorage* tape_ {};
            ParseError* err_ {};
        };
    } // namespace detail

    template <class Sink>
    class JsonWriterCore {
    public:
        JsonWriterCore(Sink sink, const bool pretty, const std::size_t max_depth = 512, ParseError* err = nullptr): impl_(std::move(sink), pretty, max_depth, err) { }

        [[nodiscard]] UJSON_FORCEINLINE bool write(const ValueRef v) {
            return impl_.write(v);
        }

        [[nodiscard]] UJSON_FORCEINLINE auto finish() {
            return impl_.finish();
        }

    private:
        detail::JsonWriterCoreImpl<Sink, detail::StringEscapePolicy::Escape> impl_;
    };

    struct FixedBufferSink {
        char* buf {};
        std::size_t cap {};
        std::size_t pos {};

        [[nodiscard]] UJSON_FORCEINLINE bool put(const char c) {
            if (pos >= cap)
                return false;
            buf[pos++] = c;
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool puts(const std::string_view s) {
            if (pos + s.size() > cap)
                return false;
            std::memcpy(buf + pos, s.data(), s.size());
            pos += s.size();
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool puts(const char* s) {
            return puts(std::string_view {s});
        }

        [[nodiscard]] UJSON_FORCEINLINE std::string_view finish() const {
            return {buf, pos};
        }
    };

    struct StringSink {
        std::string out;

        [[nodiscard]] UJSON_FORCEINLINE bool put(const char c) {
            out.push_back(c);
            return true;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool puts(const std::string_view s) {
            out.append(s);
            return true;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool puts(const char* s) {
            out.append(s);
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE std::string finish() {
            return std::move(out);
        }
    };

    class FixedWriter {
    public:
        explicit FixedWriter(Arena& a, const std::size_t cap = 1 << 20, const bool pretty = false, ParseError* err = nullptr)
            : core_(FixedBufferSink {.buf = static_cast<char*>(a.alloc(cap, 1)), .cap = cap, .pos = 0}, pretty, 512, err) { }

        [[nodiscard]] UJSON_FORCEINLINE bool write(const ValueRef& v) {
            return core_.write(v);
        }

        [[nodiscard]] UJSON_FORCEINLINE std::string_view finish() {
            return core_.finish();
        }

    private:
        JsonWriterCore<FixedBufferSink> core_;
    };

    [[nodiscard]] UJSON_FORCEINLINE std::string encode(const ValueRef& v, const bool pretty, ParseError* err) {
        JsonWriterCore core(StringSink {}, pretty, 512, err);
        if (!core.write(v))
            return {};
        return core.finish();
    }

    [[nodiscard]] UJSON_FORCEINLINE std::string encode(const ValueRef& v, const bool pretty = false) {
        return encode(v, pretty, nullptr);
    }

    struct ValidateHandler : ArenaHolder {
        using ArenaHolder::ArenaHolder;

        ValidateHandler(): ValidateHandler {true} { }

        [[nodiscard]] Arena& arena() const {
            return *arena_;
        }

        [[nodiscard]] UJSON_FORCEINLINE char* string_buffer(const std::size_t n) const {
            return static_cast<char*>(arena_->alloc(n, 1));
        }

        static bool on_null() {
            return true;
        }
        static bool on_bool(bool) {
            return true;
        }
        static bool on_integer(std::int64_t) {
            return true;
        }
        static bool on_number(double) {
            return true;
        }
        static bool on_string(std::string_view) {
            return true;
        }
        static bool on_key(std::string_view) {
            return true;
        }
        static bool on_array_begin() {
            return true;
        }
        static bool on_array_end() {
            return true;
        }
        static bool on_object_begin() {
            return true;
        }
        static bool on_object_end() {
            return true;
        }
    };

    template <bool StrictUtf8 = true>
    UJSON_FORCEINLINE ParseError validate(HasCStringData auto&& input, const std::uint32_t max_depth = 512) {
        ValidateHandler h {};
        CoreParser<false, StrictUtf8, ValidateHandler> p(h, std::string_view {input.data(), input.size()}, max_depth);
        return p.parse_root();
    }

    template <bool StrictUtf8 = true, size_t N = 0>
    UJSON_FORCEINLINE ParseError validate(const char (&input)[N], const std::uint32_t max_depth = 512) {
        return validate<StrictUtf8>(std::string_view {input, (N ? N - 1 : 0)}, max_depth);
    }

    template <class Handler>
    class SaxParser : public ArenaHolder {
    public:
        explicit SaxParser(Handler& h, const std::string_view s, const std::uint32_t max_depth = 512, const detail::key_format fmt = detail::key_format::None): ArenaHolder {true}, h_(h) {
            init(s, max_depth, fmt);
        }

        explicit SaxParser(Handler& h, const std::string_view s, Arena& a, const std::uint32_t max_depth = 512, const detail::key_format fmt = detail::key_format::None): ArenaHolder {a}, h_(h) {
            init(s, max_depth, fmt);
        }

        template <HasCStringData T>
        [[nodiscard]] static SaxParser parse(Handler& h, T&& s, const std::uint32_t max_depth = 512, const detail::key_format fmt = detail::key_format::None) {
            return SaxParser {h, std::string_view {s.data(), s.size()}, max_depth, fmt};
        }

        template <HasCStringData T>
        [[nodiscard]] static SaxParser parse(Handler& h, T&& s, Arena& a, const std::uint32_t max_depth = 512, const detail::key_format fmt = detail::key_format::None) {
            return SaxParser {h, std::string_view {s.data(), s.size()}, a, max_depth, fmt};
        }

        template <AllocatorLike Allocator, HasCStringData T>
        [[nodiscard]] static SaxParser parse(Handler& h, T&& s, Allocator& allocator, const std::uint32_t max_depth = 512, const detail::key_format fmt = detail::key_format::None) {
            return SaxParser {h, std::string_view {s.data(), s.size()}, allocator, max_depth, fmt};
        }

        template <std::size_t N>
        [[nodiscard]] static SaxParser parse(Handler& h, const char (&s)[N], const std::uint32_t max_depth = 512, const detail::key_format fmt = detail::key_format::None) {
            return SaxParser {h, std::string_view {s, (N ? N - 1 : 0)}, max_depth, fmt};
        }

        template <std::size_t N>
        [[nodiscard]] static SaxParser parse(Handler& h, const char (&s)[N], Arena& a, const std::uint32_t max_depth = 512, const detail::key_format fmt = detail::key_format::None) {
            return SaxParser {h, std::string_view {s, (N ? N - 1 : 0)}, a, max_depth, fmt};
        }

        template <bool MaterializeStrings = false, bool StrictUtf8 = true>
        UJSON_FORCEINLINE ParseError parse() {
            SaxAdapter a {h_, ctx_};
            CoreParser<MaterializeStrings, StrictUtf8, SaxAdapter> p(a, s_, max_depth_);
            return p.parse_root();
        }

    private:
        struct SaxAdapter;

        template <AllocatorLike Allocator>
        SaxParser(Handler& h, const std::string_view s, Allocator& allocator, const std::uint32_t max_depth, const detail::key_format fmt): ArenaHolder {allocator}, h_(h) {
            init(s, max_depth, fmt);
        }

        void init(const std::string_view s, const std::uint32_t max_depth, const detail::key_format fmt) {
            s_ = s;
            max_depth_ = max_depth;
            ctx_.arena = arena_;
            ctx_.fmt = init_key_format(*arena_, fmt);
        }

        Handler& h_; // NOLINT
        std::string_view s_;
        std::uint32_t max_depth_ {512};
        DomContext ctx_ {};
    };

    template <class Handler>
    struct SaxParser<Handler>::SaxAdapter {
        Handler& h;
        DomContext& ctx;
        detail::KeyScratch scratch;

        SaxAdapter(Handler& hh, DomContext& context): h(hh), ctx(context) { }

        [[nodiscard]] char* string_buffer(const std::size_t n) const {
            return ctx.arena ? static_cast<char*>(ctx.arena->alloc(n, 1)) : nullptr;
        }

        [[nodiscard]] Arena& arena() const {
            return *ctx.arena;
        }

        bool on_null() {
            return h.on_null();
        }
        bool on_bool(bool v) {
            return h.on_bool(v);
        }
        bool on_integer(std::int64_t v) {
            return h.on_integer(v);
        }
        bool on_number(double v) {
            return h.on_number(v);
        }
        bool on_string(std::string_view v) {
            return h.on_string(v);
        }
        bool on_key(std::string_view k) {
            std::string_view normalized;
            if (!detail::normalize_ascii(k, ctx.fmt, ctx.arena, scratch, normalized))
                return false;
            return h.on_key(normalized);
        }
        bool on_array_begin() {
            return h.on_array_begin();
        }
        bool on_array_end() {
            return h.on_array_end();
        }
        bool on_object_begin() {
            return h.on_object_begin();
        }
        bool on_object_end() {
            return h.on_object_end();
        }
    };

    class DomBuilder : public ArenaHolder {
    public:
        explicit DomBuilder(): ArenaHolder {true} {
            ctx_.arena = arena_;
            ctx_.fmt = init_key_format(*arena_, detail::key_format::None);
        }
        explicit DomBuilder(Arena& arena): ArenaHolder {arena} {
            ctx_.arena = arena_;
            ctx_.fmt = init_key_format(*arena_, detail::key_format::None);
        }

        template <AllocatorLike Allocator>
        explicit DomBuilder(Allocator& alloc): ArenaHolder {alloc} {
            ctx_.arena = arena_;
            ctx_.fmt = init_key_format(*arena_, detail::key_format::None);
        }

        class ObjectScope {
            DomBuilder& b_;

        public:
            explicit ObjectScope(DomBuilder& b): b_(b) { }
            ~ObjectScope() {
                if (b_.ok())
                    (void)b_.pop();
            }
        };

        class ArrayScope {
            DomBuilder& b_;

        public:
            explicit ArrayScope(DomBuilder& b): b_(b) { }
            ~ArrayScope() {
                if (b_.ok())
                    (void)b_.pop();
            }
        };

        template <class Fn>
            requires(std::invocable<Fn> || std::invocable<Fn, DomBuilder&>)
        Node* object(Fn&& fn) {
            if (!ok())
                return nullptr;

            Node* n = make_container(Type::Object);
            if (!n)
                return nullptr;
            if (!push(n))
                return nullptr;

            Node** self_slot = current_slot_;

            ObjectScope scope {*this};

            if (ok()) {
                if constexpr (std::invocable<Fn, DomBuilder&>)
                    std::forward<Fn>(fn)(*this);
                else
                    std::forward<Fn>(fn)();
            }

            return ok() ? (self_slot ? *self_slot : nullptr) : nullptr;
        }

        template <class Fn>
            requires(std::invocable<Fn> || std::invocable<Fn, DomBuilder&>)
        Node* array(Fn&& fn) {
            if (!ok())
                return nullptr;

            Node* n = make_container(Type::Array);
            if (!n)
                return nullptr;
            if (!push(n))
                return nullptr;

            Node** self_slot = current_slot_;

            ArrayScope scope {*this};

            if (ok()) {
                if constexpr (std::invocable<Fn, DomBuilder&>)
                    std::forward<Fn>(fn)(*this);
                else
                    std::forward<Fn>(fn)();
            }

            return ok() ? (self_slot ? *self_slot : nullptr) : nullptr;
        }

        class KeyProxy {
            DomBuilder& b_;

        public:
            KeyProxy(DomBuilder& b, const std::string_view k): b_(b) {
                if (!b_.ok()) {
                    b_.pending_key_ = {};
                    b_.has_pending_key_ = false;
                    return;
                }

                if (const Node* cur = b_.current_container(); !cur || cur->type != Type::Object) {
                    b_.pending_key_ = {};
                    b_.has_pending_key_ = false;
                    b_.set_err(ErrorCode::BuilderKeyOutsideObject);
                    return;
                }

                b_.pending_key_ = k;
                b_.has_pending_key_ = true;
            }

            template <class T>
            KeyProxy& operator=(T&& v) {
                if (!b_.ok())
                    return *this;
                b_.value(std::forward<T>(v));
                return *this;
            }

            KeyProxy& operator=(Node* n) {
                if (!b_.ok())
                    return *this;
                b_.attach(n);
                return *this;
            }
        };

        KeyProxy operator[](std::string_view k) {
            return {*this, k};
        }

        void value(std::nullptr_t) {
            if (!ok())
                return;
            if (Node* n = make_value_node(Type::Null))
                attach(n);
        }

        void value(const bool v) {
            if (!ok())
                return;
            Node* n = make_value_node(Type::Bool);
            if (!n)
                return;
            n->data.b = v;
            attach(n);
        }

        void value(const double v) {
            if (!ok())
                return;
            Node* n = make_value_node(Type::Number);
            if (!n)
                return;
            n->data.num = Number::from_double(v);
            attach(n);
        }

        void value(const std::int64_t v) {
            if (!ok())
                return;
            Node* n = make_value_node(Type::Number);
            if (!n)
                return;
            n->data.num = Number::from_i64(v);
            attach(n);
        }

        template <std::integral T>
        void value(T v) {
            if (!ok())
                return;
            if constexpr (std::is_signed_v<T>) {
                value(static_cast<std::int64_t>(v));
            } else {
                if (v <= static_cast<std::make_unsigned_t<std::int64_t>>(INT64_MAX))
                    value(static_cast<std::int64_t>(v));
                else
                    value(static_cast<double>(v));
            }
        }

        void value(const std::string_view s) {
            if (!ok())
                return;
            Node* n = make_value_node(Type::String);
            if (!n)
                return;

            n->data.str = s;
            attach(n);
        }

        void value(Node* n) {
            if (!ok())
                return;
            attach(n);
        }

        [[nodiscard]] Node* root() const noexcept {
            return root_;
        }
        [[nodiscard]] bool ok() const noexcept {
            return err_.code == ErrorCode::None;
        }
        [[nodiscard]] const ParseError& error() const noexcept {
            return err_;
        }

        [[nodiscard]] std::string encode(const bool pretty = false) const {
            if (!root_ || !ok())
                return {};
            return ujson::encode(ValueRef(root_, &ctx_), pretty);
        }

    private:
        UJSON_FORCEINLINE void set_err(const ErrorCode c) {
            if (err_.ok())
                err_.set(c, nullptr);

            pending_key_ = {};
            has_pending_key_ = false;
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* current_container() const noexcept {
            return current_slot_ ? *current_slot_ : nullptr;
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* make_value_node(const Type t) {
            Node* n = arena().make<Node>();
            if (!n) {
                set_err(ErrorCode::BuilderInvalidState);
                return nullptr;
            }
            n->type = t;
            n->key = {};
            n->key_hash = 0;
            return n;
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* make_container(const Type t, const std::uint32_t cap = 16) {
            Node* n = arena().make<Node>();
            if (!n) {
                set_err(ErrorCode::BuilderInvalidState);
                return nullptr;
            }

            Node** items = arena().make_array<Node*>(cap);
            if (!items) {
                set_err(ErrorCode::BuilderInvalidState);
                return nullptr;
            }

            n->type = t;
            n->key = {};
            n->data.kids.count = 0;
            n->data.kids.capacity = cap;
            n->next = 0;
            n->data.kids.index = nullptr;
            n->data.kids.items = items;
            return n;
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* grow_container(const Node* old) {
            const auto& ok = old->data.kids;
            const std::uint32_t new_cap = ok.capacity ? ok.capacity * 2 : 16;

            Node* n = make_container(old->type, new_cap);
            if (!n)
                return nullptr;

            n->key = old->key;
            n->key_hash = old->key_hash;
            n->data.kids.count = ok.count;
            if (ok.items && n->data.kids.items)
                std::memcpy(n->data.kids.items, ok.items, ok.count * sizeof(Node*));

            n->data.kids.index = nullptr;

            return n;
        }

        Node* append_child(Node* parent, Node* child) {

            if (const auto& k = parent->data.kids; k.count == k.capacity) {
                Node* bigger = grow_container(parent);
                if (!bigger)
                    return nullptr;
                parent = bigger;
            }

            if (parent->type == Type::Object) {
                if (!has_pending_key_) {
                    set_err(ErrorCode::BuilderMissingKey);
                    return nullptr;
                }
                std::string_view normalized;
                if (!detail::normalize_ascii(pending_key_, ctx_.fmt, ctx_.arena, key_scratch_, normalized)) {
                    set_err(ErrorCode::WriterOverflow);
                    return nullptr;
                }

                if (normalized.data() >= key_scratch_.buf && normalized.data() < key_scratch_.buf + sizeof(key_scratch_.buf)) {
                    auto* dst = static_cast<char*>(arena().alloc(normalized.size() + 1u, 1u));
                    if (!dst) {
                        set_err(ErrorCode::WriterOverflow);
                        return nullptr;
                    }
                    std::memcpy(dst, normalized.data(), normalized.size());
                    dst[normalized.size()] = '\0';
                    normalized = std::string_view {dst, normalized.size()};
                }

                child->key = normalized;
                child->key_hash = hash32(normalized.data(), normalized.size());
                pending_key_ = {};
                has_pending_key_ = false;
            }

            parent->data.kids.items[parent->data.kids.count++] = child;
            return parent;
        }

        UJSON_FORCEINLINE bool attach(Node* n) {
            Node** inserted_slot {};
            return attach(n, inserted_slot);
        }

        [[nodiscard]] UJSON_FORCEINLINE bool attach(Node* n, Node**& out_inserted_slot) {
            if (!root_) {
                root_ = n;
                out_inserted_slot = &root_;
                return true;
            }

            if (!current_slot_) {
                set_err(ErrorCode::BuilderInvalidState);
                return false;
            }

            Node* parent = *current_slot_;
            if (!parent || !Node::is_container(parent->type)) {
                set_err(ErrorCode::BuilderInvalidState);
                return false;
            }

            Node* new_parent = append_child(parent, n);
            if (!new_parent)
                return false;

            if (new_parent != parent)
                *current_slot_ = new_parent;

            out_inserted_slot = &new_parent->data.kids.items[new_parent->data.kids.count - 1];
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool push(Node* n) {
            Node** inserted_slot {};
            if (!attach(n, inserted_slot))
                return false;

            if (!ensure_stack(sp_ + 1))
                return false;

            stack_[sp_++] = current_slot_;
            current_slot_ = inserted_slot;
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool pop() {
            if (sp_ == 0) {
                set_err(ErrorCode::BuilderPopUnderflow);
                return false;
            }
            current_slot_ = stack_[--sp_];
            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool ensure_stack(const std::uint32_t need) {
            if (cap_ >= need)
                return true;

            std::uint32_t new_cap = cap_ ? cap_ * 2 : 32;
            while (new_cap < need)
                new_cap *= 2;

            Node*** ns = arena().make_array<Node**>(new_cap);
            if (!ns) {
                set_err(ErrorCode::BuilderInvalidState);
                return false;
            }

            if (stack_ && sp_)
                std::memcpy(ns, stack_, sp_ * sizeof(Node**));

            stack_ = ns;
            cap_ = new_cap;
            return true;
        }

        Node* root_ {};
        Node** current_slot_ {};
        Node*** stack_ {};

        std::uint32_t sp_ {};
        std::uint32_t cap_ {};

        std::string_view pending_key_ {};
        bool has_pending_key_ {};
        DomContext ctx_ {};
        mutable detail::KeyScratch key_scratch_ {};

        ParseError err_ {};
    };

} // namespace ujson

namespace ujson {

    enum class EraseMode : std::uint8_t {
        Stable,
        Fast
    };

    enum class StringPolicy : std::uint8_t {
        View,
        Copy
    };

    enum class EncodingPolicy : std::uint8_t {
        Utf8, // UTF-8 output (non-ASCII allowed)
        JsonEscaped, // \uXXXX for non-ASCII (ASCII-only output)
    };

    class ValueBuilder;

    class NodeRef {
    public:
        NodeRef() = default;

        [[nodiscard]] UJSON_FORCEINLINE bool ok() const noexcept;
        [[nodiscard]] UJSON_FORCEINLINE const ParseError& error() const noexcept;

        [[nodiscard]] UJSON_FORCEINLINE Node* raw() const noexcept {
            return slot_ ? *slot_ : nullptr;
        }

        [[nodiscard]] UJSON_FORCEINLINE Type type() const noexcept {
            const Node* n = raw();
            return n ? n->type : Type::Null;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool is_object() const noexcept {
            return type() == Type::Object;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_array() const noexcept {
            return type() == Type::Array;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_string() const noexcept {
            return type() == Type::String;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_number() const noexcept {
            return type() == Type::Number;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_bool() const noexcept {
            return type() == Type::Bool;
        }
        [[nodiscard]] UJSON_FORCEINLINE bool is_null() const noexcept {
            return type() == Type::Null;
        }

        UJSON_FORCEINLINE const NodeRef& set_object(std::uint32_t cap = 16) const;
        UJSON_FORCEINLINE const NodeRef& set_array(std::uint32_t cap = 16) const;

        template <class T>
        UJSON_FORCEINLINE NodeRef& operator=(T&& v);

        // object
        NodeRef operator[](std::string_view key) const; // create-missing as null
        [[nodiscard]] UJSON_FORCEINLINE NodeRef get(std::string_view key) const; // non-creating
        [[nodiscard]] UJSON_FORCEINLINE bool contains(std::string_view key) const;

        template <class T>
        NodeRef add(std::string_view key, T&& v);

        [[nodiscard]] NodeRef add_object(std::string_view key, std::uint32_t cap = 16) const;
        [[nodiscard]] NodeRef add_array(std::string_view key, std::uint32_t cap = 16) const;

        UJSON_FORCEINLINE bool erase(std::string_view key, EraseMode mode = EraseMode::Stable) const;

        // array
        UJSON_FORCEINLINE NodeRef operator[](std::size_t idx) const; // expand with nulls
        [[nodiscard]] UJSON_FORCEINLINE NodeRef at(std::size_t idx) const; // no expand

        template <class T>
        NodeRef add(T&& v);

        [[nodiscard]] UJSON_FORCEINLINE NodeRef add_object(std::uint32_t cap = 16) const;
        [[nodiscard]] UJSON_FORCEINLINE NodeRef add_array(std::uint32_t cap = 16) const;

        [[nodiscard]] std::uint32_t size() const noexcept {
            const Node* n = raw();
            return n && Node::is_container(n->type) ? n->data.kids.count : 0;
        }

    private:
        friend class ValueBuilder;

        ValueBuilder* b_ {};
        mutable Node** slot_ {};
        Node* parent_ {}; // parent container (null for root)

        explicit NodeRef(ValueBuilder* b, Node** slot, Node* parent): b_(b), slot_(slot), parent_(parent) { }
    };

    class ValueBuilder : public ArenaHolder {
    public:
        struct Options {
            StringPolicy strings = StringPolicy::Copy;
            EncodingPolicy encoding = EncodingPolicy::Utf8;
            detail::key_format key_format = detail::key_format::None;
            std::uint32_t max_depth = 512;
            float max_load = 0.70f;
        };

        explicit ValueBuilder(const Options opt = {}): ArenaHolder {true}, opt_(opt) {
            initialize();
        }

        explicit ValueBuilder(Arena& arena, const Options opt = {}): ArenaHolder {arena}, opt_(opt) {
            initialize();
        }

        template <AllocatorLike Allocator>
        explicit ValueBuilder(Allocator& alloc, const Options opt = {}): ArenaHolder {alloc}, opt_(opt) {
            initialize();
        }

        [[nodiscard]] UJSON_FORCEINLINE bool ok() const noexcept {
            return err_.code == ErrorCode::None;
        }

        [[nodiscard]] UJSON_FORCEINLINE const ParseError& error() const noexcept {
            return err_;
        }

        [[nodiscard]] UJSON_FORCEINLINE Arena& arena() const noexcept {
            return ArenaHolder::arena();
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* root_raw() const noexcept {
            return root_;
        }

        [[nodiscard]] UJSON_FORCEINLINE NodeRef root() noexcept {
            return NodeRef {this, &root_, nullptr};
        }

        [[nodiscard]] std::string encode(const bool pretty = false) const {
            if (!root_ || !ok())
                return {};

            detail::JsonWriterCoreImpl<StringSink, detail::StringEscapePolicy::PreEscaped> writer(StringSink {}, pretty, kDefaultMaxDepth, nullptr);
            if (!writer.write(ValueRef(root_, &ctx_)))
                return {};
            return writer.finish();
        }

    private:
        friend class NodeRef;

        void initialize() {
            ctx_.arena = arena_;
            ctx_.fmt = init_key_format(*arena_, opt_.key_format);
            root_ = arena().make<Node>();
            if (!root_) {
                err_.code = ErrorCode::WriterOverflow;
                err_.at = nullptr;
                return;
            }
            root_->type = Type::Null;
            root_->key = {};
            root_->key_hash = 0;
            root_->data.kids = {};
        }

        UJSON_FORCEINLINE void set_err(const ErrorCode c) noexcept {
            if (err_.ok())
                err_.set(c, nullptr);
        }

        UJSON_FORCEINLINE static std::uint32_t next_pow2(const std::uint32_t v) noexcept {
            if (v <= 1)
                return 1;
            return 1u << (32u - std::countl_zero(v - 1u));
        }

        [[nodiscard]] UJSON_FORCEINLINE static ObjIndex* obj_index(const Node* obj) noexcept {
            return obj && obj->type == Type::Object ? static_cast<ObjIndex*>(obj->data.kids.index) : nullptr;
        }

        [[nodiscard]] UJSON_FORCEINLINE std::string_view materialize(const std::string_view s) {
            if (opt_.strings == StringPolicy::View)
                return s;
            if (s.empty())
                return {};

            auto p = static_cast<char*>(arena().alloc(s.size() + 1, 1));
            if (!p) {
                set_err(ErrorCode::WriterOverflow);
                return {};
            }
            std::memcpy(p, s.data(), s.size());
            p[s.size()] = '\0';
            return {p, s.size()};
        }

        [[nodiscard]] UJSON_FORCEINLINE Node* make_value_node(const Type t) {
            Node* n = arena().make<Node>();
            if (!n) {
                set_err(ErrorCode::WriterOverflow);
                return nullptr;
            }
            n->type = t;
            n->key = {};
            n->key_hash = 0;
            return n;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool normalize_key_store(const std::string_view key, std::string_view& out) {
            if (!detail::normalize_ascii(key, ctx_.fmt, ctx_.arena, key_scratch_, out)) {
                set_err(ErrorCode::WriterOverflow);
                return false;
            }

            if (out.data() >= key_scratch_.buf && out.data() < key_scratch_.buf + sizeof(key_scratch_.buf)) {
                auto* dst = static_cast<char*>(arena().alloc(out.size() + 1u, 1u));
                if (!dst) {
                    set_err(ErrorCode::WriterOverflow);
                    return false;
                }
                std::memcpy(dst, out.data(), out.size());
                dst[out.size()] = '\0';
                out = std::string_view {dst, out.size()};
            }

            if (opt_.strings == StringPolicy::Copy && out.data() == key.data() && out.size() == key.size()) {
                out = materialize(out);
                if (out.data() == nullptr && !out.empty()) {
                    set_err(ErrorCode::WriterOverflow);
                    return false;
                }
            }

            return true;
        }

        [[nodiscard]] UJSON_FORCEINLINE bool normalize_key_lookup(const std::string_view key, detail::KeyScratch& scratch, std::string_view& out) const noexcept {
            return detail::normalize_ascii(key, ctx_.fmt, ctx_.arena, scratch, out);
        }

        [[nodiscard]] Node* make_container(const Type t, std::uint32_t cap) {
            cap = std::max(1u, cap);
            Node* n = arena().make<Node>();
            if (!n) {
                set_err(ErrorCode::WriterOverflow);
                return nullptr;
            }

            Node** items = arena().make_array<Node*>(cap);
            if (!items) {
                set_err(ErrorCode::WriterOverflow);
                return nullptr;
            }

            n->type = t;
            n->key = {};
            n->key_hash = 0;
            n->data.kids.count = 0;
            n->data.kids.capacity = cap;
            n->next = 0;
            n->data.kids.index = nullptr;
            n->data.kids.items = items;
            return n;
        }

        [[nodiscard]] Node* grow_container(const Node* old) {
            const auto& k = old->data.kids;
            const std::uint32_t new_cap = k.capacity ? k.capacity * 2u : 16u;

            Node* bigger = make_container(old->type, new_cap);
            if (!bigger)
                return nullptr;

            bigger->key = old->key;
            bigger->key_hash = old->key_hash;

            bigger->data.kids.count = k.count;
            if (k.items && bigger->data.kids.items)
                std::memcpy(bigger->data.kids.items, k.items, k.count * sizeof(Node*));

            bigger->data.kids.index = old->data.kids.index;
            return bigger;
        }

        bool ensure_container(const NodeRef& self, const Type t, const std::uint32_t cap) {
            if (!ok() || !normalize_slot(self))
                return false;

            const Node* cur = self.raw();
            if (cur && cur->type == t)
                return true;

            Node* n = make_container(t, cap);
            if (!n)
                return false;

            if (cur) {
                n->key = cur->key;
                n->key_hash = cur->key_hash;
            }

            *self.slot_ = n;
            return true;
        }

        void ensure_obj_index(Node* obj) {
            if (!ok() || !obj || obj->type != Type::Object)
                return;

            auto& kids = obj->data.kids;
            if (kids.index)
                return;

            ObjIndex* ix = arena().make<ObjIndex>();
            if (!ix) {
                set_err(ErrorCode::WriterOverflow);
                return;
            }

            const std::uint32_t cap = std::max(16u, next_pow2(kids.count * 2u));
            ix->cap = cap;
            ix->size = 0;
            ix->slots = arena().make_array<ObjIndex::Slot>(cap);
            if (!ix->slots) {
                set_err(ErrorCode::WriterOverflow);
                return;
            }

            std::memset(ix->slots, 0, sizeof(ObjIndex::Slot) * cap);

            kids.index = ix;

            const std::uint32_t mask = cap - 1u;
            for (std::uint32_t i = 0; i < kids.count; ++i) {
                Node* n = kids.items[i];
                if (!n)
                    continue;

                const std::uint32_t h = n->key_hash != kInvalidHash ? n->key_hash : hash32(n->key.data(), n->key.size());
                n->key_hash = h;

                std::uint32_t pos = h & mask;
                for (;;) {
                    // ReSharper disable once CppUseStructuredBinding
                    if (auto& s = ix->slots[pos]; s.h == ObjIndex::kEmpty) {
                        s.h = h;
                        s.pos = i;
                        ++ix->size;
                        break;
                    }
                    pos = (pos + 1u) & mask;
                }
            }
        }

        [[nodiscard]] static ObjIndex::Slot* objindex_find_slot(const Node* obj, const std::string_view key, const std::uint32_t h) noexcept {
            if (!obj || obj->type != Type::Object)
                return nullptr;
            const ObjIndex* ix = obj_index(obj);
            if (!ix || ix->cap == 0)
                return nullptr;

            const auto& kids = obj->data.kids;
            const std::uint32_t mask = ix->cap - 1u;
            std::uint32_t p = h & mask;

            for (std::uint32_t step = 0; step < ix->cap; ++step) {
                auto& s = ix->slots[p];
                if (s.h == ObjIndex::kEmpty)
                    return nullptr;

                if (s.h == h && s.pos != ObjIndex::kTomb && s.pos < kids.count) {
                    if (const Node* cand = kids.items[s.pos]; cand && cand->key == key)
                        return &s;
                }

                p = (p + 1u) & mask;
            }
            return nullptr;
        }

        void rebuild_index(Node* obj) {
            if (!ok() || !obj || obj->type != Type::Object)
                return;

            auto& kids = obj->data.kids;
            const std::uint32_t cap = std::max(16u, next_pow2(kids.count * 2u));

            ObjIndex* ix = arena().make<ObjIndex>();
            if (!ix) {
                set_err(ErrorCode::WriterOverflow);
                return;
            }

            ix->cap = cap;
            ix->size = 0;
            ix->slots = arena().make_array<ObjIndex::Slot>(cap);
            if (!ix->slots) {
                set_err(ErrorCode::WriterOverflow);
                return;
            }

            std::memset(ix->slots, 0, sizeof(ObjIndex::Slot) * cap);

            const std::uint32_t mask = cap - 1u;

            for (std::uint32_t i = 0; i < kids.count; ++i) {
                Node* n = kids.items[i];
                if (!n)
                    continue;

                const std::uint32_t h = n->key_hash != kInvalidHash ? n->key_hash : hash32(n->key.data(), n->key.size());
                n->key_hash = h;

                std::uint32_t pos = h & mask;
                while (ix->slots[pos].h != ObjIndex::kEmpty)
                    pos = (pos + 1u) & mask;

                ix->slots[pos].h = h;
                ix->slots[pos].pos = i;
                ++ix->size;
            }

            kids.index = ix;
        }

        void objindex_insert(Node* obj, Node* child) {
            if (!ok())
                return;
            ensure_obj_index(obj);
            ObjIndex* ix = obj_index(obj);
            if (!ix)
                return;

            if (ix->size + 1u > static_cast<std::uint32_t>(ix->cap * opt_.max_load)) {
                rebuild_index(obj);
                ix = obj_index(obj);
                if (!ix)
                    return;
            }

            const std::uint32_t h = child->key_hash != kInvalidHash ? child->key_hash : hash32(child->key.data(), child->key.size());
            child->key_hash = h;

            const std::uint32_t mask = ix->cap - 1u;
            std::uint32_t pos = h & mask;
            auto first_tomb = 0xFFFFFFFFu;

            for (std::uint32_t step = 0; step < ix->cap; ++step) {
                auto& s = ix->slots[pos];

                if (s.h == ObjIndex::kEmpty) {
                    auto& dst = first_tomb != 0xFFFFFFFFu ? ix->slots[first_tomb] : s;
                    dst.h = h;
                    const auto& kids = obj->data.kids;
                    const std::uint32_t child_pos = kids.count ? kids.count - 1u : 0u;
                    dst.pos = child_pos;
                    ++ix->size;
                    return;
                }

                if (s.pos == ObjIndex::kTomb && first_tomb == 0xFFFFFFFFu)
                    first_tomb = pos;

                pos = (pos + 1u) & mask;
            }

            rebuild_index(obj);
            objindex_insert(obj, child);
        }

        UJSON_FORCEINLINE static void objindex_update_pos_if_present(const Node* obj, const std::string_view key, const std::uint32_t h, const std::uint32_t new_pos) noexcept {
            if (ObjIndex::Slot* s = objindex_find_slot(obj, key, h))
                s->pos = new_pos;
        }

        UJSON_FORCEINLINE static void objindex_erase(const Node* obj, const std::string_view key, const std::uint32_t h) {
            ObjIndex* ix = obj_index(obj);
            if (!ix)
                return;

            if (ObjIndex::Slot* s = objindex_find_slot(obj, key, h)) {
                s->pos = ObjIndex::kTomb;
                if (ix->size)
                    --ix->size;
            }
        }

        struct FindRes {
            Node** slot {};
            std::uint32_t pos {};
            std::uint32_t h {};
            std::string_view key {};
            bool found {};
        };

        FindRes obj_find(Node* obj, const std::string_view key, detail::KeyScratch& scratch) {
            FindRes r {};
            if (!obj || obj->type != Type::Object)
                return r;

            std::string_view nk = key;
            if (!normalize_key_lookup(key, scratch, nk))
                return r;

            r.key = nk;
            r.h = hash32(nk.data(), nk.size());
            const auto& kids = obj->data.kids;

            if (kids.count >= 12)
                ensure_obj_index(obj);

            if (obj_index(obj)) {
                if (const ObjIndex::Slot* s = objindex_find_slot(obj, nk, r.h)) {
                    if (s->pos < kids.count) {
                        r.pos = s->pos;
                        r.slot = &kids.items[r.pos];
                        r.found = r.slot && *r.slot;
                        return r;
                    }

                    rebuild_index(obj);

                    if (const ObjIndex::Slot* s2 = objindex_find_slot(obj, nk, r.h)) {
                        if (s2->pos < kids.count) {
                            r.pos = s2->pos;
                            r.slot = &kids.items[r.pos];
                            r.found = r.slot && *r.slot;
                        }
                    }
                }
                return r;
            }

            for (std::uint32_t i = 0; i < kids.count; ++i) {
                if (const Node* cand = kids.items[i]; cand && cand->key_equals(r.h, nk)) {
                    r.slot = &kids.items[i];
                    r.pos = i;
                    r.found = true;
                    return r;
                }
            }

            return r;
        }

        Node** obj_append_slot(const NodeRef& self) {
            Node* obj = self.raw();
            if (!obj || obj->type != Type::Object) {
                set_err(ErrorCode::BuilderInvalidState);
                return nullptr;
            }

            if (obj->data.kids.count >= obj->data.kids.capacity) {
                Node* bigger = grow_container(obj);
                if (!bigger)
                    return nullptr;

                *self.slot_ = bigger;
                obj = bigger;
            }

            auto& k = obj->data.kids;
            return &k.items[k.count++];
        }

        Node** arr_append_slot(const NodeRef& self) {
            Node* arr = self.raw();
            if (!arr || arr->type != Type::Array) {
                set_err(ErrorCode::BuilderInvalidState);
                return nullptr;
            }

            if (arr->data.kids.count >= arr->data.kids.capacity) {
                Node* bigger = grow_container(arr);
                if (!bigger)
                    return nullptr;
                *self.slot_ = bigger;
                arr = bigger;
            }

            auto& k = arr->data.kids;
            return &k.items[k.count++];
        }

        NodeRef arr_push_null(const NodeRef& self) {
            Node** slot = arr_append_slot(self);
            if (!slot)
                return {};
            Node* nn = make_value_node(Type::Null);
            if (!nn)
                return {};
            *slot = nn;
            return NodeRef {this, slot, self.raw()};
        }

        NodeRef arr_get_or_expand(const NodeRef& self, const std::size_t idx) {
            if (!ok())
                return {};
            (void)self.set_array();
            Node* arr = self.raw();
            if (!arr)
                return {};

            while (arr->data.kids.count <= idx) {
                if (!arr_push_null(self).ok())
                    return {};
                arr = self.raw();
                if (!arr)
                    return {};
            }

            return NodeRef {this, &arr->data.kids.items[static_cast<std::uint32_t>(idx)], arr};
        }

        template <class T>
        [[nodiscard]] UJSON_FORCEINLINE Node* make_from_value(T&& v) { // NOLINT(cppcoreguidelines-missing-std-forward)
            using U = std::decay_t<std::remove_cvref_t<T>>;

            if constexpr (std::is_same_v<U, Node*>) {
                return v;

            } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
                return make_value_node(Type::Null);

            } else if constexpr (std::is_same_v<U, bool>) {
                Node* n = make_value_node(Type::Bool);
                if (!n)
                    return nullptr;
                n->data.b = v;
                return n;

            } else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, bool>) {
                Node* n = make_value_node(Type::Number);
                if (!n)
                    return nullptr;

                if constexpr (std::is_signed_v<U>) {
                    n->data.num = Number::from_i64(static_cast<std::int64_t>(v));
                } else {
                    if (v <= static_cast<std::make_unsigned_t<std::int64_t>>(INT64_MAX))
                        n->data.num = Number::from_i64(static_cast<std::int64_t>(v));
                    else
                        n->data.num = Number::from_double(static_cast<double>(v));
                }
                return n;

            } else if constexpr (std::is_floating_point_v<U>) {
                Node* n = make_value_node(Type::Number);
                if (!n)
                    return nullptr;
                n->data.num = Number::from_double(static_cast<double>(v));
                return n;

            } else if constexpr (concepts::string_like<U>) {
                Node* n = make_value_node(Type::String);
                if (!n)
                    return nullptr;

                // null pointer special-case: const char* / wchar_t* / etc.
                if constexpr (std::is_pointer_v<U>) {
                    if (!v) {
                        n->data.str = materialize_text(std::string_view {}); // empty
                        return n;
                    }
                }

                n->data.str = materialize_any_string(std::forward<T>(v));
                return n;

            } else {
                static_assert(!sizeof(U), "Unsupported value type for ValueBuilder");
                return nullptr;
            }
        }

        template <class T>
        void assign_value(NodeRef& self, T&& v) {
            if (!ok() || !normalize_slot(self))
                return;

            const Node* parent = self.parent_;
            const Node* old = self.raw();

            Node* nn = make_from_value(std::forward<T>(v));
            if (!nn)
                return;

            if (old) {
                nn->key = old->key;
                nn->key_hash = old->key_hash;
            }

            *self.slot_ = nn;

            // update index mapping if parent is object and key exists
            if (parent && parent->type == Type::Object && !nn->key.empty()) {
                const std::uint32_t h = nn->key_hash != kInvalidHash ? nn->key_hash : hash32(nn->key.data(), nn->key.size());
                nn->key_hash = h;
            }
        }

        template <class T>
        NodeRef obj_insert_or_assign(NodeRef& self, const std::string_view key, T&& v) {
            if (!ok() || !normalize_slot(self))
                return {};

            (void)self.set_object();
            Node* obj = self.raw();
            if (!obj)
                return {};

            detail::KeyScratch scratch;
            const FindRes f = obj_find(obj, key, scratch);
            if (f.found && f.slot && *f.slot) {
                const Node* old = *f.slot;

                Node* nn = make_from_value(std::forward<T>(v));
                if (!nn)
                    return {};

                nn->key = old->key;
                nn->key_hash = old->key_hash ? old->key_hash : f.h;

                *f.slot = nn;

                return NodeRef {this, f.slot, obj};
            }

            Node** slot = obj_append_slot(self);
            if (!slot)
                return {};

            obj = self.raw();
            if (!obj)
                return {};

            Node* nn = make_from_value(std::forward<T>(v));
            if (!nn)
                return {};

            std::string_view normalized;
            if (!normalize_key_store(key, normalized))
                return {};

            nn->key = normalized;
            nn->key_hash = hash32(normalized.data(), normalized.size());

            *slot = nn;
            objindex_insert(obj, nn);

            return NodeRef {this, slot, obj};
        }

        bool obj_erase(const NodeRef& self, const std::string_view key, const EraseMode mode) {
            if (!ok() || !normalize_slot(self))
                return false;

            Node* obj = self.raw();
            if (!obj || obj->type != Type::Object)
                return false;

            auto& k = obj->data.kids;
            if (k.count == 0)
                return false;

            detail::KeyScratch scratch;
            const FindRes f = obj_find(obj, key, scratch);
            if (!f.found || !f.slot || !*f.slot)
                return false;

            const Node* victim = *f.slot;
            const std::string_view nk = f.key.empty() ? key : f.key;
            const std::uint32_t h = victim->key_hash != kInvalidHash ? victim->key_hash : hash32(nk.data(), nk.size());

            objindex_erase(obj, nk, h);

            const std::uint32_t pos = f.pos;

            if (mode == EraseMode::Fast) {
                if (const std::uint32_t last = k.count - 1u; pos != last) {
                    Node* moved = k.items[last];
                    k.items[pos] = moved;
                    if (moved) {
                        const std::uint32_t mh = moved->key_hash != kInvalidHash ? moved->key_hash : hash32(moved->key.data(), moved->key.size());
                        moved->key_hash = mh;
                        objindex_update_pos_if_present(obj, moved->key, mh, pos);
                    }
                }
                --k.count;
            } else {
                for (std::uint32_t i = pos + 1; i < k.count; ++i)
                    k.items[i - 1] = k.items[i];
                --k.count;
                k.index = nullptr;
            }

            if (const ObjIndex* ix = obj_index(obj)) {
                if (ix->cap > 16u && ix->size < ix->cap / 4u)
                    rebuild_index(obj);
            }

            return true;
        }

    private:
        template <class S>
        [[nodiscard]] UJSON_FORCEINLINE std::string_view materialize_any_string(S&& s) {
            auto tv = detail::as_text(std::forward<S>(s)); // traits::text_view<CharT>
            auto v = tv.view(); // basic_string_view<CharT>
            return materialize_text(v); // Utf8 or JsonEscaped (arena/view)
        }

        template <class CharT>
        [[nodiscard]] UJSON_FORCEINLINE std::string_view materialize_utf8_bytes(std::basic_string_view<CharT> in) {
            // Utf8 output bytes, stored in arena if needed
            auto materialize_utf8_from_bytes = [&](const std::string_view sv) -> std::string_view {
                const bool needs_escape = detail::find_escape_in_string(sv.data(), sv.data() + sv.size()) != sv.data() + sv.size();
                const std::size_t cap = needs_escape ? sv.size() * 6 + 1 : sv.size() + 1;
                auto p = static_cast<char*>(arena().alloc(cap, 1));
                if (!p) {
                    set_err(ErrorCode::WriterOverflow);
                    return {};
                }

                if (needs_escape) {
                    std::size_t out_len = 0;
                    if (!detail::json_escape_utf8_control_chars(sv.data(), sv.data() + sv.size(), p, out_len)) {
                        set_err(ErrorCode::WriterOverflow);
                        return {};
                    }
                    p[out_len] = '\0';
                    return {p, out_len};
                }

                std::memcpy(p, sv.data(), sv.size());
                p[sv.size()] = '\0';
                return {p, sv.size()};
            };

            if constexpr (std::same_as<CharT, char>) {
                const std::string_view sv {in.data(), in.size()};
                const bool needs_escape = detail::find_escape_in_string(sv.data(), sv.data() + sv.size()) != sv.data() + sv.size();
                if (!needs_escape && opt_.strings == StringPolicy::View)
                    return sv;

                const std::size_t cap = needs_escape ? sv.size() * 6 + 1 : sv.size() + 1;
                auto p = static_cast<char*>(arena().alloc(cap, 1));
                if (!p) {
                    set_err(ErrorCode::WriterOverflow);
                    return {};
                }

                if (needs_escape) {
                    std::size_t out_len = 0;
                    if (!detail::json_escape_utf8_control_chars(sv.data(), sv.data() + sv.size(), p, out_len)) {
                        set_err(ErrorCode::WriterOverflow);
                        return {};
                    }
                    p[out_len] = '\0';
                    return {p, out_len};
                }

                std::memcpy(p, sv.data(), sv.size());
                p[sv.size()] = '\0';
                return {p, sv.size()};
#ifdef __cpp_char8_t
            } else if constexpr (std::same_as<CharT, char8_t>) {
                const std::string_view sv {reinterpret_cast<const char*>(in.data()), in.size()};
                const bool needs_escape = detail::find_escape_in_string(sv.data(), sv.data() + sv.size()) != sv.data() + sv.size();
                if (!needs_escape && opt_.strings == StringPolicy::View)
                    return sv;

                const std::size_t cap = needs_escape ? sv.size() * 6 + 1 : sv.size() + 1;
                auto p = static_cast<char*>(arena().alloc(cap, 1));
                if (!p) {
                    set_err(ErrorCode::WriterOverflow);
                    return {};
                }

                if (needs_escape) {
                    std::size_t out_len = 0;
                    if (!detail::json_escape_utf8_control_chars(sv.data(), sv.data() + sv.size(), p, out_len)) {
                        set_err(ErrorCode::WriterOverflow);
                        return {};
                    }
                    p[out_len] = '\0';
                    return {p, out_len};
                }

                std::memcpy(p, sv.data(), sv.size());
                p[sv.size()] = '\0';
                return {p, sv.size()};
#endif
            } else {
                if constexpr (std::same_as<CharT, char16_t> || std::same_as<CharT, char32_t>) {
                    auto all_bytes = true;
                    for (const auto ch : in) {
                        if (static_cast<std::uint32_t>(ch) > 0xFFu) {
                            all_bytes = false;
                            break;
                        }
                    }

                    if (all_bytes) {
                        std::string bytes;
                        bytes.reserve(in.size());
                        for (const auto ch : in)
                            bytes.push_back(static_cast<char>(ch));

                        if (detail::validate_utf8_segment(bytes.data(), bytes.data() + bytes.size()))
                            return materialize_utf8_from_bytes(bytes);
                    }
                }

                // wide -> utf8 in arena
                // worst-case 4 bytes per code unit (safe upper bound)
                const std::size_t cap = in.size() * 6 + 1;
                auto p = static_cast<char*>(arena().alloc(cap, 1));
                if (!p) {
                    set_err(ErrorCode::WriterOverflow);
                    return {};
                }

                char* dst = p;
                auto ok = true;

                if constexpr (std::same_as<CharT, char32_t>) {
                    for (const char32_t ch : in) {
                        const auto cp = static_cast<std::uint32_t>(ch);
                        if (cp < 0x80u) {
                            dst = ujson::detail::append_ascii_escaped(dst, static_cast<unsigned char>(cp));
                        } else {
                            const std::size_t n = ujson::detail::utf8_encode(dst, cp);
                            if (!n) {
                                ok = false;
                                break;
                            }
                            dst += n;
                        }
                    }
                } else if constexpr (std::same_as<CharT, char16_t>) {
                    for (std::size_t i = 0; i < in.size(); ++i) {
                        const std::uint32_t w1 = in[i];
                        std::uint32_t cp;

                        if (w1 >= 0xD800u && w1 <= 0xDBFFu) {
                            if (i + 1 >= in.size()) {
                                ok = false;
                                break;
                            }
                            const std::uint32_t w2 = in[i + 1];
                            if (w2 < 0xDC00u || w2 > 0xDFFFu) {
                                ok = false;
                                break;
                            }
                            cp = 0x10000u + (((w1 - 0xD800u) << 10) | (w2 - 0xDC00u));
                            ++i;
                        } else if (w1 >= 0xDC00u && w1 <= 0xDFFFu) {
                            ok = false;
                            break;
                        } else {
                            cp = w1;
                        }

                        if (cp < 0x80u) {
                            dst = ujson::detail::append_ascii_escaped(dst, static_cast<unsigned char>(cp));
                        } else {
                            const std::size_t n = ujson::detail::utf8_encode(dst, cp);
                            if (!n) {
                                ok = false;
                                break;
                            }
                            dst += n;
                        }
                    }
                } else if constexpr (std::same_as<CharT, wchar_t>) {
                    if constexpr (sizeof(wchar_t) == 2) {
                        return materialize_utf8_bytes(std::basic_string_view<char16_t> {reinterpret_cast<const char16_t*>(in.data()), in.size()});
                    } else {
                        return materialize_utf8_bytes(std::basic_string_view<char32_t> {reinterpret_cast<const char32_t*>(in.data()), in.size()});
                    }
                } else {
                    static_assert(!sizeof(CharT), "unsupported CharT");
                }

                if (!ok) {
                    set_err(ErrorCode::InvalidUnicode);
                    return {};
                }
                const auto out_len = static_cast<std::size_t>(dst - p);
                p[out_len] = '\0';
                return {p, out_len};
            }
        }

        template <class CharT>
        [[nodiscard]] UJSON_FORCEINLINE std::string_view materialize_json_escaped(std::basic_string_view<CharT> in) {
            // Produce ASCII-only JSON-escaped bytes in arena
            // worst-case: every code unit -> surrogate pair => 12 bytes, safe cap = in.size()*12 + 1
            const std::size_t cap = in.size() * 12 + 1;
            auto p = static_cast<char*>(arena().alloc(cap, 1));
            if (!p) {
                set_err(ErrorCode::WriterOverflow);
                return {};
            }

            std::size_t out_len = 0;

            if constexpr (std::same_as<CharT, char>) {
                if (!ujson::detail::json_escape_utf8_to_ascii(in.data(), in.data() + in.size(), p, out_len)) {
                    set_err(ErrorCode::InvalidUnicode);
                    return {};
                }
#ifdef __cpp_char8_t
            } else if constexpr (std::same_as<CharT, char8_t>) {
                auto b = reinterpret_cast<const char*>(in.data());
                if (!ujson::detail::json_escape_utf8_to_ascii(b, b + in.size(), p, out_len)) {
                    set_err(ErrorCode::InvalidUnicode);
                    return {};
                }
#endif
            } else {
                if constexpr (std::same_as<CharT, char16_t> || std::same_as<CharT, char32_t>) {
                    auto all_bytes = true;
                    for (const auto ch : in) {
                        if (static_cast<std::uint32_t>(ch) > 0xFFu) {
                            all_bytes = false;
                            break;
                        }
                    }

                    if (all_bytes) {
                        std::string bytes;
                        bytes.reserve(in.size());
                        for (const auto ch : in)
                            bytes.push_back(static_cast<char>(ch));

                        if (detail::validate_utf8_segment(bytes.data(), bytes.data() + bytes.size())) {
                            if (!detail::json_escape_utf8_to_ascii(bytes.data(), bytes.data() + bytes.size(), p, out_len)) {
                                set_err(ErrorCode::InvalidUnicode);
                                return {};
                            }
                            p[out_len] = '\0';
                            return {p, out_len};
                        }
                    }
                }

                // wide: write escapes directly (no intermediate utf8)
                char* dst = p;

                auto emit_cp = [&](std::uint32_t cp) {
                    if (cp < 0x80u) {
                        dst = ujson::detail::append_ascii_escaped(dst, static_cast<unsigned char>(cp));
                        return true;
                    }
                    if (cp <= 0xFFFFu) {
                        dst = ujson::detail::append_u16_escape(dst, static_cast<std::uint16_t>(cp));
                        return true;
                    }
                    cp -= 0x10000u;
                    const auto hi = static_cast<std::uint16_t>(0xD800u + (cp >> 10));
                    const auto lo = static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu));
                    dst = ujson::detail::append_u16_escape(dst, hi);
                    dst = ujson::detail::append_u16_escape(dst, lo);
                    return true;
                };

                auto ok = true;

                if constexpr (std::same_as<CharT, char32_t>) {
                    for (const char32_t ch : in) {
                        const auto cp = static_cast<std::uint32_t>(ch);
                        if (cp >= 0xD800u && cp <= 0xDFFFu) {
                            ok = false;
                            break;
                        }
                        if (cp > 0x10FFFFu) {
                            ok = false;
                            break;
                        }
                        if (!emit_cp(cp)) {
                            ok = false;
                            break;
                        }
                    }
                } else if constexpr (std::same_as<CharT, char16_t>) {
                    for (std::size_t i = 0; i < in.size(); ++i) {
                        const std::uint32_t w1 = in[i];
                        std::uint32_t cp = 0;

                        if (w1 >= 0xD800u && w1 <= 0xDBFFu) {
                            if (i + 1 >= in.size()) {
                                ok = false;
                                break;
                            }
                            const std::uint32_t w2 = in[i + 1];
                            if (w2 < 0xDC00u || w2 > 0xDFFFu) {
                                ok = false;
                                break;
                            }
                            cp = 0x10000u + (((w1 - 0xD800u) << 10) | (w2 - 0xDC00u));
                            ++i;
                        } else if (w1 >= 0xDC00u && w1 <= 0xDFFFu) {
                            ok = false;
                            break;
                        } else {
                            cp = w1;
                        }

                        if (!emit_cp(cp)) {
                            ok = false;
                            break;
                        }
                    }
                } else if constexpr (std::same_as<CharT, wchar_t>) {
                    if constexpr (sizeof(wchar_t) == 2) {
                        return materialize_json_escaped(std::basic_string_view<char16_t> {reinterpret_cast<const char16_t*>(in.data()), in.size()});
                    } else {
                        return materialize_json_escaped(std::basic_string_view<char32_t> {reinterpret_cast<const char32_t*>(in.data()), in.size()});
                    }
                } else {
                    static_assert(!sizeof(CharT), "unsupported CharT");
                }

                if (!ok) {
                    set_err(ErrorCode::InvalidUnicode);
                    return {};
                }
                out_len = static_cast<std::size_t>(dst - p);
            }

            p[out_len] = '\0';
            return {p, out_len};
        }

        template <class CharT>
        [[nodiscard]] UJSON_FORCEINLINE std::string_view materialize_text(std::basic_string_view<CharT> in) {
            if (opt_.encoding == EncodingPolicy::Utf8)
                return materialize_utf8_bytes(in);
            return materialize_json_escaped(in);
        }

        static bool normalize_slot(const NodeRef& self) {
            if (!self.slot_)
                return false;
            if (!self.parent_)
                return true;

            const Node* parent = self.parent_;
            if (!parent || !Node::is_container(parent->type)) {
                self.slot_ = nullptr;
                return false;
            }

            const auto& k = parent->data.kids;
            if (!k.items || k.count == 0) {
                self.slot_ = nullptr;
                return false;
            }

            if (self.slot_ >= k.items && self.slot_ < k.items + k.count)
                return true;

            const Node* target = *self.slot_;
            if (!target) {
                self.slot_ = nullptr;
                return false;
            }

            for (std::uint32_t i = 0; i < k.count; ++i) {
                if (k.items[i] == target) {
                    self.slot_ = &k.items[i];
                    return true;
                }
            }

            self.slot_ = nullptr;
            return false;
        }

        Options opt_ {};
        Node* root_ {};
        DomContext ctx_ {};
        mutable detail::KeyScratch key_scratch_ {};
        ParseError err_ {};
    };
    UJSON_FORCEINLINE bool NodeRef::ok() const noexcept {
        if (!b_ || !b_->ok())
            return false;
        const auto* self = const_cast<NodeRef*>(this);
        return ValueBuilder::normalize_slot(*self) && self->slot_;
    }

    UJSON_FORCEINLINE const ParseError& NodeRef::error() const noexcept {
        static ParseError dummy {};
        return b_ ? b_->error() : dummy;
    }

    UJSON_FORCEINLINE const NodeRef& NodeRef::set_object(const std::uint32_t cap) const {
        if (ok())
            (void)b_->ensure_container(*this, Type::Object, cap);
        return *this;
    }

    UJSON_FORCEINLINE const NodeRef& NodeRef::set_array(const std::uint32_t cap) const {
        if (ok())
            (void)b_->ensure_container(*this, Type::Array, cap);
        return *this;
    }

    template <class T>
    UJSON_FORCEINLINE NodeRef& NodeRef::operator=(T&& v) {
        if (ok())
            b_->assign_value(*this, std::forward<T>(v));
        return *this;
    }

    inline NodeRef NodeRef::operator[](const std::string_view key) const {
        if (!ok())
            return {};

        (void)set_object();
        Node* obj = raw();
        if (!obj)
            return {};

        detail::KeyScratch scratch;
        if (const auto f = b_->obj_find(obj, key, scratch); f.found && f.slot)
            return NodeRef {b_, f.slot, obj};

        Node** slot = b_->obj_append_slot(*this);
        if (!slot)
            return {};

        obj = raw();
        if (!obj)
            return {};

        Node* nn = b_->make_value_node(Type::Null);
        if (!nn)
            return {};

        std::string_view normalized;
        if (!b_->normalize_key_store(key, normalized))
            return {};

        nn->key = normalized;
        nn->key_hash = hash32(nn->key.data(), nn->key.size());
        nn->key = normalized;
        nn->key_hash = hash32(nn->key.data(), nn->key.size());

        *slot = nn;
        b_->objindex_insert(obj, nn);

        return NodeRef {b_, slot, obj};
    }

    UJSON_FORCEINLINE NodeRef NodeRef::get(const std::string_view key) const {
        if (!ok())
            return {};
        Node* obj = raw();
        if (!obj || obj->type != Type::Object)
            return {};
        detail::KeyScratch scratch;
        if (const auto f = b_->obj_find(obj, key, scratch); f.found && f.slot)
            return NodeRef {b_, f.slot, obj};
        return {};
    }

    UJSON_FORCEINLINE bool NodeRef::contains(const std::string_view key) const {
        return static_cast<bool>(get(key).raw());
    }

    template <class T>
    UJSON_FORCEINLINE NodeRef NodeRef::add(std::string_view key, T&& v) {
        if (!ok())
            return {};
        return b_->obj_insert_or_assign(*this, key, std::forward<T>(v));
    }

    inline NodeRef NodeRef::add_object(const std::string_view key, const std::uint32_t cap) const {
        if (!ok())
            return {};
        (void)set_object();

        Node** slot = b_->obj_append_slot(*this);
        if (!slot)
            return {};

        Node* child = b_->make_container(Type::Object, cap);
        if (!child)
            return {};

        std::string_view normalized;
        if (!b_->normalize_key_store(key, normalized))
            return {};

        child->key = normalized;
        child->key_hash = hash32(child->key.data(), child->key.size());

        *slot = child;
        b_->objindex_insert(raw(), child);
        return NodeRef {b_, slot, raw()};
    }

    inline NodeRef NodeRef::add_array(const std::string_view key, const std::uint32_t cap) const {
        if (!ok())
            return {};
        (void)set_object();

        Node** slot = b_->obj_append_slot(*this);
        if (!slot)
            return {};

        Node* child = b_->make_container(Type::Array, cap);
        if (!child)
            return {};

        std::string_view normalized;
        if (!b_->normalize_key_store(key, normalized))
            return {};

        child->key = normalized;
        child->key_hash = hash32(child->key.data(), child->key.size());

        *slot = child;
        b_->objindex_insert(raw(), child);
        return NodeRef {b_, slot, raw()};
    }

    UJSON_FORCEINLINE bool NodeRef::erase(const std::string_view key, const EraseMode mode) const {
        if (!ok())
            return false;
        (void)set_object();
        return b_->obj_erase(*this, key, mode);
    }

    UJSON_FORCEINLINE NodeRef NodeRef::operator[](const std::size_t idx) const {
        if (!ok())
            return {};
        return b_->arr_get_or_expand(const_cast<NodeRef&>(*this), idx);
    }

    UJSON_FORCEINLINE NodeRef NodeRef::at(const std::size_t idx) const {
        if (!ok())
            return {};
        Node* arr = raw();
        if (!arr || arr->type != Type::Array)
            return {};
        const auto& k = arr->data.kids;
        if (idx >= k.count)
            return {};
        return NodeRef {b_, &k.items[static_cast<std::uint32_t>(idx)], arr};
    }

    template <class T>
    UJSON_FORCEINLINE NodeRef NodeRef::add(T&& v) {
        if (!ok())
            return {};
        (void)set_array();

        Node** slot = b_->arr_append_slot(*this);
        if (!slot)
            return {};

        Node* nn = b_->make_from_value(std::forward<T>(v));
        if (!nn)
            return {};

        *slot = nn;
        return NodeRef {b_, slot, raw()};
    }

    UJSON_FORCEINLINE NodeRef NodeRef::add_object(const std::uint32_t cap) const {
        if (!ok())
            return {};
        (void)set_array();

        Node** slot = b_->arr_append_slot(*this);
        if (!slot)
            return {};

        Node* child = b_->make_container(Type::Object, cap);
        if (!child)
            return {};

        *slot = child;
        return NodeRef {b_, slot, raw()};
    }

    UJSON_FORCEINLINE NodeRef NodeRef::add_array(const std::uint32_t cap) const {
        if (!ok())
            return {};
        (void)set_array();

        Node** slot = b_->arr_append_slot(*this);
        if (!slot)
            return {};

        Node* child = b_->make_container(Type::Array, cap);
        if (!child)
            return {};

        *slot = child;
        return NodeRef {b_, slot, raw()};
    }

} // namespace ujson

#endif // UJSON_HPP
