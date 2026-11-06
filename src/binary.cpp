#include "updater/binary.hpp"

#include <cstdint>

namespace updater {
namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

template<std::size_t N>
std::optional<std::array<std::byte, N>> parse_hex(std::string_view text)
{
    if (std::ssize(text) != 2 * static_cast<std::ptrdiff_t>(N)) return {};
    std::array<std::byte, N> result{};
    for (auto& byte : result) {
        const auto high = hex_value(text[0]);
        const auto low = hex_value(text[1]);
        if (high < 0 || low < 0) return {};
        byte = static_cast<std::byte>(16 * high + low);
        text.remove_prefix(2);
    }
    return result;
}

} // namespace

std::optional<Uuid> parse_uuid(std::string_view text, bool allow_nil)
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-'
        || text[18] != '-' || text[23] != '-') return {};
    std::array<char, 32> hex{};
    auto next = hex.begin();
    for (const auto c : text) {
        if (c != '-') {
            if (next == hex.end()) return {};
            *next++ = c;
        }
    }
    if (next != hex.end()) return {};
    const auto id = parse_hex<16>({hex.data(), hex.size()});
    if (!id) return {};
    if (allow_nil && *id == nil_uuid) return id;
    if (((*id)[6] & std::byte{0xf0}) != std::byte{0x40}
        || ((*id)[8] & std::byte{0xc0}) != std::byte{0x80}) return {};
    return id;
}

std::optional<Sha256> parse_sha256(std::string_view text)
{
    return parse_hex<32>(text);
}

std::string hex_string(std::span<const std::byte> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

std::string uuid_string(const Uuid& id)
{
    const auto hex = hex_string(id);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4)
        + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

} // namespace updater
