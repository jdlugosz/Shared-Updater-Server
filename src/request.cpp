#include "updater/request.hpp"
#include "updater/identifier.hpp"

namespace updater {

std::expected<CheckRequest, RouteError> parse_check_target(std::string_view target)
{
    constexpr std::string_view prefix = "/updchk/";
    if (!target.starts_with(prefix)) return std::unexpected(RouteError::not_found);
    if (target.size() > 256 || target.find_first_of("#%") != target.npos)
        return std::unexpected(RouteError::invalid);
    const auto question = target.find('?');
    auto path = target.subview(0, question);
    path.remove_prefix(prefix.size());
    const auto slash = path.find('/');
    if (slash == path.npos) return std::unexpected(RouteError::invalid);
    const auto id = parse_uuid(path.subview(0, slash));
    const auto suite = path.subview(slash + 1);
    if (!id || suite.empty() || suite.size() > 64) return std::unexpected(RouteError::invalid);
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(suite.front())) return std::unexpected(RouteError::invalid);
    std::string canonical;
    for (const auto c : suite) {
        if (!letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '-')
            return std::unexpected(RouteError::invalid);
        canonical.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    CheckRequest request{*id, std::move(canonical)};
    if (question == target.npos) return request;

    constexpr std::string_view platform_parameter = "platform=";
    auto query = target.subview(question + 1);
    for (;;) {
        const auto separator = query.find('&');
        const auto parameter = query.subview(0, separator);
        if (!parameter.starts_with(platform_parameter)) return std::unexpected(RouteError::invalid);
        const auto platform = parameter.subview(platform_parameter.size());
        if (!valid_catalog_identifier(platform)) return std::unexpected(RouteError::invalid);
        request.platforms.emplace_back(platform);
        if (separator == query.npos) return request;
        query.remove_prefix(separator + 1);
    }
}

} // namespace updater
