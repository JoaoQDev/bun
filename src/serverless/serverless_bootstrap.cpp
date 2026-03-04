#include "worker_config.h"

#include <cstdio>
#include <cstdlib>

extern "C" int bun_serverless_main(const char* config_path, int port) {
    if (config_path == nullptr) {
        fprintf(stderr, "[bun-serverless] Error: config path is required\n");
        return 1;
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[bun-serverless] Error: invalid port number: %d\n", port);
        return 1;
    }

    // Parse config
    bool config_ok = false;
    auto config = serverless::WorkerConfig::load(config_path, config_ok);
    if (!config_ok) {
        return 1;
    }

    fprintf(stdout, "[bun-serverless] Loaded %zu workers:\n", config.workers.size());
    for (const auto& w : config.workers) {
        fprintf(stdout, "  - %s -> %s (%s)\n", w.route.c_str(), w.name.c_str(), w.script.c_str());
    }
    fprintf(stdout, "[bun-serverless] Config parsed successfully. Runtime not yet implemented (see US-002+).\n");

    return 0;
}
