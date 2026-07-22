#include "net/http_server.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
#define close closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace kforge::net {

// --------------- MIME types ---------------

std::string HttpServer::mime_type(const std::string& path) {
    auto ext = path.find_last_of('.');
    if (ext == std::string::npos) return "application/octet-stream";

    std::string e = path.substr(ext);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);

    if (e == ".html") return "text/html; charset=utf-8";
    if (e == ".css")  return "text/css; charset=utf-8";
    if (e == ".js")   return "application/javascript; charset=utf-8";
    if (e == ".json") return "application/json";
    if (e == ".svg")  return "image/svg+xml";
    if (e == ".png")  return "image/png";
    if (e == ".ico")  return "image/x-icon";
    if (e == ".woff") return "font/woff";
    if (e == ".woff2")return "font/woff2";
    return "application/octet-stream";
}

// --------------- WinSock init ---------------

#ifdef _WIN32
void HttpServer::init_winsock() {
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
}
#endif

// --------------- Constructor / Destructor ---------------

HttpServer::HttpServer() {
#ifdef _WIN32
    init_winsock();
#endif
}

HttpServer::~HttpServer() { stop(); }

// --------------- Route registration ---------------

void HttpServer::get(const std::string& path, Handler h) {
    get_routes_[path] = std::move(h);
}

void HttpServer::post(const std::string& path, Handler h) {
    post_routes_[path] = std::move(h);
}

// --------------- Static file serving ---------------

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void HttpServer::serve_static(const std::string& path, Response& r) {
    // Sanitize path
    std::string safe;
    for (char c : path) {
        if (c == '.' && safe.size() >= 2 && safe[safe.size()-2] == '.') {
            safe.resize(safe.size() - 2);  // strip ".."
            continue;
        }
        if (c == '\\') c = '/';
        safe += c;
    }

    std::string file = doc_root_ + safe;
    // Default to index.html for directory paths
    if (file.back() == '/' || file.find('.', file.rfind('/')) == std::string::npos) {
        if (file.back() != '/') file += '/';
        file += "index.html";
    }

    std::string content = read_file(file);
    if (content.empty()) {
        // Try without extension or with .html
        file = doc_root_ + safe + ".html";
        content = read_file(file);
    }

    if (content.empty()) {
        r.status = 404;
        r.body = "404 Not Found";
        r.content_type = "text/plain";
        return;
    }

    r.status = 200;
    r.body = std::move(content);
    r.content_type = mime_type(file);
}

// --------------- HTTP parsing ---------------

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\r' || s[e-1] == '\n')) e--;
    return s.substr(b, e - b);
}

static std::string url_decode(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = s[i+1], lo = s[i+2];
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            r += (char)(hex(hi) * 16 + hex(lo));
            i += 2;
        } else if (s[i] == '+') {
            r += ' ';
        } else {
            r += s[i];
        }
    }
    return r;
}

static void parse_params(const std::string& query,
                          std::unordered_map<std::string, std::string>& params) {
    size_t pos = 0;
    while (pos < query.size()) {
        auto eq = query.find('=', pos);
        auto amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        if (eq != std::string::npos && eq < amp) {
            params[url_decode(query.substr(pos, eq - pos))]
                = url_decode(query.substr(eq + 1, amp - eq - 1));
        }
        pos = amp + 1;
    }
}

void HttpServer::handle_request(int client_fd) {
    // Read HTTP request
    char buf[65536];
    int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = 0;

    std::string data(buf, n);
    auto hdr_end = data.find("\r\n\r\n");
    if (hdr_end == std::string::npos) { close(client_fd); return; }

    // Parse request line
    auto first_nl = data.find("\r\n");
    std::string req_line = data.substr(0, first_nl);

    // method path HTTP/1.x
    auto sp1 = req_line.find(' ');
    auto sp2 = req_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        close(client_fd); return;
    }

    Request req;
    req.method = req_line.substr(0, sp1);
    std::string full_path = req_line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Strip query string from path
    auto qm = full_path.find('?');
    req.path = full_path.substr(0, qm);
    if (qm != std::string::npos) {
        parse_params(full_path.substr(qm + 1), req.params);
    }

    // Parse body (for POST)
    req.body = data.substr(hdr_end + 4);

    // Route
    Response r;
    bool matched = false;

    if (req.method == "GET") {
        auto it = get_routes_.find(req.path);
        if (it != get_routes_.end()) {
            it->second(req, r);
            matched = true;
        }
    } else if (req.method == "POST") {
        auto it = post_routes_.find(req.path);
        if (it != post_routes_.end()) {
            it->second(req, r);
            matched = true;
        } else {
            // Also check GET routes for POST (some endpoints use either)
            auto git = get_routes_.find(req.path);
            if (git != get_routes_.end()) {
                git->second(req, r);
                matched = true;
            }
        }
    }

    // Fall back to static file serving
    if (!matched && req.method == "GET" && !doc_root_.empty()) {
        serve_static(req.path, r);
    } else if (!matched) {
        r.status = 404;
        r.body = "{\"error\":\"not found\"}";
        r.content_type = "application/json";
    }

    // Build response
    std::ostringstream resp;
    resp << "HTTP/1.1 " << r.status << " ";
    if (r.status == 200) resp << "OK";
    else if (r.status == 404) resp << "Not Found";
    else resp << "Error";
    resp << "\r\n";
    resp << "Content-Type: " << r.content_type << "\r\n";
    resp << "Content-Length: " << r.body.size() << "\r\n";
    resp << "Connection: close\r\n";
    resp << "Access-Control-Allow-Origin: *\r\n";
    resp << "\r\n";
    resp << r.body;

    std::string out = resp.str();
    send(client_fd, out.c_str(), (int)out.size(), 0);
    close(client_fd);
}

// --------------- Server ---------------

void HttpServer::worker(int port) {
    server_fd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        printf("HTTP: socket() failed\n");
        return;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((unsigned short)port);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("HTTP: bind() failed on port %d\n", port);
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    if (::listen(server_fd_, 16) < 0) {
        printf("HTTP: listen() failed\n");
        close(server_fd_);
        server_fd_ = -1;
        return;
    }

    printf("HTTP server listening on port %d\n", port);

    while (running_) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd_, &fds);

        struct timeval tv = {1, 0};  // 1-second timeout for checking running_
        int rc = select(server_fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (rc <= 0) continue;

        struct sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        int client = (int)accept(server_fd_, (struct sockaddr*)&client_addr, &addr_len);
        if (client < 0) continue;

        handle_request(client);
    }

    close(server_fd_);
    server_fd_ = -1;
}

bool HttpServer::listen(int port) {
    if (running_) return false;
    running_ = true;
    thread_ = std::thread(&HttpServer::worker, this, port);
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (server_fd_ >= 0) {
#ifdef _WIN32
        closesocket(server_fd_);
#else
        ::close(server_fd_);
#endif
        server_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

}  // namespace kforge::net
