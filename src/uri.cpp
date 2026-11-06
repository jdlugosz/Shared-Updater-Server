#include "updater/uri.hpp"

#include <arpa/inet.h>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <algorithm>
#include <charconv>
#include <string>
#endif

namespace updater {
namespace {
using namespace std::string_view_literals;
bool digit(char c) { return c >= '0' && c <= '9'; }

bool lower(char c) { return c >= 'a' && c <= 'z'; }

bool alpha(char c) { return lower(c) || (c >= 'A' && c <= 'Z'); }

bool alnum(char c) { return alpha(c) || digit(c); }

int hex_value(char c) {
    if (digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool host_name(std::string_view host)
{
    if (host.ends_with('.')) host.remove_suffix(1);
    if (host.empty() || host.size() > 253 || host.ends_with('.')) return false;
    for (;;) {
        const auto dot = host.find('.');
        const auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63 || !alnum(label.front()) || !alnum(label.back())
            || !std::ranges::all_of(label, [](char c) { return alnum(c) || c == '-'; }))
            return false;
        if (dot == host.npos) return true;
        host.remove_prefix(dot + 1);
    }
}

} // namespace

bool valid_https_url(std::string_view url)
{
    constexpr auto scheme = "https://"sv;
    if (url.size() < scheme.size() || !std::ranges::equal(url.substr(0, scheme.size()),
            scheme, [](char a, char b) {
                return a == b || (a >= 'A' && a <= 'Z' && a - 'A' + 'a' == b);
            }))
        return false;
    url.remove_prefix(scheme.size());
    const auto end = url.find_first_of("/?#");
    auto authority = url.substr(0, end);
    if (authority.empty() || authority.contains('@')) return false;

    if (authority.starts_with('[')) {
        const auto close = authority.find(']');
        if (close == authority.npos) return false;
        const auto literal = authority.substr(1, close - 1);
        if (literal.contains('\0')) return false;
        const std::string ip(literal);
        in6_addr address{};
        if (inet_pton(AF_INET6, ip.c_str(), &address) != 1) return false;
        authority.remove_prefix(close + 1);
    } else {
        const auto colon = authority.find(':');
        if (!host_name(authority.substr(0, colon))) return false;
        authority.remove_prefix(colon == authority.npos ? authority.size() : colon);
    }
    if (!authority.empty()) {
        if (!authority.starts_with(':')) return false;
        authority.remove_prefix(1);
        unsigned port = 0;
        const auto [after, ec] = std::from_chars(authority.data(), authority.data() + authority.size(), port);
        if (ec != std::errc{} || after != authority.data() + authority.size() || port == 0 || port > 65535)
            return false;
    }

    if (end == url.npos) return true;
    bool fragment = false;
    for (std::size_t i = end; i < url.size(); ++i) {
        const auto c = url[i];
        if (c == '%') {
            if (i + 2 >= url.size() || hex_value(url[i + 1]) < 0 || hex_value(url[i + 2]) < 0)
                return false;
            i += 2;
        } else if (c == '#') {
            if (fragment) return false;
            fragment = true;
        } else if (!alnum(c) && !"-._~!$&'()*+,;=:@/?"sv.contains(c)) {
            return false;
        }
    }
    return true;
}

} // namespace updater
