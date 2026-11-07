#include "updater/request.hpp"

namespace updater {

std::expected<CheckRequest, RouteError> parse_check_target(std::string_view target)
{
    constexpr std::string_view prefix = "/updchk/";
    if (!target.starts_with(prefix)) return std::unexpected(RouteError::not_found);
    if (target.size() > 256 || target.find_first_of("?#%") != target.npos)
        return std::unexpected(RouteError::invalid);
    target.remove_prefix(prefix.size());
    const auto slash = target.find('/');
    if (slash == target.npos) return std::unexpected(RouteError::invalid);
    const auto id = parse_uuid(target.subview(0, slash));
    auto suite = target.subview(slash + 1);
    if (!id || suite.empty() || suite.size() > 64) return std::unexpected(RouteError::invalid);
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(suite.front())) return std::unexpected(RouteError::invalid);
    std::string canonical;
    for (const auto c : suite) {
        if (!letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '-')
            return std::unexpected(RouteError::invalid);
        canonical.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return CheckRequest{*id, std::move(canonical)};
}

} // namespace updater
