#include "updater/service.hpp"

#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    using namespace updater;
    std::filesystem::path config_file = "/etc/shared-updater-server/server.toml";
    bool check_only = false;
    bool feed_mode = false;
    bool id_given = false;
    std::filesystem::path catalog_override;
    std::string suite;
    std::string id = uuid_string(nil_uuid);
    try {
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
            else if (argument == "--version") { std::cout << "Shared Updater Server 0.1.0\n"; return 0; }
            else if (argument == "--help") {
                std::cout << "shared-updater-server --config FILE [--check-config | --feed SUITE [--id UUID]]\n"
                          << "Without an offline option, serve on the configured loopback listener.\n";
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
        if (check_only) { std::cout << format_success(**catalog) << std::endl; return 0; }
        if (feed_mode) {
            auto request = parse_check_target("/updchk/" + id + "/" + suite);
            if (!request) throw std::runtime_error("invalid feed request");
            auto feed = render_feed(**catalog, *request, config->feed);
            if (!feed) throw std::runtime_error(feed.error());
            std::cout << *feed;
            return 0;
        }
        return run_service(std::move(*config), std::move(*catalog));
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return 1;
    }
}
