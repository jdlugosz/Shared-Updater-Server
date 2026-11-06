#pragma once

#include <iterator>
#include <ostream>
#include <string_view>

namespace updater {

inline std::ostream& write_text(std::ostream& out, std::string_view text)
{
    return out.write(text.empty() ? "" : text.data(), std::ssize(text));
}

} // namespace updater
