#include "updater/xml.hpp"

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#endif

namespace updater::xml {
namespace {

std::optional<char32_t> consume_utf8(std::string_view& text)
{
    const auto first = static_cast<unsigned char>(text.front());
    if (first < 0x80) {
        text.remove_prefix(1);
        return first;
    }
    if (first < 0xc2 || first > 0xf4) return std::nullopt;
    const std::size_t width = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    if (text.size() < width) return std::nullopt;
    char32_t point = first & (0x7f >> width);
    for (std::size_t i = 1; i < width; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xc0) != 0x80) return std::nullopt;
        point = (point << 6) | (byte & 0x3f);
    }
    constexpr char32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (point < minimum[width] || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
        return std::nullopt;
    text.remove_prefix(width);
    return point;
}

bool xml_text(std::string_view text)
{
    while (!text.empty()) {
        const auto point = consume_utf8(text);
        if (!point) return false;
        const auto c = *point;
        if (!(c == 9 || c == 10 || c == 13 || (c >= 0x20 && c <= 0xd7ff)
              || (c >= 0xe000 && c <= 0xfffd) || (c >= 0x10000 && c <= 0x10ffff)))
            return false;
    }
    return true;
}

// XML 1.0 NameStartChar ranges, excluding the namespace separator.
bool xml_name_start(char32_t c)
{
    constexpr std::pair<char32_t, char32_t> ranges[] = {
        {'A', 'Z'}, {'a', 'z'}, {0xc0, 0xd6}, {0xd8, 0xf6}, {0xf8, 0x2ff},
        {0x370, 0x37d}, {0x37f, 0x1fff}, {0x200c, 0x200d}, {0x2070, 0x218f},
        {0x2c00, 0x2fef}, {0x3001, 0xd7ff}, {0xf900, 0xfdcf}, {0xfdf0, 0xfffd},
        {0x10000, 0xeffff}
    };
    return c == '_' || std::ranges::any_of(ranges, [c](auto range) {
        return c >= range.first && c <= range.second;
    });
}

bool xml_local_name(std::string_view name)
{
    if (name.empty()) return false;
    const auto first = consume_utf8(name);
    if (!first || !xml_name_start(*first)) return false;
    while (!name.empty()) {
        const auto point = consume_utf8(name);
        if (!point) return false;
        const auto c = *point;
        if (!(xml_name_start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9')
              || c == 0xb7 || (c >= 0x300 && c <= 0x36f) || (c >= 0x203f && c <= 0x2040)))
            return false;
    }
    return true;
}

} // namespace

bool valid_text(std::string_view text) { return xml_text(text); }

bool valid_local_name(std::string_view name) { return xml_local_name(name); }

} // namespace updater::xml
