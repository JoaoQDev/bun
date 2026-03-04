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

    // Placeholder: future stories will implement config parsing, JsRuntime, TenantManager, HttpServer
    fprintf(stdout, "[bun-serverless] Starting with config: %s on port %d\n", config_path, port);
    fprintf(stdout, "[bun-serverless] Runtime not yet implemented. See US-001 through US-010.\n");

    return 0;
}
