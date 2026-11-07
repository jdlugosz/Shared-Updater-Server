#pragma once

#include <string_view>

namespace updater {

[[nodiscard]] constexpr bool valid_catalog_identifier(std::string_view text, bool suite = false) noexcept
{
    const auto letter = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (text.empty() || text.size() > 64 || !(letter(text.front()) || (!suite && digit(text.front()))))
        return false;
    for (const auto c : text)
        if (!letter(c) && !digit(c) && c != '-' && c != '_') return false;
    return true;
}

} // namespace updater
