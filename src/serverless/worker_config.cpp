#include "worker_config.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>
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

namespace serverless {

// Minimal JSON parser for the workers.json format.
// Supports: strings, arrays, objects with string values. No numbers/bools/null needed.
namespace {

struct Parser {
    const char* src;
    size_t pos;
    size_t len;
    std::string error;

    Parser(const char* data, size_t length) : src(data), pos(0), len(length) {}

    void skipWhitespace() {
        while (pos < len && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r'))
            pos++;
    }

    bool expect(char c) {
        skipWhitespace();
        if (pos < len && src[pos] == c) {
            pos++;
            return true;
        }
        error = std::string("Expected '") + c + "' at position " + std::to_string(pos);
        return false;
    }

    char peek() {
        skipWhitespace();
        return pos < len ? src[pos] : '\0';
    }

    bool parseString(std::string& out) {
        skipWhitespace();
        if (pos >= len || src[pos] != '"') {
            error = "Expected '\"' at position " + std::to_string(pos);
            return false;
        }
        pos++; // skip opening quote
        out.clear();
        while (pos < len && src[pos] != '"') {
            if (src[pos] == '\\') {
                pos++;
                if (pos >= len) { error = "Unterminated string escape"; return false; }
                switch (src[pos]) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    default: out += src[pos]; break;
                }
            } else {
                out += src[pos];
            }
            pos++;
        }
        if (pos >= len) { error = "Unterminated string"; return false; }
        pos++; // skip closing quote
        return true;
    }

    // Skip any JSON value (for unknown keys)
    bool skipValue() {
        skipWhitespace();
        if (pos >= len) { error = "Unexpected end of input"; return false; }
        char c = src[pos];
        if (c == '"') {
            std::string dummy;
            return parseString(dummy);
        } else if (c == '{') {
            pos++;
            skipWhitespace();
            if (peek() == '}') { pos++; return true; }
            while (true) {
                std::string key;
                if (!parseString(key)) return false;
                if (!expect(':')) return false;
                if (!skipValue()) return false;
                skipWhitespace();
                if (peek() == '}') { pos++; return true; }
                if (!expect(',')) return false;
            }
        } else if (c == '[') {
            pos++;
            skipWhitespace();
            if (peek() == ']') { pos++; return true; }
            while (true) {
                if (!skipValue()) return false;
                skipWhitespace();
                if (peek() == ']') { pos++; return true; }
                if (!expect(',')) return false;
            }
        } else if (c == 't' || c == 'f' || c == 'n') {
            // true, false, null
            while (pos < len && src[pos] >= 'a' && src[pos] <= 'z') pos++;
            return true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            // number
            if (c == '-') pos++;
            while (pos < len && ((src[pos] >= '0' && src[pos] <= '9') || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' || src[pos] == '+' || src[pos] == '-'))
                pos++;
            return true;
        }
        error = std::string("Unexpected character '") + c + "' at position " + std::to_string(pos);
        return false;
    }

    bool parseWorkerEntry(WorkerEntry& entry) {
        if (!expect('{')) return false;
        bool hasName = false, hasRoute = false, hasScript = false;
        skipWhitespace();
        if (peek() == '}') { pos++; goto check_fields; }
        while (true) {
            std::string key;
            if (!parseString(key)) return false;
            if (!expect(':')) return false;

            if (key == "name") {
                if (!parseString(entry.name)) return false;
                hasName = true;
            } else if (key == "route") {
                if (!parseString(entry.route)) return false;
                hasRoute = true;
            } else if (key == "script") {
                if (!parseString(entry.script)) return false;
                hasScript = true;
            } else {
                if (!skipValue()) return false;
            }

            skipWhitespace();
            if (peek() == '}') { pos++; break; }
            if (!expect(',')) return false;
        }

    check_fields:
        if (!hasName) { error = "Worker entry missing required field: \"name\""; return false; }
        if (!hasRoute) { error = "Worker entry missing required field: \"route\""; return false; }
        if (!hasScript) { error = "Worker entry missing required field: \"script\""; return false; }
        return true;
    }

    bool parseWorkersArray(std::vector<WorkerEntry>& workers) {
        if (!expect('[')) return false;
        skipWhitespace();
        if (peek() == ']') { pos++; return true; }
        while (true) {
            WorkerEntry entry;
            if (!parseWorkerEntry(entry)) return false;
            workers.push_back(std::move(entry));
            skipWhitespace();
            if (peek() == ']') { pos++; return true; }
            if (!expect(',')) return false;
        }
    }

    bool parseConfig(WorkerConfig& config) {
        if (!expect('{')) return false;
        skipWhitespace();
        if (peek() == '}') { pos++; return true; }
        while (true) {
            std::string key;
            if (!parseString(key)) return false;
            if (!expect(':')) return false;

            if (key == "workers") {
                if (!parseWorkersArray(config.workers)) return false;
            } else {
                if (!skipValue()) return false;
            }

            skipWhitespace();
            if (peek() == '}') { pos++; break; }
            if (!expect(',')) return false;
        }
        return true;
    }
};

std::string getDirectory(const char* filepath) {
#if defined(_WIN32)
    std::string path(filepath);
    size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) return ".";
    return path.substr(0, pos);
#else
    // dirname may modify its argument, so copy
    char* buf = strdup(filepath);
    std::string dir(dirname(buf));
    free(buf);
    return dir;
#endif
}

std::string resolvePath(const std::string& base_dir, const std::string& relative) {
    if (!relative.empty() && relative[0] == PATH_SEP) {
        return relative; // already absolute
    }
    std::string combined = base_dir + PATH_SEP + relative;
#if !defined(_WIN32)
    char resolved[PATH_MAX];
    if (realpath(combined.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }
#endif
    return combined; // fallback if realpath fails (file may not exist yet)
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // anonymous namespace

WorkerConfig WorkerConfig::load(const char* config_path, bool& success) {
    WorkerConfig config;
    success = false;

    // Check if config file exists
    if (!fileExists(config_path)) {
        fprintf(stderr, "[bun-serverless] Error: config file not found: %s\n", config_path);
        return config;
    }

    // Read file
    std::ifstream file(config_path);
    if (!file.is_open()) {
        fprintf(stderr, "[bun-serverless] Error: could not open config file: %s\n", config_path);
        return config;
    }

    std::stringstream buf;
    buf << file.rdbuf();
    std::string content = buf.str();

    // Parse JSON
    Parser parser(content.c_str(), content.size());
    if (!parser.parseConfig(config)) {
        fprintf(stderr, "[bun-serverless] Error: %s\n", parser.error.c_str());
        return config;
    }

    // Resolve script paths relative to config file directory
    std::string config_dir = getDirectory(config_path);

    // Validate: check for duplicate names and resolve paths
    std::unordered_set<std::string> seen_names;
    for (auto& worker : config.workers) {
        // Check duplicate names
        if (seen_names.count(worker.name)) {
            fprintf(stderr, "[bun-serverless] Error: Duplicate worker name: \"%s\"\n", worker.name.c_str());
            return config;
        }
        seen_names.insert(worker.name);

        // Resolve script path relative to config directory
        worker.script = resolvePath(config_dir, worker.script);

        // Check script file exists
        if (!fileExists(worker.script)) {
            fprintf(stderr, "[bun-serverless] Error: worker \"%s\" script not found: %s\n",
                    worker.name.c_str(), worker.script.c_str());
            return config;
        }
    }

    success = true;
    return config;
}

} // namespace serverless
