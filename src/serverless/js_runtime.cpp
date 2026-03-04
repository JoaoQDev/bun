#include "js_runtime.h"

#include <cstdio>
#include <vector>

namespace serverless {

// Internal representation of a worker handle.
// Contains the JSC objects needed to invoke the fetch handler.
// Stored as opaque WorkerHandle* to external callers.
struct WorkerHandle {
    std::string scriptPath;
    // TODO (US-003): Add JSC::JSGlobalObject*, JSC::Strong<JSC::JSObject> for default export
    bool valid;
};

// Private implementation of JsRuntime (pimpl idiom keeps JSC headers out of the public header).
struct JsRuntime::Impl {
    // TODO (US-003): JSC::VM* vm = nullptr;
    std::vector<WorkerHandle*> handles;
    bool initialized = false;
};

JsRuntime::JsRuntime() : impl_(nullptr) {}

JsRuntime::~JsRuntime() {
    if (impl_) {
        deinit();
    }
}

bool JsRuntime::init() {
    if (impl_) {
        fprintf(stderr, "[JsRuntime] Error: already initialized\n");
        return false;
    }

    impl_ = new Impl();

    // TODO (US-003): Create JSC::VM instance here
    // auto heapSize = JSC::HeapType::Large;
    // impl_->vm = JSC::VM::tryCreate(heapSize).get();

    impl_->initialized = true;
    return true;
}

WorkerHandle* JsRuntime::createWorker(const std::string& scriptPath) {
    if (!impl_ || !impl_->initialized) {
        fprintf(stderr, "[JsRuntime] Error: runtime not initialized\n");
        return nullptr;
    }

    // TODO (US-003): Actual implementation:
    // 1. Create Zig::GlobalObject in the shared VM
    // 2. Load scriptPath as ESM module via JSC::loadAndEvaluateModule
    // 3. Extract default export and verify it has a callable fetch property
    // 4. gcProtect the GlobalObject
    // 5. Store references in the handle

    auto* handle = new WorkerHandle();
    handle->scriptPath = scriptPath;
    handle->valid = true;

    impl_->handles.push_back(handle);

    return handle;
}

FetchResult JsRuntime::callFetch(WorkerHandle* handle,
                                  const std::string& method,
                                  const std::string& url,
                                  const std::vector<std::pair<std::string, std::string>>& headers,
                                  const std::string& body) {
    FetchResult result;
    result.ok = false;

    if (!impl_ || !impl_->initialized) {
        result.error = "Runtime not initialized";
        return result;
    }

    if (!handle || !handle->valid) {
        result.error = "Invalid worker handle";
        return result;
    }

    // TODO (US-005): Actual implementation:
    // 1. Acquire JSC lock (JSC::JSLockHolder)
    // 2. Construct JS Request object from method, url, headers, body
    // 3. Create empty env object {}
    // 4. Call fetch(request, env) via JSC::call on the stored default export
    // 5. Await the returned Promise via vm.drainMicrotasks()
    // 6. Extract Response: status, headers, body -> FetchResult
    // 7. Handle exceptions -> FetchResult with ok=false

    result.error = "Not yet implemented (US-005)";
    return result;
}

void JsRuntime::destroyWorker(WorkerHandle* handle) {
    if (!handle) return;

    // TODO (US-003): gcUnprotect the GlobalObject, release JSC references

    handle->valid = false;

    // Remove from tracked handles
    if (impl_) {
        for (auto it = impl_->handles.begin(); it != impl_->handles.end(); ++it) {
            if (*it == handle) {
                impl_->handles.erase(it);
                break;
            }
        }
    }

    delete handle;
}

void JsRuntime::deinit() {
    if (!impl_) return;

    // Destroy all remaining worker handles
    for (auto* handle : impl_->handles) {
        if (handle) {
            // TODO (US-003): gcUnprotect, release JSC references
            handle->valid = false;
            delete handle;
        }
    }
    impl_->handles.clear();

    // TODO (US-003): Destroy the VM here — only place that destroys the VM
    // delete impl_->vm;
    // impl_->vm = nullptr;

    delete impl_;
    impl_ = nullptr;
}

size_t JsRuntime::heapSizeBytes() const {
    if (!impl_ || !impl_->initialized) return 0;
    // TODO (US-009): Return vm->heap.size()
    return 0;
}

size_t JsRuntime::heapCapacityBytes() const {
    if (!impl_ || !impl_->initialized) return 0;
    // TODO (US-009): Return vm->heap.capacity()
    return 0;
}

} // namespace serverless
