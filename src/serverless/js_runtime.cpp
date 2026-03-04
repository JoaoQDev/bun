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
#include <JavaScriptCore/ObjectConstructor.h>
#include <JavaScriptCore/FunctionPrototype.h>
#include <JavaScriptCore/JSArray.h>
#include <JavaScriptCore/IteratorOperations.h>
#include <JavaScriptCore/ArrayPrototype.h>
#include <JavaScriptCore/JSPromise.h>
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

WorkerHandle* JsRuntime::createWorker(const std::string& scriptPath, const std::string& workerName) {
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
        // Check if the module has a named 'fetch' export (common mistake: `export function fetch` without `export default`)
        auto fetchIdent = JSC::Identifier::fromString(vm, "fetch"_s);
        auto namedFetchValue = moduleNamespace->get(globalObject, fetchIdent);
        if (scope.exception()) scope.clearException();

        if (namedFetchValue.isCallable()) {
            fprintf(stderr, "[JsRuntime] Error: Worker \"%s\": default export must be an object with a fetch method\n",
                    workerName.c_str());
        } else {
            fprintf(stderr, "[JsRuntime] Error: Worker \"%s\": no default export found. "
                    "Workers must use: export default { fetch(request, env) { ... } }\n",
                    workerName.c_str());
        }
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    if (!defaultExportValue.isObject()) {
        fprintf(stderr, "[JsRuntime] Error: Worker \"%s\": default export must be an object with a fetch method\n",
                workerName.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    auto* defaultExportObj = defaultExportValue.getObject();

    // 6. Verify the default export has a callable 'fetch' property.
    auto fetchIdent = JSC::Identifier::fromString(vm, "fetch"_s);
    auto fetchValue = defaultExportObj->get(globalObject, fetchIdent);

    if (scope.exception()) {
        scope.clearException();
        fprintf(stderr, "[JsRuntime] Error: exception accessing fetch property for worker \"%s\"\n",
                workerName.c_str());
        JSC::gcUnprotect(globalObject);
        return nullptr;
    }

    if (!fetchValue.isCallable()) {
        fprintf(stderr, "[JsRuntime] Error: Worker \"%s\": default export must be an object with a fetch method\n",
                workerName.c_str());
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

    JSC::VM& vm = *impl_->vm;
    JSC::JSLockHolder locker(vm);

    auto* globalObject = handle->globalObject;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    // 1. Construct a JS Request object via `new Request(url, { method, headers, body })`
    auto requestConstructorValue = globalObject->get(globalObject, JSC::Identifier::fromString(vm, "Request"_s));
    if (scope.exception()) {
        scope.clearException();
        result.error = "Failed to get Request constructor";
        return result;
    }

    auto* requestConstructor = JSC::jsDynamicCast<JSC::JSObject*>(requestConstructorValue);
    if (!requestConstructor) {
        result.error = "Request constructor not found";
        return result;
    }

    // Build the init object: { method, headers: {}, body }
    auto* initObj = JSC::constructEmptyObject(globalObject);

    // Set method
    initObj->putDirect(vm, JSC::Identifier::fromString(vm, "method"_s),
                       JSC::jsString(vm, WTF::String::fromUTF8(method.c_str())));

    // Set headers as a plain object
    auto* headersObj = JSC::constructEmptyObject(globalObject);
    for (const auto& h : headers) {
        headersObj->putDirect(vm, JSC::Identifier::fromString(vm, WTF::String::fromUTF8(h.first.c_str())),
                              JSC::jsString(vm, WTF::String::fromUTF8(h.second.c_str())));
    }
    initObj->putDirect(vm, JSC::Identifier::fromString(vm, "headers"_s), headersObj);

    // Set body (only for methods that can have a body)
    if (!body.empty() && method != "GET" && method != "HEAD") {
        initObj->putDirect(vm, JSC::Identifier::fromString(vm, "body"_s),
                           JSC::jsString(vm, WTF::String::fromUTF8(body.c_str())));
    }

    // Construct: new Request(url, init)
    JSC::MarkedArgumentBuffer requestArgs;
    requestArgs.append(JSC::jsString(vm, WTF::String::fromUTF8(url.c_str())));
    requestArgs.append(initObj);

    auto* requestObj = JSC::construct(globalObject, requestConstructor,
                                      requestArgs, "Request"_s);
    if (scope.exception()) {
        auto* exception = scope.exception();
        scope.clearException();
        auto errorStr = exception->value().toWTFString(globalObject);
        result.error = std::string(errorStr.utf8().data());
        return result;
    }

    if (!requestObj) {
        result.error = "Failed to construct Request object";
        return result;
    }

    // 2. Create empty env object (placeholder per US-006 convention)
    auto* envObj = JSC::constructEmptyObject(globalObject);

    // 3. Call fetch(request, env) on the default export
    auto fetchIdent = JSC::Identifier::fromString(vm, "fetch"_s);
    auto fetchFnValue = handle->defaultExport.get()->get(globalObject, fetchIdent);
    if (scope.exception()) {
        scope.clearException();
        result.error = "Failed to get fetch function from worker";
        return result;
    }

    JSC::MarkedArgumentBuffer fetchArgs;
    fetchArgs.append(requestObj);
    fetchArgs.append(envObj);

    auto fetchResultValue = JSC::call(globalObject, fetchFnValue,
                                       handle->defaultExport.get(), fetchArgs, "fetch"_s);
    if (scope.exception()) {
        auto* exception = scope.exception();
        scope.clearException();
        auto errorStr = exception->value().toWTFString(globalObject);
        result.error = std::string(errorStr.utf8().data());
        return result;
    }

    // 4. If the result is a Promise, drain microtasks and resolve it
    JSC::JSValue responseValue = fetchResultValue;
    if (auto* promise = JSC::jsDynamicCast<JSC::JSPromise*>(fetchResultValue)) {
        vm.drainMicrotasks();

        auto status = promise->status();
        if (status == JSC::JSPromise::Status::Rejected) {
            auto rejectionValue = promise->result();
            auto errorStr = rejectionValue.toWTFString(globalObject);
            if (scope.exception()) scope.clearException();
            result.error = std::string(errorStr.utf8().data());
            return result;
        }
        if (status == JSC::JSPromise::Status::Pending) {
            result.error = "Worker fetch handler did not resolve";
            return result;
        }
        responseValue = promise->result();
    }

    // 5. Verify the result is a Response-like object with status/headers/text()
    if (!responseValue.isObject()) {
        result.error = "Worker returned a non-Response value";
        return result;
    }

    auto* responseObj = responseValue.getObject();

    // Check if it has a 'status' property (basic Response duck-typing)
    auto statusIdent = JSC::Identifier::fromString(vm, "status"_s);
    auto statusValue = responseObj->get(globalObject, statusIdent);
    if (scope.exception()) {
        scope.clearException();
        result.error = "Worker returned a non-Response value";
        return result;
    }

    if (!statusValue.isNumber()) {
        result.error = "Worker returned a non-Response value";
        return result;
    }

    result.status = statusValue.asNumber();

    // 6. Extract headers via response.headers (a Headers object)
    auto headersIdent = JSC::Identifier::fromString(vm, "headers"_s);
    auto responseHeadersValue = responseObj->get(globalObject, headersIdent);
    if (scope.exception()) scope.clearException();

    if (responseHeadersValue.isObject()) {
        // Call headers.entries() to iterate
        auto* responseHeaders = responseHeadersValue.getObject();
        auto forEachIdent = JSC::Identifier::fromString(vm, "forEach"_s);
        auto forEachValue = responseHeaders->get(globalObject, forEachIdent);
        if (scope.exception()) scope.clearException();

        if (forEachValue.isCallable()) {
            // Use a simpler approach: get known headers via get()
            // Actually, let's use entries() and iterate
            auto entriesIdent = JSC::Identifier::fromString(vm, "entries"_s);
            auto entriesFnValue = responseHeaders->get(globalObject, entriesIdent);
            if (scope.exception()) scope.clearException();

            if (entriesFnValue.isCallable()) {
                JSC::MarkedArgumentBuffer noArgs;
                auto iteratorValue = JSC::call(globalObject, entriesFnValue,
                                                responseHeaders, noArgs, "entries"_s);
                if (scope.exception()) scope.clearException();

                if (iteratorValue.isObject()) {
                    auto* iterator = iteratorValue.getObject();
                    auto nextIdent = JSC::Identifier::fromString(vm, "next"_s);

                    for (int i = 0; i < 100; i++) { // Safety limit
                        auto nextFnValue = iterator->get(globalObject, nextIdent);
                        if (scope.exception()) { scope.clearException(); break; }
                        if (!nextFnValue.isCallable()) break;

                        auto iterResult = JSC::call(globalObject, nextFnValue,
                                                     iterator, noArgs, "next"_s);
                        if (scope.exception()) { scope.clearException(); break; }
                        if (!iterResult.isObject()) break;

                        auto* iterResultObj = iterResult.getObject();
                        auto doneValue = iterResultObj->get(globalObject,
                            JSC::Identifier::fromString(vm, "done"_s));
                        if (scope.exception()) { scope.clearException(); break; }
                        if (doneValue.isTrue()) break;

                        auto entryValue = iterResultObj->get(globalObject,
                            JSC::Identifier::fromString(vm, "value"_s));
                        if (scope.exception()) { scope.clearException(); break; }
                        if (!entryValue.isObject()) break;

                        auto* entryArr = entryValue.getObject();
                        auto nameVal = entryArr->getIndex(globalObject, 0);
                        auto valVal = entryArr->getIndex(globalObject, 1);
                        if (scope.exception()) { scope.clearException(); break; }

                        if (nameVal.isString() && valVal.isString()) {
                            auto nameStr = nameVal.toWTFString(globalObject);
                            auto valStr = valVal.toWTFString(globalObject);
                            result.headers.push_back({
                                std::string(nameStr.utf8().data()),
                                std::string(valStr.utf8().data())
                            });
                        }
                    }
                }
            }
        }
    }

    // 7. Extract body via response.text()
    auto textIdent = JSC::Identifier::fromString(vm, "text"_s);
    auto textFnValue = responseObj->get(globalObject, textIdent);
    if (scope.exception()) scope.clearException();

    if (textFnValue.isCallable()) {
        JSC::MarkedArgumentBuffer noArgs;
        auto textResult = JSC::call(globalObject, textFnValue,
                                     responseObj, noArgs, "text"_s);
        if (scope.exception()) {
            scope.clearException();
            result.body = "";
        } else if (auto* textPromise = JSC::jsDynamicCast<JSC::JSPromise*>(textResult)) {
            vm.drainMicrotasks();
            if (textPromise->status() == JSC::JSPromise::Status::Fulfilled) {
                auto bodyStr = textPromise->result().toWTFString(globalObject);
                if (scope.exception()) scope.clearException();
                result.body = std::string(bodyStr.utf8().data());
            }
        } else if (textResult.isString()) {
            auto bodyStr = textResult.toWTFString(globalObject);
            result.body = std::string(bodyStr.utf8().data());
        }
    }

    result.ok = true;
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
