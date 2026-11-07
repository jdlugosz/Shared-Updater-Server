// Shared Updater Server - include/updater/atom.hpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#pragma once

#include "updater/catalog.hpp"
#include "updater/request.hpp"
#include "updater/result.hpp"

#include <string_view>

namespace updater {

inline constexpr std::string_view atom_namespace = "http://www.w3.org/2005/Atom";
inline constexpr std::string_view updater_namespace = "https://dlugosz.enterprises/ns/updater/1";

struct FeedOptions {
    std::string publisher_id_base = "https://dlugosz.enterprises/updater/id/";
    std::ptrdiff_t maximum_bytes = 512 * 1024;
};

[[nodiscard]] std::string timestamp_string(const Timestamp& timestamp);
[[nodiscard]] Result<std::string> render_feed(
    const Catalog& catalog, const CheckRequest& request, const FeedOptions& options = {});
[[nodiscard]] Result<void> preflight_feeds(const Catalog& catalog, const FeedOptions& options = {});

} // namespace updater
