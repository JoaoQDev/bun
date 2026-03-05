#include "http_server.h"
#include "tenant_manager.h"
#include "js_runtime.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <picohttpparser/picohttpparser.h>
#include <nlohmann/json.hpp>

namespace serverless {

static constexpr int    MAX_HEADERS      = 64;
static constexpr size_t MAX_BODY_SIZE    = 4 * 1024 * 1024; // 4MB
static constexpr size_t MAX_HEADER_SIZE  = 8192;             // 8KB

struct HttpRequest {
    std::string method;
    std::string path;
    std::string fullUrl;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool valid = false;
    int errorStatus = 0;
    std::string errorMessage;
};

static HttpRequest parseRequest(int clientFd) {
    HttpRequest req;

    std::string buf;
    buf.reserve(4096);
    char tmp[4096];

    const char* method = nullptr;
    size_t      method_len = 0;
    const char* path = nullptr;
    size_t      path_len = 0;
    int         minor_version = 0;
    struct phr_header headers[MAX_HEADERS];
    size_t      num_headers = MAX_HEADERS;
    size_t      last_len = 0;
    int         pret = -2;

    // Read until picohttpparser has a complete header section
    while (pret == -2) {
        ssize_t n = ::read(clientFd, tmp, sizeof(tmp));
        if (n <= 0) return req;

        buf.append(tmp, static_cast<size_t>(n));

        if (buf.size() > MAX_HEADER_SIZE) {
            req.errorStatus = 431;
            req.errorMessage = "Request Header Fields Too Large";
            return req;
        }

        num_headers = MAX_HEADERS;
        pret = phr_parse_request(buf.c_str(), buf.size(),
                                 &method, &method_len,
                                 &path, &path_len,
                                 &minor_version,
                                 headers, &num_headers,
                                 last_len);
        last_len = buf.size();
    }

    if (pret == -1) {
        req.errorStatus = 400;
        req.errorMessage = "Bad Request";
        return req;
    }

    req.method = std::string(method, method_len);
    req.path   = std::string(path, path_len);

    // Collect headers, look for Content-Length / Transfer-Encoding / Host
    size_t contentLength = 0;
    std::string hostHeader;

    for (size_t i = 0; i < num_headers; i++) {
        std::string name(headers[i].name, headers[i].name_len);
        std::string value(headers[i].value, headers[i].value_len);

        if (strcasecmp(name.c_str(), "content-length") == 0) {
            contentLength = static_cast<size_t>(std::stoul(value));
        } else if (strcasecmp(name.c_str(), "transfer-encoding") == 0) {
            if (value.find("chunked") != std::string::npos) {
                req.errorStatus = 411;
                req.errorMessage = "Chunked transfer encoding not supported";
                return req;
            }
        } else if (strcasecmp(name.c_str(), "host") == 0) {
            hostHeader = value;
        }

        req.headers.push_back({std::move(name), std::move(value)});
    }

    if (contentLength > MAX_BODY_SIZE) {
        req.errorStatus = 413;
        req.errorMessage = "Payload Too Large";
        return req;
    }

    // Body: pret = bytes consumed by headers, rest of buf is body start
    req.body = buf.substr(static_cast<size_t>(pret));
    while (req.body.size() < contentLength) {
        ssize_t n = ::read(clientFd, tmp, sizeof(tmp));
        if (n <= 0) break;
        req.body.append(tmp, static_cast<size_t>(n));
    }
    if (req.body.size() > contentLength)
        req.body.resize(contentLength);

    req.fullUrl = "http://" + (hostHeader.empty() ? "localhost" : hostHeader) + req.path;
    req.valid = true;
    return req;
}

static void sendResponse(int clientFd, int status, const std::string& statusText,
                         const std::vector<std::pair<std::string, std::string>>& headers,
                         const std::string& body) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << statusText << "\r\n";

    bool hasContentLength = false;
    bool hasContentType   = false;
    bool hasConnection    = false;

    for (const auto& h : headers) {
        resp << h.first << ": " << h.second << "\r\n";
        if (strcasecmp(h.first.c_str(), "content-length") == 0) hasContentLength = true;
        if (strcasecmp(h.first.c_str(), "content-type") == 0)   hasContentType   = true;
        if (strcasecmp(h.first.c_str(), "connection") == 0)      hasConnection    = true;
    }

    if (!hasContentLength) resp << "Content-Length: " << body.size() << "\r\n";
    if (!hasContentType)   resp << "Content-Type: text/plain\r\n";
    if (!hasConnection)    resp << "Connection: close\r\n";

    resp << "\r\n" << body;

    std::string responseStr = resp.str();
    const char* data = responseStr.c_str();
    size_t remaining = responseStr.size();
    while (remaining > 0) {
        ssize_t written = ::write(clientFd, data, remaining);
        if (written <= 0) break;
        data      += written;
        remaining -= static_cast<size_t>(written);
    }
}

static void sendJsonError(int clientFd, int status, const std::string& statusText,
                          const std::string& errorMsg) {
    std::string body = nlohmann::json{{"error", errorMsg}}.dump();
    sendResponse(clientFd, status, statusText, {{"Content-Type", "application/json"}}, body);
}

static std::string statusTextForCode(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 505: return "HTTP Version Not Supported";
        default:  return "OK";
    }
}

HttpServer::HttpServer() : serverFd_(-1) {}

HttpServer::~HttpServer() {
    if (serverFd_ >= 0) {
        ::close(serverFd_);
        serverFd_ = -1;
    }
}

int HttpServer::listen(int port, TenantManager* tenantManager, JsRuntime* runtime,
                       std::atomic<bool>* shutdownFlag) {
    serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        fprintf(stderr, "[HttpServer] Error: socket() failed: %s\n", strerror(errno));
        return 1;
    }

    int opt = 1;
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (::bind(serverFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "[HttpServer] Error: bind() failed on port %d: %s\n", port, strerror(errno));
        ::close(serverFd_);
        serverFd_ = -1;
        return 1;
    }

    if (::listen(serverFd_, 128) < 0) {
        fprintf(stderr, "[HttpServer] Error: listen() failed: %s\n", strerror(errno));
        ::close(serverFd_);
        serverFd_ = -1;
        return 1;
    }

    fprintf(stdout, "[bun-serverless] Listening on http://localhost:%d\n", port);
    fflush(stdout);

    while (!shutdownFlag->load(std::memory_order_acquire)) {
        struct pollfd pfd;
        pfd.fd      = serverFd_;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        int pollResult = ::poll(&pfd, 1, 500);
        if (pollResult < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[HttpServer] Error: poll() failed: %s\n", strerror(errno));
            break;
        }
        if (pollResult == 0) continue;

        int clientFd = ::accept(serverFd_, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[HttpServer] Error: accept() failed: %s\n", strerror(errno));
            continue;
        }

        HttpRequest httpReq = parseRequest(clientFd);

        if (httpReq.errorStatus != 0) {
            sendJsonError(clientFd, httpReq.errorStatus,
                          statusTextForCode(httpReq.errorStatus),
                          httpReq.errorMessage);
            ::close(clientFd);
            continue;
        }

        if (!httpReq.valid) {
            ::close(clientFd);
            continue;
        }

        // /_metrics endpoint
        if (httpReq.path == "/_metrics" || httpReq.path.find("/_metrics?") == 0) {
            if (httpReq.method != "GET") {
                sendJsonError(clientFd, 405, "Method Not Allowed", "Method Not Allowed");
            } else {
                nlohmann::json j;
                j["vm_heap_size_bytes"]     = runtime->heapSizeBytes();
                j["vm_heap_capacity_bytes"] = runtime->heapCapacityBytes();
                j["workers"]                = nlohmann::json::array();
                for (const auto& info : tenantManager->getWorkerInfos())
                    j["workers"].push_back({{"name", info.name}, {"route", info.route}});

                sendResponse(clientFd, 200, "OK",
                             {{"Content-Type", "application/json"}},
                             j.dump());
            }
            ::close(clientFd);
            continue;
        }

        // Route to worker
        Worker* worker = tenantManager->route(httpReq.path);
        if (!worker) {
            sendJsonError(clientFd, 404, "Not Found", "No worker found for route");
            ::close(clientFd);
            continue;
        }

        FetchResult fetchResult = runtime->callFetch(
            worker->handle,
            httpReq.method,
            httpReq.fullUrl,
            httpReq.headers,
            httpReq.body
        );

        if (!fetchResult.ok) {
            sendJsonError(clientFd, 500, "Internal Server Error", fetchResult.error);
        } else {
            sendResponse(clientFd, fetchResult.status,
                         statusTextForCode(fetchResult.status),
                         fetchResult.headers, fetchResult.body);
        }

        ::close(clientFd);
    }

    ::close(serverFd_);
    serverFd_ = -1;
    return 0;
}

} // namespace serverless
