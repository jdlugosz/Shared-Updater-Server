#pragma once
#include <string_view>

namespace updater {
[[nodiscard]] bool valid_https_url(std::string_view url);
}
