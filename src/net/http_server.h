// Minimal HTTP/1.1 server — replaces the need for httplib.
// Single-threaded, synchronous, supports GET/POST, static files, JSON API.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <mutex>

namespace kforge::net {

struct Request {
    std::string method;       // "GET", "POST", ...
    std::string path;         // e.g. "/api/status"
    std::string body;         // raw request body
    std::unordered_map<std::string, std::string> params;  // query params

    std::string param(const std::string& key) const {
        auto it = params.find(key);
        return (it != params.end()) ? it->second : "";
    }
};

struct Response {
    int status{200};
    std::string content_type{"application/json"};
    std::string body;
};

using Handler = std::function<void(const Request&, Response&)>;

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    // Route registration
    void get(const std::string& path, Handler h);
    void post(const std::string& path, Handler h);

    // Serve static files from a directory
    void set_doc_root(const std::string& dir) { doc_root_ = dir; }

    // Start listening. Returns false if port is in use.
    bool listen(int port);

    // Stop the server.
    void stop();

private:
    void worker(int port);
    void handle_request(int client_fd);
    void serve_static(const std::string& path, Response& r);
    static std::string mime_type(const std::string& path);

    std::unordered_map<std::string, Handler> get_routes_;
    std::unordered_map<std::string, Handler> post_routes_;
    std::string doc_root_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int server_fd_{-1};

#ifdef _WIN32
    void init_winsock();
#endif
};

}  // namespace kforge::net
