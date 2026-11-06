#pragma once
#include <expected>
#include <string>

namespace updater {
template<class T> using Result = std::expected<T, std::string>;
}
