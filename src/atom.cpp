// Shared Updater Server - src/atom.cpp
// <Oort.dlugosz.enterprises/john/Shared-Updater-Server> - see LICENCE.

#include "updater/atom.hpp"
#include "updater/uri.hpp"
#include "updater/xml.hpp"

#include <libxml/xmlwriter.h>
#include <libxml/parser.h>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <algorithm>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#endif

namespace updater {
namespace {

struct WriterError {};
struct Sink {
    std::string bytes;
    std::ptrdiff_t limit;
    bool failed = false;

    static int write(void* context, const char* data, int length) noexcept
    {
        auto& self = *static_cast<Sink*>(context);
        if (length < 0 || length > self.limit - std::ssize(self.bytes)) {
            self.failed = true;
            return -1;
        }
        try {
            self.bytes.append(data, static_cast<std::size_t>(length));
            return length;
        } catch (...) {
            self.failed = true;
            return -1;
        }
    }

    static int close(void*) noexcept { return 0; }
};

class Writer {
public:
    explicit Writer(Sink& sink)
    {
        static std::once_flag initialized;
        std::call_once(initialized, [] { xmlInitParser(); });
        auto* output = xmlOutputBufferCreateIO(Sink::write, Sink::close, &sink, nullptr);
        if (!output) throw WriterError{};
        writer_.reset(xmlNewTextWriter(output));
        if (!writer_) {
            xmlOutputBufferClose(output);
            throw WriterError{};
        }
        check(xmlTextWriterStartDocument(writer_.get(), nullptr, "UTF-8", nullptr));
    }

    void start(const char* name) { check(xmlTextWriterStartElement(writer_.get(), chars(name))); }

    void end() { check(xmlTextWriterEndElement(writer_.get())); }

    void attribute(const char* name, std::string_view value)
    {
        const auto owned = checked(value);
        check(xmlTextWriterWriteAttribute(writer_.get(), chars(name), chars(owned.c_str())));
    }

    void element(const char* name, std::string_view value)
    {
        const auto owned = checked(value);
        check(xmlTextWriterWriteElement(writer_.get(), chars(name), chars(owned.c_str())));
    }

    void extension(const Extension& extension)
    {
        if (!xml::valid_local_name(extension.name)) throw WriterError{};
        const auto value = checked(extension.text);
        check(xmlTextWriterWriteElementNS(writer_.get(), chars("ext"), chars(extension.name.c_str()),
                                         chars(extension.namespace_uri.c_str()), chars(value.c_str())));
    }

    void finish() { check(xmlTextWriterEndDocument(writer_.get())); }

private:
    std::unique_ptr<xmlTextWriter, decltype(&xmlFreeTextWriter)> writer_{nullptr, xmlFreeTextWriter};
    static const xmlChar* chars(const char* text) { return reinterpret_cast<const xmlChar*>(text); }

    static void check(int status) { if (status < 0) throw WriterError{}; }

    static std::string checked(std::string_view value)
    {
        if (!xml::valid_text(value)) throw WriterError{};
        return std::string(value);
    }
};

using Track = std::tuple<std::string, std::string, std::string>; // package, platform, name
using EffectiveTracks = std::map<Track, std::string>;

EffectiveTracks effective_tracks(const Catalog& catalog, const CheckRequest& request)
{
    EffectiveTracks result;
    for (bool targeted : {false, true})
        for (const auto& [key, build] : catalog.tracks) {
            if (key.suite != request.suite || key.installation_id.has_value() != targeted) continue;
            if (targeted && *key.installation_id != request.installation_id) continue;
            result.insert_or_assign({key.package, key.platform, key.name}, build);
        }
    return result;
}

Result<std::string> render(const Catalog& catalog, const CheckRequest& request,
                           const FeedOptions& options, bool conservative)
{
    if (options.maximum_bytes <= 0 || !valid_https_url(options.publisher_id_base)
        || !options.publisher_id_base.ends_with('/')
        || options.publisher_id_base.find_first_of("?#") != std::string::npos)
        return std::unexpected("invalid feed options");
    const auto suite = catalog.suites.find(request.suite);
    if (suite == catalog.suites.end()) return std::unexpected("unknown suite");
    const auto tracks = effective_tracks(catalog, request);
    Sink sink{{}, options.maximum_bytes};
    try {
        Writer out(sink);
        out.start("feed");
        out.attribute("xmlns", atom_namespace);
        out.attribute("xmlns:upd", updater_namespace);
        out.element("id", std::format("{}suites/{}", options.publisher_id_base, request.suite));
        out.element("title", suite->second.title);
        out.element("updated", timestamp_string(catalog.header.published_at));
        out.start("author");
        out.element("name", catalog.header.author);
        out.end();
        out.element("upd:suite", request.suite);
        out.element("upd:revision", catalog.header.revision);
        for (const auto& [key, build] : catalog.builds) {
            if (!std::ranges::binary_search(suite->second.packages, key.package)) continue;
            const auto package = catalog.packages.find(key.package);
            if (package == catalog.packages.end()) return std::unexpected("invalid package reference");
            for (const auto& [platform, artifact] : build.artifacts) {
                if (!request.platforms.empty() && platform != "any" && !std::ranges::contains(request.platforms, platform))
                    continue;
                std::set<std::string> tags;
                if (conservative) {
                    for (const auto& [track, target] : catalog.tracks)
                        if (track.suite == request.suite && track.package == key.package
                            && track.platform == platform && target == key.id)
                            tags.insert(track.name);
                } else {
                    for (const auto& [track, target] : tracks)
                        if (std::get<0>(track) == key.package && std::get<1>(track) == platform
                            && target == key.id)
                            tags.insert(std::get<2>(track));
                }
                if (!conservative && build.visibility == Visibility::targeted && tags.empty()) continue;
                out.start("entry");
                out.element("id", std::format("{}packages/{}/builds/{}/artifacts/{}",
                                              options.publisher_id_base, key.package, key.id, platform));
                out.element("title", std::format("{} {} ({})", package->second.title, build.version, platform));
                out.element("updated", timestamp_string(catalog.header.published_at));
                out.element("published", timestamp_string(build.published_at));
                out.start("link");
                out.attribute("rel", "alternate");
                out.attribute("href", build.about_url);
                out.end();
                out.start("link");
                out.attribute("rel", "enclosure");
                out.attribute("href", artifact.url);
                out.attribute("type", artifact.media_type);
                out.attribute("length", std::to_string(artifact.size));
                out.end();
                out.element("upd:package", key.package);
                out.element("upd:kind", package->second.kind);
                out.element("upd:build", key.id);
                out.element("upd:version", build.version);
                out.element("upd:platform", platform);
                out.element("upd:sha256", hex_string(artifact.sha256));
                for (const auto& tag : tags) out.element("upd:track", tag);
                for (const auto& requirement : build.requirements) out.element("upd:requires", requirement);
                for (const auto& extension : build.extensions)
                    if (!extension.suite || *extension.suite == request.suite) out.extension(extension);
                out.end();
            }
        }
        out.end();
        out.finish();
        if (sink.failed) return std::unexpected("feed exceeds output limit");
        return std::move(sink.bytes);
    } catch (const WriterError&) {
        return std::unexpected(sink.failed ? "feed exceeds output limit" : "invalid XML output");
    }
}

} // namespace

std::string timestamp_string(const Timestamp& timestamp)
{
    using namespace std::chrono;
    const auto date = floor<days>(timestamp.seconds);
    const year_month_day ymd(date);
    const hh_mm_ss time(timestamp.seconds - date);
    auto result = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}",
        int(ymd.year()), unsigned(ymd.month()), unsigned(ymd.day()),
        time.hours().count(), time.minutes().count(), time.seconds().count());
    if (timestamp.fraction != nanoseconds::zero()) {
        auto fraction = std::format("{:09}", timestamp.fraction.count());
        while (fraction.ends_with('0')) fraction.pop_back();
        result += '.';
        result += fraction;
    }
    result += 'Z';
    return result;
}

Result<std::string> render_feed(const Catalog& catalog, const CheckRequest& request, const FeedOptions& options)
{
    return render(catalog, request, options, false);
}

Result<void> preflight_feeds(const Catalog& catalog, const FeedOptions& options)
{
    // Validate options even when a catalog contains no suites.
    Catalog empty = {};
    empty.header = catalog.header;
    empty.suites.emplace("validation", Suite{"", {}});
    if (auto checked = render(empty, {nil_uuid, "validation"}, options, true); !checked)
        return std::unexpected(checked.error());
    for (const auto& suite : catalog.suites.keys())
        if (auto checked = render(catalog, {nil_uuid, suite}, options, true); !checked)
            return std::unexpected(std::format("suite '{}': {}", suite, checked.error()));
    return {};
}

} // namespace updater
