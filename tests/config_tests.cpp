#include "updater/config.hpp"
#include "updater/json.hpp"
#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace updater;

TEST_CASE("Server configuration resolves paths and accepts explicit resource policies", "[config]")
{
    test::Directory directory;
    const auto file = directory.write("server.toml", R"(
listen = "[::1]:0"
catalog_files = ["one.toml", "two.toml"]
state_dir = "state"
log_dir = "logs"
[limits]
unfinished_requests = 7
response_bytes = 4000
[timeouts]
worker_ms = 300
queue_ms = 50
[retention]
activity_days = 30
inactive_installation_days = 90
)");
    const auto config = read_server_config(file);
    REQUIRE(config);
    REQUIRE(config->http.address == "::1");
    REQUIRE(config->http.port == 0);
    REQUIRE(config->catalog_files == std::vector<std::filesystem::path>{directory.root / "one.toml", directory.root / "two.toml"});
    REQUIRE(config->state_directory == directory.root / "state");
    REQUIRE(config->worker.maximum_unfinished == 7);
    REQUIRE(config->feed.maximum_bytes == 4000);
    REQUIRE(config->worker.idle_interval == std::chrono::milliseconds(100));
    REQUIRE(config->worker.retention.activity_days == 30);
    REQUIRE_FALSE(std::filesystem::exists(config->state_directory));
}

TEST_CASE("Server configuration rejects ambiguous sources unsafe listeners and misspelled keys", "[config]")
{
    test::Directory directory;
    for (const auto source : {
        "catalog_dir='catalog'\ncatalog_files=['one']",
        "state_dir='state'",
        "catalog_dir='catalog'\nlisten='0.0.0.0:8080'",
        "catalog_dir='catalog'\nlisten='127.0.0.1:65536'",
        "catalog_dir='catalog'\nlisten='127.0.0.1:'",
        "catalog_files=[]",
        "catalog_dir='catalog'\n[limits]\nunfinished_requests=-1",
        "catalog_dir='catalog'\n[timeouts]\nqueue_ms=0",
        "catalog_dir='catalog'\n[retention]\nactivity_days=0",
        "catalog_dir='catalog'\n[limits]\nconnections_typo=64",
        "catalog_dir='catalog'\n[timeouts]\nworker_ms=true"}) {
        INFO(source);
        REQUIRE_FALSE(read_server_config(directory.write("bad.toml", source)));
    }
}

TEST_CASE("Health JSON escapes syntax controls and preserves UTF-8", "[json]")
{
    REQUIRE(json_quote("Długosz") == R"("Długosz")");
    REQUIRE(json_quote("a\"\\\n\t") == R"("a\"\\\u000a\u0009")");
}
