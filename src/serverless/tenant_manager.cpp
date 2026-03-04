#include "tenant_manager.h"
#include "js_runtime.h"

#include <algorithm>
#include <cstdio>

namespace serverless {

TenantManager::TenantManager() : runtime_(nullptr) {}

TenantManager::~TenantManager() {
    if (runtime_) {
        deinit();
    }
}

void TenantManager::init(JsRuntime* runtime) {
    runtime_ = runtime;
}

bool TenantManager::loadWorker(const std::string& name, const std::string& route, const std::string& scriptPath) {
    if (!runtime_) {
        fprintf(stderr, "[TenantManager] Error: not initialized\n");
        return false;
    }

    // Check for duplicate name
    if (workers_.count(name)) {
        fprintf(stderr, "[TenantManager] Error: duplicate worker name: \"%s\"\n", name.c_str());
        return false;
    }

    // Delegate to JsRuntime for all JSC work
    WorkerHandle* handle = runtime_->createWorker(scriptPath);
    if (!handle) {
        fprintf(stderr, "[TenantManager] Error: failed to create worker \"%s\" from %s\n",
                name.c_str(), scriptPath.c_str());
        return false;
    }

    Worker worker;
    worker.name = name;
    worker.route = route;
    worker.handle = handle;
    workers_[name] = worker;

    rebuildRouteIndex();
    return true;
}

Worker* TenantManager::route(const std::string& path) {
    // 1. Exact match first
    auto exactIt = exactRoutes_.find(path);
    if (exactIt != exactRoutes_.end()) {
        return &workers_[exactIt->second];
    }

    // 2. Longest prefix match for routes ending in /*
    for (const auto& entry : prefixRoutes_) {
        const std::string& prefix = entry.first;
        if (path.size() >= prefix.size() &&
            path.compare(0, prefix.size(), prefix) == 0) {
            return &workers_[entry.second];
        }
    }

    return nullptr;
}

std::vector<WorkerInfo> TenantManager::getWorkerInfos() const {
    std::vector<WorkerInfo> infos;
    infos.reserve(workers_.size());
    for (const auto& pair : workers_) {
        WorkerInfo info;
        info.name = pair.second.name;
        info.route = pair.second.route;
        infos.push_back(info);
    }
    return infos;
}

size_t TenantManager::workerCount() const {
    return workers_.size();
}

void TenantManager::deinit() {
    if (!runtime_) return;

    for (auto& pair : workers_) {
        runtime_->destroyWorker(pair.second.handle);
        pair.second.handle = nullptr;
    }
    workers_.clear();
    exactRoutes_.clear();
    prefixRoutes_.clear();
    runtime_ = nullptr;
}

void TenantManager::rebuildRouteIndex() {
    exactRoutes_.clear();
    prefixRoutes_.clear();

    for (const auto& pair : workers_) {
        const std::string& route = pair.second.route;
        const std::string& name = pair.second.name;

        // Check if route ends with /*
        if (route.size() >= 2 && route.substr(route.size() - 2) == "/*") {
            // Store prefix without the trailing /*
            std::string prefix = route.substr(0, route.size() - 2);
            prefixRoutes_.push_back({prefix, name});
        } else {
            exactRoutes_[route] = name;
        }
    }

    // Sort prefix routes by length descending (longest prefix first)
    std::sort(prefixRoutes_.begin(), prefixRoutes_.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) {
                  return a.first.size() > b.first.size();
              });
}

} // namespace serverless
