#include "http_server.h"
#include "tenant_manager.h"
#include "js_runtime.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace serverless {

static constexpr int MAX_HEADERS = 64;
static constexpr size_t MAX_BODY_SIZE = 4 * 1024 * 1024; // 4MB
static constexpr size_t MAX_HEADER_SECTION = 8192;        // 8KB for request line + headers

// Parsed HTTP request.
struct HttpRequest {
    std::string method;
    std::string path;
    std::string fullUrl;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool valid = false;
    int errorStatus = 0; // Non-zero if we should respond with an error immediately
    std::string errorMessage;
};

// Read all data from a socket up to the end of the header section (\r\n\r\n),
// then read body based on Content-Length.
static HttpRequest parseRequest(int clientFd) {
    HttpRequest req;

    // Read header section
    std::string headerBuf;
    headerBuf.reserve(4096);
    char buf[4096];
    size_t headerEnd = std::string::npos;

    while (headerEnd == std::string::npos) {
        ssize_t n = ::read(clientFd, buf, sizeof(buf));
        if (n <= 0) {
            return req; // Connection closed or error
        }
        headerBuf.append(buf, static_cast<size_t>(n));
        headerEnd = headerBuf.find("\r\n\r\n");

        if (headerEnd == std::string::npos && headerBuf.size() > MAX_HEADER_SECTION) {
            req.errorStatus = 431;
            req.errorMessage = "Request Header Fields Too Large";
            return req;
        }
    }

    // Split header section from any body data already read
    std::string headerSection = headerBuf.substr(0, headerEnd);
    std::string bodyStart = headerBuf.substr(headerEnd + 4);

    // Parse request line
    size_t firstLine = headerSection.find("\r\n");
    std::string requestLine = (firstLine != std::string::npos)
        ? headerSection.substr(0, firstLine)
        : headerSection;

    // Parse: METHOD PATH HTTP/1.1
    size_t sp1 = requestLine.find(' ');
    if (sp1 == std::string::npos) {
        req.errorStatus = 400;
        req.errorMessage = "Bad Request";
        return req;
    }
    size_t sp2 = requestLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) {
        req.errorStatus = 400;
        req.errorMessage = "Bad Request";
        return req;
    }

    req.method = requestLine.substr(0, sp1);
    req.path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    std::string httpVersion = requestLine.substr(sp2 + 1);
    if (httpVersion != "HTTP/1.1" && httpVersion != "HTTP/1.0") {
        req.errorStatus = 505;
        req.errorMessage = "HTTP Version Not Supported";
        return req;
    }

    // Parse headers
    std::string headersStr = (firstLine != std::string::npos)
        ? headerSection.substr(firstLine + 2)
        : "";

    size_t pos = 0;
    int headerCount = 0;
    while (pos < headersStr.size()) {
        size_t lineEnd = headersStr.find("\r\n", pos);
        if (lineEnd == std::string::npos) lineEnd = headersStr.size();
        std::string line = headersStr.substr(pos, lineEnd - pos);
        pos = lineEnd + 2;

        if (line.empty()) break;

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // Trim leading whitespace from value
        size_t valStart = value.find_first_not_of(' ');
        if (valStart != std::string::npos) value = value.substr(valStart);

        headerCount++;
        if (headerCount > MAX_HEADERS) {
            req.errorStatus = 431;
            req.errorMessage = "Too many headers";
            return req;
        }

        req.headers.push_back({name, value});
    }

    // Determine content length
    size_t contentLength = 0;
    bool hasTransferEncodingChunked = false;
    std::string hostHeader;
    for (const auto& h : req.headers) {
        if (strcasecmp(h.first.c_str(), "Content-Length") == 0) {
            contentLength = static_cast<size_t>(std::stoul(h.second));
        } else if (strcasecmp(h.first.c_str(), "Transfer-Encoding") == 0) {
            if (h.second.find("chunked") != std::string::npos) {
                hasTransferEncodingChunked = true;
            }
        } else if (strcasecmp(h.first.c_str(), "Host") == 0) {
            hostHeader = h.second;
        }
    }

    if (hasTransferEncodingChunked) {
        req.errorStatus = 411;
        req.errorMessage = "Chunked transfer encoding not supported";
        return req;
    }

    if (contentLength > MAX_BODY_SIZE) {
        req.errorStatus = 413;
        req.errorMessage = "Payload Too Large";
        return req;
    }

    // Read body
    req.body = bodyStart;
    while (req.body.size() < contentLength) {
        ssize_t n = ::read(clientFd, buf, sizeof(buf));
        if (n <= 0) break;
        req.body.append(buf, static_cast<size_t>(n));
    }
    if (req.body.size() > contentLength) {
        req.body.resize(contentLength);
    }

    // Build full URL
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
    bool hasContentType = false;
    bool hasConnection = false;
    for (const auto& h : headers) {
        resp << h.first << ": " << h.second << "\r\n";
        if (strcasecmp(h.first.c_str(), "Content-Length") == 0) hasContentLength = true;
        if (strcasecmp(h.first.c_str(), "Content-Type") == 0) hasContentType = true;
        if (strcasecmp(h.first.c_str(), "Connection") == 0) hasConnection = true;
    }

    if (!hasContentLength) {
        resp << "Content-Length: " << body.size() << "\r\n";
    }
    if (!hasContentType) {
        resp << "Content-Type: text/plain\r\n";
    }
    if (!hasConnection) {
        resp << "Connection: close\r\n";
    }

    resp << "\r\n";
    resp << body;

    std::string responseStr = resp.str();
    const char* data = responseStr.c_str();
    size_t remaining = responseStr.size();
    while (remaining > 0) {
        ssize_t written = ::write(clientFd, data, remaining);
        if (written <= 0) break;
        data += written;
        remaining -= static_cast<size_t>(written);
    }
}

static void sendJsonError(int clientFd, int status, const std::string& statusText,
                           const std::string& errorMsg) {
    std::string body = "{\"error\":\"" + errorMsg + "\"}";
    std::vector<std::pair<std::string, std::string>> headers;
    headers.push_back({"Content-Type", "application/json"});
    sendResponse(clientFd, status, statusText, headers, body);
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
        default: return "Error";
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
    // Create socket
    serverFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        fprintf(stderr, "[HttpServer] Error: socket() failed: %s\n", strerror(errno));
        return 1;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

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

    // Main accept loop — use poll() so we can check the shutdown flag
    while (!shutdownFlag->load(std::memory_order_acquire)) {
        struct pollfd pfd;
        pfd.fd = serverFd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pollResult = ::poll(&pfd, 1, 500); // 500ms timeout
        if (pollResult < 0) {
            if (errno == EINTR) continue; // Signal interrupted, check shutdown flag
            fprintf(stderr, "[HttpServer] Error: poll() failed: %s\n", strerror(errno));
            break;
        }
        if (pollResult == 0) continue; // Timeout, check shutdown flag

        int clientFd = ::accept(serverFd_, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[HttpServer] Error: accept() failed: %s\n", strerror(errno));
            continue;
        }

        // Parse the HTTP request
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

        // Intercept /_metrics (handled by US-009, for now return 501)
        if (httpReq.path == "/_metrics" || httpReq.path.find("/_metrics?") == 0) {
            if (httpReq.method != "GET") {
                sendJsonError(clientFd, 405, "Method Not Allowed",
                             "Method Not Allowed");
            } else {
                // Build metrics JSON
                size_t heapSize = runtime->heapSizeBytes();
                size_t heapCapacity = runtime->heapCapacityBytes();
                auto workerInfos = tenantManager->getWorkerInfos();

                std::ostringstream json;
                json << "{\"vm_heap_size_bytes\":" << heapSize
                     << ",\"vm_heap_capacity_bytes\":" << heapCapacity
                     << ",\"workers\":[";
                for (size_t i = 0; i < workerInfos.size(); i++) {
                    if (i > 0) json << ",";
                    json << "{\"name\":\"" << workerInfos[i].name
                         << "\",\"route\":\"" << workerInfos[i].route << "\"}";
                }
                json << "]}";

                std::string body = json.str();
                std::vector<std::pair<std::string, std::string>> headers;
                headers.push_back({"Content-Type", "application/json"});
                sendResponse(clientFd, 200, "OK", headers, body);
            }
            ::close(clientFd);
            continue;
        }

        // Route the request
        Worker* worker = tenantManager->route(httpReq.path);
        if (!worker) {
            sendJsonError(clientFd, 404, "Not Found", "No worker found for route");
            ::close(clientFd);
            continue;
        }

        // Call the worker's fetch handler
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
            std::string statusText = statusTextForCode(fetchResult.status);
            if (statusText == "Error") {
                // For non-standard status codes, use a generic text
                statusText = "OK";
            }
            sendResponse(clientFd, fetchResult.status, statusText,
                         fetchResult.headers, fetchResult.body);
        }

        ::close(clientFd);
    }

    ::close(serverFd_);
    serverFd_ = -1;
    return 0;
}

} // namespace serverless
