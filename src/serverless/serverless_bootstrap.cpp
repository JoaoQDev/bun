#include "worker_config.h"
#include "js_runtime.h"
#include "tenant_manager.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// Global state for signal handler access during graceful shutdown.
static std::atomic<bool> g_shutdown_requested{false};
static serverless::TenantManager* g_tenantManager = nullptr;
static serverless::JsRuntime* g_runtime = nullptr;

static void sigint_handler(int) {
    g_shutdown_requested.store(true, std::memory_order_release);
}

extern "C" int bun_serverless_main(const char* config_path, int port, void* jsc_vm) {
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

    // 2. Init JsRuntime (uses the JSC::VM created by Zig)
    serverless::JsRuntime runtime;
    if (!runtime.init(jsc_vm)) {
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

    // 5. Register SIGINT handler for graceful shutdown
    g_tenantManager = &tenantManager;
    g_runtime = &runtime;

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // 6. Start HttpServer (US-005 will replace this with actual HTTP server)
    // TODO (US-005): HttpServer::listen(port, &tenantManager, &runtime);
    fprintf(stdout, "[bun-serverless] Listening on http://localhost:%d\n", port);
    fflush(stdout);

    // 7. Keep process alive until SIGINT/SIGTERM
    while (!g_shutdown_requested.load(std::memory_order_acquire)) {
        pause(); // Blocks until a signal is delivered
    }

    fprintf(stdout, "\n[bun-serverless] Shutting down...\n");
    fflush(stdout);

    // 8. Graceful shutdown: destroy all workers via TenantManager, then JsRuntime
    tenantManager.deinit();
    runtime.deinit();

    g_tenantManager = nullptr;
    g_runtime = nullptr;

    fprintf(stdout, "[bun-serverless] Shutdown complete.\n");
    fflush(stdout);

    return 0;
}
