#pragma once
#include <string>
#include <string_view>

namespace updater {
[[nodiscard]] std::string json_quote(std::string_view text);
}
