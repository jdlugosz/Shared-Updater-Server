#pragma once
#include "updater/config.hpp"

namespace updater {
int run_service(ServerConfig config, std::unique_ptr<const Catalog> catalog);
}
