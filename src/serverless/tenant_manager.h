#pragma once

// TenantManager handles worker registry, lifecycle, and routing.
// It does NOT include any JSC headers and does NOT call any JSC API directly.
// All JSC interaction is delegated to JsRuntime through opaque WorkerHandle pointers.

#include <string>
#include <vector>
#include <unordered_map>

namespace serverless {

// Forward declarations — TenantManager only sees opaque pointers.
class JsRuntime;
struct WorkerHandle;

struct Worker {
    std::string name;
    std::string route;
    WorkerHandle* handle;
};

// Worker info for metrics/inspection (no JSC types exposed).
struct WorkerInfo {
    std::string name;
    std::string route;
};

class TenantManager {
public:
    TenantManager();
    ~TenantManager();

    // Initializes with reference to JsRuntime. Does not take ownership.
    void init(JsRuntime* runtime);

    // Loads a worker: calls runtime->createWorker(), stores Worker{ name, route, handle }.
    // Returns true on success. On failure (script load error), prints to stderr and returns false.
    bool loadWorker(const std::string& name, const std::string& route, const std::string& scriptPath);

    // Resolves which worker handles the given path.
    // Algorithm: exact match first, then longest prefix match for routes ending in /*.
    // Returns nullptr if no worker matches.
    Worker* route(const std::string& path);

    // Returns info about all loaded workers (for metrics).
    std::vector<WorkerInfo> getWorkerInfos() const;

    // Returns the number of loaded workers.
    size_t workerCount() const;

    // Iterates all workers and calls runtime->destroyWorker() for each.
    void deinit();

private:
    JsRuntime* runtime_;
    // Keyed by worker name for quick lookup during deinit.
    std::unordered_map<std::string, Worker> workers_;
    // Separate index for exact route matching.
    std::unordered_map<std::string, std::string> exactRoutes_;   // route -> worker name
    // Prefix routes (ending in /*) sorted by length descending for longest-prefix matching.
    std::vector<std::pair<std::string, std::string>> prefixRoutes_; // prefix (without /*) -> worker name

    void rebuildRouteIndex();

    // Non-copyable
    TenantManager(const TenantManager&) = delete;
    TenantManager& operator=(const TenantManager&) = delete;
};

} // namespace serverless
