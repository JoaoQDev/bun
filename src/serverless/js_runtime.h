#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace serverless {

// Opaque handle returned by JsRuntime::createWorker().
// The caller must not dereference or inspect this — only pass it back to JsRuntime methods.
struct WorkerHandle;

// Result of invoking a worker's fetch handler.
struct FetchResult {
    int status;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool ok; // false if the call itself failed (exception, non-Response, etc.)
    std::string error; // populated when ok == false
};

// JsRuntime handles everything JSC-related:
// - VM lifecycle
// - GlobalObject creation per worker
// - ESM module loading
// - fetch() invocation
//
// JsRuntime has NO knowledge of route names, worker names, or routing logic.
class JsRuntime {
public:
    JsRuntime();
    ~JsRuntime();

    // Initializes the runtime with the JSC::VM created by the Zig runtime.
    // The jsc_vm parameter is a pointer to JSC::VM (passed as void* to avoid JSC headers in this header).
    // Must be called before any other method.
    // Returns true on success.
    bool init(void* jsc_vm);

    // Creates a Zig::GlobalObject, loads the ESM script, extracts `export default { fetch }`.
    // Returns an opaque WorkerHandle* on success, nullptr on failure.
    // On failure, prints an error to stderr.
    // workerName is used for descriptive error messages only.
    WorkerHandle* createWorker(const std::string& scriptPath, const std::string& workerName);

    // Acquires JSC lock, invokes the fetch handler with the given request data,
    // awaits the returned Promise, extracts status/headers/body.
    // Returns a FetchResult.
    FetchResult callFetch(WorkerHandle* handle,
                          const std::string& method,
                          const std::string& url,
                          const std::vector<std::pair<std::string, std::string>>& headers,
                          const std::string& body);

    // Unprotects the GlobalObject from GC and frees resources associated with this handle.
    void destroyWorker(WorkerHandle* handle);

    // Destroys all remaining handles and the VM.
    // Only place that destroys the VM.
    void deinit();

    // Returns current heap size in bytes (for metrics).
    size_t heapSizeBytes() const;

    // Returns heap capacity in bytes (for metrics).
    size_t heapCapacityBytes() const;

private:
    struct Impl;
    Impl* impl_;

    // Non-copyable
    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;
};

} // namespace serverless
