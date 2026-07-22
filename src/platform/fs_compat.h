// Minimal std::filesystem replacement for GCC 16/MinGW compatibility.
// GCC 16 on MinGW lacks codecvt symbols needed by <filesystem>.
// Provides the subset we use: Path, directory_iterator, exists(), etc.
#pragma once

#include <string>
#include <vector>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kforge::platform {

// Forward declarations
class Path;
bool exists(const Path& p);
bool is_directory(const Path& p);
bool is_regular_file(const Path& p);

class Path {
public:
    Path() = default;
    Path(const char* s) : str_(s) { normalize(); }
    Path(std::string s) : str_(std::move(s)) { normalize(); }

    Path operator/(const char* sub) const {
        if (str_.empty() || str_ == ".") return Path(sub);
        std::string r = str_;
        if (r.back() != '/' && r.back() != '\\') r += '/';
        r += sub;
        return Path(r);
    }
    Path operator/(const std::string& sub) const { return *this / sub.c_str(); }

    Path parent_path() const {
        auto p = str_.find_last_of("\\/");
        if (p == std::string::npos) return Path("");
        return Path(str_.substr(0, p));
    }
    Path filename() const {
        auto p = str_.find_last_of("\\/");
        if (p == std::string::npos) return *this;
        return Path(str_.substr(p + 1));
    }
    Path extension() const {
        auto& s = str_;
        auto d = s.find_last_of("\\/");
        auto p = s.find_last_of('.');
        if (p == std::string::npos || (d != std::string::npos && p < d)) return Path("");
        return Path(s.substr(p));
    }
    Path stem() const {
        auto fn = filename().string();
        auto p = fn.find_last_of('.');
        return Path((p != std::string::npos) ? fn.substr(0, p) : fn);
    }
    Path make_preferred() const { return *this; }
    std::string string() const { return str_; }
#ifdef _WIN32
    std::wstring wstring() const {
        int n = MultiByteToWideChar(CP_UTF8, 0, str_.c_str(), -1, nullptr, 0);
        std::wstring w(n - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, str_.c_str(), -1, &w[0], n);
        return w;
    }
#else
    std::string wstring() const { return str_; }
#endif
    const std::string& native() const { return str_; }
    const char* c_str() const { return str_.c_str(); }

    Path& operator/=(const char* sub) { *this = *this / sub; return *this; }
    Path& operator/=(const std::string& sub) { *this = *this / sub; return *this; }
    bool empty() const { return str_.empty(); }
    bool operator==(const Path& o) const { return str_ == o.str_; }

    // directory_entry compatibility
    const Path& path() const { return *this; }
    bool is_regular_file() const { return std::filesystem::is_regular_file(*this); }
    bool is_directory() const { return std::filesystem::is_directory(*this); }

private:
    void normalize() {
        for (auto& c : str_) if (c == '\\') c = '/';
    }
    std::string str_;
};

inline bool exists(const Path& p) {
#ifdef _WIN32
    return GetFileAttributesA(p.string().c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(p.string().c_str(), &st) == 0;
#endif
}

inline bool is_directory(const Path& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.string().c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.string().c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

inline bool is_regular_file(const Path& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.string().c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.string().c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

inline bool remove_all(const Path& p) {
#ifdef _WIN32
    std::string cmd = "rd /s /q \"" + p.string() + "\"";
    return system(cmd.c_str()) == 0;
#else
    std::string cmd = "rm -rf \"" + p.string() + "\"";
    return system(cmd.c_str()) == 0;
#endif
}

inline bool create_directories(const Path& p) {
    std::string s = p.string();
#ifdef _WIN32
    std::string cur;
    for (size_t i = 0; i < s.size(); i++) {
        cur += s[i];
        if (s[i] == '/' || s[i] == '\\' || i == s.size() - 1) {
            if (!cur.empty() && cur.back() != ':')
                CreateDirectoryA(cur.c_str(), nullptr);
        }
    }
    return is_directory(s);
#else
    std::string cmd = "mkdir -p \"" + s + "\"";
    return system(cmd.c_str()) == 0;
#endif
}

class directory_iterator {
public:
    directory_iterator(const Path& p) : dir_path_(p.string()) {
#ifdef _WIN32
        std::string pattern = dir_path_ + "\\*";
        h_ = FindFirstFileA(pattern.c_str(), &fd_);
        if (h_ == INVALID_HANDLE_VALUE) { done_ = true; return; }
        advance();
#else
        d_ = opendir(dir_path_.c_str());
        if (!d_) { done_ = true; return; }
        advance();
#endif
    }
    ~directory_iterator() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) FindClose(h_);
#else
        if (d_) closedir(d_);
#endif
    }

    bool operator!=(const directory_iterator&) const { return !done_; }
    directory_iterator& operator++() { advance(); return *this; }

    // directory_entry-like accessor
    const Path& path() const { return current_; }
    bool is_regular_file() const { return std::filesystem::is_regular_file(current_); }
    bool is_directory() const { return std::filesystem::is_directory(current_); }

    const Path& operator*() const { return current_; }
    const Path* operator->() const { return &current_; }

    directory_iterator() : done_(true) {}  // end iterator

    // range-for support
    directory_iterator begin() const { return *this; }
    directory_iterator end() const { return directory_iterator(); }

private:
    void advance() {
#ifdef _WIN32
        while (true) {
            if (done_) return;
            std::string name = fd_.cFileName;
            if (name != "." && name != "..") {
                current_ = Path(dir_path_ + "/" + name);
                if (!FindNextFileA(h_, &fd_)) done_ = true;
                return;
            }
            if (!FindNextFileA(h_, &fd_)) { done_ = true; return; }
        }
#else
        while (d_) {
            struct dirent* e = readdir(d_);
            if (!e) { done_ = true; return; }
            std::string name = e->d_name;
            if (name != "." && name != "..") {
                current_ = Path(dir_path_ + "/" + name);
                return;
            }
        }
#endif
    }

    std::string dir_path_;
    Path current_;
    bool done_{false};
#ifdef _WIN32
    HANDLE h_{INVALID_HANDLE_VALUE};
    WIN32_FIND_DATAA fd_;
#else
    DIR* d_{nullptr};
#endif
};

// Recursive variant (used by database.cpp)
class recursive_directory_iterator {
public:
    recursive_directory_iterator() : done_(true) {}
    recursive_directory_iterator(const Path& p) { stack_.push_back(p); advance(); }

    bool operator!=(const recursive_directory_iterator&) const { return !done_; }
    recursive_directory_iterator& operator++() { advance(); return *this; }
    const Path& operator*() const { return current_; }
    const Path* operator->() const { return &current_; }

    const Path& path() const { return current_; }
    bool is_regular_file() const { return std::filesystem::is_regular_file(current_); }
    bool is_directory() const { return std::filesystem::is_directory(current_); }

    // range-for support
    recursive_directory_iterator begin() const { return *this; }
    recursive_directory_iterator end() const { return recursive_directory_iterator(); }

private:
    void advance() {
        while (!stack_.empty()) {
            auto dir = stack_.back();
            stack_.pop_back();
            for (auto& entry : directory_iterator(dir)) {
                if (entry.is_directory()) {
                    stack_.push_back(entry);
                } else {
                    current_ = entry;
                    return;
                }
            }
        }
        done_ = true;
    }
    std::vector<Path> stack_;
    Path current_;
    bool done_{false};
};

}  // namespace kforge::platform
