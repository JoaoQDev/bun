// Test worker for US-002 validation
// This module follows the worker convention: export default { fetch }

import { readFile } from "node:fs/promises";

export default {
  async fetch(request: Request, env: Record<string, unknown>): Promise<Response> {
    const url = new URL(request.url);

    if (url.pathname === "/node-api-test") {
      // Validate node:fs/promises is available
      // We don't need to actually read a file, just verify the import worked
      const hasReadFile = typeof readFile === "function";
      return new Response(JSON.stringify({ readFileAvailable: hasReadFile }), {
        headers: { "Content-Type": "application/json" },
      });
    }

    return new Response("ok");
  },
};
