// Shared Updater Server - include/updater/json.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once
#include <string>
#include <string_view>

namespace updater {
[[nodiscard]] std::string json_quote(std::string_view text);
}
