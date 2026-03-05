#include "worker_config.h"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define PATH_SEP '\\'
#else
#include <libgen.h>
#include <climits>
#include <cstdlib>
#define PATH_SEP '/'
#endif

#include <nlohmann/json.hpp>

namespace serverless {

namespace {

std::string getDirectory(const char* filepath) {
#if defined(_WIN32)
    std::string path(filepath);
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
#else
    char* buf = strdup(filepath);
    std::string dir(dirname(buf));
    free(buf);
    return dir;
#endif
}

std::string resolvePath(const std::string& base_dir, const std::string& relative) {
    if (!relative.empty() && relative[0] == PATH_SEP)
        return relative; // already absolute
    std::string combined = base_dir + PATH_SEP + relative;
#if !defined(_WIN32)
    char resolved[PATH_MAX];
    if (realpath(combined.c_str(), resolved) != nullptr)
        return std::string(resolved);
#endif
    return combined;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // anonymous namespace

WorkerConfig WorkerConfig::load(const char* config_path, bool& success) {
    WorkerConfig config;
    success = false;

    if (!fileExists(config_path)) {
        fprintf(stderr, "[bun-serverless] Error: config file not found: %s\n", config_path);
        return config;
    }

    std::ifstream file(config_path);
    if (!file.is_open()) {
        fprintf(stderr, "[bun-serverless] Error: could not open config file: %s\n", config_path);
        return config;
    }

    nlohmann::json j = nlohmann::json::parse(file, nullptr, /*exceptions=*/false);
    if (j.is_discarded()) {
        fprintf(stderr, "[bun-serverless] Error: invalid JSON in config file: %s\n", config_path);
        return config;
    }

    if (!j.is_object() || !j.contains("workers") || !j["workers"].is_array()) {
        fprintf(stderr, "[bun-serverless] Error: config must have a \"workers\" array\n");
        return config;
    }

    std::string config_dir = getDirectory(config_path);

    for (const auto& w : j["workers"]) {
        if (!w.is_object()) {
            fprintf(stderr, "[bun-serverless] Error: each worker entry must be an object\n");
            return config;
        }

        for (const char* field : {"name", "route", "script"}) {
            if (!w.contains(field) || !w[field].is_string()) {
                fprintf(stderr, "[bun-serverless] Error: worker entry missing required string field: \"%s\"\n", field);
                return config;
            }
        }

        WorkerEntry entry;
        entry.name   = w["name"].get<std::string>();
        entry.route  = w["route"].get<std::string>();
        entry.script = resolvePath(config_dir, w["script"].get<std::string>());

        // Check for duplicate names
        for (const auto& existing : config.workers) {
            if (existing.name == entry.name) {
                fprintf(stderr, "[bun-serverless] Error: duplicate worker name: \"%s\"\n", entry.name.c_str());
                return config;
            }
        }

        if (!fileExists(entry.script)) {
            fprintf(stderr, "[bun-serverless] Error: worker \"%s\" script not found: %s\n",
                    entry.name.c_str(), entry.script.c_str());
            return config;
        }

        config.workers.push_back(std::move(entry));
    }

    success = true;
    return config;
}

} // namespace serverless
