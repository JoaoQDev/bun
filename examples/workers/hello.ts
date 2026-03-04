export default {
  async fetch(request: Request): Promise<Response> {
    return new Response("Hello from bun-serverless!", {
      headers: { "Content-Type": "text/plain" },
    });
  },
};
