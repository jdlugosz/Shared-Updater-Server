// Shared Updater Server - include/updater/result.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once
#include <expected>
#include <string>

namespace updater {
template<class T> using Result = std::expected<T, std::string>;
}
