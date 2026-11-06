#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>
#include <toml++/toml.hpp>

// Catch2::Catch2WithMain supplies the executable's main().
TEST_CASE("Dependency headers compile and libraries link", "[bootstrap]")
{
    // Reference library APIs so the build must resolve their linked symbols.
    INFO("SQLite " << sqlite3_libversion());
    [[maybe_unused]] const auto configuration = toml::parse("bootstrap = true");

    SUCCEED("SQLite, toml++, and Catch2 are available.");
}
