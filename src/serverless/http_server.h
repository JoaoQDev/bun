#pragma once

#include <string>
#include <atomic>

namespace serverless {

class TenantManager;
class JsRuntime;

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    // Starts the HTTP server on the given port. Blocks until shutdown is requested.
    // Returns 0 on graceful shutdown, 1 on error.
    int listen(int port, TenantManager* tenantManager, JsRuntime* runtime,
               std::atomic<bool>* shutdownFlag);

private:
    int serverFd_;

    // Non-copyable
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
};

} // namespace serverless
