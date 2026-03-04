#include "js_runtime.h"

#include <cstdio>
#include <vector>

// JSC / Bun includes — available because serverless .cpp files are compiled
// in the same target as src/bun.js/bindings/*.cpp.
#include "root.h"
#include <JavaScriptCore/VM.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSGlobalObject.h>
#include <JavaScriptCore/JSModuleLoader.h>
#include <JavaScriptCore/JSInternalPromise.h>
#include <JavaScriptCore/Completion.h>
#include <JavaScriptCore/Heap.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <JavaScriptCore/Strong.h>
#include <JavaScriptCore/SourceOrigin.h>
#include "ZigGlobalObject.h"
#include "ScriptExecutionContext.h"

namespace serverless {

// Internal representation of a worker handle.
// Contains the JSC objects needed to invoke the fetch handler.
struct WorkerHandle {
    std::string scriptPath;
    Zig::GlobalObject* globalObject = nullptr;
    JSC::Strong<JSC::JSObject> defaultExport; // The cached { fetch } object
    bool valid = false;
};

// Private implementation of JsRuntime (pimpl idiom keeps JSC headers out of the public header).
struct JsRuntime::Impl {
    JSC::VM* vm = nullptr;
    std::vector<WorkerHandle*> handles;
    bool initialized = false;
    int nextContextId = 100; // Start worker context IDs at 100 to avoid conflicts
};

JsRuntime::JsRuntime() : impl_(nullptr) {}

JsRuntime::~JsRuntime() {
    if (impl_) {
        deinit();
    }
}

bool JsRuntime::init(void* jsc_vm) {
    if (impl_) {
        fprintf(stderr, "[JsRuntime] Error: already initialized\n");
        return false;
    }

    if (!jsc_vm) {
        fprintf(stderr, "[JsRuntime] Error: jsc_vm is null\n");
        return false;
    }

    impl_ = new Impl();
    impl_->vm = static_cast<JSC::VM*>(jsc_vm);
    impl_->initialized = true;

    return true;
}

WorkerHandle* JsRuntime::createWorker(const std::string& scriptPath) {
    if (!impl_ || !impl_->initialized) {
        fprintf(stderr, "[JsRuntime] Error: runtime not initialized\n");
        return nullptr;
    }

    JSC::VM& vm = *impl_->vm;
    JSC::JSLockHolder locker(vm);

    // 1. Create a new Zig::GlobalObject for this worker in the shared VM.
    //    Each worker gets its own isolated GlobalObject with a unique context ID.
    int contextId = impl_->nextContextId++;
    auto* structure = Zig::GlobalObject::createStructure(vm);
    if (!structure) {
        fprintf(stderr, "[JsRuntime] Error: failed to create GlobalObject structure for %s\n",
                scriptPath.c_str());
        return nullptr;
    }

    auto* globalObject = Zig::GlobalObject::create(
        vm,
        structure,
        static_cast<Bun::ScriptExecutionContextIdentifier>(contextId)
    );

    if (!globalObject) {
        fprintf(stderr, "[JsRuntime] Error: failed to create GlobalObject for %s\n",
                scriptPath.c_str());
        return nullptr;
    }

    // 2. Protect the GlobalObject from GC while the worker is active.
    JSC::gcProtect(globalObject);

    // 3. Load the worker script as an ESM module using importModule.
    //    importModule returns a promise that resolves to the module namespace object
    //    (unlike loadAndEvaluateModule which resolves to undefined).
    auto moduleKey = WTF::String::fromUTF8(scriptPath.c_str());
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    auto* moduleString = JSC::jsString(vm, moduleKey);
    auto* promise = globalObject->moduleLoader()->importModule(
        globalObject,
        moduleString,
        JSC::jsUndefined(),
        JSC::SourceOrigin()
    );

    if (!promise || scope.exception()) {
        if (auto* exception = scope.exception()) {
            scope.clearException();
            auto errorStr = exception->value().toWTFString(globalObject);
            fprintf(stderr, "[JsRuntime] Error loading module %s: %s\n",
                    scriptPath.c_str(), errorStr.utf8().data());
        } else {
            fprintf(stderr, "[JsRuntime] Error: importModule returned null for %s\n",
                    scriptPath.c_str());
        }
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    // 4. Drain microtasks to resolve the module loading promise.
    vm.drainMicrotasks();

    // Check promise status
    auto status = promise->status();
    if (status == JSC::JSPromise::Status::Rejected) {
        auto rejectionValue = promise->result();
        auto errorStr = rejectionValue.toWTFString(globalObject);
        fprintf(stderr, "[JsRuntime] Error loading module %s: %s\n",
                scriptPath.c_str(), errorStr.utf8().data());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    if (status == JSC::JSPromise::Status::Pending) {
        fprintf(stderr, "[JsRuntime] Error: module loading did not complete for %s\n",
                scriptPath.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    // 5. Extract the default export from the module namespace.
    //    importModule resolves to the module namespace object.
    auto promiseResult = promise->result();
    JSC::JSObject* moduleNamespace = nullptr;

    if (promiseResult.isObject()) {
        moduleNamespace = promiseResult.getObject();
    }

    if (!moduleNamespace) {
        fprintf(stderr, "[JsRuntime] Error: could not get module namespace for %s\n",
                scriptPath.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    // Get the default export
    auto defaultIdent = vm.propertyNames->defaultKeyword;
    auto defaultExportValue = moduleNamespace->get(globalObject, defaultIdent);

    if (scope.exception()) {
        scope.clearException();
        fprintf(stderr, "[JsRuntime] Error: exception accessing default export for %s\n",
                scriptPath.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    if (defaultExportValue.isUndefinedOrNull()) {
        fprintf(stderr, "[JsRuntime] Error: Worker \"%s\": no default export found. "
                "Workers must use: export default { fetch(request, env) { ... } }\n",
                scriptPath.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    if (!defaultExportValue.isObject()) {
        fprintf(stderr, "[JsRuntime] Error: Worker \"%s\": default export must be an object with a fetch method\n",
                scriptPath.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    auto* defaultExportObj = defaultExportValue.getObject();

    // 6. Verify the default export has a callable 'fetch' property.
    auto fetchIdent = JSC::Identifier::fromString(vm, "fetch"_s);
    auto fetchValue = defaultExportObj->get(globalObject, fetchIdent);

    if (scope.exception()) {
        scope.clearException();
        fprintf(stderr, "[JsRuntime] Error: exception accessing fetch property for %s\n",
                scriptPath.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    if (!fetchValue.isCallable()) {
        fprintf(stderr, "[JsRuntime] Error: Worker \"%s\": default export must be an object with a fetch method\n",
                scriptPath.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    // 7. Create the handle with all JSC references.
    auto* handle = new WorkerHandle();
    handle->scriptPath = scriptPath;
    handle->globalObject = globalObject;
    handle->defaultExport.set(vm, defaultExportObj);
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

    if (handle->globalObject && impl_ && impl_->vm) {
        JSC::JSLockHolder locker(*impl_->vm);
        handle->defaultExport.clear();
        JSC::gcUnprotect(handle->globalObject);
        handle->globalObject = nullptr;
    }

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

    if (impl_->vm) {
        JSC::JSLockHolder locker(*impl_->vm);

        for (auto* handle : impl_->handles) {
            if (handle) {
                if (handle->globalObject) {
                    handle->defaultExport.clear();
                    JSC::gcUnprotect(handle->globalObject);
                    handle->globalObject = nullptr;
                }
                handle->valid = false;
                delete handle;
            }
        }
        impl_->handles.clear();
    }

    // We do NOT destroy the VM — it was created by the Zig runtime and is owned there.
    impl_->vm = nullptr;

    delete impl_;
    impl_ = nullptr;
}

size_t JsRuntime::heapSizeBytes() const {
    if (!impl_ || !impl_->initialized || !impl_->vm) return 0;
    return impl_->vm->heap.size();
}

size_t JsRuntime::heapCapacityBytes() const {
    if (!impl_ || !impl_->initialized || !impl_->vm) return 0;
    return impl_->vm->heap.capacity();
}

} // namespace serverless
