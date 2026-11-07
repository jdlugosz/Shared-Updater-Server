#include "updater/processor.hpp"

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <format>
#endif

namespace updater {

CheckProcessor::CheckProcessor(std::unique_ptr<const Catalog> catalog, FeedOptions options,
                               std::unique_ptr<ContactSink> log, std::unique_ptr<CensusSink> census)
    : catalog_(std::move(catalog)), options_(std::move(options)),
      log_(std::move(log)), census_(std::move(census)) {}

Result<void> CheckProcessor::initialize()
{
    if (!catalog_ || !log_ || !census_) return std::unexpected("incomplete processor dependencies");
    return census_->register_suites(*catalog_);
}

Reply CheckProcessor::process(const CheckRequest& request, const RequestContext& context,
                              const std::function<bool()>& enter_persistence)
{
    if (!catalog_->suites.contains(request.suite)) return {Outcome::not_found, {}, "unknown_suite"};
    auto feed = render_feed(*catalog_, request, options_);
    if (!feed) return {Outcome::unavailable, {}, "feed", feed.error()};
    if (!enter_persistence()) return {Outcome::unavailable, {}, "cancelled"};
    if (auto logged = log_->append(request, context.observed_at); !logged)
        return {Outcome::unavailable, {}, "log", logged.error()};
    if (auto recorded = census_->record(request, context.observed_at); !recorded)
        return {Outcome::unavailable, {}, "census", recorded.error()};
    return {Outcome::ok, std::move(*feed), {}};
}

Result<void> CheckProcessor::activate(std::unique_ptr<const Catalog> candidate)
{
    if (!candidate) return std::unexpected("missing candidate");
    if (candidate->header.revision == catalog_->header.revision
        && (candidate->header != catalog_->header || candidate->suites != catalog_->suites
            || candidate->packages != catalog_->packages || candidate->builds != catalog_->builds
            || candidate->tracks != catalog_->tracks))
        return std::unexpected("catalog revision was reused for different content");
    for (const auto& [key, old] : catalog_->builds) {
        const auto found = candidate->builds.find(key);
        if (found == candidate->builds.end()) continue;
        if (found->second.version != old.version || found->second.published_at != old.published_at)
            return std::unexpected(std::format("immutable build identity changed: {}/{}", key.package, key.id));
        for (const auto& [platform, artifact] : old.artifacts) {
            const auto next = found->second.artifacts.find(platform);
            if (next != found->second.artifacts.end() && next->second != artifact)
                return std::unexpected(std::format("immutable artifact changed: {}/{}/{}", key.package, key.id, platform));
        }
    }
    if (auto registered = census_->register_suites(*candidate); !registered) return registered;
    catalog_ = std::move(candidate);
    return {};
}

Result<bool> CheckProcessor::maintain(WallTime now, const Retention& retention)
{
    return census_->prune(now, retention);
}

std::string CheckProcessor::revision() const { return catalog_->header.revision; }

} // namespace updater
