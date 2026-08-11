/**
 * server.h - 元宝 Bot 主服务器头文件（含跨平台兼容层）
 *
 * 编译 (Windows MinGW64):
 *   g++ -std=c++17 -O3 -Wall -I. yuanbao.cpp -o yuanbao_server.exe -lws2_32 -lssl -lcrypto
 *
 * 编译 (Linux):
 *   g++ -std=c++17 -O3 -Wall -I. yuanbao.cpp -o yuanbao_server -lpthread -lrt -lm -lssl -lcrypto
 *
 * 编译 (macOS):
 *   g++ -std=c++17 -O3 -Wall -I. yuanbao.cpp -o yuanbao_server -lpthread -lm -lssl -lcrypto
 */
#pragma once

#include <string>
#include <vector>
#include <deque>
#include <utility>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#ifndef YUANBAO_NO_OPENSSL
  #include <openssl/ssl.h>
#endif

// ═══════════════════════════════════════════════
//  跨平台兼容层
// ═══════════════════════════════════════════════
#if defined(_WIN32) || defined(_WIN64)
  #define YUANBAO_PLATFORM_WINDOWS 1
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  #ifdef REPEAT
    #undef REPEAT
  #endif
  #ifdef DEFAULT
    #undef DEFAULT
  #endif
  #ifdef ALTERNATE
    #undef ALTERNATE
  #endif
  #ifdef RANDOM
    #undef RANDOM
  #endif
  #ifdef DELETE
    #undef DELETE
  #endif
  #ifdef GetMessage
    #undef GetMessage
  #endif
  #ifdef SendMessage
    #undef SendMessage
  #endif
  #ifdef GetObject
    #undef GetObject
  #endif
  #ifdef ERROR
    #undef ERROR
  #endif
  #ifdef RGB
    #undef RGB
  #endif
  #ifdef TRANSPARENT
    #undef TRANSPARENT
  #endif

  #ifndef ssize_t
    typedef SSIZE_T ssize_t;
  #endif

  #ifndef close_socket
    #define close_socket(s) ::closesocket(s)
  #endif
  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif

  #include <direct.h>
  #define mkdir(path, mode) _mkdir(path)
  #define unlink _unlink
  #define localtime_r(tp, tm) localtime_s(tm, tp)

  struct yb_dirent { char d_name[260]; };
  typedef struct { HANDLE handle; WIN32_FIND_DATAA findData; yb_dirent ent; int first; } yb_DIR;

  inline yb_DIR* yb_opendir(const char* path) {
      std::string pattern(path);
      if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
          pattern += "\\*";
      else pattern += "*";
      yb_DIR* d = new yb_DIR();
      d->handle = FindFirstFileA(pattern.c_str(), &d->findData);
      if (d->handle == INVALID_HANDLE_VALUE) { delete d; return nullptr; }
      d->first = 1;
      return d;
  }
  inline yb_dirent* yb_readdir(yb_DIR* d) {
      if (!d) return nullptr;
      if (d->first) { d->first = 0; }
      else { if (!FindNextFileA(d->handle, &d->findData)) return nullptr; }
      strncpy(d->ent.d_name, d->findData.cFileName, sizeof(d->ent.d_name) - 1);
      d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
      return &d->ent;
  }
  inline void yb_closedir(yb_DIR* d) { if (d) { FindClose(d->handle); delete d; } }
  #define opendir yb_opendir
  #define readdir yb_readdir
  #define closedir yb_closedir
  #define dirent yb_dirent
  #define DIR yb_DIR

  #ifndef MAP_FAILED
    #define MAP_FAILED ((void*)-1)
  #endif

#elif defined(__linux__) || defined(__ANDROID__)
  #define YUANBAO_PLATFORM_LINUX 1
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <sys/select.h>
  #include <signal.h>
  #include <netdb.h>
  #include <dirent.h>
  #include <errno.h>
  #define close_socket(s) ::close(s)

#elif defined(__APPLE__) || defined(__MACH__)
  #define YUANBAO_PLATFORM_MACOS 1
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <sys/time.h>
  #include <sys/select.h>
  #include <signal.h>
  #include <netdb.h>
  #include <dirent.h>
  #include <errno.h>
  #define close_socket(s) ::close(s)

#else
  #error "Unsupported platform"
#endif

// ═══════════════════════════════════════════════
//  跨平台网络工具
// ═══════════════════════════════════════════════
namespace yb_platform {

inline bool net_init() {
#ifdef YUANBAO_PLATFORM_WINDOWS
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

inline void net_cleanup() {
#ifdef YUANBAO_PLATFORM_WINDOWS
    WSACleanup();
#endif
}

inline bool set_nonblocking(int fd) {
#ifdef YUANBAO_PLATFORM_WINDOWS
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
#endif
}

inline bool set_reuseaddr(int fd) {
    int opt = 1;
#ifdef YUANBAO_PLATFORM_WINDOWS
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) == 0;
#else
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0;
#endif
}

inline bool set_recv_timeout(int fd, int timeout_ms) {
#ifdef YUANBAO_PLATFORM_WINDOWS
    DWORD tv = (DWORD)timeout_ms;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

inline bool set_send_timeout(int fd, int timeout_ms) {
#ifdef YUANBAO_PLATFORM_WINDOWS
    DWORD tv = (DWORD)timeout_ms;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) == 0;
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

inline uint64_t now_ms() {
#ifdef YUANBAO_PLATFORM_WINDOWS
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000 / freq.QuadPart);
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

} // namespace yb_platform

#ifdef YUANBAO_PLATFORM_WINDOWS
  #define YB_SEND(fd, buf, len, flags) ::send(fd, (const char*)(buf), (int)(len), flags)
  #define YB_RECV(fd, buf, len, flags) ::recv(fd, (char*)(buf), (int)(len), flags)
#else
  #define YB_SEND(fd, buf, len, flags) ::send(fd, buf, len, flags | MSG_NOSIGNAL)
  #define YB_RECV(fd, buf, len, flags) ::recv(fd, buf, len, flags)
#endif
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <memory>
#include <variant>
#include <deque>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdlib>

// ═══════════════════════════════════════════════
//  基础类型别名
// ═══════════════════════════════════════════════
using Bytes = std::vector<uint8_t>;

inline void bytes_append(Bytes& dst, const Bytes& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}
inline Bytes bytes_concat(const Bytes& a, const Bytes& b) {
    Bytes r = a; r.insert(r.end(), b.begin(), b.end()); return r;
}

// ═══════════════════════════════════════════════
//  工具函数
// ═══════════════════════════════════════════════
namespace util {

inline std::string random_id(int len = 16) {
    static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 61);
    std::string r;
    for (int i = 0; i < len; i++) r += chars[dis(gen)];
    return r;
}
// Python uuid.uuid4().hex → 32 字符小写 hex
inline std::string random_hex_id() {
    static const char hx[] = "0123456789abcdef";
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::string r;
    for (int i = 0; i < 32; i++) r += hx[dis(gen)];
    return r;
}

// 解析图片宽高（PNG/JPEG/GIF/BMP），解析失败返回 false
inline bool parse_image_dimensions(const std::string& data, int& w, int& h) {
    w = h = 0;
    if (data.size() < 16) return false;
    // PNG: 8字节签名 + IHDR(宽高在 16-24 字节，大端)
    if (data.size() >= 24 &&
        (uint8_t)data[0] == 0x89 && (uint8_t)data[1] == 0x50 && (uint8_t)data[2] == 0x4E && (uint8_t)data[3] == 0x47) {
        w = ((uint8_t)data[16] << 24) | ((uint8_t)data[17] << 16) | ((uint8_t)data[18] << 8) | (uint8_t)data[19];
        h = ((uint8_t)data[20] << 24) | ((uint8_t)data[21] << 16) | ((uint8_t)data[22] << 8) | (uint8_t)data[23];
        return w > 0 && h > 0;
    }
    // JPEG: FF D8 FF ... 扫描 SOFn 段（C0-CF 但排除 C4/C8/CC）
    if ((uint8_t)data[0] == 0xFF && (uint8_t)data[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < data.size()) {
            if ((uint8_t)data[i] != 0xFF) { i++; continue; }
            unsigned char marker = (uint8_t)data[i + 1];
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                h = ((uint8_t)data[i + 5] << 8) | (uint8_t)data[i + 6];
                w = ((uint8_t)data[i + 7] << 8) | (uint8_t)data[i + 8];
                return w > 0 && h > 0;
            }
            i += 2;
            size_t seg_len = ((uint8_t)data[i] << 8) | (uint8_t)data[i + 1];
            i += seg_len;
        }
        return false;
    }
    // GIF: GIF87a/GIF89a，宽高在 6-10 字节（小端）
    if (data.size() >= 10 &&
        data.compare(0, 3, "GIF") == 0) {
        w = (uint8_t)data[6] | ((uint8_t)data[7] << 8);
        h = (uint8_t)data[8] | ((uint8_t)data[9] << 8);
        return w > 0 && h > 0;
    }
    // BMP: "BM"，宽高在 18-26 字节（小端 int32）
    if (data.size() >= 26 && data.compare(0, 2, "BM") == 0) {
        w = (uint8_t)data[18] | ((uint8_t)data[19] << 8) | ((uint8_t)data[20] << 16) | ((uint8_t)data[21] << 24);
        h = (uint8_t)data[22] | ((uint8_t)data[23] << 8) | ((uint8_t)data[24] << 16) | ((uint8_t)data[25] << 24);
        if (h < 0) h = -h;
        return w > 0 && h > 0;
    }
    return false;
}

// URL 解码：%XX → 字符，+ → 空格
inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)((h << 4) | l);
                i += 2;
                continue;
            }
            out += s[i];
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// ← 补：URL 百分号编码（非 ASCII / 保留字符 → %XX），用于构造带中文参数/路径的 URL
inline std::string url_encode(const std::string& s) {
    static const char* hexdig = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hexdig[(c >> 4) & 0xF];
            out += hexdig[c & 0xF];
        }
    }
    return out;
}

// ← 补：UTF-8 与宽字符互转（Windows 下中文文件名必须用宽字符 API 读写）
inline std::wstring utf8_to_wide(const std::string& s) {
#ifdef YUANBAO_PLATFORM_WINDOWS
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
#else
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = 0; int take = 0;
        if (c < 0x80) { cp = c; take = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; take = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; take = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; take = 4; }
        else { w += (wchar_t)s[i]; i++; continue; }
        if (i + take > s.size()) { i++; continue; }
        for (int k = 1; k < take; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        if (cp > 0xFFFF) { cp -= 0x10000; w += (wchar_t)(0xD800 + (cp >> 10)); w += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
        else w += (wchar_t)cp;
        i += take;
    }
    return w;
#endif
}

inline std::string wide_to_utf8(const std::wstring& w) {
#ifdef YUANBAO_PLATFORM_WINDOWS
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
#else
    std::string s;
    for (wchar_t ch : w) {
        uint32_t cp = (uint32_t)ch;
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            cp = 0x10000 + ((uint32_t)(ch - 0xD800) << 10);
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            cp += (uint32_t)(ch - 0xDC00);
            // 写入合成后的码点
        }
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    return s;
#endif
}

// ← 补：按 UTF-8 路径读取二进制文件（Windows 走宽字符 API，支持中文文件名）
inline bool read_file_utf8(const std::string& path_utf8, std::string& out) {
    std::ifstream f(std::filesystem::path(utf8_to_wide(path_utf8)), std::ios::binary);
    if (!f.good()) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

inline uint64_t now_ms() {
    return yb_platform::now_ms();
}

inline std::string time_iso() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    return std::string(buf);
}

inline std::string current_date() {
    time_t t = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d", &tm_buf);
    return std::string(buf);
}

inline std::string hex_encode(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) oss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return oss.str();
}

inline std::string hex_decode(const std::string& s) {
    std::string r;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        int v;
        std::stringstream ss(s.substr(i, 2));
        ss >> std::hex >> v;
        r += (char)v;
    }
    return r;
}

struct MultipartPart {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string data;
};

/** 解析 multipart/form-data（与前端 FormData 上传一致） */
inline bool parse_multipart(const std::string& content_type, const std::string& body,
                            std::vector<MultipartPart>& parts) {
    size_t bpos = content_type.find("boundary=");
    if (bpos == std::string::npos) return false;
    std::string boundary = "--" + content_type.substr(bpos + 9);
    while (!boundary.empty() && (boundary.back() == '\r' || boundary.back() == '\n' || boundary.back() == '"' || boundary.back() == ' '))
        boundary.pop_back();

    size_t pos = body.find(boundary);
    while (pos != std::string::npos) {
        size_t part_start = pos + boundary.size();
        if (part_start + 1 < body.size() && body[part_start] == '-' && body[part_start + 1] == '-')
            break; // 结尾 --boundary--
        if (part_start + 1 < body.size() && body[part_start] == '\r' && body[part_start + 1] == '\n')
            part_start += 2;
        size_t header_end = body.find("\r\n\r\n", part_start);
        if (header_end == std::string::npos) break;
        std::string headers = body.substr(part_start, header_end - part_start);
        size_t data_start = header_end + 4;
        size_t next_boundary = body.find(boundary, data_start);
        if (next_boundary == std::string::npos) break;

        std::string data = body.substr(data_start, next_boundary - data_start);
        if (!data.empty() && data.size() >= 2 && data.substr(data.size() - 2) == "\r\n")
            data.erase(data.size() - 2);

        MultipartPart part;
        part.content_type = "application/octet-stream";
        size_t ct = headers.find("Content-Type:");
        if (ct != std::string::npos) {
            size_t cs = ct + 13;
            while (cs < headers.size() && (headers[cs] == ' ' || headers[cs] == '\t')) cs++;
            size_t ce = headers.find("\r\n", cs);
            if (ce == std::string::npos) ce = headers.size();
            part.content_type = headers.substr(cs, ce - cs);
        }
        size_t cd = headers.find("name=\"");
        if (cd != std::string::npos) {
            size_t ns = cd + 6;
            size_t ne = headers.find('"', ns);
            if (ne != std::string::npos) part.name = headers.substr(ns, ne - ns);
        }
        size_t fn = headers.find("filename=\"");
        if (fn != std::string::npos) {
            size_t fs = fn + 10;
            size_t fe = headers.find('"', fs);
            if (fe != std::string::npos) part.filename = headers.substr(fs, fe - fs);
        }
        part.data = std::move(data);
        parts.push_back(std::move(part));

        pos = body.find(boundary, data_start);
    }
    return true;
}

inline std::string mime_type(const std::string& path) {
    auto ext = path.substr(path.rfind('.') + 1);
    // ← 修复：HTML/CSS/JS 显式携带 charset=utf-8，避免部分系统/老浏览器按
    //   本地代码页（如 GBK）解析中文字符串导致乱码
    static const std::map<std::string, std::string> m = {
        {"html","text/html; charset=utf-8"},{"htm","text/html; charset=utf-8"},{"css","text/css; charset=utf-8"},
        {"js","application/javascript; charset=utf-8"},{"json","application/json; charset=utf-8"},
        {"png","image/png"},{"jpg","image/jpeg"},{"jpeg","image/jpeg"},
        {"gif","image/gif"},{"svg","image/svg+xml"},{"ico","image/x-icon"},
        {"woff","font/woff"},{"woff2","font/woff2"},{"ttf","font/ttf"},
        {"txt","text/plain"},{"pdf","application/pdf"},{"wasm","application/wasm"},
    };
    auto it = m.find(ext);
    return it != m.end() ? it->second : "application/octet-stream";
}

} // namespace util

// ═══════════════════════════════════════════════
//  ProtoBuf 编解码（完全与 Python sender.py 一致）
// ═══════════════════════════════════════════════
namespace proto {

inline Bytes to_bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }

// pb_varint: 与 Python SimpleProtobufCodec.encode_varint 一致
inline Bytes pb_varint(uint64_t v) {
    Bytes r;
    while (v > 0x7F) { r.push_back((uint8_t)((v & 0x7F) | 0x80)); v >>= 7; }
    r.push_back((uint8_t)(v & 0x7F));
    return r;
}

// pb_tag: 与 Python pb_tag(field, wire) 一致 — tag 也用 varint 编码
inline Bytes pb_tag(int fn, int wire) {
    return pb_varint((uint64_t)((fn << 3) | wire));
}

// pb_string: 与 Python pb_string(field, value) 一致
inline Bytes pb_string(int fn, const std::string& s) {
    Bytes d = to_bytes(s);
    Bytes r = pb_tag(fn, 2);
    auto lv = pb_varint(s.size());
    r.insert(r.end(), lv.begin(), lv.end());
    r.insert(r.end(), d.begin(), d.end());
    return r;
}

// pb_uint32: 与 Python pb_uint32(field, value) 一致
inline Bytes pb_uint32(int fn, uint32_t v) {
    Bytes r = pb_tag(fn, 0);
    auto vi = pb_varint(v);
    r.insert(r.end(), vi.begin(), vi.end());
    return r;
}

// pb_msg: 与 Python pb_msg(field, inner) 一致
inline Bytes pb_msg(int fn, const Bytes& inner) {
    Bytes r = pb_tag(fn, 2);
    auto lv = pb_varint(inner.size());
    r.insert(r.end(), lv.begin(), lv.end());
    r.insert(r.end(), inner.begin(), inner.end());
    return r;
}

// 兼容旧接口
inline Bytes encode_string(int fn, const std::string& s) { return pb_string(fn, s); }
inline Bytes encode_uint32(int fn, uint32_t v) { return pb_uint32(fn, v); }
inline Bytes encode_message(int fn, const Bytes& d) { return pb_msg(fn, d); }

// ==================== Protobuf 解码器 ====================
struct PBField {
    int fn = 0;
    int wire = 0;
    uint64_t varint_val = 0;
    std::string str_val;
    Bytes bytes_val;
    std::vector<PBField> sub_fields;  // 嵌套消息
};

// 从字节数组解码 varint，返回解码值和消耗的字节数
inline std::pair<uint64_t, size_t> decode_varint(const uint8_t* d, size_t max_len) {
    uint64_t v = 0; size_t i = 0; int shift = 0;
    while (i < max_len) { v |= ((uint64_t)(d[i] & 0x7F)) << shift; i++; if (!(d[i-1] & 0x80)) break; shift += 7; }
    return {v, i};
}

// 解析单个 protobuf 消息，返回字段列表
inline std::vector<PBField> decode_pb_message(const uint8_t* d, size_t len) {
    std::vector<PBField> fields;
    size_t pos = 0;
    while (pos < len) {
        auto [tag_raw, tag_len] = decode_varint(d + pos, len - pos);
        pos += tag_len;
        if (tag_raw == 0 && tag_len == 1 && d[pos - 1] == 0) break;
        int fn = (int)(tag_raw >> 3);
        int wire = (int)(tag_raw & 0x07);
        
        PBField f; f.fn = fn; f.wire = wire;
        if (wire == 0) {  // varint
            auto [v, vl] = decode_varint(d + pos, len - pos);
            f.varint_val = v; pos += vl;
        } else if (wire == 2) {  // length-delimited
            auto [vlen, vlen_len] = decode_varint(d + pos, len - pos);
            pos += vlen_len;
            size_t slen = (size_t)vlen;
            if (pos + slen <= len) {
                // 始终保留原始字节（字符串字段可能被误判为嵌套消息，导致 cmd 丢失）
                f.bytes_val = Bytes(d + pos, d + pos + slen);
                // 尝试作为嵌套消息解析
                auto sub = decode_pb_message(d + pos, slen);
                if (!sub.empty()) { f.sub_fields = std::move(sub); }
                pos += slen;
            }
        } else { break; }  // 不支持的类型
        
        fields.push_back(std::move(f));
    }
    return fields;
}

// 从字段列表中按字段号查找子字段
inline const PBField* find_field(const std::vector<PBField>& fields, int fn) {
    for (auto& f : fields) if (f.fn == fn) return &f;
    return nullptr;
}

// 解码后的 ConnMsg 结构
struct DecodedMsg {
    int cmd_type = 0;
    std::string cmd, msg_id, module;
    uint32_t seq_no = 0;
    Bytes data;  // field 2 的原始 bytes (push 消息是 JSON 字符串)
};

// 直接从二进制数据中按 tag 提取指定 field 的 bytes
inline Bytes extract_field_bytes(const uint8_t* d, size_t len, int target_fn, int target_wire = 2) {
    size_t pos = 0;
    while (pos < len) {
        auto [tag_raw, tag_len] = decode_varint(d + pos, len - pos);
        pos += tag_len;
        int fn = (int)(tag_raw >> 3);
        int wire = (int)(tag_raw & 0x07);
        
        if (wire == 0) {
            auto [v, vl] = decode_varint(d + pos, len - pos); pos += vl;
            if (fn == target_fn && target_wire == 0) return {(uint8_t)(v & 0xFF)};
        } else if (wire == 2) {
            auto [vlen, vlen_len] = decode_varint(d + pos, len - pos);
            pos += vlen_len;
            size_t slen = (size_t)vlen;
            if (pos + slen <= len) {
                if (fn == target_fn && target_wire == 2) return Bytes(d + pos, d + pos + slen);
                pos += slen;
            } else break;
        } else { break; }
    }
    return {};
}

inline DecodedMsg decode_conn_msg(const Bytes& payload) {
    DecodedMsg m;
    
    // 方式1: 有 ConnMsg 外包装 (field 1 = Head, field 2 = Data)
    auto head_bytes = extract_field_bytes(payload.data(), payload.size(), 1);
    if (!head_bytes.empty()) {
        auto hf = decode_pb_message(head_bytes.data(), head_bytes.size());
        auto* ct = find_field(hf, 1); if (ct) m.cmd_type = (int)ct->varint_val;
        auto* cm = find_field(hf, 2); if (cm) m.cmd = cm->bytes_val.empty() ? cm->str_val : std::string(cm->bytes_val.begin(), cm->bytes_val.end());
        auto* sn = find_field(hf, 3); if (sn) m.seq_no = (uint32_t)sn->varint_val;
        auto* mi = find_field(hf, 4); if (mi) m.msg_id = mi->bytes_val.empty() ? mi->str_val : std::string(mi->bytes_val.begin(), mi->bytes_val.end());
        auto* mo = find_field(hf, 5); if (mo) m.module = mo->bytes_val.empty() ? mo->str_val : std::string(mo->bytes_val.begin(), mo->bytes_val.end());
        m.data = extract_field_bytes(payload.data(), payload.size(), 2);
        return m;
    }
    
    // 方式2: 无 ConnMsg 外包装，数据直接就是 Head
    // (某些响应如 auth-bind 响应直接发送 Head)
    auto hf = decode_pb_message(payload.data(), payload.size());
    auto* ct = find_field(hf, 1); if (ct) m.cmd_type = (int)ct->varint_val;
    auto* cm = find_field(hf, 2); if (cm) m.cmd = cm->bytes_val.empty() ? cm->str_val : std::string(cm->bytes_val.begin(), cm->bytes_val.end());
    auto* sn = find_field(hf, 3); if (sn) m.seq_no = (uint32_t)sn->varint_val;
    auto* mi = find_field(hf, 4); if (mi) m.msg_id = mi->bytes_val.empty() ? mi->str_val : std::string(mi->bytes_val.begin(), mi->bytes_val.end());
    auto* mo = find_field(hf, 5); if (mo) m.module = mo->bytes_val.empty() ? mo->str_val : std::string(mo->bytes_val.begin(), mo->bytes_val.end());
    
    return m;
}

inline std::string base64_encode(const unsigned char* d, size_t len) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string r;
    for (size_t i = 0; i < len; i += 3) {
        int b = (d[i] << 16);
        if (i + 1 < len) b |= d[i + 1] << 8;
        if (i + 2 < len) b |= d[i + 2];
        r += b64[(b >> 18) & 63];
        r += b64[(b >> 12) & 63];
        r += i + 1 < len ? b64[(b >> 6) & 63] : '=';
        r += i + 2 < len ? b64[b & 63] : '=';
    }
    return r;
}

struct WSFrame {
    bool fin = true;
    uint8_t opcode = 1;
    bool mask = false;
    size_t payload_len = 0;
    Bytes payload;
    Bytes mask_key;
};

inline WSFrame parse_ws_frame(const uint8_t* data, size_t len, size_t& pos) {
    WSFrame f;
    pos = 0;
    if (pos + 2 > len) return f;
    f.fin = (data[pos] & 0x80) != 0;
    f.opcode = data[pos] & 0x0F;
    pos++;
    uint8_t b = data[pos++];
    f.mask = (b & 0x80) != 0;
    f.payload_len = b & 0x7F;
    if (f.payload_len == 126) {
        if (pos + 2 > len) { pos = 0; return f; }
        f.payload_len = ((size_t)data[pos] << 8) | data[pos + 1];
        pos += 2;
    } else if (f.payload_len == 127) {
        if (pos + 8 > len) { pos = 0; return f; }
        f.payload_len = 0;
        for (int i = 0; i < 8; i++)
            f.payload_len = (f.payload_len << 8) | data[pos++];
    }
    if (f.mask && pos + 4 > len) { pos = 0; return f; }  // 数据不足
    if (f.mask) {
        f.mask_key = Bytes(data + pos, data + pos + 4);
        pos += 4;
    }
    if (pos + f.payload_len > len) { pos = 0; return f; }  // 数据不足，等更多数据
    f.payload = Bytes(data + pos, data + pos + f.payload_len);
    pos += f.payload_len;
    if (f.mask)
        for (size_t i = 0; i < f.payload.size(); i++)
            f.payload[i] ^= f.mask_key[i % 4];
    return f;
}

inline Bytes build_ws_frame(uint8_t opcode, const Bytes& payload, bool client_frame = true) {
    Bytes frame;
    frame.push_back((uint8_t)(0x80 | opcode));
    size_t plen = payload.size();
    if (client_frame) {
        // 客户端帧必须 mask
        if (plen < 126) {
            frame.push_back((uint8_t)(plen | 0x80));
        } else if (plen < 65536) {
            frame.push_back((uint8_t)(126 | 0x80));
            frame.push_back((uint8_t)((plen >> 8) & 0xFF));
            frame.push_back((uint8_t)(plen & 0xFF));
        } else {
            frame.push_back((uint8_t)(127 | 0x80));
            for (int i = 7; i >= 0; i--)
                frame.push_back((uint8_t)((plen >> (i * 8)) & 0xFF));
        }
        // 生成 4 字节 mask key
        uint8_t mask[4];
        static std::mt19937 rng(std::random_device{}());
        for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rng() & 0xFF);
        frame.insert(frame.end(), mask, mask + 4);
        // mask 数据
        for (size_t i = 0; i < plen; i++)
            frame.push_back(payload[i] ^ mask[i % 4]);
    } else {
        if (plen < 126) {
            frame.push_back((uint8_t)plen);
        } else if (plen < 65536) {
            frame.push_back(126);
            frame.push_back((uint8_t)((plen >> 8) & 0xFF));
            frame.push_back((uint8_t)(plen & 0xFF));
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; i--)
                frame.push_back((uint8_t)((plen >> (i * 8)) & 0xFF));
        }
        frame.insert(frame.end(), payload.begin(), payload.end());
    }
    return frame;
}

// ── 消息编码辅助 ──
inline Bytes encode_conn_head(int cmd_type, const std::string& cmd,
                              uint32_t seq_no, const std::string& msg_id,
                              const std::string& module) {
    Bytes r;
    bytes_append(r, encode_uint32(1, (uint32_t)cmd_type));
    bytes_append(r, encode_string(2, cmd));
    bytes_append(r, encode_uint32(3, seq_no));
    bytes_append(r, encode_string(4, msg_id));
    bytes_append(r, encode_string(5, module));
    return r;
}

inline Bytes encode_conn_msg(const Bytes& head, const Bytes& body = {}) {
    Bytes r = encode_message(1, head);
    if (!body.empty()) bytes_append(r, encode_message(2, body));
    return r;
}

inline Bytes encode_text_elem(const std::string& text) {
    Bytes content = encode_string(1, text);
    Bytes elem = encode_string(1, "TIMTextElem");
    bytes_append(elem, encode_message(2, content));
    return elem;
}

inline Bytes encode_face_elem(const std::string& sticker_json) {
    Bytes mc;
    bytes_append(mc, encode_uint32(9, 0));
    bytes_append(mc, encode_string(4, sticker_json));
    Bytes elem = encode_string(1, "TIMFaceElem");
    bytes_append(elem, encode_message(2, mc));
    return elem;
}

inline Bytes encode_image_elem(const std::string& url, const std::string& uuid = "",
                               int size = 0, int w = 0, int h = 0) {
    // 与 Python encode_tim_image_elem 完全一致
    Bytes img_info;
    bytes_append(img_info, encode_uint32(1, 1));
    bytes_append(img_info, encode_uint32(2, (uint32_t)size));
    bytes_append(img_info, encode_uint32(3, (uint32_t)w));
    bytes_append(img_info, encode_uint32(4, (uint32_t)h));
    bytes_append(img_info, encode_string(5, url));
    Bytes mc;
    // ← 对齐 Python 实际发图路径 _build_image_elem（sender.py 1860-1875）：
    //   msg_content: field 2=uuid, field 3=image_format, field 8=image_info_array
    if (!uuid.empty()) bytes_append(mc, encode_string(2, uuid));
    bytes_append(mc, encode_uint32(3, 255));
    bytes_append(mc, encode_message(8, img_info));
    Bytes elem = encode_string(1, "TIMImageElem");
    bytes_append(elem, encode_message(2, mc));
    return elem;
}

inline Bytes encode_file_elem(const std::string& url, const std::string& uuid = "",
                              int file_size = 0, const std::string& file_name = "") {
    // 与 Python encode_tim_file_elem 完全一致
    Bytes mc;
    if (!uuid.empty()) bytes_append(mc, encode_string(2, uuid));
    bytes_append(mc, encode_string(10, url));
    if (file_size > 0) bytes_append(mc, encode_uint32(11, (uint32_t)file_size));
    if (!file_name.empty()) bytes_append(mc, encode_string(12, file_name));
    Bytes elem = encode_string(1, "TIMFileElem");
    bytes_append(elem, encode_message(2, mc));
    return elem;
}

/** JSON 字符串转义（ensure_ascii 模式，与 Python json.dumps 默认一致） */
inline std::string json_escape_str(const std::string& s) {
    static const char* hexdig = "0123456789abcdef";
    std::string r;
    r.reserve(s.size() + 8);
    size_t i = 0;
    auto append_hex4 = [&](uint32_t cp) {
        r += "\\u";
        r += hexdig[(cp >> 12) & 0xF];
        r += hexdig[(cp >> 8) & 0xF];
        r += hexdig[(cp >> 4) & 0xF];
        r += hexdig[cp & 0xF];
    };
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { r += "\\\""; i++; }
        else if (c == '\\') { r += "\\\\"; i++; }
        else if (c == '\n') { r += "\\n"; i++; }
        else if (c == '\r') { r += "\\r"; i++; }
        else if (c == '\t') { r += "\\t"; i++; }
        else if (c < 0x20) { append_hex4(c); i++; }
        else if (c < 0x80) { r += (char)c; i++; }
        else {
            // UTF-8 解码 → Unicode 码点，再 \uXXXX 转义（Python ensure_ascii）
            uint32_t cp = 0;
            int n = 0;
            if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
            else { append_hex4(c); i++; continue; }
            bool ok = true;
            for (int k = 1; k <= n; k++) {
                if (i + (size_t)k >= s.size()) { ok = false; break; }
                unsigned char cc = (unsigned char)s[i + (size_t)k];
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (!ok) { append_hex4(c); i++; continue; }
            if (cp < 0x10000) append_hex4(cp);
            else {
                // 增补平面：编码为代理对（与 Python json.dumps 一致）
                cp -= 0x10000;
                uint32_t hi = 0xD800 + (cp >> 10);
                uint32_t lo = 0xDC00 + (cp & 0x3FF);
                append_hex4(hi); append_hex4(lo);
            }
            i += (size_t)n + 1;
        }
    }
    return r;
}

/** @某人的 TIMCustomElem（与 Python _build_at_message 完全一致） */
inline Bytes encode_at_custom_elem(const std::string& user_id, const std::string& display_name = "") {
    std::string disp = display_name.empty() ? user_id : display_name;
    // Python: json.dumps({"elem_type": 1002, "text": f"@{display_name}", "user_id": at_user_id})
    // 默认 ensure_ascii=True + 带空格分隔符
    std::string at_data = "{\"elem_type\": 1002, \"text\": \"@" + json_escape_str(disp)
                          + "\", \"user_id\": \"" + json_escape_str(user_id) + "\"}";
    Bytes content = encode_string(4, at_data);
    Bytes elem = encode_string(1, "TIMCustomElem");
    bytes_append(elem, encode_message(2, content));
    return elem;
}

/** @某人 + 文本 群消息（与 Python _build_at_message 一致，多个 field 6） */
inline Bytes encode_send_group_at_msg(const std::string& msg_id,
                                      const std::string& group_code,
                                      const std::string& from_account,
                                      const std::string& text,
                                      const std::string& user_id,
                                      const std::string& display_name = "") {
    Bytes body;
    bytes_append(body, encode_string(1, msg_id));
    bytes_append(body, encode_string(2, group_code));
    bytes_append(body, encode_string(3, from_account));
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 4294967295);
    bytes_append(body, encode_string(5, std::to_string(rnd)));
    bytes_append(body, encode_message(6, encode_at_custom_elem(user_id, display_name)));
    // ← 修复：text 为空（纯 @ 不跟内容）时不再追加空文本元素，与 Python 一致
    if (!text.empty()) bytes_append(body, encode_message(6, encode_text_elem(text)));
    return body;
}

/** 有序消息片段：type 0=文本, 1=@（保持 @ 在输入时的原始位置） */
struct SendPart {
    int type = 0;          // 0=text, 1=at
    std::string text;      // type=0 时的文本
    std::string user_id;   // type=1 时的被 @ 用户
    std::string display;   // type=1 时的展示昵称
};

/** 按顺序编码 text/at 片段为群消息（@ 位置保持，多个 field 6） */
inline Bytes encode_send_group_parts_msg(const std::string& msg_id,
                                         const std::string& group_code,
                                         const std::string& from_account,
                                         const std::vector<SendPart>& parts) {
    Bytes body;
    bytes_append(body, encode_string(1, msg_id));
    bytes_append(body, encode_string(2, group_code));
    bytes_append(body, encode_string(3, from_account));
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 4294967295);
    bytes_append(body, encode_string(5, std::to_string(rnd)));
    for (auto& p : parts) {
        if (p.type == 1)
            bytes_append(body, encode_message(6, encode_at_custom_elem(p.user_id, p.display)));
        else if (!p.text.empty())
            bytes_append(body, encode_message(6, encode_text_elem(p.text)));
    }
    return body;
}

/** 批量 @ 多人 + 文本 群消息（与 Python _build_multi_at_message 一致） */
inline Bytes encode_send_group_multi_at_msg(const std::string& msg_id,
                                            const std::string& group_code,
                                            const std::string& from_account,
                                            const std::string& text,
                                            const std::vector<std::pair<std::string, std::string>>& at_users) {
    Bytes body;
    bytes_append(body, encode_string(1, msg_id));
    bytes_append(body, encode_string(2, group_code));
    bytes_append(body, encode_string(3, from_account));
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 4294967295);
    bytes_append(body, encode_string(5, std::to_string(rnd)));
    for (auto& u : at_users)
        bytes_append(body, encode_message(6, encode_at_custom_elem(u.first, u.second)));
    // ← 修复：text 为空（纯 @ 不跟内容）时不再追加空文本元素，与 Python 一致
    if (!text.empty()) bytes_append(body, encode_message(6, encode_text_elem(text)));
    return body;
}

inline Bytes encode_send_group_msg(const std::string& msg_id,
                                    const std::string& group_code,
                                    const std::string& from_account,
                                    const std::string& text,
                                    const std::string& ref_msg_id = "",
                                    const std::string& at_user = "",
                                    const std::string& at_nick = "") {
    // 与 Python encode_send_group_req / _build_reply_msg(at) 一致
    Bytes body;
    bytes_append(body, encode_string(1, msg_id));
    bytes_append(body, encode_string(2, group_code));
    bytes_append(body, encode_string(3, from_account));
    bytes_append(body, encode_string(4, ""));  // 空字符串
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 4294967295);
    bytes_append(body, encode_string(5, std::to_string(rnd)));  // string 类型!
    if (!at_user.empty())
        bytes_append(body, encode_message(6, encode_at_custom_elem(at_user, at_nick)));
    bytes_append(body, encode_message(6, encode_text_elem(text)));
    bytes_append(body, encode_string(7, ref_msg_id));  // 引用消息ID（或空）
    return body;
}

inline Bytes encode_send_c2c_msg(const std::string& msg_id,
                                  const std::string& to_account,
                                  const std::string& from_account,
                                  const std::string& text) {
    Bytes body;
    bytes_append(body, encode_string(1, msg_id));
    bytes_append(body, encode_string(2, to_account));
    bytes_append(body, encode_string(3, from_account));
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000);
    bytes_append(body, encode_uint32(4, rnd));
    bytes_append(body, encode_message(5, encode_text_elem(text)));
    return body;
}

inline Bytes encode_sticker_group_msg(const std::string& msg_id,
                                       const std::string& group_code,
                                       const std::string& from_account,
                                       const std::string& sticker_json,
                                       const std::string& at_user = "",
                                       const std::string& at_nick = "",
                                       const std::string& text = "") {
    // 与 Python _build_sticker_msg / _build_sticker_with_at_msg 一致：
    // field 1 msg_id / 2 group / 3 from / 5 random(str) / 6 元素列表（无 field 4、7）
    Bytes body;
    bytes_append(body, encode_string(1, msg_id));
    bytes_append(body, encode_string(2, group_code));
    bytes_append(body, encode_string(3, from_account));
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000);
    bytes_append(body, encode_string(5, std::to_string(rnd)));  // string 类型，与文字消息一致
    if (!at_user.empty())
        bytes_append(body, encode_message(6, encode_at_custom_elem(at_user, at_nick)));
    bytes_append(body, encode_message(6, encode_face_elem(sticker_json)));
    if (!text.empty())
        bytes_append(body, encode_message(6, encode_text_elem(text)));
    return body;
}

inline Bytes encode_auth_bind(const std::string& bot_id, const std::string& token) {
    // 与 Python sender.py 完全一致: biz_id + auth_info + device_info
    Bytes auth_info;
    bytes_append(auth_info, encode_string(1, bot_id));
    bytes_append(auth_info, encode_string(2, "web"));
    bytes_append(auth_info, encode_string(3, token));
    Bytes device_info;
    bytes_append(device_info, encode_string(1, "2.0.1"));
    bytes_append(device_info, encode_string(2, "Linux"));
    bytes_append(device_info, encode_string(3, "2026.3.23-2"));
    bytes_append(device_info, encode_string(4, "16"));
    Bytes data;
    bytes_append(data, encode_string(1, "ybBot"));
    bytes_append(data, encode_message(2, auth_info));
    bytes_append(data, encode_message(3, device_info));
    return data;
}

} // namespace proto

// ═══════════════════════════════════════════════
//  SHA1 手写实现 (无外部依赖)
// ═══════════════════════════════════════════════
inline void sha1_raw(const uint8_t* input, size_t ilen, uint8_t output[20]) {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    // ← 修复：SHA1 用循环左移（ROTL），原实现误用右旋导致所有 SHA1 结果错误
    auto rotl = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };

    size_t padded = ((ilen + 8) / 64 + 1) * 64;
    std::vector<uint8_t> dat(padded, 0);
    memcpy(dat.data(), input, ilen);
    dat[ilen] = 0x80;
    uint64_t bits = ilen * 8;
    for (int i = 0; i < 8; i++)
        dat[padded - 1 - i] = (uint8_t)((bits >> (i * 8)) & 0xFF);

    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = ((uint32_t)dat[chunk + i * 4] << 24) | ((uint32_t)dat[chunk + i * 4 + 1] << 16) |
                   ((uint32_t)dat[chunk + i * 4 + 2] << 8) | dat[chunk + i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    for (int i = 0; i < 5; i++) {
        output[i * 4]     = (uint8_t)((h[i] >> 24) & 0xFF);
        output[i * 4 + 1] = (uint8_t)((h[i] >> 16) & 0xFF);
        output[i * 4 + 2] = (uint8_t)((h[i] >> 8) & 0xFF);
        output[i * 4 + 3] = (uint8_t)(h[i] & 0xFF);
    }
}

inline std::string sha1_hex(const std::string& s) {
    uint8_t out[20];
    sha1_raw((const uint8_t*)s.data(), s.size(), out);
    char buf[41];
    for (int i = 0; i < 20; i++) sprintf(buf + i * 2, "%02x", out[i]);
    return std::string(buf, 40);
}

/** HMAC-SHA1 十六进制（基于手写 SHA1，无外部依赖；用于腾讯云 COS 签名） */
inline std::string hmac_sha1_hex(const std::string& key, const std::string& data) {
    uint8_t k[64] = {0};
    size_t keylen = key.size();
    if (keylen > 64) {
        uint8_t kh[20];
        sha1_raw((const uint8_t*)key.data(), keylen, kh);
        memcpy(k, kh, 20);
    } else {
        memcpy(k, key.data(), keylen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    std::vector<uint8_t> inner;
    inner.insert(inner.end(), ipad, ipad + 64);
    inner.insert(inner.end(), data.begin(), data.end());
    uint8_t ih[20];
    sha1_raw(inner.data(), inner.size(), ih);
    std::vector<uint8_t> outer;
    outer.insert(outer.end(), opad, opad + 64);
    outer.insert(outer.end(), ih, ih + 20);
    uint8_t oh[20];
    sha1_raw(outer.data(), outer.size(), oh);
    char buf[41];
    for (int i = 0; i < 20; i++) sprintf(buf + i * 2, "%02x", oh[i]);
    return std::string(buf, 40);
}

// ═══════════════════════════════════════════════
//  JSON (轻量无依赖实现)
// ═══════════════════════════════════════════════
struct JsonVal {
    std::variant<std::monostate, std::string, double, bool> v;
    std::map<std::string, JsonVal> obj;
    std::vector<JsonVal> arr;

    bool is_string() const { return v.index() == 1; }
    bool is_number() const { return v.index() == 2; }
    bool is_bool() const   { return v.index() == 3; }
    bool is_null() const   { return v.index() == 0; }
    bool empty() const     { return obj.empty() && arr.empty() && v.index() == 0; }

    std::string asString() const { return is_string() ? std::get<1>(v) : ""; }
    int         asInt()    const { return (int)asDouble(); }
    double      asDouble() const { return is_number() ? std::get<2>(v) : 0.0; }
    bool        asBool()   const { return is_bool() ? std::get<3>(v) : false; }

    const JsonVal& operator[](const std::string& k) const {
        static JsonVal nil;
        auto it = obj.find(k);
        return it != obj.end() ? it->second : nil;
    }
    JsonVal& operator[](const std::string& k) { return obj[k]; }

    const JsonVal& operator[](size_t i) const {
        static JsonVal nil;
        return i < arr.size() ? arr[i] : nil;
    }
    JsonVal& operator[](size_t i) {
        if (i >= arr.size()) arr.resize(i + 1);
        return arr[i];
    }

    size_t size() const { return arr.empty() ? obj.size() : arr.size(); }

    JsonVal& operator=(const std::string& s) { v = s; obj.clear(); arr.clear(); return *this; }
    JsonVal& operator=(const char* s)        { v = std::string(s ? s : ""); obj.clear(); arr.clear(); return *this; }
    JsonVal& operator=(double d)             { v = d; obj.clear(); arr.clear(); return *this; }
    JsonVal& operator=(bool b)               { v = b; obj.clear(); arr.clear(); return *this; }
    JsonVal& operator=(int i)                { v = (double)i; obj.clear(); arr.clear(); return *this; }
};

std::string json_quote(const std::string& s);
std::string json_compact(const JsonVal& v);
bool        json_parse(const std::string& s, JsonVal& out);

// ═══════════════════════════════════════════════
//  HTTP 请求 / 响应
// ═══════════════════════════════════════════════
struct HttpRequest {
    std::string method, path, version, body;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> query_params;
};

struct HttpResponse {
    std::string version = "HTTP/1.1";
    int status = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
    Bytes body_bytes;

    HttpResponse() {}
    HttpResponse& status_code(int s, const std::string& t = "") {
        status = s; status_text = t; return *this;
    }
    HttpResponse& json(const std::string& b) {
        body = b;
        headers["Content-Type"] = "application/json; charset=utf-8";
        headers["Access-Control-Allow-Origin"] = "*";
        return *this;
    }
    HttpResponse& js(const std::string& b) {
        body = b;
        headers["Content-Type"] = "application/javascript; charset=utf-8";
        return *this;
    }
    HttpResponse& html(const std::string& b) {
        body = b;
        headers["Content-Type"] = "text/html; charset=utf-8";
        return *this;
    }
    HttpResponse& css(const std::string& c) {
        body = c;
        headers["Content-Type"] = "text/css; charset=utf-8";
        return *this;
    }
    HttpResponse& not_found() {
        return status_code(404, "Not Found").json("{\"error\":\"not found\"}");
    }
    HttpResponse& bad_request() {
        return status_code(400, "Bad Request").json("{\"error\":\"bad request\"}");
    }
};

bool  parse_http_request(const Bytes& data, HttpRequest& req);
Bytes build_http_response_bytes(HttpResponse resp);

// ═══════════════════════════════════════════════
//  SSE Broker
// ═══════════════════════════════════════════════
class SSEBroker {
public:
    using ClientCb = std::function<void(const std::string&)>;
    void add_client(ClientCb cb);
    void remove_all();
    void publish(const std::string& event, const std::string& data);
    size_t client_count() const;

private:
    mutable std::mutex mu_;
    std::vector<ClientCb> clients_;
};

// ═══════════════════════════════════════════════
//  消息日志
// ═══════════════════════════════════════════════
class MessageLogger {
public:
    std::string base_dir;
    std::atomic<uint64_t> total_written{0};
    std::atomic<bool> running_{true};
    std::atomic<bool> enabled{true};   // 消息记录总开关（可在设置中动态切换）
    mutable std::mutex mu_;
    std::deque<std::string> queue_;
    std::thread writer_;
    std::string today_date_;

    MessageLogger(const std::string& dir = "./logs");
    ~MessageLogger();

    void log(const JsonVal& msg);
    void log_raw(const std::string& line);
    void stop();
    void clear_all();   // 清空待写队列并删除所有已写入的日志文件

    size_t pending_count() const;
    int64_t today_jsonl_size() const;
    int64_t today_txt_size() const;
    int64_t total_size() const;

private:
    std::string current_date();
    void write_loop();
    void flush(const std::vector<std::string>& buf);
};

// ═══════════════════════════════════════════════
//  刷屏引擎
// ═══════════════════════════════════════════════
namespace flood {

enum class FloodMode {
    RANDOM = 0, FULLWIDTH, MOCK, ZALGO, REPEAT, ALTERNATE, EMOJI,
    GHOST, MATRIX, EARTHQUAKE, BUBBLE, FIREWORK, RAINBOW, STORM, TSUNAMI, QUANTUM
};

struct FloodTask {
    std::string text;
    int count = 10;
    int delay_ms = 50;
    int batch_size = 3;
    FloodMode mode = FloodMode::RANDOM;
    std::string group_code;
    std::atomic<bool> cancelled{false};
    uint64_t created_at = 0;
    std::atomic<int> sent{0};
    std::atomic<bool> running{false};

    // atomic 成员不可拷贝，需显式定义
    FloodTask() = default;
    FloodTask(const FloodTask& o)
        : text(o.text), count(o.count), delay_ms(o.delay_ms),
          batch_size(o.batch_size), mode(o.mode), group_code(o.group_code),
          cancelled(o.cancelled.load()), created_at(o.created_at),
          sent(o.sent.load()), running(o.running.load()) {}
};

struct FloodStats {
    int total_tasks = 0;
    int total_sent = 0;
    int active_tasks = 0;
    double avg_speed = 0;
};

std::string transform_text(const std::string& text, FloodMode mode);

class FloodEngine {
public:
    std::function<void(const std::string& group, const std::string& msg)> sender_;
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<FloodTask>> tasks_;
    FloodStats stats_;

    std::string create_task(const FloodTask& task);
    void cancel_task(const std::string& id);
    std::string get_task_status(const std::string& id);
    std::string list_tasks();
    FloodStats get_stats() const;
};

} // namespace flood

// ═══════════════════════════════════════════════
//  贴纸库
// ═══════════════════════════════════════════════
class StickerLibrary {
public:
    std::map<std::string, std::string> stickers;
    std::map<std::string, std::string> icons;  // 贴纸名称 → ico 文件名（UTF-8，如 "01_六六六.ico"）
    StickerLibrary();
    std::string find_by_name(const std::string& name) const;
    void scan_icons(const std::string& dir);   // 扫描 ico 目录，按名称（去"数字_"前缀）建立映射
    std::string icon_file(const std::string& name) const;  // 返回对应 ico 文件名，无则空
};

// ═══════════════════════════════════════════════
//  大模型 API 配置
// ═══════════════════════════════════════════════
struct LLMConfig {
    std::string api_url;        // API 地址，兼容 OpenAI/DeepSeek/通义千问等
    std::string api_key;        // API Key
    std::string model;          // 模型名称
    std::string system_prompt;  // 系统提示词
    int max_tokens = 200;       // 最大输出 token 数
    double temperature = 0.8;   // 温度参数
    int timeout_sec = 15;       // 请求超时（秒）
    bool enabled = false;       // 是否启用大模型回复（api_key 非空时自动启用）
};

// ═══════════════════════════════════════════════
//  Bot 配置
// ═══════════════════════════════════════════════
struct BotConfig {
    std::string app_key, app_secret, api_domain, ws_url;
    std::string yuanbao_id;
    // 多群监听：所有被监听群，第一个为默认目标群（发送/转发/查询的默认目标）
    std::vector<std::string> listen_groups;
    int heartbeat_interval = 10;
    int port = 5000;
    LLMConfig llm;
    bool msg_log_enabled = true;         // 消息记录开关
    bool recall_monitor_enabled = false; // 撤回监控开关

    bool load(const std::string& path);

    // 默认目标群：监听列表第一项（主群概念已移除，无监听群时返回空串）
    const std::string& default_target() const {
        static const std::string empty;
        return listen_groups.empty() ? empty : listen_groups.front();
    }

    // 是否监听指定群
    bool is_listening(const std::string& group) const {
        if (group.empty()) return false;
        for (auto& g : listen_groups) if (g == group) return true;
        return false;
    }
};

// ═══════════════════════════════════════════════
//  JSON 配置插件（C++ 版插件系统：plugins/*.json）
//  Python 版可用 importlib 动态加载代码，C++ 无等价机制，
//  故采用"命令→回复"的声明式插件：收到群消息匹配命令前缀即回复。
//  示例 plugins/example.json:
//    { "name":"example","version":"1.0.0","author":"xx","description":"示例",
//      "active":true,"commands":[{"command":"/ping","reply":"pong"}] }
// ═══════════════════════════════════════════════
struct PluginCommand {
    std::string command;   // 命令前缀，如 "/ping"
    std::string reply;     // 回复文本（支持 {user} 占位 = 发送者昵称）
};
struct PluginInfo {
    std::string name, version, author, description;
    bool active = true;
    std::vector<PluginCommand> commands;
    std::string error;     // 加载错误信息（前端展示）
};

// ═══════════════════════════════════════════════
//  主服务器类
// ═══════════════════════════════════════════════
class YuanbaoServer {
public:
    YuanbaoServer();
    ~YuanbaoServer();

    bool load_config(const std::string& path);
    int get_port() const { return config_.port; }
    bool start(int port);
    void stop();
    bool is_running() const { return running_.load(); }

    // 刷屏
    std::string flood_start(const std::string& text, int count, int delay, int batch,
                            const std::string& mode, const std::string& group);
    void flood_cancel(const std::string& id);
    std::string flood_stats();
    void flood_set_sender(std::function<void(const std::string&, const std::string&)> cb) {
        flood_engine_.sender_ = std::move(cb);
    }

    // Bot 消息发送
    bool send_group_text(const std::string& group, const std::string& text);
    bool send_c2c_text(const std::string& to, const std::string& text);
    void record_sent_message(const std::string& group, const std::string& text);
    // ← 扩展：记录自己发送的消息（可携带 media_info，如贴纸），供前端实时显示
    void record_sent_message(const std::string& group, const std::string& text, const JsonVal& media_info);
    // ← 补：记录自己发送的私聊消息（带 peer，供前端按人过滤历史）
    void record_sent_c2c_message(const std::string& to, const std::string& text);
    // Bot 在群里的显示昵称（从成员缓存获取，找不到回退"元宝"）
    std::string bot_display_name();
    bool send_group_sticker(const std::string& group, const std::string& sticker_json,
                            const std::string& at_user = "", const std::string& at_nick = "",
                            const std::string& text = "");
    bool send_group_image(const std::string& group, const std::string& url,
                          const std::string& uuid = "", int size = 0, int w = 0, int h = 0,
                          const std::string& at_user = "", const std::string& at_nick = "");
    bool send_group_file(const std::string& group, const std::string& url,
                         const std::string& file_name = "",
                         const std::string& uuid = "", int file_size = 0,
                         const std::string& at_user = "", const std::string& at_nick = "");
    bool send_group_at(const std::string& group, const std::string& text,
                       const std::string& user_id, const std::string& display_name = "");
    bool send_group_multi_at(const std::string& group, const std::string& text,
                             const std::vector<std::pair<std::string, std::string>>& at_users);
    bool send_group_parts(const std::string& group, const std::vector<proto::SendPart>& parts);
    bool send_group_at_all(const std::string& group);
    bool send_group_reply(const std::string& group, const std::string& text, const std::string& ref_msg_id,
                          const std::string& at_user = "", const std::string& at_nick = "");
    // 文件上传：genUploadInfo → COS 上传，返回文件 URL（与 Python sender.py 一致）
    bool upload_media(const std::string& file_name, const std::string& file_data,
                      std::string& out_url, std::string& out_uuid);

    // 消息缓存
    static constexpr size_t MAX_MSG_CACHE = 500;
    std::vector<JsonVal> msg_cache_;
    std::mutex msg_mu_;
    void cache_message(const JsonVal& msg);

    // 群成员缓存（get_group_member_list 响应），按群号分别缓存
    struct MemberCacheEntry {
        std::vector<std::pair<std::string, std::string>> members; // {user_id, nick_name}
        std::string owner_id;
        std::string group_name;
        int64_t fetched_at = 0;
    };
    std::map<std::string, MemberCacheEntry> members_cache_map_;   // group_code -> 缓存
    std::string members_owner_id_;    // 当前查询群主（兼容旧引用）
    std::string members_group_name_;  // 当前查询群名（兼容旧引用）
    std::mutex members_mu_;
    std::mutex members_query_mu_;     // 串行化成员/群信息查询，避免并发覆盖响应标志
    std::condition_variable members_cv_;
    bool members_response_ = false;
    bool group_info_response_ = false;
    int64_t members_fetched_at_ = 0;  // 最近成功获取时间（ms），用于复用缓存避免轮询风暴
    std::string members_request_group_; // 最近一次成员查询的群号（get_group_member_list 响应写入缓存归属）
    // ← 图片代理：保存最近一次上传的 COS 临时凭证，用于重新签名 GET 下载 URL
    std::string cos_upload_region_, cos_upload_bucket_, cos_upload_location_;
    std::string cos_upload_secret_id_, cos_upload_secret_key_, cos_upload_token_;
    std::string cos_upload_key_time_;
    int64_t cos_upload_expired_ = 0;
    // 多群聊监听：PUSH/日志中发现的 bot 所在群（group_code -> group_name，空名待查询）
    std::map<std::string, std::string> known_groups_;
    bool groups_scanned_ = false;       // 是否已从持久文件/历史日志恢复过群列表
    void persist_known_groups();        // 群列表落盘（logs/known_groups.json）
    void load_known_groups();           // 从持久文件恢复群列表
    void auto_listen_from_known();      // 自动获取监听群：listen_groups 为空时全量同步所有已发现群

    // 在途 query_group_info 请求关联表（msg_id -> group_code）
    // 取代原先单槽位 pending_query_group_，并发查询响应按 msg_id 精确回写，杜绝群名张冠李戴
    std::map<std::string, std::string> pending_query_groups_;

    // 前端推送
    void push_to_frontend(const std::string& event, const JsonVal& data);
    void broadcast_fe(const std::string& msg);
    void add_fe_ws(int fd);
    void remove_fe_ws(int fd);

    // Bot 连接
    bool bot_connect();
    void bot_disconnect();
    bool bot_is_connected() const { return bot_connected_.load(); }
    bool bot_sign_token();

    // 贴纸
    StickerLibrary stickers_;
    void process_llm_reply(const JsonVal& msg);
    void handle_recall_notification(const JsonVal& push_json);  // 撤回事件通知（对齐 Python group_monitor.py）
    std::string call_llm_api(const std::string& user_message, const std::string& sender_name);
    void save_config();

    // 转发模式
    bool forward_enabled_ = false;
    bool forward_at_only_ = false;
    bool forward_at_yuanbao_ = true;
    std::string wait_yuanbao_reply_ = "true";
    std::mutex forward_mu_;
    // 代理转发：等待元宝回复回传的 FIFO 队列（原群, 原消息 msg_id）
    std::deque<std::pair<std::string, std::string>> forward_queue_;

    // 插件系统（JSON 配置插件）
    std::vector<PluginInfo> plugins_;
    std::mutex plugins_mu_;
    void load_plugins();                       // 扫描 plugins/*.json
    bool save_plugin(const PluginInfo& p);     // 写回 plugins/<name>.json
    bool handle_plugin_command(const std::string& group, const std::string& content);  // 命令匹配→回复

    // 心跳
    int heartbeat_interval_ = 10;
    std::atomic<int64_t> last_heartbeat_{0};

private:
    BotConfig config_;
    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::vector<std::thread> worker_threads_;
    flood::FloodEngine flood_engine_;
    MessageLogger msg_logger_{"./logs"};

    std::atomic<uint64_t> flood_total_sent_{0};

    // Bot WebSocket
    std::atomic<bool> bot_connected_{false};
    std::atomic<bool> bot_running_{false};
    std::atomic<bool> auth_response_received_{false};
    bool sync_sent_ = false;
    int bot_ws_fd_ = -1;
#ifndef YUANBAO_NO_OPENSSL
    SSL* bot_ws_ssl_ = nullptr;
    SSL_CTX* bot_ws_ssl_ctx_ = nullptr;
#endif
    std::thread bot_ws_thread_;
    std::string client_token_;
    uint32_t bot_seq_no_ = 0;
    std::mutex bot_send_mu_;
    std::set<std::string> seen_msg_ids_;
    std::mutex seen_mu_;

    // 前端 WebSocket
    std::set<int> fe_ws_fds_;
    std::mutex fe_ws_mu_;

    HttpResponse route_api(const HttpRequest& req);
    std::string resolve_path(const std::string& url_path);
    void route_http(int fd);
    void route_websocket(int fd, const HttpRequest& req);

    // Bot 内部方法
    bool bot_ws_connect();
    bool bot_ws_send_bytes(const Bytes& data);
    Bytes bot_ws_build_auth();
    Bytes bot_ws_build_ping();
    Bytes bot_ws_build_sync_info();
    Bytes bot_ws_build_send_group_msg(const std::string& text, const std::string& group);
    Bytes bot_ws_build_send_c2c_msg(const std::string& text, const std::string& to);
    Bytes bot_ws_build_sticker_msg(const std::string& sticker_json, const std::string& group,
                                   const std::string& at_user = "", const std::string& at_nick = "",
                                   const std::string& text = "");
    Bytes bot_ws_build_get_members_msg(const std::string& group);
    Bytes bot_ws_build_query_group_info_msg(const std::string& group, const std::string& msg_id);
    void bot_ws_recv_loop();
    void bot_handle_frame(const Bytes& data);
};
