#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace updater {

using Uuid = std::array<std::byte, 16>;
using Sha256 = std::array<std::byte, 32>;
inline constexpr Uuid nil_uuid{};

[[nodiscard]] std::optional<Uuid> parse_uuid(std::string_view text, bool allow_nil = true);
[[nodiscard]] std::optional<Sha256> parse_sha256(std::string_view text);
[[nodiscard]] std::string hex_string(std::span<const std::byte> bytes);
[[nodiscard]] std::string uuid_string(const Uuid& id);

} // namespace updater
