// Shared Updater Server - include/updater/xml.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include <string_view>

namespace updater::xml {

// UTF-8 text representable by XML 1.0, and a namespace-local XML name.
[[nodiscard]] bool valid_text(std::string_view text);
[[nodiscard]] bool valid_local_name(std::string_view name);

} // namespace updater::xml
