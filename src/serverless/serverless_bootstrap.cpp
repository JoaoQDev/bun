#include "worker_config.h"
#include "js_runtime.h"
#include "tenant_manager.h"

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

    // 1. Parse config
    bool config_ok = false;
    auto config = serverless::WorkerConfig::load(config_path, config_ok);
    if (!config_ok) {
        return 1;
    }

    // 2. Init JsRuntime (creates JSC::VM)
    serverless::JsRuntime runtime;
    if (!runtime.init()) {
        fprintf(stderr, "[bun-serverless] Error: failed to initialize JS runtime\n");
        return 1;
    }

    // 3. Init TenantManager and load workers
    serverless::TenantManager tenantManager;
    tenantManager.init(&runtime);

    for (const auto& entry : config.workers) {
        if (!tenantManager.loadWorker(entry.name, entry.route, entry.script)) {
            fprintf(stderr, "[bun-serverless] Error: failed to load worker \"%s\"\n", entry.name.c_str());
            tenantManager.deinit();
            runtime.deinit();
            return 1;
        }
    }

    // 4. Print startup info
    fprintf(stdout, "[bun-serverless] Loaded %zu workers:\n", tenantManager.workerCount());
    auto infos = tenantManager.getWorkerInfos();
    for (const auto& info : infos) {
        fprintf(stdout, "  - %s -> %s\n", info.route.c_str(), info.name.c_str());
    }

    // TODO (US-005/US-008): Start HttpServer here
    // HttpServer::listen(port, &tenantManager);
    fprintf(stdout, "[bun-serverless] Config parsed and workers loaded. HTTP server not yet implemented (see US-005).\n");

    // Cleanup
    tenantManager.deinit();
    runtime.deinit();

    return 0;
}
