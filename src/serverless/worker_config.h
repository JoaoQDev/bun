#pragma once

#include <string>
#include <vector>

namespace serverless {

struct WorkerEntry {
    std::string name;
    std::string route;
    std::string script; // Resolved absolute path
};

struct WorkerConfig {
    std::vector<WorkerEntry> workers;

    // Loads and parses workers.json from the given path.
    // Returns a WorkerConfig on success.
    // On failure, prints an error to stderr and returns an empty optional-like result (success == false).
    static WorkerConfig load(const char* config_path, bool& success);
};

} // namespace serverless
