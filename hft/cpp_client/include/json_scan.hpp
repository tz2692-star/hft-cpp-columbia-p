// ─────────────────────────────────────────────────────────────────────────────
// json_scan.hpp — tiny helpers that pull values straight out of JSON text.
//
// Instead of building a full JSON tree (slow, allocates memory), we search the
// raw text for the few keys we need and read the value right where it sits.
// Nothing here allocates memory.
//
// Only works for the simple, flat messages the exchange sends us. It is NOT a
// general JSON parser.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <cstdlib>
#include <string_view>

namespace arena
{

    // A special value meaning "not found". std::string_view::npos is a huge
    // number that can never be a real position in the text.
    constexpr std::size_t kNotFound = std::string_view::npos;

    // Move position `i` forward past any spaces.
    // Python writes  "key": value  (with a space after ':' and ','), so we
    // must skip those spaces before reading anything.
    inline std::size_t skip_ws(std::string_view s, std::size_t i)
    {
        while (i < s.size() && s[i] == ' ')
            ++i;
        return i;
    }

    // Find  "key":  in the text and return the position where its VALUE starts.
    // Pass the key WITH its quotes, e.g.  "\"mid_price\""  — the quotes stop
    // "price" from accidentally matching inside "mid_price".
    // Returns kNotFound if the key isn't there.
    inline std::size_t find_value(std::string_view s, std::string_view quoted_key)
    {
        std::size_t i = s.find(quoted_key);      // where does the key start?
        if (i == kNotFound)
            return kNotFound;

        i = skip_ws(s, i + quoted_key.size());   // jump past the key
        if (i >= s.size() || s[i] != ':')        // next thing must be ':'
            return kNotFound;

        return skip_ws(s, i + 1);                // jump past ':' and spaces
    }

    // The value at position `i` is text in quotes, like "AAPL".
    // Return the text between the quotes (no copy — it points into `s`).
    // Returns empty text if there's no string there.
    inline std::string_view read_string(std::string_view s, std::size_t i)
    {
        if (i == kNotFound || i >= s.size() || s[i] != '"')
            return {};

        std::size_t end = s.find('"', i + 1);    // find the closing quote
        if (end == kNotFound)
            return {};

        return s.substr(i + 1, end - i - 1);     // the part in between
    }

    // The value at position `i` is a number, like 100.5. Return it.
    // Returns 0.0 if not found (same as m.value("x", 0.0) did before).
    // strtod reads digits until it hits something that isn't part of a number
    // (a comma, a bracket, ...). It does not allocate memory.
    inline double read_double(std::string_view s, std::size_t i)
    {
        if (i == kNotFound || i >= s.size())
            return 0.0;
        return std::strtod(s.data() + i, nullptr);
    }

    // Same as read_double, but for whole numbers like quantities.
    inline int read_int(std::string_view s, std::size_t i)
    {
        if (i == kNotFound || i >= s.size())
            return 0;
        return static_cast<int>(std::strtol(s.data() + i, nullptr, 10));
    }

} // namespace arena
