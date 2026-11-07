#include "updater/build_info.hpp"
#include "updater/service.hpp"
#include "updater/write_text.hpp"

#if defined(UPDATER_USE_STD_MODULE) && !defined(__INTELLISENSE__)
import std;
#else
#include <format>
#include <iostream>
#include <stdexcept>
#endif

int main(int argc, char** argv)
{
    using namespace updater;
    try {
        std::filesystem::path config_file = "/etc/shared-updater-server/server.toml";
        bool check_only = false;
        bool feed_mode = false;
        bool id_given = false;
        std::filesystem::path catalog_override;
        std::string suite;
        std::string id = uuid_string(nil_uuid);

        for (int n = 1; n < argc; ++n) {
            const std::string_view argument(argv[n]);
            const auto value = [&]() -> std::string {
                if (n + 1 >= argc) throw std::runtime_error("missing option value");
                return argv[++n];
            };
            if (argument == "--config") config_file = value();
            else if (argument == "--check-config") check_only = true;
            else if (argument == "--feed") { feed_mode = true; suite = value(); }
            else if (argument == "--id") { id_given = true; id = value(); }
            else if (argument == "--catalog-dir") catalog_override = value();
            else if (argument == "--version") {
                write_text(std::cout, build_info.describe()).put('\n');
                return 0;
            } else if (argument == "--help") {
                write_text(std::cout, "shared-updater-server [--config FILE] [--check-config | --feed SUITE [--id UUID]]\n"
                                      "shared-updater-server --version | --help\n"
                                      "Use --feed to print a suite's Atom feed and exit.\n"
                                      "Without an offline option, serve on the configured loopback listener.\n");
                return 0;
            } else throw std::runtime_error("unknown option");
        }
        if (check_only && feed_mode) throw std::runtime_error("choose one offline operation");
        if (id_given && !feed_mode) throw std::runtime_error("--id requires --feed");
        if (!catalog_override.empty() && !check_only) throw std::runtime_error("--catalog-dir requires --check-config");
        auto config = read_server_config(config_file);
        if (!config) throw std::runtime_error(config.error());
        if (!catalog_override.empty()) {
            config->catalog_directory = std::filesystem::absolute(catalog_override);
            config->catalog_files.clear();
        }
        auto catalog = prepare_catalog(*config);
        if (!catalog) throw std::runtime_error(catalog.error());
        if (check_only) {
            write_text(std::cout, format_success(**catalog)).put('\n');
            return 0;
        }
        if (feed_mode) {
            auto request = parse_check_target(std::format("/updchk/{}/{}", id, suite));
            if (!request) throw std::runtime_error("invalid feed request");
            auto feed = render_feed(**catalog, *request, config->feed);
            if (!feed) throw std::runtime_error(feed.error());
            write_text(std::cout, *feed);
            return 0;
        }
        return run_service(std::move(*config), std::move(*catalog));
    } catch (const std::exception& error) {
        write_text(std::cerr, "Error: ");
        write_text(std::cerr, error.what()).put('\n');
        return 1;
    } catch (...) {
        write_text(std::cerr, "Error: unexpected exception type\n");
        return 1;
    }
}
