#include "updater/atom.hpp"
#include "updater/xml.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <libxml/parser.h>

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <algorithm>
#endif

namespace {
using namespace updater;

struct Document {
    explicit Document(const std::string& bytes)
        : document(xmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), "feed.atom", nullptr,
                                 XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING), xmlFreeDoc)
    { REQUIRE(document); }
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document;
    xmlNode* root() const { return xmlDocGetRootElement(document.get()); }
};

std::vector<xmlNode*> children(xmlNode* node, std::string_view name, std::string_view uri = atom_namespace)
{
    std::vector<xmlNode*> result;
    for (auto* child = node->children; child; child = child->next)
        if (child->type == XML_ELEMENT_NODE && child->ns
            && name == reinterpret_cast<const char*>(child->name)
            && uri == reinterpret_cast<const char*>(child->ns->href))
            result.push_back(child);
    return result;
}

std::string text(xmlNode* node)
{
    auto* content = xmlNodeGetContent(node);
    std::string result(reinterpret_cast<const char*>(content));
    xmlFree(content);
    return result;
}

std::string field(xmlNode* node, std::string_view name, std::string_view uri = atom_namespace)
{
    const auto nodes = children(node, name, uri);
    REQUIRE(nodes.size() == 1);
    return text(nodes.front());
}
}

TEST_CASE("UUIDs and digests use opaque bytes and canonical hexadecimal text", "[binary]")
{
    const auto id = parse_uuid("12345678-1234-4234-8234-1234567890AB");
    REQUIRE(id);
    REQUIRE((*id)[6] == std::byte{0x42});
    REQUIRE(uuid_string(*id) == "12345678-1234-4234-8234-1234567890ab");
    REQUIRE(parse_uuid("00000000-0000-0000-0000-000000000000") == nil_uuid);
    REQUIRE_FALSE(parse_uuid("00000000-0000-0000-0000-000000000000", false));
    for (const auto invalid : {"12345678-1234-1234-8234-1234567890ab",
                              "12345678-1234-4234-1234-1234567890ab",
                              "12345678-1234-4234-8234-1234567890ag",
                              "12345678_1234-4234-8234-1234567890ab"})
        REQUIRE_FALSE(parse_uuid(invalid));
    const auto digest = parse_sha256(std::string(64, 'A'));
    REQUIRE(digest);
    REQUIRE(hex_string(*digest) == std::string(64, 'a'));
    REQUIRE_FALSE(parse_sha256(std::string(63, '0')));
}

TEST_CASE("Route parsing normalizes suites and validates the exact two segments", "[request]")
{
    const auto request = parse_check_target("/updchk/12345678-1234-4234-8234-1234567890AB/Launcher");
    REQUIRE(request);
    REQUIRE(request->suite == "launcher");
    REQUIRE(uuid_string(request->installation_id).ends_with("90ab"));
    REQUIRE(parse_check_target("/updchk/00000000-0000-0000-0000-000000000000/Launcher"));
    for (const auto target : {"/updchk/", "/updchk/bad/launcher",
        "/updchk/00000000-0000-0000-0000-000000000000/1suite",
        "/updchk/00000000-0000-0000-0000-000000000000/launcher/",
        "/updchk/00000000-0000-0000-0000-000000000000/launcher?track=beta",
        "/updchk/00000000-0000-0000-0000-000000000000/la%2Funcher"})
        REQUIRE(parse_check_target(target).error() == RouteError::invalid);
    REQUIRE(parse_check_target("/something").error() == RouteError::not_found);
}

TEST_CASE("XML checking is reusable independently of TOML or a writer", "[xml]")
{
    REQUIRE(xml::valid_text("Długosz <&>\t\n\r"));
    REQUIRE_FALSE(xml::valid_text(std::string("a\0b", 3)));
    REQUIRE_FALSE(xml::valid_text("\x01"));
    REQUIRE_FALSE(xml::valid_text("\xc0\x80"));
    REQUIRE_FALSE(xml::valid_text("\xed\xa0\x80"));
    REQUIRE(xml::valid_local_name("Łącze"));
    REQUIRE(xml::valid_local_name("_a-1.2"));
    REQUIRE_FALSE(xml::valid_local_name("prefix:name"));
    REQUIRE_FALSE(xml::valid_local_name("1name"));
}

TEST_CASE("Atom output has required metadata and round-trips escaped values", "[feed]")
{
    const auto catalog = test::catalog();
    const auto rendered = render_feed(*catalog, {nil_uuid, "launcher"});
    REQUIRE(rendered);
    Document doc(*rendered);
    REQUIRE(std::string_view(reinterpret_cast<const char*>(doc.root()->name)) == "feed");
    REQUIRE(field(doc.root(), "title") == "Launcher & tools");
    REQUIRE(field(doc.root(), "updated") == "2026-11-05T12:00:00.5Z");
    REQUIRE(field(doc.root(), "revision", updater_namespace) == "sample-001");
    const auto entries = children(doc.root(), "entry");
    REQUIRE(entries.size() == 1);
    const auto entry = entries.front();
    REQUIRE(field(entry, "published") == "2026-11-05T12:00:00.125Z");
    REQUIRE(field(entry, "requires", updater_namespace) == "signed-packages");
    REQUIRE(field(entry, "Łącze", "https://example.invalid/extensions") == "<notes> & details");
    REQUIRE(field(entry, "sha256", updater_namespace) == hex_string(catalog->builds.begin()->second.artifacts.begin()->second.sha256));
    const auto links = children(entry, "link");
    REQUIRE(links.size() == 2);
    auto* url = xmlGetProp(links.back(), reinterpret_cast<const xmlChar*>("href"));
    REQUIRE(std::string_view(reinterpret_cast<const char*>(url)) == "https://example.invalid/Launcher%20x64.exe?a=1&b=2");
    xmlFree(url);
    REQUIRE(preflight_feeds(*catalog));
}

TEST_CASE("Targeted tracks replace tags without hiding archived public builds", "[feed]")
{
    auto catalog = test::catalog();
    auto target = catalog->builds.begin()->second;
    target.visibility = Visibility::targeted;
    target.version = "special";
    catalog->builds.emplace(BuildKey{"app", "targeted"}, std::move(target));
    const auto id = *parse_uuid("12345678-1234-4234-8234-1234567890ab");
    catalog->tracks.emplace(TrackKey{"launcher", "app", "current", "windows-x64", id}, "targeted");
    for (const auto other : {nil_uuid, *parse_uuid("abcdef01-1234-4234-8234-1234567890ab")}) {
        const auto rendered = render_feed(*catalog, {other, "launcher"});
        REQUIRE(rendered);
        Document doc(*rendered);
        const auto entries = children(doc.root(), "entry");
        REQUIRE(entries.size() == 1);
        REQUIRE(field(entries.front(), "track", updater_namespace) == "current");
    }
    const auto rendered = render_feed(*catalog, {id, "launcher"});
    REQUIRE(rendered);
    Document doc(*rendered);
    const auto entries = children(doc.root(), "entry");
    REQUIRE(entries.size() == 2);
    REQUIRE(field(entries.front(), "build", updater_namespace) == "build-001");
    REQUIRE(children(entries.front(), "track", updater_namespace).empty());
    REQUIRE(field(entries.back(), "track", updater_namespace) == "current");
    const auto public_rendered = render_feed(*catalog, {nil_uuid, "launcher"});
    Document public_doc(*public_rendered);
    REQUIRE(field(entries.front(), "id") == field(children(public_doc.root(), "entry").front(), "id"));
    REQUIRE(preflight_feeds(*catalog));
}

TEST_CASE("Feed caps apply to serialized bytes and preflight includes targeted offerings", "[feed]")
{
    auto catalog = test::catalog();
    const auto full = render_feed(*catalog, {nil_uuid, "launcher"});
    REQUIRE(full);
    FeedOptions options;
    options.maximum_bytes = std::ssize(*full);
    REQUIRE(render_feed(*catalog, {nil_uuid, "launcher"}, options));
    --options.maximum_bytes;
    REQUIRE_FALSE(render_feed(*catalog, {nil_uuid, "launcher"}, options));
    options.maximum_bytes = std::ssize(*full);
    auto target = catalog->builds.begin()->second;
    target.visibility = Visibility::targeted;
    catalog->builds.emplace(BuildKey{"app", "targeted"}, std::move(target));
    REQUIRE(render_feed(*catalog, {nil_uuid, "launcher"}, options));
    REQUIRE_FALSE(preflight_feeds(*catalog, options));
}

TEST_CASE("Empty suites and invalid output data have explicit outcomes", "[feed]")
{
    auto catalog = test::catalog();
    catalog->suites.emplace("empty", Suite{"Empty", {}});
    const auto rendered = render_feed(*catalog, {nil_uuid, "empty"});
    REQUIRE(rendered);
    Document doc(*rendered);
    REQUIRE(children(doc.root(), "entry").empty());
    REQUIRE_FALSE(render_feed(*catalog, {nil_uuid, "missing"}));
    catalog->suites.at("launcher").title = "\x01";
    REQUIRE_FALSE(render_feed(*catalog, {nil_uuid, "launcher"}));
    REQUIRE_FALSE(preflight_feeds(*catalog));
    REQUIRE_FALSE(render_feed(*catalog, {nil_uuid, "empty"}, {"http://example.invalid/", 1024}));
}
