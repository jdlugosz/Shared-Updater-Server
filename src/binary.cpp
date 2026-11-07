// Shared Updater Server - src/binary.cpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#include "updater/binary.hpp"

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <stdexcept>
#endif

namespace updater {
namespace {

void write_hex_byte(char*& output, std::byte byte) noexcept
{
    constexpr std::string_view digits = "0123456789abcdef";
    const auto value = std::to_integer<unsigned>(byte);
    *output++ = digits[value >> 4];
    *output++ = digits[value & 15];
}

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
    std::string result;
    if (bytes.size() > result.max_size() / 2) throw std::length_error("hex string is too large");
    result.resize_and_overwrite(bytes.size() * 2, [bytes](char* output, std::size_t size) noexcept {
        for (const auto byte : bytes) write_hex_byte(output, byte);
        return size;
    });
    return result;
}

std::string uuid_string(const Uuid& id)
{
    std::string result;
    result.resize_and_overwrite(36, [&id](char* output, std::size_t size) noexcept {
        for (std::size_t i = 0; i < id.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) *output++ = '-';
            write_hex_byte(output, id[i]);
        }
        return size;
    });
    return result;
}

} // namespace updater
