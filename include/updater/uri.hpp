// Shared Updater Server - include/updater/uri.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once
#include <string_view>

namespace updater {
[[nodiscard]] bool valid_https_url(std::string_view url);
}
