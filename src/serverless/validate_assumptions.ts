#!/usr/bin/env bun
// US-002: Validate technical assumptions — GlobalObject, ESM, JS invocation
//
// This validation script proves that Bun's JSC infrastructure supports the
// serverless worker pattern. Each hypothesis is tested and reported.
//
// Run: bun src/serverless/validate_assumptions.ts
// Exit code 0 = all pass, 1 = any failure

const CHECK = "\u2713";
const CROSS = "\u2717";

let allPassed = true;

function report(passed: boolean, description: string): void {
  if (passed) {
    console.log(`  ${CHECK} ${description}`);
  } else {
    console.log(`  ${CROSS} ${description}`);
    allPassed = false;
  }
}

async function main(): Promise<void> {
  console.log("[US-002] Validating technical assumptions...\n");

  // Hypothesis 1: Load a trivial ESM module with export default { fetch }
  let workerModule: any;
  try {
    workerModule = await import("./test_worker.ts");
    report(true, "ESM module loaded successfully via dynamic import");
  } catch (e) {
    report(false, `ESM module loading failed: ${e}`);
    process.exit(1);
  }

  // Hypothesis 2: Extract the default export
  const defaultExport = workerModule.default;
  report(
    defaultExport !== undefined && defaultExport !== null && typeof defaultExport === "object",
    "Default export is a valid object",
  );

  // Hypothesis 3: Verify default export has a callable fetch property
  report(typeof defaultExport?.fetch === "function", "Default export has a callable 'fetch' property");

  // Hypothesis 4: Invoke the fetch function with a Request and get a valid Response
  let response: Response | undefined;
  try {
    const request = new Request("http://localhost/test");
    const env = {}; // placeholder env object
    response = await defaultExport.fetch(request, env);
    report(response instanceof Response, "fetch() invocation returns a Response instance");
  } catch (e) {
    report(false, `fetch() invocation failed: ${e}`);
  }

  // Hypothesis 5: Verify the Response body is correct
  if (response) {
    const body = await response.text();
    report(body === "ok", `Response body is correct (got: "${body}")`);
  } else {
    report(false, "Response body check skipped (no response)");
  }

  // Hypothesis 6: Validate that import { readFile } from "node:fs/promises" works
  let nodeApiResponse: Response | undefined;
  try {
    const request = new Request("http://localhost/node-api-test");
    nodeApiResponse = await defaultExport.fetch(request, {});
    const json = await nodeApiResponse.json();
    report(json.readFileAvailable === true, "node:fs/promises readFile is available inside worker module");
  } catch (e) {
    report(false, `node:fs/promises validation failed: ${e}`);
  }

  // Hypothesis 7: Validate Promise resolution via JSC's microtask queue
  // The fact that we successfully awaited the async fetch() above proves this,
  // but let's also validate a direct Promise chain
  try {
    const promiseResult = await new Promise<string>(resolve => {
      // Use queueMicrotask to validate microtask queue
      queueMicrotask(() => {
        resolve("microtask-resolved");
      });
    });
    report(promiseResult === "microtask-resolved", "Promise resolves correctly via JSC microtask queue");
  } catch (e) {
    report(false, `Promise resolution failed: ${e}`);
  }

  // Hypothesis 8: Validate Web APIs available in GlobalObject context
  try {
    const webApisAvailable =
      typeof fetch === "function" &&
      typeof crypto !== "undefined" &&
      typeof URL === "function" &&
      typeof URLSearchParams === "function" &&
      typeof TextEncoder === "function" &&
      typeof TextDecoder === "function" &&
      typeof Response === "function" &&
      typeof Request === "function";
    report(webApisAvailable, "Web APIs (fetch, crypto, URL, TextEncoder, etc.) are available in GlobalObject");
  } catch (e) {
    report(false, `Web API validation failed: ${e}`);
  }

  // Summary
  console.log("");
  if (allPassed) {
    console.log("[US-002] All hypotheses validated successfully!");
    console.log("[US-002] The PoC approach using Bun's JSC infrastructure is viable.");
  } else {
    console.log("[US-002] Some hypotheses FAILED. The PoC approach needs reevaluation.");
    process.exit(1);
  }
}

main().catch(e => {
  console.error(`[US-002] ${CROSS} Validation script crashed: ${e}`);
  process.exit(1);
});
