/**
 * yuanbao_unified.cpp - 元宝 Bot 统一服务器（全功能 C++ 实现）
 * 支持 Windows (MinGW64) / Linux / macOS
 *
 * 编译 (Windows MinGW64):
 *   g++ -std=c++17 -O3 -Wall -I. yuanbao_unified.cpp -o yuanbao_server.exe -lws2_32 -lssl -lcrypto
 * 编译 (Linux):
 *   g++ -std=c++17 -O3 -Wall -I. yuanbao_unified.cpp -o yuanbao_server -lpthread -lrt -lm -lssl -lcrypto
 * 编译 (macOS):
 *   g++ -std=c++17 -O3 -Wall -I. yuanbao_unified.cpp -o yuanbao_server -lpthread -lm -lssl -lcrypto
 */
#include "server.h"
#include <regex>
#include <cstdlib>
#include <fstream>
#include <set>
#include <cstddef>
#include <cstdarg>
#include <cstdio>

// ── MinGW-w64 (UCRT) 静态链接 OpenSSL 兼容层 ──
// 问题：静态库 libcrypto.a 引用 MSVCRT 风格的导入符号 __imp__vsnprintf，
//       而 UCRT 版 MinGW-w64 不提供该导入 thunk，导致静态链接失败。
// 解决：手工定义 __imp__vsnprintf 为函数指针数据符号，转发到 vsnprintf。
#if defined(_WIN32) && defined(__MINGW32__)
extern "C" {
static int vsnprintf_shim(char* s, size_t n, const char* format, va_list ap) {
    return vsnprintf(s, n, format, ap);
}
// libcrypto.a 通过 __imp__vsnprintf 间接调用，此处提供函数指针数据符号
int (*__imp__vsnprintf)(char*, size_t, const char*, va_list) = vsnprintf_shim;
}
#endif

#ifndef YUANBAO_NO_OPENSSL
  #include <openssl/ssl.h>
  #include <openssl/err.h>
  #include <openssl/sha.h>
  #include <openssl/hmac.h>
  static std::string sha256_hex_openssl(const std::string& data) {
      unsigned char hash[SHA256_DIGEST_LENGTH];
      SHA256((const unsigned char*)data.data(), data.size(), hash);
      char buf[65];
      for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) sprintf(buf + i * 2, "%02x", hash[i]);
      return std::string(buf, 64);
  }
#endif

// =====================================================================
//  JSON 实现
// =====================================================================
std::string json_quote(const std::string& s) {
    std::ostringstream oss;
    oss << '"';
    for (char c : s) {
        if (c == '"') oss << "\\\"";
        else if (c == '\\') oss << "\\\\";
        else if (c == '\n') oss << "\\n";
        else if (c == '\r') oss << "\\r";
        else if (c == '\t') oss << "\\t";
        else if ((unsigned char)c >= 32) oss << c;
    }
    oss << '"';
    return oss.str();
}

static std::string json_val_str(const JsonVal& val) {
    if (!val.obj.empty()) {
        std::ostringstream o; o << '{'; bool f = true;
        for (auto& kv : val.obj) {
            if (!f) o << ','; f = false;
            o << json_quote(kv.first) << ':' << json_compact(kv.second);
        }
        o << '}'; return o.str();
    } else if (!val.arr.empty()) {
        std::ostringstream o; o << '['; bool f = true;
        for (auto& i : val.arr) {
            if (!f) o << ','; f = false;
            o << json_compact(i);
        }
        o << ']'; return o.str();
    }
    switch (val.v.index()) {
        case 1: return json_quote(std::get<1>(val.v));
        case 2: { std::ostringstream o; o << std::get<2>(val.v); return o.str(); }
        case 3: return std::get<3>(val.v) ? "true" : "false";
        default: return "null";
    }
}

std::string json_compact(const JsonVal& v) { return json_val_str(v); }

bool json_parse(const std::string& s, JsonVal& out) {
    size_t pos = 0;
    std::function<JsonVal()> parse_val;

    auto skip_ws = [&]() {
        while (pos < s.size() && std::isspace((unsigned char)s[pos])) pos++;
    };

    auto parse_str = [&]() -> std::string {
        if (pos >= s.size() || s[pos] != '"') return "";
        pos++;
        std::string r;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                pos++;
                switch (s[pos]) {
                    case 'n': r += '\n'; break;
                    case 'r': r += '\r'; break;
                    case 't': r += '\t'; break;
                    case '"': r += '"'; break;
                    case '\\': r += '\\'; break;
                    case '/': r += '/'; break;
                    case 'u': {
                        // ← 修复：支持 \uXXXX Unicode 转义。浏览器 JSON.stringify 会把
                        //   中文等非 ASCII 字符转义为 \uXXXX 形式（如 "害羞" → "\u5bb3\u7f9e"），
                        //   原实现未处理导致中文内容/贴纸名称解析成乱码而匹配失败。
                        //   这里按 UTF-16 代理对处理，完整支持基本平面 + 增补平面（emoji）。
                        if (pos + 4 < s.size()) {
                            auto hexval = [](char c) -> int {
                                if (c >= '0' && c <= '9') return c - '0';
                                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                                return -1;
                            };
                            int h1 = hexval(s[pos + 1]), h2 = hexval(s[pos + 2]);
                            int h3 = hexval(s[pos + 3]), h4 = hexval(s[pos + 4]);
                            if (h1 >= 0 && h2 >= 0 && h3 >= 0 && h4 >= 0) {
                                uint32_t cp = (uint32_t)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                                pos += 4;
                                // 处理代理对（emoji 等增补平面字符）
                                if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 < s.size()
                                    && s[pos + 1] == '\\' && s[pos + 2] == 'u') {
                                    int l1 = hexval(s[pos + 3]), l2 = hexval(s[pos + 4]);
                                    int l3 = hexval(s[pos + 5]), l4 = hexval(s[pos + 6]);
                                    if (l1 >= 0 && l2 >= 0 && l3 >= 0 && l4 >= 0) {
                                        uint32_t lo = (uint32_t)((l1 << 12) | (l2 << 8) | (l3 << 4) | l4);
                                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                            pos += 6;
                                        }
                                    }
                                }
                                // UTF-8 编码
                                if (cp < 0x80) {
                                    r += (char)cp;
                                } else if (cp < 0x800) {
                                    r += (char)(0xC0 | (cp >> 6));
                                    r += (char)(0x80 | (cp & 0x3F));
                                } else if (cp < 0x10000) {
                                    r += (char)(0xE0 | (cp >> 12));
                                    r += (char)(0x80 | ((cp >> 6) & 0x3F));
                                    r += (char)(0x80 | (cp & 0x3F));
                                } else {
                                    r += (char)(0xF0 | (cp >> 18));
                                    r += (char)(0x80 | ((cp >> 12) & 0x3F));
                                    r += (char)(0x80 | ((cp >> 6) & 0x3F));
                                    r += (char)(0x80 | (cp & 0x3F));
                                }
                            } else {
                                r += 'u';
                            }
                        } else {
                            r += 'u';
                        }
                        break;
                    }
                    default: r += s[pos]; break;
                }
            } else {
                r += s[pos];
            }
            pos++;
        }
        if (pos < s.size()) pos++;
        return r;
    };

    parse_val = [&]() -> JsonVal {
        skip_ws();
        if (pos >= s.size()) return JsonVal{};
        char c = s[pos];

        if (c == '{') {
            JsonVal v; pos++;
            skip_ws();
            if (pos < s.size() && s[pos] == '}') { pos++; return v; }
            while (pos < s.size()) {
                skip_ws();
                if (s[pos] == '}') { pos++; break; }
                std::string key = parse_str();
                skip_ws();
                if (pos < s.size() && s[pos] == ':') pos++;
                v.obj[key] = parse_val();
                skip_ws();
                if (pos < s.size() && s[pos] == ',') pos++;
            }
            return v;
        } else if (c == '[') {
            JsonVal v; pos++;
            skip_ws();
            if (pos < s.size() && s[pos] == ']') { pos++; return v; }
            while (pos < s.size()) {
                skip_ws();
                if (s[pos] == ']') { pos++; break; }
                v.arr.push_back(parse_val());
                skip_ws();
                if (pos < s.size() && s[pos] == ',') pos++;
            }
            return v;
        } else if (c == '"') {
            JsonVal v; v.v = parse_str(); return v;
        } else if (c == 't' && s.substr(pos, 4) == "true") {
            pos += 4; JsonVal v; v.v = true; return v;
        } else if (c == 'f' && s.substr(pos, 5) == "false") {
            pos += 5; JsonVal v; v.v = false; return v;
        } else if (c == 'n' && s.substr(pos, 4) == "null") {
            pos += 4; return JsonVal{};
        } else {
            size_t start = pos;
            while (pos < s.size() &&
                   (std::isdigit((unsigned char)s[pos]) || s[pos] == '.' ||
                    s[pos] == '-' || s[pos] == '+' || s[pos] == 'e' || s[pos] == 'E'))
                pos++;
            if (start < pos) {
                try {
                    JsonVal v; v.v = std::stod(s.substr(start, pos - start)); return v;
                } catch (...) {
                    // stod 异常（如 "1e"、损坏数字）：推进一位后继续，避免崩溃
                }
            }
            // 非法字符/损坏数字：强制推进，避免解析器无限循环（恶意/损坏 JSON 的健壮性）
            pos++;
        }
        return JsonVal{};
    };

    out = parse_val();
    return true;
}

// =====================================================================
//  HTTP 解析 / 响应
// =====================================================================
bool parse_http_request(const Bytes& data, HttpRequest& req) {
    std::string s((char*)data.data(), data.size());
    size_t pos = 0, line_end;

    line_end = s.find("\r\n", pos);
    if (line_end == std::string::npos) return false;
    {
        std::istringstream line(s.substr(pos, line_end - pos));
        line >> req.method >> req.path >> req.version;
    }
    pos = line_end + 2;

    while (pos < s.size()) {
        line_end = s.find("\r\n", pos);
        if (line_end == std::string::npos) return false;
        if (line_end == pos) { pos += 2; break; }
        std::string header_line = s.substr(pos, line_end - pos);
        size_t colon = header_line.find(':');
        if (colon != std::string::npos) {
            std::string k = header_line.substr(0, colon);
            std::string v = header_line.substr(colon + 1);
            while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(v.begin());
            if (!v.empty() && v.back() == '\r') v.pop_back();
            req.headers[k] = v;
        }
        pos = line_end + 2;
    }

    {
        size_t q = req.path.find('?');
        if (q != std::string::npos) {
            std::string qs = req.path.substr(q + 1);
            req.path = req.path.substr(0, q);
            std::istringstream ss(qs);
            std::string pair;
            while (std::getline(ss, pair, '&')) {
                size_t eq = pair.find('=');
                std::string key = pair.substr(0, eq);
                std::string val = (eq != std::string::npos) ? pair.substr(eq + 1) : "";
                // ← 修复：URL 解码 query 值（用户ID 含 + / = 等特殊字符，前端 encodeURIComponent
                //   后 %2B/%2F 必须还原，否则私聊历史按用户过滤永远匹配不上）
                val = util::url_decode(val);
                req.query_params[key] = val;
            }
        }
    }

    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
        int len = std::stoi(it->second);
        if ((int)s.size() >= (int)pos + len)
            req.body = s.substr(pos, (size_t)len);
        else
            req.body = s.substr(pos);
    }
    return true;
}

Bytes build_http_response_bytes(HttpResponse resp) {
    std::ostringstream oss;
    oss << resp.version << " " << resp.status << " " << resp.status_text << "\r\n";

    size_t content_len = resp.body_bytes.empty() ? resp.body.size() : resp.body_bytes.size();
    resp.headers["Content-Length"] = std::to_string(content_len);
    resp.headers["Access-Control-Allow-Origin"] = "*";
    resp.headers["Access-Control-Allow-Methods"] = "GET, POST, DELETE, OPTIONS";
    resp.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
    // ← 修复：响应后连接即关闭，显式声明 Connection: close，兼容部分旧浏览器/代理
    if (resp.headers.find("Connection") == resp.headers.end())
        resp.headers["Connection"] = "close";

    for (auto& kv : resp.headers)
        oss << kv.first << ": " << kv.second << "\r\n";
    oss << "\r\n";

    std::string header_str = oss.str();
    Bytes result(header_str.begin(), header_str.end());

    if (!resp.body_bytes.empty()) {
        result.insert(result.end(), resp.body_bytes.begin(), resp.body_bytes.end());
    } else if (!resp.body.empty()) {
        result.insert(result.end(), resp.body.begin(), resp.body.end());
    }
    return result;
}

// =====================================================================
//  SSE Broker 实现
// =====================================================================
void SSEBroker::add_client(ClientCb cb) {
    std::lock_guard<std::mutex> l(mu_);
    clients_.push_back(std::move(cb));
}
void SSEBroker::remove_all() {
    std::lock_guard<std::mutex> l(mu_);
    clients_.clear();
}
void SSEBroker::publish(const std::string& event, const std::string& data) {
    std::lock_guard<std::mutex> l(mu_);
    std::string payload = "event: " + event + "\ndata: " + data + "\n\n";
    for (auto& cb : clients_) {
        try { cb(payload); } catch (...) {}
    }
}
size_t SSEBroker::client_count() const {
    std::lock_guard<std::mutex> l(mu_);
    return clients_.size();
}

// =====================================================================
//  消息日志实现
// =====================================================================
MessageLogger::MessageLogger(const std::string& dir) : base_dir(dir) {
#ifdef _WIN32
    _mkdir(base_dir.c_str());
#else
    mkdir(base_dir.c_str(), 0755);
#endif
    today_date_ = current_date();
    writer_ = std::thread([this]() { write_loop(); });
}
MessageLogger::~MessageLogger() { stop(); }

std::string MessageLogger::current_date() {
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

void MessageLogger::log(const JsonVal& msg) { if (!enabled.load()) return; log_raw(json_compact(msg)); }
void MessageLogger::log_raw(const std::string& line) {
    if (!enabled.load()) return;
    std::lock_guard<std::mutex> l(mu_);
    queue_.push_back(line);
}
void MessageLogger::stop() {
    running_ = false;
    if (writer_.joinable()) writer_.join();
}

void MessageLogger::clear_all() {
    // 1) 清空待写队列，避免残留历史消息被 flush 重新写回
    {
        std::lock_guard<std::mutex> l(mu_);
        queue_.clear();
    }
    // 2) 删除所有已写入的日志文件（jsonl + txt）
    DIR* d = opendir(base_dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string n = ent->d_name;
        bool is_msg_log = (n.rfind("messages_", 0) == 0 && n.size() > 4 &&
                           (n.compare(n.size() - 4, 4, ".log") == 0 ||
                            n.compare(n.size() - 4, 4, ".txt") == 0));
        if (is_msg_log) std::remove((base_dir + "/" + n).c_str());
    }
    closedir(d);
}

void MessageLogger::write_loop() {
    try {
    std::vector<std::string> buf;
    int64_t last_flush = 0;
    while (running_) {
        {
            std::lock_guard<std::mutex> l(mu_);
            while (!queue_.empty()) {
                buf.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
        if (buf.size() >= 10 || (!buf.empty() && util::now_ms() - last_flush > 2000)) {
            flush(buf);
            buf.clear();
            last_flush = util::now_ms();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    flush(buf);
    } catch (const std::exception& e) {
        std::cerr << "[Logger] write_loop 异常: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[Logger] write_loop 未知异常\n";
    }
}

void MessageLogger::flush(const std::vector<std::string>& buf) {
    if (buf.empty()) return;
    std::string date = current_date();
    if (date != today_date_) today_date_ = date;

    std::string jsonl_path = base_dir + "/messages_" + today_date_ + ".log";
    std::string txt_path = base_dir + "/messages_" + today_date_ + ".txt";

    FILE* fj = fopen(jsonl_path.c_str(), "a");
    FILE* ft = fopen(txt_path.c_str(), "a");
    if (fj && ft) {
        for (auto& line : buf) {
            fprintf(fj, "%s\n", line.c_str());
            JsonVal v;
            if (json_parse(line, v)) {
                std::string ts = v["timestamp"].asString();
                std::string gc = v["group_code"].asString();
                std::string name = v["sender_name"].asString();
                std::string uid = v["sender_id"].asString();
                std::string content = v["content"].asString();
                std::string media_tag;
                if (v["media_type"].asString() == "image") media_tag = " [图片]";
                else if (v["media_type"].asString() == "sticker") media_tag = " [贴纸]";
                fprintf(ft, "[%s] [群:%s] %s(%s): %s%s\n",
                        ts.c_str(), gc.c_str(), name.c_str(), uid.c_str(),
                        content.c_str(), media_tag.c_str());
            }
        }
        fflush(fj); fclose(fj);
        fflush(ft); fclose(ft);
        total_written += buf.size();
    }
}

size_t MessageLogger::pending_count() const {
    std::lock_guard<std::mutex> l(mu_);
    return queue_.size();
}

static int64_t msglog_file_size(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
        return ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return 0;
#else
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return (int64_t)st.st_size;
    return 0;
#endif
}

int64_t MessageLogger::today_jsonl_size() const {
    return msglog_file_size(base_dir + "/messages_" + today_date_ + ".log");
}

int64_t MessageLogger::today_txt_size() const {
    return msglog_file_size(base_dir + "/messages_" + today_date_ + ".txt");
}

int64_t MessageLogger::total_size() const {
    int64_t total = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    std::string pattern = base_dir + "/*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &ffd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                total += ((int64_t)ffd.nFileSizeHigh << 32) | ffd.nFileSizeLow;
        } while (FindNextFileA(h, &ffd));
        FindClose(h);
    }
#else
    DIR* d = opendir(base_dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d))) {
            std::string n = ent->d_name;
            if (n == "." || n == "..") continue;
            total += msglog_file_size(base_dir + "/" + n);
        }
        closedir(d);
    }
#endif
    return total;
}

// =====================================================================
//  刷屏引擎实现
// =====================================================================
namespace flood {

static std::string to_fullwidth(const std::string& s) {
    std::string r;
    for (char c : s)
        r += (c >= '!' && c <= '~') ? (char)((unsigned char)c + 0xFEE0) : c;
    return r;
}

static std::string to_mock(const std::string& s) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::bernoulli_distribution d(0.5);
    std::string r;
    for (char c : s) {
        if (std::isalpha((unsigned char)c))
            r += d(gen) ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
        else r += c;
    }
    return r;
}

static std::string to_zalgo(const std::string& s) {
    static const char16_t ups[] = {0x030D,0x030E,0x0304,0x0305,0x0311,0x0306,0x0310,0x0352,0x0357,0x0307,0x0308,0x030A};
    static const char16_t dns[] = {0x0316,0x0317,0x0318,0x0319,0x031C,0x031D,0x031E,0x031F,0x0320,0x0324,0x0325,0x0329};
    static std::random_device rd; static std::mt19937 gen(rd());
    std::uniform_int_distribution<> ud(0, 11), dd(0, 5);
    std::u16string r;
    for (char ch : s) {
        char16_t c = (unsigned char)ch;
        r += c;
        for (int i = 0; i < ud(gen); i++) r += ups[gen() % 12];
        for (int i = 0; i < dd(gen); i++) r += dns[gen() % 12];
    }
    return std::string(r.begin(), r.end());
}

static std::string to_repeat(const std::string& s) {
    static std::random_device rd; static std::mt19937 gen(rd());
    std::uniform_int_distribution<> d(2, 5);
    std::string r;
    for (char c : s) for (int i = 0; i < d(gen); i++) r += c;
    return r;
}

static std::string to_alternate(const std::string& s) {
    std::string r; bool up = true;
    for (char c : s) {
        if (std::isalpha((unsigned char)c)) {
            r += up ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
            up = !up;
        } else r += c;
    }
    return r;
}

static std::string to_emojify(const std::string& s) {
    static const char* faces[] = {"\xF0\x9F\x98\x80","\xF0\x9F\x98\x82","\xF0\x9F\x94\xA5","\xF0\x9F\x92\xAF","\xE2\x9C\xA8","\xE2\x9A\xA1","\xF0\x9F\x9A\x80","\xF0\x9F\x8E\x89","\xF0\x9F\x92\xA5","\xF0\x9F\x8C\x9F"};
    static const char* mid[]   = {"\xE2\xAD\x90","\xF0\x9F\x8C\x99","\xF0\x9F\x8C\x9E","\xF0\x9F\x8C\x88","\xF0\x9F\x8D\x80","\xF0\x9F\x8E\xB5","\xE2\x9A\xBD","\xF0\x9F\xA6\x8B"};
    static std::random_device rd; static std::mt19937 gen(rd());
    std::uniform_int_distribution<> fd(0, 9), md(0, 7);
    std::string r = std::string(faces[fd(gen)]) + " ";
    for (char c : s) { r += c; if (std::isalnum((unsigned char)c)) r += mid[md(gen)]; }
    r += " " + std::string(faces[fd(gen)]);
    return r;
}

static std::string to_ghost(const std::string& s) {
    static const char16_t ghost[] = {0x200B,0x200C,0x200D,0x2060,0x202F,0x205F};
    static std::random_device rd; static std::mt19937 gen(rd());
    std::u16string r;
    for (char ch : s) {
        r += (unsigned char)ch;
        if (std::isalnum((unsigned char)ch))
            for (size_t i = 0; i < (size_t)(gen() % 4); i++)
                r += ghost[gen() % 6];
    }
    return std::string(r.begin(), r.end());
}

static std::string to_matrix(const std::string& s) {
    static const char* nums = "0123456789";
    static std::random_device rd; static std::mt19937 gen(rd());
    std::uniform_int_distribution<> d(0, 9);
    std::string r;
    r += nums[d(gen)]; r += nums[d(gen)]; r += " ";
    for (char c : s) { r += c; r += nums[d(gen)]; }
    r += " " + std::string(1, nums[d(gen)]);
    return r;
}

std::string transform_text(const std::string& text, FloodMode mode) {
    switch (mode) {
        case FloodMode::FULLWIDTH: return to_fullwidth(text);
        case FloodMode::MOCK:      return to_mock(text);
        case FloodMode::ZALGO:     return to_zalgo(text);
        case FloodMode::REPEAT:    return to_repeat(text);
        case FloodMode::ALTERNATE: return to_alternate(text);
        case FloodMode::EMOJI:     return to_emojify(text);
        case FloodMode::GHOST:     return to_ghost(text);
        case FloodMode::MATRIX:    return to_matrix(text);
        default:                   return text;
    }
}

std::string FloodEngine::create_task(const FloodTask& task) {
    std::string id = util::random_id(12);
    auto t = std::make_shared<FloodTask>(task);
    t->created_at = util::now_ms();
    t->running = true;
    { std::lock_guard<std::mutex> l(mu_); tasks_[id] = t; stats_.total_tasks++; }

    std::thread([this, id, t]() {
        std::random_device rd; std::mt19937 gen(rd());
        std::uniform_int_distribution<> delay_jitter(0, 20);
        for (int i = 0; i < t->count && !t->cancelled; i++) {
            std::string out_text = flood::transform_text(t->text, t->mode);
            if (sender_) sender_(t->group_code, out_text);
            t->sent++; stats_.total_sent += 1;
            int jitter = delay_jitter(gen);
            std::this_thread::sleep_for(std::chrono::milliseconds(t->delay_ms + jitter));
        }
        t->running = false;
        { std::lock_guard<std::mutex> l2(mu_); tasks_.erase(id); }
    }).detach();
    return id;
}

void FloodEngine::cancel_task(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = tasks_.find(id);
    if (it != tasks_.end()) it->second->cancelled = true;
}

std::string FloodEngine::get_task_status(const std::string& id) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return "{}";
    auto& t = it->second;
    std::ostringstream o;
    o << "{\"sent\":" << t->sent.load()
      << ",\"cancelled\":" << (t->cancelled.load() ? "true" : "false")
      << ",\"running\":" << (t->running.load() ? "true" : "false") << "}";
    return o.str();
}

std::string FloodEngine::list_tasks() {
    std::lock_guard<std::mutex> l(mu_);
    std::ostringstream o; o << "[";
    bool first = true;
    for (auto& kv : tasks_) {
        if (!first) o << ","; first = false;
        o << "{\"id\":\"" << kv.first << "\",\"sent\":" << kv.second->sent.load() << "}";
    }
    o << "]"; return o.str();
}

FloodStats FloodEngine::get_stats() const {
    std::lock_guard<std::mutex> l(mu_);
    FloodStats s;
    s.total_tasks = stats_.total_tasks;
    s.total_sent = stats_.total_sent;
    s.active_tasks = 0;
    for (auto& kv : tasks_) { if (kv.second->running.load()) s.active_tasks++; }
    return s;
}

} // namespace flood

// =====================================================================
//  贴纸库实现
// =====================================================================
StickerLibrary::StickerLibrary() {
    std::vector<std::pair<std::string, std::string>> data = {
        {"六六六","278"},{"我想开了","262"},{"害羞","130"},{"比心","252"},
        {"委屈","125"},{"亲亲","146"},{"酷","131"},{"睡","145"},
        {"发呆","152"},{"可怜","157"},{"摊手","200"},{"头大","213"},
        {"吓","256"},{"吐血","203"},{"哼","185"},{"嘿嘿","220"},
        {"头秃","218"},{"暗中观察","221"},{"我酸了","224"},{"打call","246"},
        {"庆祝","251"},{"奋斗","151"},{"惊讶","143"},{"疑问","144"},
        {"仔细分析","248"},{"撅嘴","184"},{"泪奔","199"},{"尊嘟假嘟","276"},
        {"略略略","113"},{"困","180"},{"折磨","181"},{"抠鼻","182"},
        {"鼓掌","183"},{"斜眼笑","204"},{"辣眼睛","216"},{"哦哟","217"},
        {"吃瓜","222"},{"狗头","225"},{"敬礼","227"},{"哦","231"},
        {"拿到红包","236"},{"牛吖","239"},{"贴贴","272"},{"爱心","138"},
        {"晚安","170"},{"太阳","176"},{"柠檬","266"},{"大冤种","267"},
        {"吐了","132"},{"怒","134"},{"玫瑰","165"},{"凋谢","119"},
        {"点赞","159"},{"握手","164"},{"抱拳","163"},{"ok","169"},
        {"拳头","174"},{"鞭炮","191"},{"烟花","258"},
    };
    for (auto& kv : data) stickers[kv.first] = kv.second;
}

std::string StickerLibrary::find_by_name(const std::string& name) const {
    auto it = stickers.find(name);
    if (it != stickers.end()) return it->second;
    for (auto& kv : stickers) {
        if (kv.first.find(name) != std::string::npos) return kv.second;
    }
    return "";
}

// ← 补：扫描 ico 目录，建立 贴纸名称 → ico 文件名 映射。
//   文件名格式 "数字_名称.ico"（如 "01_六六六.ico"），去掉"数字_"前缀后与贴纸库名称匹配。
void StickerLibrary::scan_icons(const std::string& dir) {
    icons.clear();
    auto strip_prefix = [](std::string base) -> std::string {
        size_t us = base.find('_');
        if (us != std::string::npos) {
            std::string num = base.substr(0, us);
            bool all_digit = !num.empty() && std::all_of(num.begin(), num.end(),
                                                         [](char c) { return c >= '0' && c <= '9'; });
            if (all_digit) return base.substr(us + 1);
        }
        return base;
    };
#ifdef YUANBAO_PLATFORM_WINDOWS
    std::wstring pattern = util::utf8_to_wide(dir) + L"\\*.ico";
    WIN32_FIND_DATAW ffd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &ffd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string fname = util::wide_to_utf8(std::wstring(ffd.cFileName));
        if (fname.size() < 5) continue;
        std::string base = fname.substr(0, fname.size() - 4);  // 去掉 ".ico"
        base = strip_prefix(base);
        if (!base.empty() && stickers.find(base) != stickers.end()) icons[base] = fname;
    } while (FindNextFileW(h, &ffd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        std::string n = ent->d_name;
        if (n.size() < 5 || n.compare(n.size() - 4, 4, ".ico") != 0) continue;
        std::string base = n.substr(0, n.size() - 4);
        base = strip_prefix(base);
        if (!base.empty() && stickers.find(base) != stickers.end()) icons[base] = n;
    }
    closedir(d);
#endif
}

std::string StickerLibrary::icon_file(const std::string& name) const {
    auto it = icons.find(name);
    return it != icons.end() ? it->second : "";
}

// =====================================================================
//  BotConfig 实现
// =====================================================================
bool BotConfig::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto gs = [&](const std::string& k) -> std::string {
        size_t p = content.find("\"" + k + "\"");
        if (p == std::string::npos) return "";
        size_t c = content.find(":", p);
        if (c == std::string::npos) return "";
        size_t q = content.find("\"", c);
        if (q == std::string::npos) return "";
        size_t qe = content.find("\"", q + 1);
        if (qe == std::string::npos) return "";
        return content.substr(q + 1, qe - q - 1);
    };

    auto gi = [&](const std::string& k, int def) -> int {
        size_t p = content.find("\"" + k + "\"");
        if (p == std::string::npos) return def;
        size_t c = content.find(":", p);
        if (c == std::string::npos) return def;
        size_t end = c + 1;
        while (end < content.size() && (content[end] == ' ' || content[end] == '\t')) end++;
        size_t start = end;
        while (end < content.size() && content[end] >= '0' && content[end] <= '9') end++;
        if (end > start) return std::stoi(content.substr(start, end - start));
        return def;
    };

    auto gf = [&](const std::string& k, double def) -> double {
        size_t p = content.find("\"" + k + "\"");
        if (p == std::string::npos) return def;
        size_t c = content.find(":", p);
        if (c == std::string::npos) return def;
        size_t end = c + 1;
        while (end < content.size() && (content[end] == ' ' || content[end] == '\t')) end++;
        size_t start = end;
        while (end < content.size() &&
               ((content[end] >= '0' && content[end] <= '9') ||
                content[end] == '.' || content[end] == '-')) end++;
        if (end > start) return std::stod(content.substr(start, end - start));
        return def;
    };

    auto gb = [&](const std::string& k, bool def) -> bool {
        size_t p = content.find("\"" + k + "\"");
        if (p == std::string::npos) return def;
        size_t c = content.find(":", p);
        if (c == std::string::npos) return def;
        // "false" 不包含子串 "true"，直接找 "true" 即可
        return content.find("true", c) != std::string::npos;
    };

    app_key = gs("APP_KEY");
    app_secret = gs("APP_SECRET");
    api_domain = gs("API_DOMAIN");
    ws_url = gs("WS_URL");
    yuanbao_id = gs("YUANBAO_ID");
    port = gi("PORT", 5000);
    heartbeat_interval = gi("HEARTBEAT_INTERVAL", 10);

    // 多群监听列表：解析 JSON 数组 ["群1","群2"]（容错：直接写逗号分隔也行）
    listen_groups.clear();
    {
        size_t p = content.find("\"LISTEN_GROUPS\"");
        if (p != std::string::npos) {
            size_t c = content.find(":", p);
            size_t b = (c != std::string::npos) ? content.find('[', c) : std::string::npos;
            size_t e = (b != std::string::npos) ? content.find(']', b) : std::string::npos;
            if (b != std::string::npos && e != std::string::npos) {
                std::string arr = content.substr(b + 1, e - b - 1);
                size_t i = 0;
                while ((i = arr.find('"', i)) != std::string::npos) {
                    size_t q = arr.find('"', i + 1);
                    if (q == std::string::npos) break;
                    std::string g = arr.substr(i + 1, q - i - 1);
                    if (!g.empty()) listen_groups.push_back(g);
                    i = q + 1;
                }
            }
        }
    }

    // LLM 配置
    llm.api_url = gs("LLM_API_URL");
    llm.api_key = gs("LLM_API_KEY");
    llm.model = gs("LLM_MODEL");
    llm.system_prompt = gs("LLM_SYSTEM_PROMPT");
    llm.max_tokens = gi("LLM_MAX_TOKENS", 200);
    llm.temperature = gf("LLM_TEMPERATURE", 0.8);
    llm.timeout_sec = gi("LLM_TIMEOUT", 15);
    llm.enabled = !llm.api_key.empty() && !llm.api_url.empty();

    msg_log_enabled = gb("MSG_LOG_ENABLED", true);
    recall_monitor_enabled = gb("RECALL_MONITOR_ENABLED", false);

    return !app_key.empty();
}

// =====================================================================
//  HTTPS POST 客户端
// =====================================================================
#ifndef YUANBAO_NO_OPENSSL
static int https_post(const std::string& host, int port, const std::string& path,
                      const std::string& body,
                      const std::map<std::string, std::string>& extra_headers,
                      std::string& response_body, int timeout_ms = 10000) {
    int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    yb_platform::set_recv_timeout(sock, timeout_ms);
    yb_platform::set_send_timeout(sock, timeout_ms);

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) { close_socket(sock); return -2; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr, (size_t)he->h_length);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(sock); return -3;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close_socket(sock); return -4; }
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); close_socket(sock); return -5; }
    SSL_set_fd(ssl, sock);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl); SSL_CTX_free(ctx); close_socket(sock); return -6;
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json; charset=utf-8\r\n"
        << "X-Source: YB\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    for (auto& kv : extra_headers)
        req << kv.first << ": " << kv.second << "\r\n";
    req << "Connection: close\r\n\r\n" << body;

    std::string req_str = req.str();
    if (SSL_write(ssl, req_str.data(), (int)req_str.size()) < 0) {
        SSL_free(ssl); SSL_CTX_free(ctx); close_socket(sock); return -7;
    }

    std::string raw;
    char buf[8192];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0'; raw += buf;
    }
    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close_socket(sock);

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return -8;

    std::string headers = raw.substr(0, header_end);
    std::string body_part = raw.substr(header_end + 4);

    bool chunked = false;
    {
        std::istringstream hs(headers);
        std::string line;
        while (std::getline(hs, line)) {
            std::transform(line.begin(), line.end(), line.begin(), ::tolower);
            if (line.find("transfer-encoding") != std::string::npos &&
                line.find("chunked") != std::string::npos) { chunked = true; break; }
        }
    }

    if (chunked) {
        std::string decoded;
        size_t pos = 0;
        while (pos < body_part.size()) {
            size_t crlf = body_part.find("\r\n", pos);
            if (crlf == std::string::npos) break;
            std::string len_str = body_part.substr(pos, crlf - pos);
            int chunk_len = (int)std::stoul(len_str, nullptr, 16);
            if (chunk_len == 0) break;
            decoded += body_part.substr(crlf + 2, (size_t)chunk_len);
            pos = crlf + 2 + (size_t)chunk_len;
        }
        response_body = decoded;
    } else {
        response_body = body_part;
    }
    return 0;
}

/** 通用 HTTPS 请求（支持 PUT/二进制 body，用于 COS 上传） */
static int https_request(const std::string& method, const std::string& host, int port,
                         const std::string& path, const std::string& body,
                         const std::string& content_type,
                         const std::map<std::string, std::string>& extra_headers,
                         std::string& response_body, int timeout_ms = 30000) {
    int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    yb_platform::set_recv_timeout(sock, timeout_ms);
    yb_platform::set_send_timeout(sock, timeout_ms);

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) { close_socket(sock); return -2; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr, (size_t)he->h_length);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(sock); return -3;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { close_socket(sock); return -4; }
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); close_socket(sock); return -5; }
    SSL_set_fd(ssl, sock);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl); SSL_CTX_free(ctx); close_socket(sock); return -6;
    }

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Content-Type: " << content_type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n";
    for (auto& kv : extra_headers)
        req << kv.first << ": " << kv.second << "\r\n";
    req << "Connection: close\r\n\r\n";
    std::string req_str = req.str();
    req_str += body;  // 二进制安全拼接

    size_t sent_total = 0;
    while (sent_total < req_str.size()) {
        int w = SSL_write(ssl, req_str.data() + sent_total, (int)(req_str.size() - sent_total));
        if (w <= 0) { SSL_free(ssl); SSL_CTX_free(ctx); close_socket(sock); return -7; }
        sent_total += (size_t)w;
    }

    std::string raw;
    char buf[16384];
    int n;
    while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0)
        raw.append(buf, (size_t)n);
    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close_socket(sock);

    size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) return -8;
    std::string headers = raw.substr(0, header_end);
    std::string body_part = raw.substr(header_end + 4);

    bool chunked = false;
    bool has_cl = false;
    size_t cl = 0;
    {
        std::istringstream hs(headers);
        std::string line;
        while (std::getline(hs, line)) {
            std::string low = line;
            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
            if (low.find("transfer-encoding") != std::string::npos &&
                low.find("chunked") != std::string::npos) chunked = true;
            if (low.find("content-length") != std::string::npos) {
                has_cl = true;
                size_t colon = low.find(':');
                if (colon != std::string::npos)
                    cl = (size_t)std::stoll(low.substr(colon + 1));
            }
        }
    }

    if (chunked) {
        std::string decoded;
        size_t pos = 0;
        while (pos < body_part.size()) {
            size_t crlf = body_part.find("\r\n", pos);
            if (crlf == std::string::npos) break;
            std::string len_str = body_part.substr(pos, crlf - pos);
            int chunk_len = (int)std::stoul(len_str, nullptr, 16);
            if (chunk_len == 0) break;
            decoded += body_part.substr(crlf + 2, (size_t)chunk_len);
            pos = crlf + 2 + (size_t)chunk_len;
        }
        response_body = decoded;
    } else if (has_cl && body_part.size() > cl) {
        response_body = body_part.substr(0, cl);
    } else {
        response_body = body_part;
    }
    return 0;
}
#endif

// =====================================================================
//  Bot 鉴权
// =====================================================================
bool YuanbaoServer::bot_sign_token() {
    // 与 Python 一致：如果已有 token，直接复用，不重复签票
    if (!client_token_.empty() && !config_.yuanbao_id.empty()) return true;
    if (config_.app_key.empty() || config_.app_secret.empty()) return false;

    // 生成 UTC+8 时间戳（系统已经是 Asia/Shanghai 时区）
    time_t t = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S+08:00", &tm_buf);
    std::string timestamp = time_buf;

    std::string nonce = util::random_hex_id();

    // 签名: HMAC-SHA256(key=APP_SECRET, data=nonce+timestamp+APP_KEY+APP_SECRET)
    std::string plain = nonce + timestamp + config_.app_key + config_.app_secret;
    std::string signature;

#ifndef YUANBAO_NO_OPENSSL
    {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int result_len = 0;
        HMAC(EVP_sha256(),
             config_.app_secret.data(), (int)config_.app_secret.size(),
             (const unsigned char*)plain.data(), plain.size(),
             result, &result_len);
        char hex[65];
        for (unsigned int i = 0; i < result_len; i++)
            sprintf(hex + i * 2, "%02x", result[i]);
        signature = std::string(hex, result_len * 2);
    }
#else
    signature = sha1_hex(plain);
#endif

    std::ostringstream body;
    body << "{\"app_key\":\"" << config_.app_key << "\","
         << "\"timestamp\":\"" << timestamp << "\","
         << "\"nonce\":\"" << nonce << "\","
         << "\"signature\":\"" << signature << "\"}";

    std::string resp_body;
    std::string api_path = "/api/v5/robotLogic/sign-token";

    std::string host = config_.api_domain;
    int port = 443;
    if (host.find("https://") == 0) host = host.substr(8);
    else if (host.find("http://") == 0) host = host.substr(7);
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::stoi(host.substr(colon + 1));
        host = host.substr(0, colon);
    }

    // 生成 instance_id (1-1000)
    std::string instance_id = std::to_string(rand() % 1000 + 1);

    int ret;
#ifdef YUANBAO_NO_OPENSSL
    {
        std::ostringstream cmd;
        cmd << "curl -s -k -X POST \"https://" << host << ":" << port << api_path << "\""
            << " -H \"Content-Type: application/json\""
            << " -H \"X-AppVersion: 1.0.11\""
            << " -H \"X-OperationSystem: linux\""
            << " -H \"X-Instance-Id: " << instance_id << "\""
            << " -H \"X-Bot-Version: 2026.3.22\""
            << " -d '" << body.str() << "'";
        FILE* fp = popen(cmd.str().c_str(), "r");
        if (!fp) return false;
        char buf2[65536];
        size_t n2 = fread(buf2, 1, sizeof(buf2) - 1, fp);
        buf2[n2] = '\0';
        pclose(fp);
        resp_body = std::string(buf2, n2);
        ret = 0;
    }
#else
    {
        std::map<std::string, std::string> extra;
        extra["X-AppVersion"] = "1.0.11";
        extra["X-OperationSystem"] = "linux";
        extra["X-Instance-Id"] = instance_id;
        extra["X-Bot-Version"] = "2026.3.22";
        ret = https_post(host, port, api_path, body.str(), extra, resp_body, 30000);
    }
#endif

    if (ret != 0) {
        std::cerr << "[Bot] 鉴权请求失败 ret=" << ret << "\n";
        return false;
    }

    JsonVal root;
    if (!json_parse(resp_body, root)) {
        std::cerr << "[Bot] 鉴权响应 JSON 解析失败\n"
                  << "[Bot] 原始响应: " << resp_body << "\n";
        return false;
    }

    // Python sender.py 检查 code == 0
    if (root["code"].asInt() == 0) {
        client_token_ = root["data"]["token"].asString();
        std::string bot_id = root["data"]["bot_id"].asString();
        if (!bot_id.empty()) {
            // 自动获取 YUANBAO_ID：鉴权响应中的 bot_id 回写 config（无需硬编码）
            bool changed = (config_.yuanbao_id != bot_id);
            config_.yuanbao_id = bot_id;
            if (changed) save_config();
        }
        std::cerr << "[DEBUG] token length: " << client_token_.size() << "\n";
        std::cerr << "[DEBUG] token preview: " << client_token_.substr(0, 50) << "...\n";
        std::cout << "[Bot] 鉴权成功! Bot ID: " << bot_id
                  << " token: " << client_token_.substr(0, 8) << "...\n";
        return true;
    }

    std::cerr << "[Bot] 鉴权失败 code=" << root["code"].asInt()
              << " msg=" << root["msg"].asString() << "\n"
              << "[Bot] 请求体: " << body.str() << "\n"
              << "[Bot] 响应体: " << resp_body << "\n";
    return false;
}

// =====================================================================
//  Bot WebSocket 客户端
// =====================================================================
bool YuanbaoServer::bot_ws_connect() {
    auth_response_received_ = false;
    if (config_.ws_url.empty()) {
        std::cerr << "[Bot] WS_URL 未配置\n"; return false;
    }

    std::string url = config_.ws_url;
    bool is_ssl = (url.find("wss://") == 0 || url.find("https://") == 0);
    if (url.find("wss://") == 0) url = url.substr(6);
    else if (url.find("https://") == 0) url = url.substr(8);
    else if (url.find("ws://") == 0) url = url.substr(5);

    std::string host, path = "/"; int port = 443;
    size_t slash = url.find('/');
    size_t colon_pos = url.find(':');
    if (colon_pos != std::string::npos && (slash == std::string::npos || colon_pos < slash)) {
        host = url.substr(0, colon_pos);
        port = std::stoi(url.substr(colon_pos + 1,
            slash == std::string::npos ? std::string::npos : slash - colon_pos - 1));
        if (slash != std::string::npos) path = url.substr(slash);
    } else {
        host = slash != std::string::npos ? url.substr(0, slash) : url;
        if (slash != std::string::npos) path = url.substr(slash);
        if (!is_ssl) port = 80;
    }

    bot_ws_fd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (bot_ws_fd_ < 0) return false;
    yb_platform::set_recv_timeout(bot_ws_fd_, 15000);
    yb_platform::set_send_timeout(bot_ws_fd_, 15000);

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) { close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr, (size_t)he->h_length);

    if (connect(bot_ws_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false;
    }

#ifndef YUANBAO_NO_OPENSSL
    if (is_ssl) {
        bot_ws_ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!bot_ws_ssl_ctx_) { close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false; }
        SSL_CTX_set_min_proto_version(bot_ws_ssl_ctx_, TLS1_2_VERSION);
        SSL_CTX_set_max_proto_version(bot_ws_ssl_ctx_, TLS1_2_VERSION);
        SSL_CTX_set_cipher_list(bot_ws_ssl_ctx_, 
            "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:"
            "ECDHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-SHA384");
        SSL_CTX_set_verify(bot_ws_ssl_ctx_, SSL_VERIFY_NONE, nullptr);
        SSL_CTX_set_options(bot_ws_ssl_ctx_, SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET);
        bot_ws_ssl_ = SSL_new(bot_ws_ssl_ctx_);
        if (!bot_ws_ssl_) { SSL_CTX_free(bot_ws_ssl_ctx_); close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false; }
        SSL_set_fd(bot_ws_ssl_, bot_ws_fd_);
        SSL_set_tlsext_host_name(bot_ws_ssl_, host.c_str());  // SNI
        if (SSL_connect(bot_ws_ssl_) != 1) {
            SSL_free(bot_ws_ssl_); SSL_CTX_free(bot_ws_ssl_ctx_); bot_ws_ssl_ = nullptr; bot_ws_ssl_ctx_ = nullptr;
            close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false;
        }
    }
#endif

    std::string ws_key = util::random_id(16);
    ws_key = proto::base64_encode((uint8_t*)ws_key.data(), ws_key.size());

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n";
    if (port == 443 || port == 80) req << "Host: " << host << "\r\n";
    else req << "Host: " << host << ":" << port << "\r\n";
    req << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << ws_key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n"
        << "Origin: https://" << host << "\r\n"
        << "\r\n";
    std::string req_str = req.str();

#ifndef YUANBAO_NO_OPENSSL
    if (is_ssl && bot_ws_ssl_) {
        if (SSL_write(bot_ws_ssl_, req_str.data(), (int)req_str.size()) < 0) {
            SSL_free(bot_ws_ssl_); SSL_CTX_free(bot_ws_ssl_ctx_); bot_ws_ssl_ = nullptr; bot_ws_ssl_ctx_ = nullptr;
            close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false;
        }
        char resp_buf[1024];
        int n = SSL_read(bot_ws_ssl_, resp_buf, sizeof(resp_buf) - 1);
        if (n <= 0) {
            SSL_free(bot_ws_ssl_); SSL_CTX_free(bot_ws_ssl_ctx_); bot_ws_ssl_ = nullptr; bot_ws_ssl_ctx_ = nullptr;
            close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false;
        }
        resp_buf[n] = '\0';
        std::string resp_str(resp_buf, n);
        if (resp_str.find("101") == std::string::npos) {
            std::cerr << "[Bot] WebSocket 握手失败: " << resp_str.substr(0, 200) << "\n";
            SSL_free(bot_ws_ssl_); SSL_CTX_free(bot_ws_ssl_ctx_); bot_ws_ssl_ = nullptr; bot_ws_ssl_ctx_ = nullptr;
            close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false;
        }
    } else
#endif
    {
        if (YB_SEND(bot_ws_fd_, req_str.data(), req_str.size(), 0) < 0) {
            close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false;
        }
        char resp_buf[1024];
        int n = YB_RECV(bot_ws_fd_, resp_buf, sizeof(resp_buf) - 1, 0);
        if (n <= 0) { close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false; }
        resp_buf[n] = '\0';
        std::string resp_str(resp_buf, n);
        if (resp_str.find("101") == std::string::npos) {
            std::cerr << "[Bot] WebSocket 握手失败: " << resp_str.substr(0, 200) << "\n";
            close_socket(bot_ws_fd_); bot_ws_fd_ = -1; return false;
        }
    }

    std::cout << "[Bot] WebSocket 已建立 fd=" << bot_ws_fd_ << " (SSL=" << (is_ssl ? "yes" : "no") << ")\n";
    bot_connected_ = true;
    return true;
}

Bytes YuanbaoServer::bot_ws_build_auth() {
    Bytes auth_data = proto::encode_auth_bind(config_.yuanbao_id, client_token_);
    // cmd_type=0 (REQUEST), module="conn_access" — 与 Python sender.py 一致
    Bytes head = proto::encode_conn_head(0, "auth-bind", ++bot_seq_no_, util::random_hex_id(), "conn_access");
    Bytes body = proto::encode_conn_msg(head, auth_data);
    Bytes frame = proto::build_ws_frame(2, body);
    std::cerr << "[DEBUG] auth_data size: " << auth_data.size() << std::endl;
    std::cerr << "[DEBUG] head size: " << head.size() << std::endl;
    std::cerr << "[DEBUG] body size: " << body.size() << std::endl;
    std::cerr << "[DEBUG] frame size: " << frame.size() << std::endl;
    return frame;
}

// 构建命令同步消息 (sync_information)
Bytes YuanbaoServer::bot_ws_build_sync_info() {
    Bytes data;
    // field 1: syncType = 1 (SYNC_INFORMATION_TYPE_COMMANDS)
    bytes_append(data, proto::encode_uint32(1, 1));
    // field 2: botVersion
    bytes_append(data, proto::encode_string(2, "1.5"));
    // field 3: pluginVersion
    bytes_append(data, proto::encode_string(3, "1.0.0"));
    
    // field 11: sync_cmds_data { field 1: SyncCommandData { field 1:cmd, 2:desc } }
    Bytes cmd_item;
    bytes_append(cmd_item, proto::encode_string(1, "/help"));
    bytes_append(cmd_item, proto::encode_string(2, "显示帮助信息"));
    Bytes sync_cmds;
    bytes_append(sync_cmds, proto::encode_message(1, cmd_item)); // field 1: repeated commands
    bytes_append(data, proto::encode_message(11, sync_cmds));
    
    Bytes head = proto::encode_conn_head(0, "sync_information", ++bot_seq_no_, util::random_hex_id(), "yuanbao_openclaw_proxy");
    return proto::build_ws_frame(2, proto::encode_conn_msg(head, data));
}

Bytes YuanbaoServer::bot_ws_build_ping() {
    Bytes head = proto::encode_conn_head(0, "ping", ++bot_seq_no_, util::random_hex_id(), "conn_access");
    return proto::build_ws_frame(2, proto::encode_conn_msg(head, {}));
}

Bytes YuanbaoServer::bot_ws_build_send_group_msg(const std::string& text, const std::string& group) {
    // Python: body 和 head 使用不同的 msg_id
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_send_group_msg(body_msg_id, group, config_.yuanbao_id, text);
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    return proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
}

Bytes YuanbaoServer::bot_ws_build_send_c2c_msg(const std::string& text, const std::string& to) {
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_send_c2c_msg(body_msg_id, to, config_.yuanbao_id, text);
    Bytes head = proto::encode_conn_head(0, "send_c2c_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    return proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
}

Bytes YuanbaoServer::bot_ws_build_sticker_msg(const std::string& sticker_json, const std::string& group,
                                              const std::string& at_user, const std::string& at_nick,
                                              const std::string& text) {
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_sticker_group_msg(body_msg_id, group, config_.yuanbao_id, sticker_json,
                                                 at_user, at_nick, text);
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    return proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
}

Bytes YuanbaoServer::bot_ws_build_get_members_msg(const std::string& group) {
    // 与 Python encode_get_group_member_list_req 一致：body = {1: group_code}
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_string(1, group);
    Bytes head = proto::encode_conn_head(0, "get_group_member_list", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    return proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
}

Bytes YuanbaoServer::bot_ws_build_query_group_info_msg(const std::string& group, const std::string& msg_id) {
    // 与 Python _build_query_group_info_req 一致：body = {1: group_code}
    // msg_id 由调用方生成并登记到 pending_query_groups_，响应按 msg_id 关联回写群名
    Bytes body = proto::encode_string(1, group);
    Bytes head = proto::encode_conn_head(0, "query_group_info", ++bot_seq_no_, msg_id, "yuanbao_openclaw_proxy");
    return proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
}

bool YuanbaoServer::bot_ws_send_bytes(const Bytes& data) {
    if (bot_ws_fd_ < 0) { std::cerr << "[bot_ws_send_bytes] fd<0 发送失败\n"; return false; }
#ifndef YUANBAO_NO_OPENSSL
    if (bot_ws_ssl_) {
        // 与接收循环的 SSL_read 串行化（同一互斥量），避免并发读写同一 SSL 对象
        // 损坏其内部状态（曾导致 SSL 错误 5、SSL_write 永久阻塞、HTTP 请求线程全部卡死）
        std::lock_guard<std::mutex> _lk(bot_send_mu_);
        int sent = SSL_write(bot_ws_ssl_, data.data(), (int)data.size());
        if (sent != (int)data.size())
            std::cerr << "[bot_ws_send_bytes] SSL_write 短写/失败 sent=" << sent << " want=" << data.size() << "\n";
        return sent == (int)data.size();
    }
#endif
    ssize_t sent = YB_SEND(bot_ws_fd_, data.data(), data.size(), 0);
    if (sent != (ssize_t)data.size()) std::cerr << "[bot_ws_send_bytes] send 短写 sent=" << sent << " want=" << data.size() << "\n";
    return sent == (ssize_t)data.size();
}

bool YuanbaoServer::send_group_text(const std::string& group, const std::string& text) {
    if (group.empty() || text.empty()) return false;
    auto frame = bot_ws_build_send_group_msg(text, group);
    bool ok = bot_ws_send_bytes(frame);
    if (ok) record_sent_message(group, text);  // 服务端不推送 bot 自己的消息，主动记录
    return ok;
}

bool YuanbaoServer::send_c2c_text(const std::string& to, const std::string& text) {
    if (to.empty() || text.empty()) return false;
    auto frame = bot_ws_build_send_c2c_msg(text, to);
    bool ok = bot_ws_send_bytes(frame);
    // ← 修复：私聊发送记录带 peer（目标用户），供前端按人过滤历史
    if (ok) record_sent_c2c_message(to, text);
    return ok;
}

void YuanbaoServer::record_sent_message(const std::string& group, const std::string& text) {
    JsonVal empty_mi;
    record_sent_message(group, text, empty_mi);
}

// ← 修复：记录自己发送的消息（含贴纸等媒体）。此前贴纸发送成功后不记录，
//   导致自己发的贴纸在界面上不显示。
// ← 修复：获取 Bot 在群里的真实昵称。此前硬编码"元宝"，
//   但 Bot 在群里的昵称可能被修改（如群成员列表中的"测试"），
//   导致消息列表显示的 Bot 昵称与群成员列表不一致。
std::string YuanbaoServer::bot_display_name() {
    std::lock_guard<std::mutex> l(members_mu_);
    // 在所有群缓存中查找 Bot 自己的昵称（按群分别缓存，遍历查找）
    for (auto& kv : members_cache_map_) {
        for (auto& m : kv.second.members) {
            if (m.first == config_.yuanbao_id && !m.second.empty()) return m.second;
        }
    }
    return "元宝";
}

void YuanbaoServer::record_sent_message(const std::string& group, const std::string& text, const JsonVal& media_info) {
    if (text.empty() && media_info.obj.empty()) return;
    JsonVal entry;
    entry["cmd"] = "send_group_message";
    entry["sender_id"] = config_.yuanbao_id;
    entry["sender_name"] = bot_display_name();
    entry["group_code"] = group;
    entry["content"] = text;
    if (!media_info.obj.empty()) entry["media_info"] = media_info;
    std::string ts = util::time_iso();
    entry["time"] = ts;
    entry["timestamp"] = ts;
    entry["msg_id"] = "self-" + util::random_hex_id();
    cache_message(entry);
    msg_logger_.log(entry);
}

// ← 补：私聊发送记录（带 peer，供前端按人过滤历史）
void YuanbaoServer::record_sent_c2c_message(const std::string& to, const std::string& text) {
    if (to.empty() || text.empty()) return;
    JsonVal entry;
    entry["cmd"] = "send_c2c_message";
    entry["sender_id"] = config_.yuanbao_id;
    entry["sender_name"] = bot_display_name();
    entry["group_code"] = "";
    entry["is_c2c"] = true;
    entry["c2c_peer"] = to;
    entry["content"] = text;
    std::string ts = util::time_iso();
    entry["time"] = ts;
    entry["timestamp"] = ts;
    entry["msg_id"] = "self-" + util::random_hex_id();
    cache_message(entry);
    msg_logger_.log(entry);
}

bool YuanbaoServer::send_group_sticker(const std::string& group, const std::string& sticker_json,
                                       const std::string& at_user, const std::string& at_nick,
                                       const std::string& text) {
    if (group.empty()) return false;
    auto frame = bot_ws_build_sticker_msg(sticker_json, group, at_user, at_nick, text);
    return bot_ws_send_bytes(frame);
}

// 与 Python sender.py `_get_upload_info` + `_upload_to_cos` 一致
bool YuanbaoServer::upload_media(const std::string& file_name, const std::string& file_data,
                                 std::string& out_url, std::string& out_uuid) {
    if (file_name.empty() || file_data.empty()) return false;
    if (!bot_sign_token()) return false;  // 确保有 token

    std::string file_id = util::random_hex_id();  // uuid4().hex
    out_uuid = file_id;

    std::string host = config_.api_domain;
    int port = 443;
    if (host.find("https://") == 0) host = host.substr(8);
    else if (host.find("http://") == 0) host = host.substr(7);
    size_t colon = host.find(':');
    if (colon != std::string::npos) {
        port = std::stoi(host.substr(colon + 1));
        host = host.substr(0, colon);
    }

    std::string resp_body;
    std::map<std::string, std::string> extra;
    extra["X-ID"] = config_.yuanbao_id;
    extra["X-Token"] = client_token_;
    extra["X-Source"] = "web";
    extra["X-AppVersion"] = "2.0.1";
    extra["X-OperationSystem"] = "Linux";
    extra["X-Instance-Id"] = "99";

    std::ostringstream body;
    body << "{\"fileName\":" << json_quote(file_name)
         << ",\"fileId\":\"" << file_id << "\""
         << ",\"docFrom\":\"localDoc\",\"docOpenId\":\"\"}";

    std::string api_path = "/api/resource/genUploadInfo";
    int ret;
#ifdef YUANBAO_NO_OPENSSL
    {
        // 无 OpenSSL 时使用 curl 命令行（与 sign-token 一致）
        std::ostringstream cmd;
        cmd << "curl -s -k -X POST \"https://" << host << ":" << port << api_path << "\""
            << " -H \"Content-Type: application/json\""
            << " -H \"X-ID: " << config_.yuanbao_id << "\""
            << " -H \"X-Token: " << client_token_ << "\""
            << " -H \"X-Source: web\""
            << " -H \"X-AppVersion: 2.0.1\""
            << " -H \"X-OperationSystem: Linux\""
            << " -H \"X-Instance-Id: 99\""
            << " -d '" << body.str() << "'";
        FILE* fp = popen(cmd.str().c_str(), "r");
        if (!fp) return false;
        char buf2[65536];
        size_t n2 = fread(buf2, 1, sizeof(buf2) - 1, fp);
        buf2[n2] = '\0';
        pclose(fp);
        resp_body = std::string(buf2, n2);
        ret = 0;
    }
#else
    ret = https_request("POST", host, port, api_path, body.str(),
                        "application/json", extra, resp_body, 30000);
#endif

    if (ret != 0) {
        std::cerr << "[Bot] genUploadInfo 请求失败 ret=" << ret << "\n";
        return false;
    }

    // genUploadInfo 响应为平铺结构（无 code/data 包装，与 Python result.get("data", result) 一致）：
    // {"error":{},"isUploaded":false,"bucketName":...,"region":...,"location":...,
    //  "encryptTmpSecretId":...,"encryptTmpSecretKey":...,"encryptToken":...,
    //  "startTime":<num>,"expiredTime":<num>,"resourceUrl":...}
    JsonVal root;
    if (!json_parse(resp_body, root)) {
        std::cerr << "[Bot] genUploadInfo 响应解析失败: " << resp_body.substr(0, 300) << "\n";
        return false;
    }

    // 兼容两种结构：平铺响应（新接口）或 {code,data:{...}} 包装（旧接口）
    JsonVal data = root["data"];
    bool is_flat = data.empty() && !root.empty();
    const JsonVal& d = is_flat ? root : data;

    std::string secret_id = d["encryptTmpSecretId"].asString();
    std::string secret_key = d["encryptTmpSecretKey"].asString();
    std::string security_token = d["encryptToken"].asString();
    std::string start_time = d["startTime"].asString();
    std::string expired_time = d["expiredTime"].asString();
    std::string bucket = d["bucketName"].asString();
    std::string region = d["region"].asString();
    std::string location = d["location"].asString();
    std::string resource_url = d["resourceUrl"].asString();

    // startTime/expiredTime 是数字类型，asString() 取不到，需转字符串
    if (start_time.empty() || expired_time.empty()) {
        double st = d["startTime"].asDouble();
        double et = d["expiredTime"].asDouble();
        if (st > 0) start_time = std::to_string((long long)st);
        if (et > 0) expired_time = std::to_string((long long)et);
    }

    if (bucket.empty() || region.empty() || location.empty() || secret_key.empty()) {
        std::cerr << "[Bot] COS 凭证不完整: " << resp_body.substr(0, 300) << "\n";
        return false;
    }


    // COS 签名（与 Python _upload_to_cos 手动签名一致）
    std::string key_time = start_time + ";" + expired_time;
    std::string sign_key = hmac_sha1_hex(secret_key, key_time);
    std::string cos_host = bucket + ".cos." + region + ".myqcloud.com";
    std::string http_string = "put\n" + location + "\n\nhost=" + cos_host + "\n";
    std::string string_to_sign = "sha1\n" + key_time + "\n" + sha1_hex(http_string) + "\n";
    std::string signature = hmac_sha1_hex(sign_key, string_to_sign);

    std::string authorization =
        "q-sign-algorithm=sha1&q-ak=" + secret_id +
        "&q-sign-time=" + key_time +
        "&q-key-time=" + key_time +
        "&q-header-list=host&q-url-param-list=&q-signature=" + signature;
    if (!security_token.empty())
        authorization += "&x-cos-security-token=" + security_token;

    std::string upload_url = "https://" + cos_host + location;
    std::map<std::string, std::string> cos_headers;
    cos_headers["Authorization"] = authorization;
    if (!security_token.empty())
        cos_headers["x-cos-security-token"] = security_token;

    std::string cos_resp;
#ifdef YUANBAO_NO_OPENSSL
    {
        std::ostringstream cmd;
        cmd << "curl -s -k -X PUT \"" << upload_url << "\""
            << " -H \"Host: " << cos_host << "\""
            << " -H \"Authorization: " << authorization << "\""
            << " -H \"Content-Type: application/octet-stream\"";
        if (!security_token.empty())
            cmd << " -H \"x-cos-security-token: " << security_token << "\"";
        // 通过 stdin 传入二进制数据
        cmd << " --data-binary @-";
        FILE* fp = popen(cmd.str().c_str(), "w");
        if (!fp) return false;
        fwrite(file_data.data(), 1, file_data.size(), fp);
        pclose(fp);
        cos_resp = "";
        ret = 0;
    }
#else
    ret = https_request("PUT", cos_host, 443, location, file_data,
                        "application/octet-stream", cos_headers, cos_resp, 60000);
#endif
    if (ret != 0) {
        std::cerr << "[Bot] COS 上传失败 ret=" << ret << " resp=" << cos_resp.substr(0, 200) << "\n";
        return false;
    }


    // ← 修复：保存上传凭证，供 /api/image-proxy 对 _64 域图片重新签名 GET URL 下载
    {
        std::lock_guard<std::mutex> l(members_mu_);
        std::cerr << "[Bot] upload_media save sk=" << secret_key.substr(0, 4) << " keytime=" << key_time << "\n";
        cos_upload_region_ = region;
        cos_upload_bucket_ = bucket;
        cos_upload_location_ = location;
        cos_upload_secret_id_ = secret_id;
        cos_upload_secret_key_ = secret_key;
        cos_upload_token_ = security_token;
        cos_upload_key_time_ = key_time;
        cos_upload_expired_ = (int64_t)strtoll(expired_time.c_str(), nullptr, 10);
    }

    // ← 修复：对齐撤回提示的图片补发逻辑（handle_recall_notification）——
    //   直接用 genUploadInfo 返回的 resourceUrl（不 resolve、不签名）。
    //   撤回补发正是用 media_info.image_urls（resourceUrl）直接 send_group_image 成功，
    //   说明服务端能通过 resourceUrl 引用图片。
    out_url = resource_url;
    return true;
}

bool YuanbaoServer::send_group_image(const std::string& group, const std::string& url,
                                     const std::string& uuid, int size, int w, int h,
                                     const std::string& at_user, const std::string& at_nick) {
    if (group.empty() || url.empty()) return false;
    // 与 Python send_group_image_message / _build_at_message 一致
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body;
    bytes_append(body, proto::encode_string(1, body_msg_id));
    bytes_append(body, proto::encode_string(2, group));
    bytes_append(body, proto::encode_string(3, config_.yuanbao_id));
    bytes_append(body, proto::encode_string(4, ""));
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000);
    bytes_append(body, proto::encode_string(5, std::to_string(rnd)));  // string 类型
    if (!at_user.empty())
        bytes_append(body, proto::encode_message(6, proto::encode_at_custom_elem(at_user, at_nick)));
    bytes_append(body, proto::encode_message(6, proto::encode_image_elem(url, uuid, size, w, h)));
    bytes_append(body, proto::encode_string(7, ""));
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    auto frame = proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
    bool ok = bot_ws_send_bytes(frame);
    if (ok) record_sent_message(group, "[图片] " + url);
    return ok;
}

bool YuanbaoServer::send_group_file(const std::string& group, const std::string& url,
                                    const std::string& file_name,
                                    const std::string& uuid, int file_size,
                                    const std::string& at_user, const std::string& at_nick) {
    if (group.empty() || url.empty()) return false;
    // 与 Python send_file → _build_file_msg / _build_at_message 一致
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body;
    bytes_append(body, proto::encode_string(1, body_msg_id));
    bytes_append(body, proto::encode_string(2, group));
    bytes_append(body, proto::encode_string(3, config_.yuanbao_id));
    bytes_append(body, proto::encode_string(4, ""));
    uint32_t rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000);
    bytes_append(body, proto::encode_string(5, std::to_string(rnd)));  // string 类型
    if (!at_user.empty())
        bytes_append(body, proto::encode_message(6, proto::encode_at_custom_elem(at_user, at_nick)));
    bytes_append(body, proto::encode_message(6, proto::encode_file_elem(url, uuid, file_size, file_name)));
    bytes_append(body, proto::encode_string(7, ""));
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    auto frame = proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
    bool ok = bot_ws_send_bytes(frame);
    if (ok) record_sent_message(group, "[文件] " + file_name);
    return ok;
}

bool YuanbaoServer::send_group_at(const std::string& group, const std::string& text,
                                  const std::string& user_id, const std::string& display_name) {
    // ← 修复：允许空文本（前端「@昵称」不带内容也可发送）
    if (group.empty() || user_id.empty()) return false;
    // 与 Python _build_at_message 完全一致：at_elem + text_elem（两个 field 6）
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_send_group_at_msg(
        body_msg_id, group, config_.yuanbao_id, text, user_id, display_name);
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    auto frame = proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
    bool ok = bot_ws_send_bytes(frame);
    // ← 修复：记录时补上 @ 前缀（对齐收到消息的 "@昵称 文本" 格式），
    //   否则自己发送的 @ 消息在消息面板中不显示 @
    if (ok) {
        std::string at_display = display_name.empty() ? user_id : display_name;
        record_sent_message(group, "@" + at_display + " " + text);
    }
    return ok;
}

bool YuanbaoServer::send_group_multi_at(const std::string& group, const std::string& text,
                                        const std::vector<std::pair<std::string, std::string>>& at_users) {
    if (group.empty() || at_users.empty()) return false;
    // 与 Python _build_multi_at_message 一致：多个 at_elem + text_elem
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_send_group_multi_at_msg(
        body_msg_id, group, config_.yuanbao_id, text, at_users);
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    auto frame = proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
    bool ok = bot_ws_send_bytes(frame);
    // ← 修复：记录时补上所有 @ 前缀，与收到的批量 @ 消息显示格式一致
    if (ok) {
        std::string ats;
        for (auto& u : at_users) {
            ats += "@" + (u.second.empty() ? u.first : u.second) + " ";
        }
        record_sent_message(group, ats + text);
    }
    return ok;
}

bool YuanbaoServer::send_group_parts(const std::string& group, const std::vector<proto::SendPart>& parts) {
    if (group.empty() || parts.empty()) return false;
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_send_group_parts_msg(
        body_msg_id, group, config_.yuanbao_id, parts);
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    auto frame = proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
    bool ok = bot_ws_send_bytes(frame);
    // 记录日志：按片段顺序还原 "@昵称" 与文本，保持原始位置
    if (ok) {
        std::string log;
        for (auto& p : parts) {
            if (p.type == 1)
                log += "@" + (p.display.empty() ? p.user_id : p.display);
            else
                log += p.text;
        }
        record_sent_message(group, log);
    }
    return ok;
}

void YuanbaoServer::bot_ws_recv_loop() {
    try {
    std::vector<char> buf(32768, 0);
    size_t buf_pos = 0;
    int64_t last_ping = util::now_ms();

    while (bot_running_.load() && bot_ws_fd_ >= 0) {
        if (util::now_ms() - last_ping > 70000) {
            auto ping_frame = bot_ws_build_ping();
            bot_ws_send_bytes(ping_frame);
            last_ping = util::now_ms();
        }

#ifndef YUANBAO_NO_OPENSSL
        if (bot_ws_ssl_) {
            // 用 select 等待数据，避免阻塞
            { fd_set rfds; FD_ZERO(&rfds); FD_SET((unsigned)bot_ws_fd_, &rfds);
              struct timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
              if (select(bot_ws_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue; }
            
            int n;
            {
                // 与发送线程串行化，避免同一 SSL 对象并发读写
                std::lock_guard<std::mutex> _lk(bot_send_mu_);
                n = SSL_read(bot_ws_ssl_, buf.data() + buf_pos, (int)(buf.size() - buf_pos) - 1);
            }
            if (n <= 0) {
                if (n == 0) { std::cerr << "[Bot] SSL 关闭\n"; bot_connected_ = false; break; }
                int e = SSL_get_error(bot_ws_ssl_, n);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
                if (e == SSL_ERROR_ZERO_RETURN) { std::cerr << "[Bot] SSL 零返回\n"; bot_connected_ = false; break; }
                std::cerr << "[Bot] SSL 错误 " << e << "\n"; bot_connected_ = false; break;
            }
            buf_pos += (size_t)n;

            // 解析所有完整帧
            size_t pos = 0;
            while (pos < buf_pos) {
                size_t consumed = 0;
                proto::WSFrame frame = proto::parse_ws_frame((uint8_t*)buf.data() + pos, buf_pos - pos, consumed);
                if (consumed == 0) break;
                pos += consumed;
                if (frame.opcode == 8) {  // close frame
                    std::cerr << "[Bot] 收到 close 帧, payload=" << frame.payload.size() << "\n";
                    // 回应 close
                    Bytes close_resp = proto::build_ws_frame(8, frame.payload, true);
                    bot_ws_send_bytes(close_resp);
                    bot_connected_ = false;
                    break;
                }
                if (frame.opcode == 9) {  // ping → pong
                    Bytes pong = proto::build_ws_frame(10, frame.payload, true);
                    bot_ws_send_bytes(pong);
                    continue;
                }
                if (!frame.payload.empty()) { bot_handle_frame(frame.payload); last_ping = util::now_ms(); }
            }
            if (!bot_connected_.load()) break;  // close frame 处理后退出
            if (pos > 0) {
                if (pos < buf_pos) memmove(buf.data(), buf.data() + pos, buf_pos - pos);
                buf_pos -= pos;
            }
            if (buf_pos >= buf.size() - 2048) buf_pos = 0;
            continue;
        }
#endif

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET((unsigned int)bot_ws_fd_, &rfds);
        struct timeval tv2; tv2.tv_sec = 5; tv2.tv_usec = 0;
        int sel = select(bot_ws_fd_ + 1, &rfds, nullptr, nullptr, &tv2);
        if (sel <= 0) continue;

        if (buf_pos >= buf.size()) buf_pos = 0;
        int n = recv(bot_ws_fd_, buf.data() + buf_pos, (int)(buf.size() - buf_pos), 0);
        if (n <= 0) {
            std::cerr << "[Bot] 连接断开，尝试重连...\n";
            bot_connected_ = false;
            close_socket(bot_ws_fd_); bot_ws_fd_ = -1;
            for (int i = 0; i < 3 && bot_running_.load(); i++) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                if (bot_ws_connect()) {
                    bot_ws_send_bytes(bot_ws_build_auth());
                    last_ping = util::now_ms();
                    break;
                }
            }
            break;
        }

        // ← 修复死循环：parse_ws_frame 内部会重置 pos（它把 pos 当纯输出），
        //   旧代码直接复用 pos 变量导致每轮都从缓冲区开头解析同一帧，永不推进。
        //   改用 consumed 输出参数 + 显式偏移，与上方 SSL 分支一致，正确逐帧消费。
        size_t pos = 0;
        while (pos < (size_t)n) {
            size_t consumed = 0;
            proto::WSFrame frame = proto::parse_ws_frame((uint8_t*)buf.data() + buf_pos + pos, (size_t)n - pos, consumed);
            if (consumed == 0) break;  // 数据不足，等待更多数据
            pos += consumed;
            if (!frame.payload.empty()) {
                bot_handle_frame(frame.payload);
                last_ping = util::now_ms();
            }
        }
        buf_pos = 0;
    }
    bot_connected_ = false;
    } catch (const std::exception& e) {
        std::cerr << "[Bot] recv_loop 异常: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[Bot] recv_loop 未知异常\n";
    }
}


void YuanbaoServer::bot_handle_frame(const Bytes& data) {
    if (data.empty()) return;

    // 1. 解码外层 ConnMsg（Protobuf）
    proto::DecodedMsg msg = proto::decode_conn_msg(data);
    
    // 如果 ConnMsg 解码失败，直接忽略
    if (msg.cmd.empty()) {
        std::cerr << "[Bot] RECV 解码失败, cmd 为空\n";
        return;
    }
    
    // 移除高频 hex 转储：每帧写 stderr（含成员列表 3KB 字节流）在 5 秒轮询下
    // 造成严重 IO 开销并放大卡顿，仅保留一次带 seq 的简要摘要供排查
    if (msg.cmd_type == 2) {
        // PUSH 消息（群消息等）也走此处，同样仅摘要
    }
    
    // 2. 根据 cmd_type 处理（与 Python sender.py 完全一致）
    int cmd_type = msg.cmd_type;  // 0=REQUEST, 1=RESPONSE, 2=PUSH
    std::string& cmd = msg.cmd;
    
    // Pong 响应
    if (cmd == "pong") return;

    // 服务端应用层 ping → 回 pong 保持连接（对照 Python websockets 库自动 pong）
    if (cmd == "ping") {
        Bytes pong_head = proto::encode_conn_head(1, "pong", ++bot_seq_no_, util::random_hex_id(), "conn_access");
        bot_ws_send_bytes(proto::build_ws_frame(2, proto::encode_conn_msg(pong_head)));
        return;
    }
    
    // Auth-bind 响应 → 发 sync（与 Python connect() 一致）
    if (cmd == "auth-bind") {
        auth_response_received_ = true;
        if (!msg.data.empty()) {
            // auth 数据为 token/IP 等 ASCII 文本，只保留可打印 ASCII，彻底避免乱码
            std::string ds(msg.data.begin(), msg.data.end());
            std::string clean;
            for (char ch : ds) {
                unsigned char uc = (unsigned char)ch;
                if (uc >= 32 && uc <= 126) clean += ch;
                else clean += '.';
            }
            if (clean.size() > 120) clean = clean.substr(0, 120);
            std::cerr << "[Bot] Auth: " << clean << "\n";
        }
        if (!sync_sent_) {
            sync_sent_ = true;
            bot_ws_send_bytes(bot_ws_build_sync_info());
            std::cerr << "[Bot] Sync 已发送\n";
        }
        return;
    }
    
    // GetGroupMemberList 响应 → 解析并缓存成员
    if (cmd == "get_group_member_list" && cmd_type == 1) {
        std::vector<std::pair<std::string, std::string>> members;
        auto fields = proto::decode_pb_message(msg.data.data(), msg.data.size());
        for (auto& f : fields) {
            if (f.fn == 3 && !f.sub_fields.empty()) {  // repeated Member
                std::string uid, nick;
                for (auto& sf : f.sub_fields) {
                    if (sf.fn == 1) uid = sf.str_val.empty() ? std::string(sf.bytes_val.begin(), sf.bytes_val.end()) : sf.str_val;
                    if (sf.fn == 2) nick = sf.str_val.empty() ? std::string(sf.bytes_val.begin(), sf.bytes_val.end()) : sf.str_val;
                }
                if (!uid.empty()) members.push_back({uid, nick});
            }
        }
        {
            std::lock_guard<std::mutex> l(members_mu_);
            // ← 修复：按群分别缓存（切换派后不同群不互相串缓存）
            std::string g = members_request_group_;
            if (g.empty()) g = config_.default_target();
            auto& entry = members_cache_map_[g];
            entry.members = std::move(members);
            entry.fetched_at = util::now_ms();
            members_response_ = true;
            members_fetched_at_ = entry.fetched_at;
        }
        members_cv_.notify_all();
        std::lock_guard<std::mutex> l(members_mu_);
        std::cerr << "[Bot] get_group_member_list 响应(" << members_request_group_
                  << "): " << members_cache_map_[members_request_group_].members.size() << " 人\n";
        return;
    }

    // QueryGroupInfo 响应 → 解析群名与群主 user_id（与 Python _decode_query_group_info_rsp 一致）
    if (cmd == "query_group_info" && cmd_type == 1) {
        std::string owner_id;
        std::string group_name;
        auto fields = proto::decode_pb_message(msg.data.data(), msg.data.size());
        for (auto& f : fields) {
            if (f.fn == 3 && !f.sub_fields.empty()) {  // GroupInfo 嵌套消息
                for (auto& sf : f.sub_fields) {
                    if (sf.fn == 1)  // group_name
                        group_name = sf.str_val.empty() ? std::string(sf.bytes_val.begin(), sf.bytes_val.end()) : sf.str_val;
                    else if (sf.fn == 2)  // group_owner_user_id
                        owner_id = sf.str_val.empty() ? std::string(sf.bytes_val.begin(), sf.bytes_val.end()) : sf.str_val;
                }
            }
        }
        // ← 修复竞态：按 msg_id 关联到发送时登记的群号（多群并发查询不再张冠李戴）
        std::string target_group;
        bool matched = false;
        {
            std::lock_guard<std::mutex> l(members_mu_);
            auto pit = pending_query_groups_.find(msg.msg_id);
            if (pit != pending_query_groups_.end()) {
                target_group = pit->second;
                pending_query_groups_.erase(pit);
                matched = true;
            }
        }
        // ← 主群概念已移除：查询目标即默认目标群，响应直接更新群信息
        {
            std::lock_guard<std::mutex> l(members_mu_);
            if (matched) {
                // 按群更新 owner/name（写入该群的成员缓存条目，群信息与成员同群）
                auto& entry = members_cache_map_[target_group];
                if (!owner_id.empty()) entry.owner_id = owner_id;
                if (!group_name.empty()) entry.group_name = group_name;
                // 兼容旧引用：默认目标群的 owner/name 同步到旧字段
                if (target_group == config_.default_target()) {
                    if (!owner_id.empty()) members_owner_id_ = owner_id;
                    if (!group_name.empty()) members_group_name_ = group_name;
                }
            }
            if (matched && !group_name.empty())
                known_groups_[target_group] = group_name;
            group_info_response_ = true;
        }
        members_cv_.notify_all();
        if (matched) persist_known_groups();   // ← 群名更新后落盘
        std::cerr << "[Bot] query_group_info 响应" << (matched ? " (matched=" + target_group + ")" : " (未匹配)")
                  << ": name=" << (group_name.empty() ? "(无)" : group_name)
                  << " owner=" << (owner_id.empty() ? "(无)" : owner_id.substr(0, 24)) << "\n";
        return;
    }
    
    // 3. PUSH 消息（群消息/C2C 消息推送）
    // Python: data 字段是 JSON 字符串
    if (cmd_type == 2 && !msg.data.empty()) {
        std::string json_str(msg.data.begin(), msg.data.end());
        JsonVal push_json;
        if (!json_parse(json_str, push_json)) {
            std::cerr << "[Bot] PUSH JSON 解析失败: " << json_str.substr(0, 100) << "\n";
            return;
        }
        
        // 提取基本字段（与 Python sender.py 一致）
        std::string sender_name = push_json["sender_nickname"].asString();
        std::string sender_id   = push_json["from_account"].asString();
        std::string group_code  = push_json["group_code"].asString();
        std::string msg_id_s    = push_json["msg_id"].asString();

        // ← 修复：撤回事件通知（对齐 Python group_monitor.py 的 Group.CallbackAfterRecallMsg）
        //   兼容多种回调命令字段名与值，并支持缺失回调字段但含撤回列表结构的兜底判定
        bool is_recall = false;
        std::string cb = push_json["callback_command"].asString();
        if (cb.empty()) cb = push_json["CallbackCommand"].asString();
        if (cb == "Group.CallbackAfterRecallMsg" || cb == "Group.CallbackAfterRecallMsgEx"
            || cb == "Group.CallbackRecallMsg") {
            is_recall = true;
        } else if (cb.empty()
                   && (!push_json["recall_msg_seq_list"].arr.empty() || !push_json["MsgSeqList"].arr.empty())
                   && !push_json["group_code"].asString().empty()) {
            // 无回调命令字段但结构上确实是撤回事件（含撤回列表 + 群号）
            is_recall = true;
        }
        if (is_recall) {
            handle_recall_notification(push_json);
            return;
        }

        // ← 修复：多群聊监听。任何群消息都说明 bot 在该群，先把群号记入 known_groups_；
        //   群名查询统一由 /api/groups 串行处理（按 msg_id 关联），PUSH 不再发异步查询，
        //   避免 PUSH 查询与前端 /api/groups、/api/members 并发导致群名错乱
        if (!group_code.empty()) {
            bool is_new = false;
            {
                std::lock_guard<std::mutex> l(members_mu_);
                if (!known_groups_.count(group_code)) { known_groups_[group_code] = ""; is_new = true; }
            }
            if (is_new) persist_known_groups();   // ← 落盘，重启不丢失
            // 自动获取监听群：新发现的群自动加入监听列表（除非用户已手动关闭过，
            // 此时 listen_groups 非空且不含该群，保持用户选择）
            if (is_new && config_.listen_groups.empty()) {
                config_.listen_groups.push_back(group_code);
                save_config();
            }
        }
        // ← 修复：按监听群列表过滤（多群监听）
        //   群消息 group_code 非空且不在 listen_groups → 只记录群、不记录消息，避免账号所在的
        //   其他群消息被记录/触发 AI 回复（即前端"不存在的消息"的来源）
        if (!group_code.empty() && !config_.is_listening(group_code)) {
            std::cerr << "[Bot] 忽略非监听群消息 (group=" << group_code << ")\n";
            return;
        }

        // 去重
        if (!msg_id_s.empty()) {
            std::lock_guard<std::mutex> l(seen_mu_);
            if (seen_msg_ids_.count(msg_id_s)) return;
            seen_msg_ids_.insert(msg_id_s);
            if (seen_msg_ids_.size() > 2000) {
                auto it = seen_msg_ids_.begin();
                for (int i = 0; i < 500 && it != seen_msg_ids_.end(); ++i, ++it);
                seen_msg_ids_.erase(seen_msg_ids_.begin(), it);
            }
        }
        
        // 解析 msg_body（与 Python 一致）
        std::string content;
        JsonVal media_info;
        if (!push_json["msg_body"].arr.empty()) {
            for (auto& elem : push_json["msg_body"].arr) {
                std::string mt = elem["msg_type"].asString();
                if (mt == "TIMTextElem") {
                    std::string txt = elem["msg_content"]["text"].asString();
                    if (!txt.empty()) content += txt;
                } else if (mt == "TIMCustomElem") {
                    // ← 修复：对齐 Python _extract_text 的自定义元素处理。
                    //   elem_type==1002 是 @消息（取 text）；其他类型取 text/content/tips；
                    //   data 解析失败时追加原始 data 字符串——原实现直接丢弃，
                    //   导致含自定义元素的消息内容从中间丢失（"从中间被截断"）。
                    std::string ds = elem["msg_content"]["data"].asString();
                    JsonVal cd;
                    if (!ds.empty() && json_parse(ds, cd) && !cd.obj.empty()) {
                        if (cd["elem_type"].asInt() == 1002) {
                            std::string t = cd["text"].asString();
                            if (!t.empty()) content += t + " ";
                        } else {
                            std::string t = cd["text"].asString();
                            if (t.empty()) t = cd["content"].asString();
                            if (t.empty()) t = cd["tips"].asString();
                            if (!t.empty()) content += t + " ";
                        }
                    } else if (!ds.empty()) {
                        // 解析失败：保留原始 data，避免内容丢失
                        content += ds + " ";
                    }
                } else if (mt == "TIMFaceElem") {
                    // ← 补：解析贴纸 media_info（对齐 Python _extract_media_info）
                    //   贴纸信息在 msg_content.data 内嵌 JSON：{sticker_id, name, package_id}
                    std::string face_name;
                    std::string fd_str = elem["msg_content"]["data"].asString();
                    if (!fd_str.empty()) {
                        JsonVal fd;
                        if (json_parse(fd_str, fd)) {
                            face_name = fd["name"].asString();
                            if (face_name.empty()) face_name = fd["face_name"].asString();
                            media_info["type"] = "sticker";
                            media_info["sticker_id"] = fd["sticker_id"].asString();
                            media_info["sticker_name"] = face_name;
                            media_info["package_id"] = fd["package_id"].asString();
                        }
                    }
                    if (face_name.empty()) face_name = elem["msg_content"]["name"].asString();
                    content += face_name.empty() ? "[贴纸]" : ("[贴纸:" + face_name + "]");
                } else if (mt == "TIMImageElem") {
                    content += "[图片]";
                    // ← 补：解析图片媒体信息，前端可渲染缩略图（对齐 Python media_info）
                    std::string url;
                    int iw = 0, ih = 0, isz = 0;
                    for (auto& ii : elem["msg_content"]["image_info_array"].arr) {
                        std::string u = ii["url"].asString();
                        if (!u.empty()) {
                            url = u;
                            iw = ii["width"].asInt();
                            ih = ii["height"].asInt();
                            isz = ii["size"].asInt();
                        }
                    }
                    if (!url.empty()) {
                        JsonVal urls;
                        JsonVal uv; uv.v = url;
                        urls.arr.push_back(uv);
                        media_info["type"] = "image";
                        media_info["image_urls"] = urls;
                        media_info["image_uuid"] = elem["msg_content"]["uuid"].asString();
                        media_info["image_width"] = iw;
                        media_info["image_height"] = ih;
                        media_info["image_size"] = isz;
                    }
                } else if (mt == "TIMFileElem") {
                    // ← 补：解析文件 media_info（对齐 Python _extract_media_info）
                    std::string fn = elem["msg_content"]["file_name"].asString();
                    media_info["type"] = "file";
                    media_info["file_url"] = elem["msg_content"]["url"].asString();
                    media_info["file_name"] = fn;
                    media_info["file_uuid"] = elem["msg_content"]["uuid"].asString();
                    media_info["file_size"] = elem["msg_content"]["file_size"].asInt();
                    content += fn.empty() ? "[文件]" : ("[文件:" + fn + "]");
                } else if (mt == "TIMVideoFileElem") {
                    // ← 补：解析视频 media_info（对齐 Python _extract_media_info）
                    std::string vn = elem["msg_content"]["file_name"].asString();
                    media_info["type"] = "video";
                    media_info["url"] = elem["msg_content"]["url"].asString();
                    media_info["uuid"] = elem["msg_content"]["uuid"].asString();
                    media_info["size"] = elem["msg_content"]["file_size"].asInt();
                    media_info["name"] = vn;
                    content += vn.empty() ? "[视频]" : ("[视频:" + vn + "]");
                }
            }
        }
        
        // ← 补：代理转发模式（对齐 Python /auto yb on）
        //   非默认目标群的群消息转发到默认目标群（监听列表第一项，AI 图片/代理均走此群），
        //   元宝的回复按 FIFO 顺序回传到原群
        // ← 主群概念已移除：转发目标改为默认目标群（监听列表第一项）
        if (forward_enabled_ && !group_code.empty()
            && sender_id != config_.yuanbao_id
            && group_code != config_.default_target() && !content.empty()) {
            bool pass = true;
            if (forward_at_only_) {
                // 仅艾特元宝时转发
                pass = false;
                for (auto& elem : push_json["msg_body"].arr) {
                    if (elem["msg_type"].asString() != "TIMCustomElem") continue;
                    JsonVal cd;
                    if (json_parse(elem["msg_content"]["data"].asString(), cd)
                        && cd["elem_type"].asInt() == 1002
                        && cd["user_id"].asString() == config_.yuanbao_id) { pass = true; break; }
                }
            }
            if (pass) {
                std::string fwd_text = "[转发] " + sender_name + ": " + content;
                // forward_at_yuanbao_ 开启时 @元宝，确保元宝回复（对齐 Python 代理模式）
                bool ok = (forward_at_yuanbao_ && !config_.yuanbao_id.empty())
                          ? send_group_at(config_.default_target(), fwd_text, config_.yuanbao_id, "元宝")
                          : send_group_text(config_.default_target(), fwd_text);
                std::cerr << "[Forward] 转发到 " << config_.default_target()
                          << " (" << (ok ? "OK" : "FAIL") << ")\n";
                if (ok) {
                    std::lock_guard<std::mutex> l(forward_mu_);
                    forward_queue_.push_back(std::make_pair(group_code, msg_id_s));
                }
            }
        }
        // ← 补：元宝回复回传（代理转发模式，FIFO 顺序匹配）
        if (forward_enabled_ && sender_id == config_.yuanbao_id && !content.empty()) {
            std::string orig_group, ref_id;
            {
                std::lock_guard<std::mutex> l(forward_mu_);
                if (!forward_queue_.empty()) {
                    orig_group = forward_queue_.front().first;
                    ref_id = forward_queue_.front().second;
                    forward_queue_.pop_front();
                }
            }
            if (!orig_group.empty()) {
                bool ok = send_group_reply(orig_group, content, ref_id);
                std::cerr << "[Forward] 元宝回复回传 -> " << orig_group
                          << " (" << (ok ? "OK" : "FAIL") << ")\n";
            }
        }

        std::cerr << "[Bot] 收到消息: " << sender_name << "(" << sender_id 
                  << ") in " << group_code << ": " << content.substr(0, 80) << "\n";
        
        JsonVal log_entry;
        std::string ts_str = util::time_iso();
        log_entry["msg_id"] = msg_id_s;
        // ← 修复：缓存 msg_seq（撤回匹配关键字段；兼容大写 MsgSeq，如腾讯云标准）
        int msg_seq = push_json["msg_seq"].asInt();
        if (msg_seq <= 0) msg_seq = push_json["MsgSeq"].asInt();
        if (msg_seq > 0) log_entry["msg_seq"] = msg_seq;
        log_entry["timestamp"] = ts_str;   // 日志用
        log_entry["time"] = ts_str;        // 前端 renderMessages 读 m.time
        log_entry["cmd"] = cmd; log_entry["sender_name"] = sender_name;
        log_entry["sender_id"] = sender_id; log_entry["group_code"] = group_code;
        log_entry["content"] = content;
        // ← 修复：标识私聊消息（group_code 为空 = C2C 私聊），记录对方 ID 供前端按人过滤历史
        bool is_c2c = group_code.empty();
        if (is_c2c) { log_entry["is_c2c"] = true; log_entry["c2c_peer"] = sender_id; }
        if (!media_info.obj.empty()) log_entry["media_info"] = media_info;
        msg_logger_.log(log_entry); cache_message(log_entry);
        
        JsonVal fe;
        fe["type"] = "message"; fe["msg_id"] = msg_id_s;
        fe["sender"] = sender_name; fe["sender_id"] = sender_id;
        fe["group"] = group_code; fe["content"] = content;
        fe["time"] = (double)util::now_ms();
        push_to_frontend("bot_message", fe);
        // ← 补：插件命令匹配（JSON 配置插件，命令优先于 AI 回复）
        if (sender_id != config_.yuanbao_id && !content.empty()) {
            if (handle_plugin_command(group_code, content)) return;
        }
        if (config_.llm.enabled && !content.empty()) process_llm_reply(log_entry);
        return;
    }
    
    // 4. 其他消息（日志）
    if (!msg.data.empty()) {
        std::string ds(msg.data.begin(), msg.data.end());
        std::cerr << "[Bot] msg: cmd=" << cmd << " module=" << msg.module 
                  << " type=" << cmd_type << " data=" << ds.substr(0, 100) << "\n";
    }
}

// ← 修复：撤回事件通知（对齐 Python group_monitor.py _send_recall_notification）
//   兼容多种字段名：群号 group_code/GroupId、撤回者 sender_nickname/Operator_Account、
//   列表 recall_msg_seq_list/MsgSeqList、条目 msg_seq/MsgSeq、msg_id/MsgId。
//   撤回的消息若是图片则尝试补发原图，其余情况发送文字通知（含 fence 防渲染）
void YuanbaoServer::handle_recall_notification(const JsonVal& push_json) {
    if (!config_.recall_monitor_enabled) {
        std::cerr << "[Bot] 撤回事件已忽略（撤回监控开关未开启）\n";
        return;
    }
    std::string group_code = push_json["group_code"].asString();
    if (group_code.empty()) group_code = push_json["GroupId"].asString();
    if (group_code.empty()) {
        std::cerr << "[Bot] 撤回事件缺少群号 group_code/GroupId\n";
        return;
    }
    std::string operator_name = push_json["sender_nickname"].asString();
    if (operator_name.empty()) operator_name = push_json["Operator_Account"].asString();
    if (operator_name.empty()) operator_name = push_json["from_account"].asString();

    const JsonVal* list = nullptr;
    if (!push_json["recall_msg_seq_list"].arr.empty()) list = &push_json["recall_msg_seq_list"];
    else if (!push_json["MsgSeqList"].arr.empty()) list = &push_json["MsgSeqList"];
    if (!list) {
        std::cerr << "[Bot] 撤回事件缺少 recall_msg_seq_list/MsgSeqList\n";
        return;
    }
    std::cerr << "[Bot] 撤回事件: group=" << group_code << " 撤回者=" << operator_name
              << " 条数=" << list->arr.size() << "\n";

    for (auto& item : list->arr) {
        std::string recalled_id = item["msg_id"].asString();
        if (recalled_id.empty()) recalled_id = item["MsgId"].asString();
        int64_t recalled_seq = item["msg_seq"].asInt();
        if (recalled_seq <= 0) recalled_seq = item["MsgSeq"].asInt();

        // 从消息缓存找回原消息（含 media_info）——按群 + msg_id/msg_seq 双条件匹配
        JsonVal orig;
        bool found = false;
        {
            std::lock_guard<std::mutex> l(msg_mu_);
            for (auto& m : msg_cache_) {
                if (m["group_code"].asString() != group_code) continue;
                if (!recalled_id.empty() && m["msg_id"].asString() == recalled_id) { orig = m; found = true; break; }
                if (recalled_seq > 0 && m["msg_seq"].asInt() == recalled_seq) { orig = m; found = true; break; }
            }
        }

        std::string notif;
        if (found) {
            std::string orig_content = orig["content"].asString();
            std::string orig_sender = orig["sender_name"].asString();
            if (orig_sender.empty()) orig_sender = orig["sender_id"].asString();
            std::string orig_time = orig["time"].asString();
            notif = "—— 撤回通知 ——\n"
                    "撤回者: " + operator_name + "\n"
                    "原发送者: " + orig_sender + "\n"
                    "发送时间: " + orig_time;
            // 原内容用代码块 fence 包裹防渲染（对齐 group_monitor.py）
            size_t max_bt = 0, bcount = 0;
            for (char ch : orig_content) {
                if (ch == '`') { bcount++; if (bcount > max_bt) max_bt = bcount; }
                else bcount = 0;
            }
            std::string fence = (max_bt >= 3) ? std::string(max_bt + 1, '`') : "```";
            notif += "\n原内容:\n" + fence + "原内容\n" + orig_content + "\n" + fence;

            // 图片被撤回：尝试补发原图（对齐 Python 自动补发）
            const JsonVal& mi = orig["media_info"];
            if (mi["type"].asString() == "image" && !mi["image_urls"].arr.empty()) {
                std::string url = mi["image_urls"].arr[0].asString();
                if (!url.empty()) {
                    bool ok = send_group_image(group_code, url,
                                               mi["image_uuid"].asString(),
                                               mi["image_size"].asInt(),
                                               mi["image_width"].asInt(),
                                               mi["image_height"].asInt());
                    std::cerr << "[Bot] 撤回图片补发: " << (ok ? "OK" : "FAIL") << "\n";
                }
            }
        } else {
            notif = "—— 撤回通知 ——\n撤回了消息\n原内容: [未找到已缓存的消息]";
        }
        bool ok = send_group_text(group_code, notif);
        std::cerr << "[Bot] 撤回通知发送: " << (ok ? "OK" : "FAIL") << "\n";
    }
}

// ═══════════════════════════════════════════════
//  插件系统（JSON 配置插件：plugins/*.json）
// ═══════════════════════════════════════════════
void YuanbaoServer::load_plugins() {
    std::vector<PluginInfo> loaded;
    namespace fs = std::filesystem;
    try {
        if (!fs::exists("plugins")) fs::create_directory("plugins");
        for (auto& entry : fs::directory_iterator("plugins")) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".json") continue;
            std::string path = entry.path().string();
            std::ifstream ifs(path);
            if (!ifs) continue;
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            JsonVal pj;
            if (!json_parse(content, pj)) {
                PluginInfo bad;
                bad.name = entry.path().stem().string();
                bad.error = "JSON 解析失败: " + path;
                loaded.push_back(bad);
                continue;
            }
            PluginInfo pi;
            pi.name = pj["name"].asString();
            if (pi.name.empty()) pi.name = entry.path().stem().string();
            pi.version = pj["version"].asString();
            pi.author = pj["author"].asString();
            pi.description = pj["description"].asString();
            pi.active = (pj["active"].is_bool() && pj["active"].asBool())
                        || (!pj["active"].is_bool() && pj["active"].asString() == "true");
            for (auto& c : pj["commands"].arr) {
                PluginCommand pc;
                pc.command = c["command"].asString();
                pc.reply = c["reply"].asString();
                if (!pc.command.empty()) pi.commands.push_back(pc);
            }
            if (pi.commands.empty()) pi.error = "无可用命令（commands 为空）";
            loaded.push_back(pi);
        }
    } catch (const std::exception& e) {
        std::cerr << "[插件] 扫描 plugins/ 异常: " << e.what() << "\n";
    }
    {
        std::lock_guard<std::mutex> l(plugins_mu_);
        plugins_ = std::move(loaded);
    }
    std::cerr << "[插件] 加载完成，共 " << plugins_.size() << " 个插件\n";
}

bool YuanbaoServer::save_plugin(const PluginInfo& p) {
    try {
        namespace fs = std::filesystem;
        if (!fs::exists("plugins")) fs::create_directory("plugins");
        std::ostringstream o;
        o << "{\n";
        o << "  \"name\": " << json_quote(p.name) << ",\n";
        o << "  \"version\": " << json_quote(p.version.empty() ? "1.0.0" : p.version) << ",\n";
        o << "  \"author\": " << json_quote(p.author) << ",\n";
        o << "  \"description\": " << json_quote(p.description) << ",\n";
        o << "  \"active\": " << (p.active ? "true" : "false") << ",\n";
        o << "  \"commands\": [\n";
        for (size_t i = 0; i < p.commands.size(); i++) {
            o << "    {\"command\": " << json_quote(p.commands[i].command)
              << ", \"reply\": " << json_quote(p.commands[i].reply) << "}";
            if (i + 1 < p.commands.size()) o << ",";
            o << "\n";
        }
        o << "  ]\n}\n";
        std::ofstream ofs("plugins/" + p.name + ".json");
        if (!ofs) return false;
        ofs << o.str();
        return true;
    } catch (...) { return false; }
}

// 命令匹配：content 以某插件命令前缀开头 → 发送回复。返回是否已处理
bool YuanbaoServer::handle_plugin_command(const std::string& group, const std::string& content) {
    if (group.empty() || content.empty()) return false;
    std::vector<PluginInfo> snapshot;
    {
        std::lock_guard<std::mutex> l(plugins_mu_);
        snapshot = plugins_;
    }
    for (auto& p : snapshot) {
        if (!p.active || p.commands.empty()) continue;
        for (auto& c : p.commands) {
            if (c.command.empty()) continue;
            // 前缀匹配（含纯命令匹配：去掉可能的 @ 等前缀）
            std::string msg = content;
            // 去掉开头的 @昵称 片段再匹配（如 "@元宝 /ping"）
            if (msg.rfind("@", 0) == 0) {
                size_t sp = msg.find(' ');
                if (sp != std::string::npos) msg = msg.substr(sp + 1);
            }
            if (msg == c.command || msg.rfind(c.command, 0) == 0) {
                std::string reply = c.reply;
                // {user} 占位 = 发送者昵称
                size_t u = reply.find("{user}");
                if (u != std::string::npos) {
                    // 需要发送者昵称：简化处理，用「你」代替
                    reply.replace(u, 6, "你");
                }
                bool ok = send_group_text(group, reply);
                std::cerr << "[插件] " << p.name << " 命令 " << c.command
                          << " -> 回复 (" << (ok ? "OK" : "FAIL") << ")\n";
                return true;
            }
        }
    }
    return false;
}

void YuanbaoServer::process_llm_reply(const JsonVal& msg) {
    std::string content = msg["content"].asString();
    std::string sender_id = msg["sender_id"].asString();
    std::string sender_name = msg["sender_name"].asString();
    std::string group_code = msg["group_code"].asString();
    bool is_c2c = msg["is_c2c"].asBool();

    if (content.empty()) return;
    if (sender_id == config_.yuanbao_id) return;

    // 判断是否需要回复
    bool should_reply = false;
    if (is_c2c) {
        should_reply = true;  // 私聊总是回复
    } else {
        // 群聊：@机器人 或 提到关键词才回复
        bool at_me = content.find("@" + config_.yuanbao_id) != std::string::npos ||
                     content.find("元宝") != std::string::npos ||
                     content.find("机器人") != std::string::npos;
        should_reply = at_me;
    }

    if (!should_reply) return;

    // 使用大模型 API 生成回复
    std::cout << "[LLM] 调用大模型生成回复...\n";
    std::string reply_text = call_llm_api(content, sender_name);
    if (reply_text.empty()) {
        std::cerr << "[LLM] 大模型返回空，本次不回复\n";
        return;
    }

    // 截断过长的回复（QQ 群消息限制约 4500 字符）
    if (reply_text.size() > 2000) {
        reply_text = reply_text.substr(0, 1997) + "...";
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300 + rand() % 500));
    bool ok = false;
    if (is_c2c) ok = send_c2c_text(sender_id, reply_text);
    else if (!group_code.empty()) ok = send_group_text(group_code, reply_text);
    std::cout << "[LLM] -> " << (is_c2c ? sender_id : group_code)
              << " reply: " << reply_text.substr(0, 80) << " " << (ok ? "OK" : "FAIL") << "\n";
}

// =====================================================================
//  大模型 API 调用
// =====================================================================
std::string YuanbaoServer::call_llm_api(const std::string& user_message, const std::string& sender_name) {
    if (config_.llm.api_url.empty() || config_.llm.api_key.empty()) {
        std::cerr << "[LLM] API 未配置\n";
        return "";
    }

    // 构建请求体 (OpenAI 兼容格式)
    std::ostringstream body;
    body << "{"
         << "\"model\":\"" << config_.llm.model << "\","
         << "\"messages\":["
         << "{\"role\":\"system\",\"content\":\"";

    // 转义系统提示词中的特殊字符
    std::string escaped_prompt;
    for (char c : config_.llm.system_prompt) {
        if (c == '"') escaped_prompt += "\\\"";
        else if (c == '\\') escaped_prompt += "\\\\";
        else if (c == '\n') escaped_prompt += "\\n";
        else if (c == '\r') escaped_prompt += "\\r";
        else if (c == '\t') escaped_prompt += "\\t";
        else escaped_prompt += c;
    }
    body << escaped_prompt << "\"},";

    // 用户消息
    std::string escaped_msg;
    for (char c : user_message) {
        if (c == '"') escaped_msg += "\\\"";
        else if (c == '\\') escaped_msg += "\\\\";
        else if (c == '\n') escaped_msg += "\\n";
        else if (c == '\r') escaped_msg += "\\r";
        else if (c == '\t') escaped_msg += "\\t";
        else escaped_msg += c;
    }
    body << "{\"role\":\"user\",\"content\":\"" << escaped_msg << "\"}],"
         << "\"max_tokens\":" << config_.llm.max_tokens << ","
         << "\"temperature\":" << config_.llm.temperature
         << "}";

    std::string body_str = body.str();
    std::cout << "[LLM] 请求模型: " << config_.llm.model
              << " max_tokens=" << config_.llm.max_tokens
              << " temp=" << config_.llm.temperature << "\n";

    // 发送 HTTPS 请求
    std::string resp_body;

#ifdef YUANBAO_NO_OPENSSL
    // 无 OpenSSL 时使用 curl 命令行
    {
        std::ostringstream cmd;
        cmd << "curl -s -k -X POST \"" << config_.llm.api_url << "\""
            << " -H \"Content-Type: application/json\""
            << " -H \"Authorization: Bearer " << config_.llm.api_key << "\""
            << " --max-time " << config_.llm.timeout_sec
            << " -d '" << body_str << "'";
        FILE* fp = popen(cmd.str().c_str(), "r");
        if (!fp) {
            std::cerr << "[LLM] curl 执行失败\n";
            return "";
        }
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        pclose(fp);
        resp_body = std::string(buf, n);
    }
#else
    {
        // 解析 API URL
        std::string url = config_.llm.api_url;
        std::string host, path = "/";
        int port = 443;

        if (url.find("https://") == 0) url = url.substr(8);
        else if (url.find("http://") == 0) { url = url.substr(7); port = 80; }

        size_t slash = url.find('/');
        if (slash != std::string::npos) {
            host = url.substr(0, slash);
            path = url.substr(slash);
        } else {
            host = url;
        }

        size_t colon = host.find(':');
        if (colon != std::string::npos) {
            port = std::stoi(host.substr(colon + 1));
            host = host.substr(0, colon);
        }

        std::map<std::string, std::string> headers;
        headers["Authorization"] = "Bearer " + config_.llm.api_key;
        int timeout_ms = config_.llm.timeout_sec * 1000;

        int ret = https_post(host, port, path, body_str, headers, resp_body, timeout_ms);
        if (ret != 0) {
            std::cerr << "[LLM] HTTPS 请求失败 ret=" << ret << "\n";
            return "";
        }
    }
#endif

    // 解析响应
    JsonVal root;
    if (!json_parse(resp_body, root)) {
        std::cerr << "[LLM] JSON 解析失败: " << resp_body.substr(0, 200) << "\n";
        return "";
    }

    // 检查错误
    if (!root["error"]["message"].asString().empty()) {
        std::cerr << "[LLM] API 错误: " << root["error"]["message"].asString() << "\n";
        return "";
    }

    // 提取回复内容 (OpenAI 格式: choices[0].message.content)
    if (!root["choices"].arr.empty()) {
        std::string reply = root["choices"][0]["message"]["content"].asString();
        if (!reply.empty()) {
            // 去除首尾空白
            while (!reply.empty() && (reply.front() == ' ' || reply.front() == '\n')) reply.erase(0, 1);
            while (!reply.empty() && (reply.back() == ' ' || reply.back() == '\n')) reply.pop_back();
            std::cout << "[LLM] 回复: " << reply.substr(0, 100) << "\n";
            return reply;
        }
    }

    std::cerr << "[LLM] 响应中未找到有效回复\n";
    return "";
}

bool YuanbaoServer::bot_connect() {
#ifdef YUANBAO_NO_OPENSSL
    std::cerr << "[Bot] ========================================\n";
    std::cerr << "[Bot] YUANBAO_NO_OPENSSL 模式: WSS 不可用\n";
    std::cerr << "[Bot] Bot 连接功能需要 OpenSSL 支持\n";
    std::cerr << "[Bot] 请安装 OpenSSL 后重新编译:\n";
    std::cerr << "[Bot]   Linux:   sudo apt install libssl-dev\n";
    std::cerr << "[Bot]   macOS:   brew install openssl\n";
    std::cerr << "[Bot]   MSYS2:   pacman -S mingw-w64-x86_64-openssl\n";
    std::cerr << "[Bot]   MinGW:   下载 winlibs 带 OpenSSL 版本\n";
    std::cerr << "[Bot] 前端控制台和 HTTP API 仍然可用\n";
    std::cerr << "[Bot] ========================================\n";
    return false;
#else
    // ← 修复：若连接线程已在运行且未主动断开，直接复用，避免重复调用 /api/connect
    //   （页面刷新/重复点连接）时杀旧线程 → 重建 → 再杀的循环，该循环造成 Bot
    //   反复断线重连（SSL 错误 5）与高频查询，是卡顿的重要来源
    if (bot_running_.load()) return true;
    bot_running_ = true;
    sync_sent_ = false;
    bot_ws_thread_ = std::thread([this]() {
        try {
        int retry = 0;
        while (bot_running_.load()) {
            sync_sent_ = false;  // 每次重试重置
            if (!bot_sign_token()) {
                std::cerr << "[Bot] 鉴权失败，等待重试...\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
                retry++;
                continue;
            }
            if (!bot_ws_connect()) {
                std::cerr << "[Bot] WebSocket 连接失败，等待重试...\n";
                std::this_thread::sleep_for(std::chrono::seconds(5));
                retry++;
                continue;
            }
            
            // 发送认证消息
            auto auth_frame = bot_ws_build_auth();
            std::cerr << "[Bot] 发送 Auth 帧, 长度=" << auth_frame.size() << "\n";
            std::cerr << "[Bot] Auth 帧 HEX(前100): ";
            for (size_t i = 0; i < std::min((size_t)100, auth_frame.size()); i++)
                fprintf(stderr, "%02x", auth_frame[i]);
            fprintf(stderr, "\n");
            if (!bot_ws_send_bytes(auth_frame)) {
                std::cerr << "[Bot] Auth 消息发送失败!\n";
                bot_connected_ = false;
                std::this_thread::sleep_for(std::chrono::seconds(2));
                retry++;
                continue;
            }
            std::cerr << "[Bot] Auth 消息已发送\n";
            
            // 标记已连接，进入接收循环（先不加 sync，测试连接稳定性）
            bot_connected_ = true;
            std::cerr << "[Bot] 进入消息接收循环...\n";
            bot_ws_recv_loop();
            
            // 接收循环退出 = 连接断开，尝试重连
            std::cerr << "[Bot] 连接断开，尝试重连...\n";
            bot_connected_ = false;
#ifndef YUANBAO_NO_OPENSSL
            if (bot_ws_ssl_) { SSL_shutdown(bot_ws_ssl_); SSL_free(bot_ws_ssl_); bot_ws_ssl_ = nullptr; }
            if (bot_ws_ssl_ctx_) { SSL_CTX_free(bot_ws_ssl_ctx_); bot_ws_ssl_ctx_ = nullptr; }
#endif
            if (bot_ws_fd_ >= 0) { close_socket(bot_ws_fd_); bot_ws_fd_ = -1; }
            retry++;
            // 指数退避重试（上限 60s），永不放弃，保证 Bot 断线后自动恢复
            int delay = (retry <= 5) ? (1 << (retry - 1)) : (retry > 20 ? 60 : 16);  // 1,2,4,8,16,16...60
            std::cerr << "[Bot] " << delay << "s 后重试 (第" << retry << "次)...\n";
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            continue;
        }
        std::cerr << "[Bot] connect 线程退出\n";
        } catch (const std::exception& e) {
            std::cerr << "[Bot] connect 线程异常: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[Bot] connect 线程未知异常\n";
        }
    });
    return true;
#endif
}

void YuanbaoServer::bot_disconnect() {
    bot_running_ = false;
    // 先关闭 socket，让 SSL_read/recv 立即返回错误
    if (bot_ws_fd_ >= 0) {
        close_socket(bot_ws_fd_);
        bot_ws_fd_ = -1;
    }
#ifndef YUANBAO_NO_OPENSSL
    if (bot_ws_ssl_) { SSL_free(bot_ws_ssl_); bot_ws_ssl_ = nullptr; }
    if (bot_ws_ssl_ctx_) { SSL_CTX_free(bot_ws_ssl_ctx_); bot_ws_ssl_ctx_ = nullptr; }
#endif
    // 等待线程退出（socket 已关闭，recv loop 会立即退出）
    if (bot_ws_thread_.joinable()) {
        auto start = std::chrono::steady_clock::now();
        while (bot_ws_thread_.joinable()) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(3)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (bot_ws_thread_.joinable()) bot_ws_thread_.detach();
    }
    bot_connected_ = false;
    client_token_.clear();
}

// =====================================================================
//  保存配置
// =====================================================================
void YuanbaoServer::save_config() {
    std::ostringstream o;
    o << "{\n"
      << "  \"PORT\": " << config_.port << ",\n"
      << "  \"APP_KEY\": \"" << config_.app_key << "\",\n"
      << "  \"APP_SECRET\": \"" << config_.app_secret << "\",\n"
      << "  \"API_DOMAIN\": \"" << config_.api_domain << "\",\n"
      << "  \"WS_URL\": \"" << config_.ws_url << "\",\n"
      << "  \"LISTEN_GROUPS\": [";
    for (size_t i = 0; i < config_.listen_groups.size(); i++) {
        if (i) o << ",";
        o << "\"" << config_.listen_groups[i] << "\"";
    }
    o << "],\n"
      << "  \"YUANBAO_ID\": \"" << config_.yuanbao_id << "\",\n"
      << "  \"HEARTBEAT_INTERVAL\": " << config_.heartbeat_interval << ",\n"
      << "  \"LLM_API_URL\": " << json_quote(config_.llm.api_url) << ",\n"
      << "  \"LLM_API_KEY\": " << json_quote(config_.llm.api_key) << ",\n"
      << "  \"LLM_MODEL\": " << json_quote(config_.llm.model) << ",\n"
      << "  \"LLM_SYSTEM_PROMPT\": " << json_quote(config_.llm.system_prompt) << ",\n"
      << "  \"LLM_MAX_TOKENS\": " << config_.llm.max_tokens << ",\n"
      << "  \"LLM_TEMPERATURE\": " << config_.llm.temperature << ",\n"
      << "  \"LLM_TIMEOUT\": " << config_.llm.timeout_sec << ",\n"
      << "  \"FORWARD_MODE_ENABLED\": " << (forward_enabled_ ? "true" : "false") << ",\n"
      << "  \"FORWARD_AT_ONLY\": " << (forward_at_only_ ? "true" : "false") << ",\n"
      << "  \"FORWARD_AT_YUANBAO\": " << (forward_at_yuanbao_ ? "true" : "false") << ",\n"
      << "  \"MSG_LOG_ENABLED\": " << (config_.msg_log_enabled ? "true" : "false") << ",\n"
      << "  \"RECALL_MONITOR_ENABLED\": " << (config_.recall_monitor_enabled ? "true" : "false") << "\n"
      << "}\n";
    std::ofstream f("config.json");
    if (f.is_open()) { f << o.str(); f.close(); }
}

// =====================================================================
//  多群列表持久化（重启后不丢失）
// =====================================================================
void YuanbaoServer::persist_known_groups() {
    std::lock_guard<std::mutex> l(members_mu_);
    std::ostringstream o;
    o << "{\"groups\":[";
    bool first = true;
    for (auto& kv : known_groups_) {
        if (!first) o << ","; first = false;
        o << "{\"group_code\":\"" << kv.first << "\",\"group_name\":"
          << json_quote(kv.second) << "}";
    }
    o << "]}";
    std::ofstream f("logs/known_groups.json");
    if (f.is_open()) { f << o.str(); f.close(); }
}

void YuanbaoServer::load_known_groups() {
    std::ifstream f("logs/known_groups.json");
    if (!f.is_open()) return;
    std::stringstream ss; ss << f.rdbuf();
    JsonVal v;
    if (!json_parse(ss.str(), v)) return;
    std::lock_guard<std::mutex> l(members_mu_);
    for (auto& g : v["groups"].arr) {
        std::string gc = g["group_code"].asString();
        if (gc.empty()) continue;
        auto it = known_groups_.find(gc);
        if (it == known_groups_.end()) {
            known_groups_[gc] = g["group_name"].asString();
        } else if (it->second.empty()) {
            it->second = g["group_name"].asString();
        }
    }
}

// 自动获取监听群：不依赖 config 硬编码。
// 仅当 listen_groups 为空（用户未手动配置）时，自动把 bot 已加入的所有群设为监听；
// 一旦用户手动开关过（listen_groups 非空），保持用户的监听选择，不再全量覆盖。
void YuanbaoServer::auto_listen_from_known() {
    bool modified = false;
    {
        std::lock_guard<std::mutex> l(members_mu_);
        if (config_.listen_groups.empty()) {
            for (auto& kv : known_groups_) {
                if (!config_.is_listening(kv.first)) {
                    config_.listen_groups.push_back(kv.first);
                    modified = true;
                }
            }
        }
    }
    if (modified) save_config();
}

// =====================================================================
//  @全体成员 发送
// =====================================================================
bool YuanbaoServer::send_group_at_all(const std::string& group) {
    std::string text = "@\xE5\x85\xA8\xE4\xBD\x93\xE6\x88\x90\xE5\x91\x98"; // UTF-8 @全体成员
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body;
    bytes_append(body, proto::encode_string(1, body_msg_id));
    bytes_append(body, proto::encode_string(2, group));
    bytes_append(body, proto::encode_string(3, config_.yuanbao_id));
    bytes_append(body, proto::encode_string(4, ""));
    bytes_append(body, proto::encode_string(5, std::to_string((uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 4294967295))));
    // @全体消息需要特殊 at 标记
    Bytes at_elem;
    bytes_append(at_elem, proto::encode_string(1, "TIMTextElem"));
    Bytes at_content;
    bytes_append(at_content, proto::encode_string(1, text));
    bytes_append(at_content, proto::encode_string(2, "@all"));  // at 标记
    bytes_append(at_elem, proto::encode_message(2, at_content));
    bytes_append(body, proto::encode_message(6, at_elem));
    bytes_append(body, proto::encode_string(7, ""));
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    auto frame = proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
    return bot_ws_send_bytes(frame);
}

// =====================================================================
//  引用回复
// =====================================================================
bool YuanbaoServer::send_group_reply(const std::string& group, const std::string& text, const std::string& ref_msg_id,
                                     const std::string& at_user, const std::string& at_nick) {
    std::string body_msg_id = util::random_hex_id();
    std::string head_msg_id = util::random_hex_id();
    Bytes body = proto::encode_send_group_msg(body_msg_id, group, config_.yuanbao_id, text, ref_msg_id,
                                              at_user, at_nick);
    Bytes head = proto::encode_conn_head(0, "send_group_message", ++bot_seq_no_, head_msg_id, "yuanbao_openclaw_proxy");
    auto frame = proto::build_ws_frame(2, proto::encode_conn_msg(head, body));
    return bot_ws_send_bytes(frame);
}

// =====================================================================
//  消息缓存
// =====================================================================
void YuanbaoServer::cache_message(const JsonVal& msg) {
    std::lock_guard<std::mutex> l(msg_mu_);
    msg_cache_.push_back(msg);
    while (msg_cache_.size() > MAX_MSG_CACHE) msg_cache_.erase(msg_cache_.begin());
}

// =====================================================================
//  前端 WebSocket 广播
// =====================================================================
void YuanbaoServer::push_to_frontend(const std::string& event, const JsonVal& data) {
    std::ostringstream ss;
    ss << "event: " << event << "\ndata: " << json_compact(data) << "\n\n";
    broadcast_fe(ss.str());
}

void YuanbaoServer::broadcast_fe(const std::string& msg) {
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> l(fe_ws_mu_);
        fds.assign(fe_ws_fds_.begin(), fe_ws_fds_.end());
    }
    auto frame = proto::build_ws_frame(1, Bytes(msg.begin(), msg.end()));
    for (int fd : fds) {
        YB_SEND(fd, frame.data(), frame.size(), 0);
    }
}

void YuanbaoServer::add_fe_ws(int fd) {
    std::lock_guard<std::mutex> l(fe_ws_mu_);
    fe_ws_fds_.insert(fd);
}

void YuanbaoServer::remove_fe_ws(int fd) {
    std::lock_guard<std::mutex> l(fe_ws_mu_);
    fe_ws_fds_.erase(fd);
}

// =====================================================================
//  静态文件路径解析
// =====================================================================
std::string YuanbaoServer::resolve_path(const std::string& url_path) {
    std::string path = url_path;
    if (path == "/" || path.empty()) path = "/index.html";

    // 去掉 /html/, /css/, /js/ 等虚拟前缀
    if (path.rfind("/html/", 0) == 0) path = path.substr(5);
    else if (path.rfind("/css/", 0) == 0) path = path.substr(4);
    else if (path.rfind("/js/", 0) == 0) path = path.substr(3);

    // 安全检查
    if (path.find("..") != std::string::npos) return "";

    // 构建实际文件路径
    if (path[0] == '/') path = path.substr(1);

    static const std::map<std::string, std::string> aliases = {
        {"index.html", "index.html"},
        {"css/style.css", "style.css"},
        {"js/app.js", "app.js"},
        {"config.json", "config.json"},
    };

    auto it = aliases.find(path);
    if (it != aliases.end()) return it->second;

    return path;
}

// =====================================================================
//  API 路由
// =====================================================================
HttpResponse YuanbaoServer::route_api(const HttpRequest& req) {
    const std::string& path = req.path;

    // 连接管理
    if (path == "/api/connect") {
        bool ok = bot_connect();
        // 前端 connect() 用 r.ok 判断成功（与 /api/send 等保持一致）
        return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") +
            ",\"connected\":" + std::string(ok ? "true" : "false") + "}");
    }
    if (path == "/api/disconnect") {
        bot_disconnect();
        return HttpResponse().json("{\"ok\":true}");
    }
    if (path == "/api/bot_status") {
        std::ostringstream o;
        o << "{\"connected\":" << (bot_connected_.load() ? "true" : "false")
          << ",\"token\":\"" << (client_token_.empty() ? "" : client_token_.substr(0, 8) + "...")
          << "\",\"seq\":" << bot_seq_no_ << "}";
        return HttpResponse().json(o.str());
    }

    // ← 修复：图片代理接口。resourceUrl（hunyuan.tencent.com/api/resource/download?resourceId=...）
    //   需要 X-Token 鉴权，浏览器 <img> 无法带该头 → 前端图片 404。
    //   由后端带 X-Token 拉取图片字节返回，前端 <img src="/api/image-proxy?resourceId=...">
    if (path == "/api/image-proxy") {
        std::string rid;
        auto rq = req.query_params.find("resourceId");
        if (rq != req.query_params.end()) rid = rq->second;
        if (rid.empty()) {
            // 兼容完整 url 参数
            auto uq = req.query_params.find("url");
            std::string u = (uq != req.query_params.end()) ? uq->second : "";
            size_t pos = u.find("resourceId=");
            if (pos != std::string::npos) {
                rid = u.substr(pos + 11);
                size_t amp = rid.find('&');
                if (amp != std::string::npos) rid = rid.substr(0, amp);
            }
        }
        if (rid.empty()) return HttpResponse().json("{\"error\":\"resourceId required\"}");
        // 解析 API 域名（host/port）
        std::string host = config_.api_domain, api_path;
        int port = 443;
        {
            std::string d = config_.api_domain;
            size_t sl = d.find("://");
            if (sl != std::string::npos) d = d.substr(sl + 3);
            size_t cl = d.find(':');
            if (cl != std::string::npos) { port = atoi(d.substr(cl + 1).c_str()); d = d.substr(0, cl); }
            size_t psl = d.find('/');
            if (psl != std::string::npos) d = d.substr(0, psl);
            host = d;
        }
        std::map<std::string, std::string> hd;
        hd["X-ID"] = config_.yuanbao_id;
        hd["X-Token"] = client_token_;
        hd["X-Source"] = "web";
        hd["X-AppVersion"] = "1.0.11";
        hd["X-OperationSystem"] = "linux";
        hd["X-Instance-Id"] = "99";
        hd["X-Bot-Version"] = "2026.3.22";
        std::string resp;
        // ← 修复：优先用上传时保存的 COS 凭证构造 GET 签名 URL（key 正确 = 我们上传的 location）。
        //   v1/download 返回的 realUrl key 与我们上传的不一致（NoSuchKey），不可用。
        int ret = -1;
        {
            std::lock_guard<std::mutex> l(members_mu_);
            if (!cos_upload_secret_key_.empty() && !cos_upload_location_.empty()
                && (cos_upload_expired_ == 0 || cos_upload_expired_ > (int64_t)(util::now_ms() / 1000))) {
                std::string cos_host = cos_upload_bucket_ + ".cos." + cos_upload_region_ + ".myqcloud.com";
                std::string sign_key = hmac_sha1_hex(cos_upload_secret_key_, cos_upload_key_time_);
                std::string http_string = "get\n" + cos_upload_location_ + "\n\nhost=" + cos_host + "\n";
                std::string sts = "sha1\n" + cos_upload_key_time_ + "\n" + sha1_hex(http_string) + "\n";
                std::string signature = hmac_sha1_hex(sign_key, sts);
                // ← 修复：签名参数放 Authorization 头（URL 不带 query），与上传一致。
                //   GET 请求 q-url-param-list 必须为空，URL 不能带签名 query 参数
                std::string authorization =
                    "q-sign-algorithm=sha1&q-ak=" + cos_upload_secret_id_ +
                    "&q-sign-time=" + cos_upload_key_time_ +
                    "&q-key-time=" + cos_upload_key_time_ +
                    "&q-header-list=host&q-url-param-list=" +
                    "&q-signature=" + signature;
                std::map<std::string, std::string> ch;
                ch["Authorization"] = authorization;
                if (!cos_upload_token_.empty()) ch["x-cos-security-token"] = cos_upload_token_;
                ret = https_request("GET", cos_host, 443, cos_upload_location_,
                                    "", "", ch, resp, 30000);
                if (ret != 0 || resp.empty()) ret = -1;
            }
        }
        // 兜底：v1/download（用于 _90 域 QQ 客户端图片）
        std::string dl;
        if (ret != 0) {
            ret = https_request("GET", host, port, "/api/resource/v1/download?resourceId=" + rid,
                                "", "", hd, dl, 30000);
        }
        if (ret == 0 && !dl.empty()) {
            JsonVal j;
            if (json_parse(dl, j)) {
                std::string real = j["realUrl"].asString();
                if (real.empty()) real = j["url"].asString();
                JsonVal dj = j["data"];
                if (real.empty() && !dj.empty()) {
                    real = dj["url"].asString();
                    if (real.empty()) real = dj["realUrl"].asString();
                }
                if (real.size() > 10) {
                    // 从 realUrl 解析 host/path 并拉取（COS 直链）
                    std::string real_host, real_path;
                    size_t sp = real.find("://");
                    if (sp != std::string::npos) {
                        std::string rest = real.substr(sp + 3);
                        size_t slash = rest.find('/');
                        real_host = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
                        real_path = (slash != std::string::npos) ? rest.substr(slash) : "/";
                    }
                    if (!real_host.empty())
                        ret = https_request("GET", real_host, 443, real_path, "", "",
                                            {}, resp, 30000);
                }
            }
        }
        if (ret != 0 || resp.empty()) return HttpResponse().json("{\"error\":\"fetch failed\"}");
        HttpResponse res;
        res.status = 200;
        // 简单判断图片类型
        std::string ct = "image/jpeg";
        if (resp.size() >= 8 && (uint8_t)resp[0] == 0x89 && (uint8_t)resp[1] == 0x50) ct = "image/png";
        else if (resp.size() >= 3 && resp.compare(0, 3, "GIF") == 0) ct = "image/gif";
        res.headers["Content-Type"] = ct;
        res.headers["Cache-Control"] = "public, max-age=86400";  // 缓存1天
        res.body_bytes = Bytes(resp.begin(), resp.end());
        return res;
    }

    // 发送消息
    if (path == "/api/send") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string text = body["text"].asString();
            std::string mode = body["mode"].asString();
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            std::string to = body["to"].asString().empty() ? body["target_id"].asString() : body["to"].asString();
            int count = body["count"].asInt(); if (count <= 0) count = 1;
            int interval = (int)(body["interval"].asDouble() * 1000); if (interval < 50) interval = 50;

            // ← 修复：有序片段模式（前端普通模式 @ 用 parts 数组），保持 @ 在输入时的原始位置
            if (body["parts"].arr.size() > 0) {
                std::vector<proto::SendPart> parts;
                for (auto& p : body["parts"].arr) {
                    proto::SendPart sp;
                    std::string pt = p["type"].asString();
                    if (pt == "at") {
                        sp.type = 1;
                        sp.user_id = p["user_id"].asString();
                        sp.display = p["display"].asString();
                    } else {
                        sp.text = p["text"].asString();
                    }
                    parts.push_back(sp);
                }
                bool hasAt = false;
                for (auto& sp : parts) if (sp.type == 1) { hasAt = true; break; }
                if (hasAt) {
                    if (count > 1) {
                        int sent_ok = 0;
                        for (int i = 0; i < count; i++) {
                            if (send_group_parts(group, parts)) sent_ok++;
                            if (i < count - 1) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                        }
                        return HttpResponse().json("{\"ok\":true,\"at\":true,\"spam\":true,\"count\":" + std::to_string(sent_ok) + "}");
                    }
                    bool ok = send_group_parts(group, parts);
                    return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + ",\"at\":true}");
                }
                // 无 @ 的纯文本片段：退化为普通群发（含刷屏）
                std::string ptext;
                for (auto& sp : parts) ptext += sp.text;
                if (!ptext.empty()) {
                    if (count > 1) {
                        flood_start(ptext, count, interval, 1, "random", group);
                        return HttpResponse().json("{\"ok\":true,\"spam\":true,\"count\":" + std::to_string(count) + "}");
                    }
                    bool ok = send_group_text(group, ptext);
                    return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + ",\"sent_to\":\"" + group + "\"}");
                }
                return HttpResponse().json("{\"error\":\"empty parts\"}");
            }

            // @ 某人模式（前端 at / atspam 传 at_user；不回退 target_id，避免拦截私聊模式）
            // 与 Python 版一致：@ 与私聊（send_c2c_message）为独立命令，target_id 仅用于 dm
            std::string at_user = body["at_user"].asString();
            if (at_user.empty()) at_user = body["user_id"].asString();
            if (!at_user.empty() && !group.empty()) {
                std::string at_nick = body["at_nickname"].asString();
                if (at_nick.empty()) at_nick = body["target_nick"].asString();
                // 支持 @ + 刷屏（循环调用 send_group_at，不经过文本变换引擎）
                if (mode.find("spam") != std::string::npos || count > 1) {
                    int sent_ok = 0;
                    for (int i = 0; i < count; i++) {
                        if (send_group_at(group, text, at_user, at_nick)) sent_ok++;
                        if (i < count - 1) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                    }
                    return HttpResponse().json("{\"ok\":true,\"at\":true,\"spam\":true,\"count\":" + std::to_string(sent_ok) + "}");
                }
                bool ok = send_group_at(group, text, at_user, at_nick);
                return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + ",\"at\":true}");
            }

            // DM / 私聊模式（含 dmspam 私聊刷屏）
            if ((mode == "dm" || mode == "dmspam") && !to.empty()) {
                if (mode == "dmspam" || count > 1) {
                    // 私聊刷屏：循环 send_c2c_text（与 Python 版 dmspam 一致）
                    int sent_ok = 0;
                    for (int i = 0; i < count; i++) {
                        if (send_c2c_text(to, text)) sent_ok++;
                        if (i < count - 1) std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                    }
                    return HttpResponse().json("{\"ok\":true,\"dmspam\":true,\"count\":" + std::to_string(sent_ok) + "}");
                }
                bool ok = send_c2c_text(to, text);
                return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
            }
            // 刷屏模式
            if (mode.find("spam") != std::string::npos || count > 1) {
                flood_start(text, count, interval, 1, "random", group);
                return HttpResponse().json("{\"ok\":true,\"spam\":true,\"count\":" + std::to_string(count) + "}");
            }
            // 普通群发
            if (!group.empty()) {
                bool ok = send_group_text(group, text);
                return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + ",\"sent_to\":\"" + group + "\"}");
            }
            if (!to.empty()) {
                bool ok = send_c2c_text(to, text);
                return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
            }
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/send_sticker" || path == "/api/send-sticker") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string name = body["name"].asString();
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            std::string sid = stickers_.find_by_name(name);
            if (sid.empty()) return HttpResponse().json("{\"error\":\"sticker not found\"}");
            // 与 Python encode_tim_face_elem 一致：完整 6 字段 JSON
            std::ostringstream sj;
            sj << "{\"sticker_id\":\"" << sid << "\",\"package_id\":\"1003\""
               << ",\"width\":128,\"height\":128,\"formats\":\"png\""
               << ",\"name\":" << json_quote(name) << "}";
            std::string at_user = body["at_user"].asString();
            std::string at_nick = body["at_nickname"].asString();
            std::string text = body["text"].asString();
            // ← 补：贴纸刷屏（对齐前端 stickerCount / stickerInterval）
            int count = body["count"].asInt();
            if (count <= 0) count = 1;
            double interval = body["interval"].asDouble();
            if (interval < 0.01) interval = 0.1;
            if (count > 100) { count = 100; interval = std::max(interval, 0.5); }  // 安全上限
            // ← 修复：构建贴纸 media_info 供前端实时显示自己发送的贴纸
            JsonVal mi;
            mi["type"] = "sticker";
            mi["sticker_id"] = sid;
            mi["sticker_name"] = name;
            mi["package_id"] = "1003";
            std::string display_text = "[贴纸:" + name + "]";
            // ← 修复：带 @ 时记录补上 @ 前缀（与收到的 "@昵称 贴纸/文本" 格式一致）
            if (!at_user.empty()) {
                std::string at_display = at_nick.empty() ? at_user : at_nick;
                display_text = "@" + at_display + " " + display_text;
            }
            if (!text.empty()) display_text = display_text + " " + text;
            if (count == 1) {
                bool ok = send_group_sticker(group, sj.str(), at_user, at_nick, text);
                if (ok) record_sent_message(group, display_text, mi);
                return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
            }
            // 多次发送：后台线程按间隔循环（不阻塞 HTTP 响应）
            std::string sj_str = sj.str();
            std::thread([this, group, sj_str, at_user, at_nick, text, count, interval, display_text, mi]() {
                int sent_ok = 0;
                for (int i = 0; i < count; i++) {
                    if (send_group_sticker(group, sj_str, at_user, at_nick, text)) sent_ok++;
                    if (i + 1 < count)
                        std::this_thread::sleep_for(std::chrono::milliseconds((int)(interval * 1000)));
                }
                if (sent_ok > 0) record_sent_message(group, display_text, mi);
            }).detach();
            return HttpResponse().json("{\"ok\":true,\"count\":" + std::to_string(count) + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/send_image" || path == "/api/send-image") {
        // 支持两种方式：multipart 文件上传（前端 FormData）或 JSON URL 发送
        auto ct_it = req.headers.find("Content-Type");
        std::string ct = (ct_it != req.headers.end()) ? ct_it->second : "";
        if (ct.find("multipart/form-data") != std::string::npos) {
            std::vector<util::MultipartPart> parts;
            if (util::parse_multipart(ct, req.body, parts)) {
                std::string file_data, file_name, group, at_user, at_nick;
                for (auto& p : parts) {
                    if (p.name == "file") { file_data = p.data; file_name = p.filename; }
                    else if (p.name == "group" || p.name == "group_code") group = p.data;
                    else if (p.name == "at_user") at_user = p.data;
                    else if (p.name == "at_nickname") at_nick = p.data;
                }
                if (file_data.empty()) return HttpResponse().json("{\"error\":\"no file\"}");
                if (group.empty()) group = config_.default_target();
                if (file_name.empty()) file_name = "image.png";
                std::string url, uuid;
                if (!upload_media(file_name, file_data, url, uuid))
                    return HttpResponse().json("{\"error\":\"upload failed\"}");
                // ← 修复：解析图片真实宽高（对齐 Python Image.open 获取宽高），
                //   w/h 传 0 会导致服务端无法正确生成图片，群里显示 404
                int img_w = 0, img_h = 0;
                util::parse_image_dimensions(file_data, img_w, img_h);
                bool ok = send_group_image(group, url, uuid, (int)file_data.size(), img_w, img_h, at_user, at_nick);
                // ← 修复：记录自己发送的图片消息（带 media_info），否则消息面板不显示
                if (ok) {
                    JsonVal mi;
                    mi["type"] = "image";
                    JsonVal urls; JsonVal uv; uv.v = url; urls.arr.push_back(uv);
                    mi["image_urls"] = urls;
                    mi["image_uuid"] = uuid;
                    mi["image_size"] = (int)file_data.size();
                    record_sent_message(group, "[图片]", mi);
                }
                return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
            }
            return HttpResponse().json("{\"error\":\"invalid multipart\"}");
        }
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string url = body["url"].asString();
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            std::string uuid = body["uuid"].asString();
            int size = body["size"].asInt();
            int w = body["width"].asInt();
            int h = body["height"].asInt();
            bool ok = send_group_image(group, url, uuid, size, w, h);
            return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }

    // 刷屏
    if (path == "/api/flood/start") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string id = flood_start(
                body["text"].asString().empty() ? "test" : body["text"].asString(),
                body["count"].asInt() > 0 ? body["count"].asInt() : 20,
                body["delay"].asInt() > 0 ? body["delay"].asInt() : 50,
                body["batch"].asInt() > 0 ? body["batch"].asInt() : 3,
                body["mode"].asString().empty() ? "random" : body["mode"].asString(),
                body["group_code"].asString().empty() ? (body["group"].asString().empty() ? config_.default_target() : body["group"].asString()) : body["group_code"].asString()
            );
            return HttpResponse().json("{\"id\":\"" + id + "\"}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/flood/cancel") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            flood_cancel(body["id"].asString());
            return HttpResponse().json("{\"ok\":true}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/flood/stats") return HttpResponse().json(flood_stats());
    if (path == "/api/flood/list") return HttpResponse().json(flood_engine_.list_tasks());

    // 消息（日志历史在前 + 最新缓存追加在后，对照 Python 版从存储读历史）
    if (path == "/api/messages") {
        int limit = 100;
        auto qit = req.query_params.find("limit");
        if (qit != req.query_params.end()) { int v = std::atoi(qit->second.c_str()); if (v > 0) limit = v; }
        if (limit > 500) limit = 500;

        // ← 修复：按群过滤。未传 group_code 时返回所有监听群消息
        //   （此前默认只返回主群，而主群可能无消息，导致"看不到别人消息"）
        std::string group_filter;
        auto git = req.query_params.find("group_code");
        if (git != req.query_params.end()) group_filter = git->second;
        // ← 补：按私聊对象过滤（c2c_user）——返回与该用户的全部私聊消息（发送+接收）
        std::string c2c_filter;
        auto cit = req.query_params.find("c2c_user");
        if (cit != req.query_params.end()) c2c_filter = cit->second;
        // 未指定群时允许的群集合 = 所有监听群；指定了群则仅该群
        std::set<std::string> allow_groups;
        if (group_filter.empty()) {
            for (auto& g : config_.listen_groups) allow_groups.insert(g);
            if (!config_.default_target().empty()) allow_groups.insert(config_.default_target());
        }

        std::vector<JsonVal> all;
        std::set<std::string> seen_ids;

        // 先加载日志历史（旧消息在前），再追加最新缓存（后），取尾部即为最新
        std::vector<JsonVal> log_msgs;
        {
            std::vector<std::string> logfiles;
            DIR* d0 = opendir("logs");
            if (d0) {
                struct dirent* ent;
                while ((ent = readdir(d0))) {
                    std::string n = ent->d_name;
                    if (n.rfind("messages_", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".log") == 0)
                        logfiles.push_back(n);
                }
                closedir(d0);
            }
            std::sort(logfiles.begin(), logfiles.end());
            for (auto& fn : logfiles) {
                std::ifstream f(std::string("logs/") + fn);
                std::string line;
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    JsonVal v;
                    if (json_parse(line, v)) {
                        // ← 补：私聊过滤优先（c2c_user）——只返回与该用户的私聊消息
                        if (!c2c_filter.empty()) {
                            bool is_c2c = v["is_c2c"].asBool();
                            if (!is_c2c) continue;
                            std::string peer = v["c2c_peer"].asString();
                            std::string sender = v["sender_id"].asString();
                            // 收到的私聊：sender=c2c_peer；发出的私聊：sender=bot, c2c_peer=目标
                            bool from_peer = (sender == c2c_filter);
                            bool to_peer = (peer == c2c_filter);
                            if (!from_peer && !to_peer) continue;
                        } else {
                            // 群过滤：指定了 group_code 时仅匹配该群；未指定时只保留监听群消息
                            if (!group_filter.empty()) {
                                if (v["group_code"].asString() != group_filter) continue;
                            } else {
                                std::string gc = v["group_code"].asString();
                                if (!allow_groups.count(gc)) continue;
                            }
                        }
                        std::string id = v["msg_id"].asString();
                        if (!id.empty()) {
                            if (seen_ids.count(id)) continue;
                            seen_ids.insert(id);
                        }
                        // 兼容旧日志：timestamp → time（前端 renderMessages 读 m.time）
                        if (v["time"].asString().empty() && !v["timestamp"].asString().empty())
                            v["time"] = v["timestamp"].asString();
                        log_msgs.push_back(v);
                    }
                }
            }
            all.insert(all.end(), log_msgs.begin(), log_msgs.end());
        }

        // 追加内存缓存（最新消息），按 msg_id 去重
        {
            std::lock_guard<std::mutex> l(msg_mu_);
            for (auto& m : msg_cache_) {
                if (!c2c_filter.empty()) {
                    bool is_c2c = m["is_c2c"].asBool();
                    if (!is_c2c) continue;
                    std::string peer = m["c2c_peer"].asString();
                    std::string sender = m["sender_id"].asString();
                    bool from_peer = (sender == c2c_filter);
                    bool to_peer = (peer == c2c_filter);
                    if (!from_peer && !to_peer) continue;
                } else {
                    if (!group_filter.empty()) {
                        if (m["group_code"].asString() != group_filter) continue;
                    } else {
                        if (!allow_groups.count(m["group_code"].asString())) continue;
                    }
                }
                std::string id = m["msg_id"].asString();
                if (!id.empty()) {
                    if (seen_ids.count(id)) continue;
                    seen_ids.insert(id);
                }
                all.push_back(m);
            }
        }

        size_t start = all.size() > (size_t)limit ? all.size() - (size_t)limit : 0;
        std::ostringstream oss; oss << "{\"ok\":true,\"messages\":[";
        bool first = true;
        // ← 修复：为每条消息附加 list_index（在当前群完整列表中的绝对位置）。
        //   前端引用回复时回传该值，后端据此精确定位，杜绝"序号与后端 index 不一致"。
        for (size_t i = start; i < all.size(); i++) {
            if (!first) oss << ","; first = false;
            JsonVal out = all[i];
            out["list_index"] = (double)i;
            oss << json_compact(out);
        }
        oss << "]}"; return HttpResponse().json(oss.str());
    }
    if (path == "/api/messages/clear") {
        std::lock_guard<std::mutex> l(msg_mu_);
        msg_cache_.clear();
        // ← 修复：同时清空待写队列与已写入的日志文件，
        //   避免重新加载时日志里的历史消息又被读回（"无法清空"）
        msg_logger_.clear_all();
        return HttpResponse().json("{\"ok\":true}");
    }
    if (path == "/api/messages/logs") {
        std::ostringstream oss; oss << "[";
        DIR* d = opendir("./logs");
        if (d) {
            struct dirent* ent;
            bool first = true;
            while ((ent = readdir(d))) {
                std::string n = ent->d_name;
                if (n.rfind("messages_", 0) == 0 && n.find(".log") != std::string::npos) {
                    if (!first) oss << ","; first = false;
                    oss << "\"" << n << "\"";
                }
            }
            closedir(d);
        }
        oss << "]"; return HttpResponse().json(oss.str());
    }

    // 状态（字段对齐 Python 版 SpamSender 状态与前端 startStatusPolling 读取）
    if (path == "/api/status") {
        auto fs = flood_engine_.get_stats();
        std::ostringstream oss;
        oss << "{\"ok\":true"
            << ",\"connected\":" << (bot_connected_.load() ? "true" : "false")
            << ",\"bot_id\":\"" << config_.yuanbao_id << "\""
            << ",\"flood_active\":" << fs.active_tasks
            << ",\"flood_total_sent\":" << flood_total_sent_.load()
            << ",\"msg_cache\":" << msg_cache_.size()
            << ",\"msg_written\":" << msg_logger_.total_written.load()
            << ",\"logger_written\":" << msg_logger_.total_written.load()
            << ",\"forward_enabled\":" << (forward_enabled_ ? "true" : "false")
            << ",\"forward_mode_enabled\":" << (forward_enabled_ ? "true" : "false")
            << ",\"forward_at_only\":" << (forward_at_only_ ? "true" : "false")
            << ",\"forward_at_yuanbao\":" << (forward_at_yuanbao_ ? "true" : "false")
            << ",\"heartbeat_interval\":" << config_.heartbeat_interval
            << ",\"current_group\":\"" << config_.default_target() << "\""
            << ",\"msg_log_enabled\":" << (config_.msg_log_enabled ? "true" : "false")
            << ",\"recall_monitor_enabled\":" << (config_.recall_monitor_enabled ? "true" : "false")
            << ",\"llm_enabled\":" << (config_.llm.enabled ? "true" : "false")
            << ",\"llm_model\":\"" << config_.llm.model << "\"}";
        return HttpResponse().json(oss.str());
    }
    if (path == "/api/health")
        return HttpResponse().json("{\"status\":\"ok\",\"cpp\":true,\"version\":\"3.0\"}");

    // 贴纸列表
    if (path == "/api/stickers") {
        // 前端 loadStickers() 读取 r.stickers；每个贴纸附带 icon（本地 ico 图标 URL，无则空）
        std::ostringstream oss; oss << "{\"stickers\":[";
        bool first = true;
        for (auto& kv : stickers_.stickers) {
            if (!first) oss << ","; first = false;
            oss << "{\"name\":" << json_quote(kv.first)
                << ",\"id\":\"" << kv.second << "\"";
            std::string ifile = stickers_.icon_file(kv.first);
            if (!ifile.empty()) {
                oss << ",\"icon\":\"/api/sticker-icon?name=" << util::url_encode(kv.first) << "\"";
            } else {
                oss << ",\"icon\":\"\"";
            }
            oss << "}";
        }
        oss << "]}"; return HttpResponse().json(oss.str());
    }

    // 本地贴纸图标（按名称返回 ico 二进制，供前端 <img> 直接显示）
    if (path == "/api/sticker-icon") {
        auto qit = req.query_params.find("name");
        if (qit == req.query_params.end() || qit->second.empty())
            return HttpResponse().bad_request();
        std::string ifile = stickers_.icon_file(qit->second);
        if (ifile.empty()) return HttpResponse().not_found();
        std::string data;
        if (!util::read_file_utf8("ico/" + ifile, data) || data.empty())
            return HttpResponse().not_found();
        HttpResponse r;
        r.body = data;
        r.headers["Content-Type"] = "image/x-icon";
        r.headers["Cache-Control"] = "public, max-age=86400";
        return r;
    }

    // ── 转发模式 ──
    if (path == "/api/forward-mode/enable") {
        forward_enabled_ = true;
        JsonVal body; if (json_parse(req.body, body)) {
            forward_at_only_ = body["at_only"].asBool();
            forward_at_yuanbao_ = body["forward_at_yuanbao"].asBool();
        }
        return HttpResponse().json("{\"ok\":true,\"forward_enabled\":true}");
    }
    if (path == "/api/forward-mode/disable") {
        forward_enabled_ = false;
        return HttpResponse().json("{\"ok\":true,\"forward_enabled\":false}");
    }
    if (path == "/api/forward-mode/config") {
        // 前端 getForwardModeConfig() 使用 r.enabled / r.queue_length / r.forward_at_yuanbao / r.at_only
        std::ostringstream o;
        o << "{\"forward_enabled\":" << (forward_enabled_ ? "true" : "false")
          << ",\"enabled\":" << (forward_enabled_ ? "true" : "false")
          << ",\"at_only\":" << (forward_at_only_ ? "true" : "false")
          << ",\"forward_at_yuanbao\":" << (forward_at_yuanbao_ ? "true" : "false")
          << ",\"queue_length\":0}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/forward-mode/queue") {
        return HttpResponse().json("{\"queue\":[],\"count\":0}");
    }
    if (path == "/api/forward-mode/clear-queue") {
        return HttpResponse().json("{\"ok\":true}");
    }
    if (path == "/api/forward-mode/toggle-at-yuanbao") {
        forward_at_yuanbao_ = !forward_at_yuanbao_;
        std::ostringstream o;
        o << "{\"ok\":true,\"forward_at_yuanbao\":" << (forward_at_yuanbao_ ? "true" : "false") << "}";
        return HttpResponse().json(o.str());
    }

    // ── LLM 配置管理 ──
    if (path == "/api/llm/config" && req.method == "GET") {
        std::ostringstream o;
        o << "{\"enabled\":" << (config_.llm.enabled ? "true" : "false")
          << ",\"api_url\":" << json_quote(config_.llm.api_url)
          << ",\"api_key\":" << json_quote(config_.llm.api_key.empty() ? "" : "***" + config_.llm.api_key.substr(config_.llm.api_key.size() > 4 ? config_.llm.api_key.size() - 4 : 0))
          << ",\"model\":\"" << config_.llm.model << "\""
          << ",\"system_prompt\":" << json_quote(config_.llm.system_prompt)
          << ",\"max_tokens\":" << config_.llm.max_tokens
          << ",\"temperature\":" << config_.llm.temperature
          << ",\"timeout_sec\":" << config_.llm.timeout_sec << "}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/llm/config" && req.method == "POST") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            if (!body["api_url"].asString().empty())
                config_.llm.api_url = body["api_url"].asString();
            if (!body["api_key"].asString().empty())
                config_.llm.api_key = body["api_key"].asString();
            if (!body["model"].asString().empty())
                config_.llm.model = body["model"].asString();
            if (!body["system_prompt"].asString().empty())
                config_.llm.system_prompt = body["system_prompt"].asString();
            if (body["max_tokens"].asInt() > 0)
                config_.llm.max_tokens = body["max_tokens"].asInt();
            if (body["temperature"].asDouble() > 0)
                config_.llm.temperature = body["temperature"].asDouble();
            if (body["timeout_sec"].asInt() > 0)
                config_.llm.timeout_sec = body["timeout_sec"].asInt();
            config_.llm.enabled = !config_.llm.api_key.empty() && !config_.llm.api_url.empty();
            save_config();
            return HttpResponse().json("{\"ok\":true,\"llm_enabled\":" + std::string(config_.llm.enabled ? "true" : "false") + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/llm/test") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string test_msg = body["message"].asString();
            if (test_msg.empty()) test_msg = "你好，请简单介绍一下自己";
            std::string reply = call_llm_api(test_msg, "测试用户");
            std::ostringstream o;
            o << "{\"ok\":" << (reply.empty() ? "false" : "true")
              << ",\"reply\":" << json_quote(reply) << "}";
            return HttpResponse().json(o.str());
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }

    // ── 发送增强 ──
    if (path == "/api/send-reply") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string text = body["text"].asString();
            // ← 修复：兼容 api() 统一注入的 group_code，回复也发到当前查看的群
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            std::string ref_id = body["ref_msg_id"].asString();
            // 前端可能只传 index（消息序号）：从「当前群」消息缓存查对应 msg_id。
            // ← 修复：原先直接索引全局 msg_cache_[idx]，多群监听时序号与前端
            //   （按群过滤的日志+缓存合并列表）错位。现按与 /api/messages 相同
            //   的顺序（日志在前 + 缓存追加在后、按群过滤、去重）重建列表再索引。
            //   前端优先回传 list_index（/api/messages 返回的完整列表绝对位置），
            //   该值与下面重建的列表一一对应，彻底杜绝序号错位。
            if (ref_id.empty()) {
                int idx = body.obj.count("list_index") ? body["list_index"].asInt() : body["index"].asInt();
                if (idx >= 0) {
                    std::vector<JsonVal> log_msgs;
                    std::set<std::string> seen_ids;
                    DIR* d0 = opendir("logs");
                    if (d0) {
                        struct dirent* ent;
                        std::vector<std::string> logfiles;
                        while ((ent = readdir(d0))) {
                            std::string n = ent->d_name;
                            if (n.rfind("messages_", 0) == 0 && n.size() > 4
                                && n.compare(n.size() - 4, 4, ".log") == 0)
                                logfiles.push_back(n);
                        }
                        closedir(d0);
                        std::sort(logfiles.begin(), logfiles.end());
                        for (auto& fn : logfiles) {
                            std::ifstream f(std::string("logs/") + fn);
                            std::string line;
                            while (std::getline(f, line)) {
                                if (line.empty()) continue;
                                JsonVal v;
                                if (json_parse(line, v)) {
                                    if (!group.empty() && v["group_code"].asString() != group) continue;
                                    std::string id = v["msg_id"].asString();
                                    if (!id.empty()) {
                                        if (seen_ids.count(id)) continue;
                                        seen_ids.insert(id);
                                    }
                                    log_msgs.push_back(v);
                                }
                            }
                        }
                    }
                    // 与 /api/messages 相同的组合：日志在前 + 缓存追加在后（去重）
                    std::vector<JsonVal> combined = log_msgs;
                    {
                        std::lock_guard<std::mutex> l(msg_mu_);
                        for (auto& m : msg_cache_) {
                            if (!group.empty() && m["group_code"].asString() != group) continue;
                            std::string id = m["msg_id"].asString();
                            if (!id.empty()) {
                                if (seen_ids.count(id)) continue;
                                seen_ids.insert(id);
                            }
                            combined.push_back(m);
                        }
                    }
                    if ((size_t)idx < combined.size())
                        ref_id = combined[idx]["msg_id"].asString();
                }
            }
            std::string at_user = body["at_user"].asString();
            std::string at_nick = body["at_nickname"].asString();
            bool ok = send_group_reply(group, text, ref_id, at_user, at_nick);
            return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/send/at-all") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            bool ok = send_group_at_all(group);
            std::ostringstream o;
            o << "{\"ok\":" << (ok ? "true" : "false")
              << ",\"message\":" << json_quote(ok ? "@全体已发送" : "发送 @全体 失败")
              << ",\"code\":\"" << (ok ? "OK" : "FAILED") << "\"}";
            return HttpResponse().json(o.str());
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }

    // ── 群聊/成员 ──
    if (path == "/api/groups") {
        // 前端 loadGroups() 使用 g.group_code / g.group_name
        // ← 修复：返回所有已发现群（PUSH 实时收集 + 持久文件/历史日志恢复）
        if (!groups_scanned_) {
            load_known_groups();   // ← 先恢复持久化的群列表
            std::set<std::string> codes;
            DIR* d0 = opendir("logs");
            if (d0) {
                struct dirent* ent;
                while ((ent = readdir(d0))) {
                    std::string n = ent->d_name;
                    if (n.rfind("messages_", 0) == 0 && n.size() > 4
                        && n.compare(n.size() - 4, 4, ".log") == 0) {
                        std::ifstream f(std::string("logs/") + n);
                        std::string line;
                        while (std::getline(f, line)) {
                            if (line.empty()) continue;
                            JsonVal v;
                            if (json_parse(line, v)) {
                                std::string gc = v["group_code"].asString();
                                if (!gc.empty()) codes.insert(gc);
                            }
                        }
                    }
                }
                closedir(d0);
            }
            {
                std::lock_guard<std::mutex> l(members_mu_);
                for (auto& c : codes) if (!known_groups_.count(c)) known_groups_[c] = "";
                groups_scanned_ = true;
            }
            // 自动获取监听群：listen_groups 为空时全量同步所有已发现群（不硬编码）
            // ← 修复：必须在锁作用域外调用（其内部会再次获取 members_mu_，
            //   std::mutex 不可重入，原代码导致同一线程自死锁，/api/groups 永久挂起）
            auto_listen_from_known();
        }
        // 组装群列表：默认目标群（监听列表第一项）置顶，其余按群号排序
        std::vector<std::pair<std::string, std::string>> groups;
        {
            std::lock_guard<std::mutex> l(members_mu_);
            std::string main_name;
            auto mit = known_groups_.find(config_.default_target());
            if (mit != known_groups_.end()) main_name = mit->second;
            if (main_name.empty()) {
                auto eit = members_cache_map_.find(config_.default_target());
                if (eit != members_cache_map_.end()) main_name = eit->second.group_name;
            }
            if (main_name.empty()) main_name = members_group_name_;
            // 无监听群时 default_target() 为空，不加入空群项
            if (!config_.default_target().empty())
                groups.push_back({config_.default_target(), main_name});
            for (auto& kv : known_groups_) {
                if (kv.first == config_.default_target()) continue;
                groups.push_back(kv);
            }
        }
        // 未获取到群名的群逐个查询（默认目标群优先，串行等待；已缓存的直接跳过）
        // 每个查询登记 msg_id -> group，响应按 msg_id 精确回写，多请求并发也不张冠李戴
        if (bot_connected_.load()) {
            for (auto& g : groups) {
                if (!g.second.empty()) continue;
                std::string qid = util::random_hex_id();
                {
                    std::lock_guard<std::mutex> l(members_mu_);
                    group_info_response_ = false;
                    pending_query_groups_[qid] = g.first;
                }
                if (!bot_ws_send_bytes(bot_ws_build_query_group_info_msg(g.first, qid))) {
                    std::lock_guard<std::mutex> l(members_mu_);
                    pending_query_groups_.erase(qid);
                    continue;
                }
                std::unique_lock<std::mutex> l(members_mu_);
                members_cv_.wait_for(l, std::chrono::milliseconds(1200),
                                     [this]{ return group_info_response_ || !bot_connected_.load(); });
                auto it = known_groups_.find(g.first);
                if (it != known_groups_.end() && !it->second.empty()) g.second = it->second;
            }
        }
        // 未获取到群名时显示群号，绝不使用占位名
        std::ostringstream o;
        o << "{\"groups\":[";
        bool first = true;
        for (auto& g : groups) {
            if (!first) o << ","; first = false;
            o << "{\"group_code\":\"" << g.first << "\",\"group_name\":"
              << json_quote(g.second.empty() ? g.first : g.second)
              << ",\"listening\":" << (config_.is_listening(g.first) ? "true" : "false")
              << ",\"message_count\":0,\"last_message\":\"\"}";
        }
        o << "],\"current_group\":\"" << config_.default_target() << "\",\"listen_groups\":[";
        first = true;
        for (auto& g : config_.listen_groups) {
            if (!first) o << ","; first = false;
            o << "\"" << g << "\"";
        }
        o << "]}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/groups/switch") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string code = body["group_code"].asString();
            if (code.empty()) return HttpResponse().json("{\"error\":\"group_code required\"}");
            // ← 主群概念已移除：current_group 固定为监听列表第一项，切换群仅作前端查看状态；
            //   这里清除旧群信息缓存，使成员/群信息下次查询重新获取
            save_config();
            // ← 修复：切换查看的群后清除该群缓存（群名/群主/成员），下次查询重新获取；
            //   其他群的缓存保留，避免无谓重查
            {
                std::lock_guard<std::mutex> l(members_mu_);
                members_cache_map_.erase(code);
                if (code == config_.default_target()) {
                    members_group_name_.clear();
                    members_owner_id_.clear();
                }
            }
            return HttpResponse().json("{\"ok\":true}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/groups/listen") {
        // 多群监听开关：{ group_code, listen:true/false }
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string code = body["group_code"].asString();
            bool listen = (body["listen"].is_bool() && body["listen"].asBool())
                          || (!body["listen"].is_bool() && body["listen"].asString() == "true");
            if (code.empty()) return HttpResponse().json("{\"error\":\"group_code required\"}");
            auto it = std::find(config_.listen_groups.begin(), config_.listen_groups.end(), code);
            bool currently = it != config_.listen_groups.end();
            if (listen && !currently) {
                config_.listen_groups.push_back(code);
            } else if (!listen && currently) {
                // ← 主群概念已移除：任何群都可取消监听
                config_.listen_groups.erase(it);
            }
            save_config();
            return HttpResponse().json("{\"ok\":true,\"listening\":" + std::string(listen ? "true" : "false") + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/members") {
        // 前端 loadMembers() 读取 r.ok / r.members[].user_id,nick_name / r.group_owner_user_id
        if (!bot_connected_.load()) {
            return HttpResponse().json("{\"ok\":false,\"message\":\"Bot 未连接，请先连接\"}");
        }
        // ← 修复：支持按群查询成员（group_code 参数），切换派后成员随群刷新；
        //   未指定时回退默认目标群
        std::string target_group = config_.default_target();
        auto mq = req.query_params.find("group_code");
        if (mq != req.query_params.end() && !mq->second.empty()) target_group = mq->second;
        // ← 修复：成员列表按群缓存（7s）。前端每 5 秒轮询 loadMembers()，每次都重新向 Bot
        //   查询群信息+成员列表（阻塞 3s+3s 等待响应），造成请求风暴与卡顿。
        //   7 秒内同群复用缓存，避免高频轮询打满 Bot 通道；缓存按群区分不串群。
        {
            std::lock_guard<std::mutex> l(members_mu_);
            auto it = members_cache_map_.find(target_group);
            if (it != members_cache_map_.end()
                && !it->second.members.empty()
                && util::now_ms() - it->second.fetched_at < 7000) {
                auto& entry = it->second;
                std::ostringstream o;
                o << "{\"ok\":true,\"cached\":true,\"members\":[";
                bool first = true;
                for (auto& m : entry.members) {
                    if (!first) o << ","; first = false;
                    o << "{\"user_id\":\"" << m.first << "\",\"nick_name\":"
                      << json_quote(m.second.empty() ? m.first : m.second) << "}";
                }
                o << "],\"total\":" << entry.members.size()
                  << ",\"group_owner_user_id\":\"" << entry.owner_id << "\"}";
                return HttpResponse().json(o.str());
            }
        }
        // 查询流程串行化：多个 HTTP 请求（loadMembers/loadGroups 轮询、切换群）同时
        // 操作 group_info_response_/members_response_ 标志会导致响应张冠李戴、超时返回空。
        // ← 修复：用 try_lock 非阻塞获取。若另一个成员查询（含发送 @ 消息后的 loadMembers）
        //   正在进行中，不排队等待（避免 3s+3s 阻塞让前端像"卡死"），直接回退已有缓存。
        {
            std::unique_lock<std::mutex> qlock(members_query_mu_, std::try_to_lock);
            if (!qlock.owns_lock()) {
                std::lock_guard<std::mutex> l(members_mu_);
                auto it = members_cache_map_.find(target_group);
                if (it != members_cache_map_.end() && !it->second.members.empty()) {
                    auto& entry = it->second;
                    std::ostringstream o;
                    o << "{\"ok\":true,\"cached\":true,\"stale\":true,\"members\":[";
                    bool first = true;
                    for (auto& m : entry.members) {
                        if (!first) o << ","; first = false;
                        o << "{\"user_id\":\"" << m.first << "\",\"nick_name\":"
                          << json_quote(m.second.empty() ? m.first : m.second) << "}";
                    }
                    o << "],\"total\":" << entry.members.size()
                      << ",\"group_owner_user_id\":\"" << entry.owner_id << "\"}";
                    return HttpResponse().json(o.str());
                }
                return HttpResponse().json("{\"ok\":false,\"message\":\"成员查询进行中，请稍后\"}");
            }
            // 查询主体继续用 qlock（随作用域结束释放）：记录该群旧缓存后执行查询
            std::lock_guard<std::mutex> l(members_mu_);
            members_request_group_ = target_group;
        }
        std::string qid = util::random_hex_id();   // 群信息查询关联 ID
        // 1) 查询群信息以获取群主 user_id（登记 msg_id，响应按 msg_id 精确关联目标群）
        {
            std::lock_guard<std::mutex> l(members_mu_);
            group_info_response_ = false;
            pending_query_groups_[qid] = target_group;
        }
        if (!bot_ws_send_bytes(bot_ws_build_query_group_info_msg(target_group, qid))) {
            std::lock_guard<std::mutex> l(members_mu_);
            pending_query_groups_.erase(qid);
            return HttpResponse().json("{\"ok\":false,\"message\":\"群信息请求发送失败\"}");
        }
        {
            std::unique_lock<std::mutex> l(members_mu_);
            members_cv_.wait_for(l, std::chrono::seconds(3), [this]{ return group_info_response_ || !bot_connected_.load(); });
        }
        // 等待期间 Bot 已断开：立即返回，避免 HTTP 请求挂起导致前端一直转圈
        if (!bot_connected_.load())
            return HttpResponse().json("{\"ok\":false,\"message\":\"Bot 连接已断开\"}");
        // 2) 查询成员列表
        {
            std::lock_guard<std::mutex> l(members_mu_);
            members_response_ = false;
        }
        if (!bot_ws_send_bytes(bot_ws_build_get_members_msg(target_group)))
            return HttpResponse().json("{\"ok\":false,\"message\":\"成员请求发送失败\"}");
        {
            std::unique_lock<std::mutex> l(members_mu_);
            members_cv_.wait_for(l, std::chrono::seconds(3), [this]{ return members_response_ || !bot_connected_.load(); });
        }
        // 3) 组装返回：优先返回本次成功查询的缓存；查询超时（响应标志未置位）时回退旧缓存
        {
            std::lock_guard<std::mutex> l(members_mu_);
            auto it = members_cache_map_.find(target_group);
            const auto* entry = (it != members_cache_map_.end()) ? &it->second : nullptr;
            std::ostringstream o;
            bool fresh = members_response_;
            o << "{\"ok\":true,\"cached\":" << (fresh ? "false" : "true")
              << ",\"stale\":" << (fresh ? "false" : "true") << ",\"members\":[";
            bool first = true;
            if (entry) {
                for (auto& m : entry->members) {
                    if (!first) o << ","; first = false;
                    o << "{\"user_id\":\"" << m.first << "\",\"nick_name\":"
                      << json_quote(m.second.empty() ? m.first : m.second) << "}";
                }
                o << "],\"total\":" << entry->members.size()
                  << ",\"group_owner_user_id\":\"" << entry->owner_id << "\"}";
            } else {
                o << "],\"total\":0,\"group_owner_user_id\":\"\"}";
            }
            return HttpResponse().json(o.str());
        }
    }

    // ── 心跳 ──
    if (path == "/api/heartbeat") {
        // 前端 checkHeartbeat() 使用 r.ok / r.connected
        std::ostringstream o;
        o << "{\"ok\":true,\"connected\":" << (bot_connected_.load() ? "true" : "false")
          << ",\"interval\":" << config_.heartbeat_interval
          << ",\"last\":" << last_heartbeat_.load() << "}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/heartbeat/interval") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            config_.heartbeat_interval = body["interval"].asInt();
            if (config_.heartbeat_interval < 5) config_.heartbeat_interval = 5;
            return HttpResponse().json("{\"ok\":true,\"interval\":" + std::to_string(config_.heartbeat_interval) + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }

    // ── 设置 ──
    if (path == "/api/settings" && req.method == "GET") {
        // 前端 loadSettings() 使用的字段：group_code/default_group、port、
        // heartbeat_interval、forward_mode_enabled、forward_at_only、forward_at_yuanbao、msg_log_enabled
        std::ostringstream o;
        o << "{\"default_group\":\"" << config_.default_target() << "\","
          << "\"group_code\":\"" << config_.default_target() << "\","
          << "\"port\":" << config_.port << ","
          << "\"heartbeat_interval\":" << config_.heartbeat_interval << ","
          << "\"forward_enabled\":" << (forward_enabled_ ? "true" : "false") << ","
          << "\"forward_mode_enabled\":" << (forward_enabled_ ? "true" : "false") << ","
          << "\"forward_at_only\":" << (forward_at_only_ ? "true" : "false") << ","
          << "\"forward_at_yuanbao\":" << (forward_at_yuanbao_ ? "true" : "false") << ","
          << "\"msg_log_enabled\":" << (config_.msg_log_enabled ? "true" : "false")
          << ",\"recall_monitor_enabled\":" << (config_.recall_monitor_enabled ? "true" : "false") << "}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/settings" && req.method == "POST") {
        // 前端 saveSettings() 发送：group_code/default_group/port/heartbeat_interval/
        // forward_mode_enabled/forward_at_only/forward_at_yuanbao/msg_log_enabled
        JsonVal body;
        if (json_parse(req.body, body)) {
            // ← 主群概念已移除：不再接受 default_group/group_code，默认目标群由监听列表第一项决定
            if (body["port"].asInt() >= 1)
                config_.port = body["port"].asInt();
            if (body.obj.count("heartbeat_interval") > 0)
                config_.heartbeat_interval = body["heartbeat_interval"].asInt();
            if (body.obj.count("forward_mode_enabled") > 0)
                forward_enabled_ = body["forward_mode_enabled"].asBool();
            if (body.obj.count("forward_at_only") > 0)
                forward_at_only_ = body["forward_at_only"].asBool();
            if (body.obj.count("forward_at_yuanbao") > 0)
                forward_at_yuanbao_ = body["forward_at_yuanbao"].asBool();
            if (body.obj.count("msg_log_enabled") > 0) {
                config_.msg_log_enabled = body["msg_log_enabled"].asBool();
                msg_logger_.enabled = config_.msg_log_enabled;
            }
            if (body.obj.count("recall_monitor_enabled") > 0)
                config_.recall_monitor_enabled = body["recall_monitor_enabled"].asBool();
            save_config();
            return HttpResponse().json("{\"ok\":true}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }

    // ── 消息日志管理 ──
    if (path == "/api/msg-log/stats") {
        std::ostringstream o;
        o << "{\"ok\":true"
          << ",\"today_written\":" << msg_logger_.total_written.load()
          << ",\"pending\":" << msg_logger_.pending_count()
          << ",\"today_jsonl_size\":" << msg_logger_.today_jsonl_size()
          << ",\"today_txt_size\":" << msg_logger_.today_txt_size()
          << ",\"total_size\":" << msg_logger_.total_size()
          << ",\"enabled\":true}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/msg-log/enable") {
        return HttpResponse().json("{\"ok\":true}");
    }
    if (path == "/api/msg-log/disable") {
        return HttpResponse().json("{\"ok\":true}");
    }
    if (path == "/api/msg-log/toggle") {
        return HttpResponse().json("{\"ok\":true}");
    }
    if (path == "/api/msg-log/files") {
        // 前端 loadLoggerFiles() 读取 r.ok / r.files[].name,size,mtime
        std::ostringstream oss; oss << "{\"ok\":true,\"files\":[";
        DIR* d = opendir("./logs");
        if (d) {
            struct dirent* ent;
            bool first = true;
            while ((ent = readdir(d))) {
                std::string n = ent->d_name;
                if (n.rfind("messages_", 0) == 0) {
                    if (!first) oss << ","; first = false;
                    int64_t sz = msglog_file_size(std::string("./logs/") + n);
                    oss << "{\"name\":\"" << n << "\",\"size\":" << sz << ",\"mtime\":\"\"}";
                }
            }
            closedir(d);
        }
        oss << "]}"; return HttpResponse().json(oss.str());
    }
    if (path == "/api/msg-log/recent") {
        // 前端 loadLoggerPreview() 读取 r.ok / r.lines
        return HttpResponse().json("{\"ok\":true,\"lines\":[]}");
    }
    if (path == "/api/msg-log/download") {
        return HttpResponse().json("{\"url\":\"/api/messages/logs\"}");
    }
    if (path == "/api/msg-log/clear-today") {
        return HttpResponse().json("{\"ok\":true}");
    }

    // ── 发送文件 ──
    if (path == "/api/send-file" || path == "/api/send_file") {
        // 支持两种方式：multipart 文件上传（前端 FormData）或 JSON URL 发送
        auto ct_it = req.headers.find("Content-Type");
        std::string ct = (ct_it != req.headers.end()) ? ct_it->second : "";
        if (ct.find("multipart/form-data") != std::string::npos) {
            std::vector<util::MultipartPart> parts;
            if (util::parse_multipart(ct, req.body, parts)) {
                std::string file_data, file_name, group, at_user, at_nick;
                for (auto& p : parts) {
                    if (p.name == "file") { file_data = p.data; file_name = p.filename; }
                    else if (p.name == "group" || p.name == "group_code") group = p.data;
                    else if (p.name == "at_user") at_user = p.data;
                    else if (p.name == "at_nickname") at_nick = p.data;
                }
                if (file_data.empty()) return HttpResponse().json("{\"error\":\"no file\"}");
                if (group.empty()) group = config_.default_target();
                if (file_name.empty()) file_name = "file.bin";
                std::string url, uuid;
                if (!upload_media(file_name, file_data, url, uuid))
                    return HttpResponse().json("{\"error\":\"upload failed\"}");
                bool ok = send_group_file(group, url, file_name, uuid, (int)file_data.size(), at_user, at_nick);
                // ← 修复：记录自己发送的文件消息（带 media_info），否则消息面板不显示
                if (ok) {
                    JsonVal mi;
                    mi["type"] = "file";
                    mi["file_url"] = url;
                    mi["file_name"] = file_name;
                    mi["file_uuid"] = uuid;
                    mi["file_size"] = (int)file_data.size();
                    record_sent_message(group, "[文件:" + file_name + "]", mi);
                }
                return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
            }
            return HttpResponse().json("{\"error\":\"invalid multipart\"}");
        }
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string url = body["url"].asString();
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            std::string file_name = body["file_name"].asString();
            if (file_name.empty()) file_name = body["name"].asString();
            std::string uuid = body["uuid"].asString();
            int file_size = body["size"].asInt();
            bool ok = send_group_file(group, url, file_name, uuid, file_size);
            return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    // ── @ 某人 ──
    if (path == "/api/send-at" || path == "/api/send_at") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string text = body["text"].asString();
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            std::string user_id = body["user_id"].asString();
            if (user_id.empty()) user_id = body["at_user_id"].asString();
            if (user_id.empty()) user_id = body["target_id"].asString();
            std::string display_name = body["nickname"].asString();
            if (display_name.empty()) display_name = body["at_nickname"].asString();
            bool ok = send_group_at(group, text, user_id, display_name);
            return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    // ── 批量 @ 多人 ──
    if (path == "/api/send-multi-at" || path == "/api/send_multi_at") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string text = body["text"].asString();
            std::string group = body["group_code"].asString();
            if (group.empty()) group = body["group"].asString();
            if (group.empty()) group = config_.default_target();
            std::vector<std::pair<std::string, std::string>> at_users;
            if (body["user_ids"].arr.size() > 0) {
                for (auto& uid : body["user_ids"].arr)
                    at_users.push_back({ uid.asString(), "" });
            } else if (body["users"].arr.size() > 0) {
                for (auto& u : body["users"].arr) {
                    std::string uid = u["user_id"].asString();
                    std::string nick = u["nickname"].asString();
                    if (!uid.empty()) at_users.push_back({ uid, nick });
                }
            }
            bool ok = send_group_multi_at(group, text, at_users);
            return HttpResponse().json("{\"ok\":" + std::string(ok ? "true" : "false")
                                       + ",\"at_count\":" + std::to_string(at_users.size()) + "}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }

    // ── 缺失端点补充 ──
    if (path == "/api/diag/at-all") {
        // 前端 sendAtAll() 读取 diag.connected / diag.group_code
        std::ostringstream o;
        o << "{\"ok\":true,\"connected\":" << (bot_connected_.load() ? "true" : "false")
          << ",\"group_code\":\"" << config_.default_target() << "\"}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/send/ai-image") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string prompt = body["prompt"].asString();
            std::string group = body["group_code"].asString();
            if (group.empty()) group = config_.default_target();
            if (!prompt.empty()) {
                send_group_text(group, "[AI图片请求] " + prompt);
                return HttpResponse().json("{\"ok\":true,\"message\":\"AI图片已请求\"}");
            }
        }
        return HttpResponse().json("{\"error\":\"prompt required\"}");
    }
    if (path == "/api/plugins") {
        std::ostringstream o;
        o << "{\"ok\":true,\"plugins\":[";
        std::lock_guard<std::mutex> pl(plugins_mu_);
        for (size_t i = 0; i < plugins_.size(); i++) {
            auto& p = plugins_[i];
            o << "{"
              << "\"name\":" << json_quote(p.name) << ","
              << "\"version\":" << json_quote(p.version.empty() ? "1.0.0" : p.version) << ","
              << "\"author\":" << json_quote(p.author) << ","
              << "\"description\":" << json_quote(p.description) << ","
              << "\"active\":" << (p.active ? "true" : "false") << ","
              << "\"error\":" << json_quote(p.error) << ","
              << "\"message_handlers\":" << p.commands.size() << ","
              << "\"routes\":" << p.commands.size() << ","
              << "\"pages\":[],\"cards\":[]"
              << "}";
            if (i + 1 < plugins_.size()) o << ",";
        }
        o << "]}";
        return HttpResponse().json(o.str());
    }
    if (path == "/api/plugins/install") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string name = body["name"].asString();
            std::string content = body["content"].asString();
            std::string url = body["url"].asString();
            if (content.empty() && !url.empty()) {
                // 从 URL 下载 JSON 插件（raw JSON 或 github raw）
                std::string host, path2 = "/", body2;
                size_t scheme = url.find("://");
                size_t slash = url.find('/', (scheme == std::string::npos) ? 0 : scheme + 3);
                if (slash == std::string::npos) { host = url.substr(scheme == std::string::npos ? 0 : scheme + 3); }
                else {
                    host = url.substr(scheme == std::string::npos ? 0 : scheme + 3, slash - (scheme == std::string::npos ? 0 : scheme + 3));
                    path2 = url.substr(slash);
                }
                if (!host.empty()) {
                    int ret = https_request("GET", host, 443, path2, "", "application/json", {}, content, 20000);
                    if (ret != 0) return HttpResponse().json("{\"error\":\"下载失败\"}");
                }
            }
            JsonVal pj;
            if (content.empty() || !json_parse(content, pj)) {
                return HttpResponse().json("{\"error\":\"插件内容不是有效 JSON（格式见 README 插件生态章节）\"}");
            }
            PluginInfo pi;
            pi.name = pj["name"].asString();
            if (pi.name.empty()) pi.name = name;
            if (pi.name.empty()) pi.name = "plugin";
            pi.version = pj["version"].asString();
            pi.author = pj["author"].asString();
            pi.description = pj["description"].asString();
            pi.active = (pj["active"].is_bool() && pj["active"].asBool())
                        || (!pj["active"].is_bool() && pj["active"].asString() != "false");
            for (auto& c : pj["commands"].arr) {
                PluginCommand pc;
                pc.command = c["command"].asString();
                pc.reply = c["reply"].asString();
                if (!pc.command.empty()) pi.commands.push_back(pc);
            }
            if (pi.commands.empty()) return HttpResponse().json("{\"error\":\"插件缺少 commands（命令列表）\"}");
            // 若同名插件已存在则先禁用替换
            if (!save_plugin(pi)) return HttpResponse().json("{\"error\":\"保存插件文件失败\"}");
            load_plugins();
            return HttpResponse().json("{\"ok\":true,\"plugin\":\"" + pi.name + "\"}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/plugins/toggle") {
        JsonVal body;
        if (json_parse(req.body, body)) {
            std::string name = body["name"].asString();
            bool enabled = (body["enabled"].is_bool() && body["enabled"].asBool())
                           || (!body["enabled"].is_bool() && body["enabled"].asString() == "true");
            std::lock_guard<std::mutex> pl(plugins_mu_);
            for (auto& p : plugins_) {
                if (p.name == name) {
                    p.active = enabled;
                    save_plugin(p);
                    return HttpResponse().json("{\"ok\":true}");
                }
            }
            return HttpResponse().json("{\"error\":\"插件不存在\"}");
        }
        return HttpResponse().json("{\"error\":\"invalid params\"}");
    }
    if (path == "/api/plugins/reload") {
        load_plugins();
        return HttpResponse().json("{\"ok\":true}");
    }
    if (path.find("/api/group/name") == 0) {
        std::ostringstream o;
        o << "{\"ok\":true,\"group_name\":\"" << config_.default_target() << "\"}";
        return HttpResponse().json(o.str());
    }

    return HttpResponse().not_found();
}

// =====================================================================
//  HTTP 路由
// =====================================================================

// 安全发送 HTTP 响应并在完成后关闭连接
// ← 修复：原先单次 send() 发送整个响应，大响应（如 base64 图片）可能被截断。
//   现改为循环发送直至全部送出（兼容部分系统/浏览器一次只收部分数据）。
static void http_respond_and_close(int fd, const Bytes& data) {
    if (data.empty()) { close_socket(fd); return; }
    size_t sent_total = 0;
    while (sent_total < data.size()) {
        int n = send(fd, (const char*)data.data() + sent_total, (int)(data.size() - sent_total), 0);
        if (n <= 0) break;   // 连接中断
        sent_total += (size_t)n;
    }
    close_socket(fd);
}

void YuanbaoServer::route_http(int fd) {
    try {
        // 设置接收超时，防止客户端不发送完整请求导致卡死
#ifdef _WIN32
        DWORD timeout = 5000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        // 循环接收直到 header+body 完整（TCP 可能把请求头和数据体分成多个包）
        // 修复：支持大文件上传（图片/文件），按 Content-Length 循环直到收满（最大 ~32MB）
        Bytes data;
        char raw_buf[16384];
        size_t need_total = 0;
        for (int i = 0; i < 2048; i++) {
            int recv_result = recv(fd, raw_buf, sizeof(raw_buf), 0);
            if (recv_result <= 0) break;
            data.insert(data.end(), raw_buf, raw_buf + recv_result);

            if (need_total == 0) {
                std::string s((char*)data.data(), data.size());
                size_t hdr_end = s.find("\r\n\r\n");
                if (hdr_end == std::string::npos) continue;  // header 未收完
                // WebSocket 升级：header 完整即可，不再读取（避免吞掉后续 WS 帧）
                if (s.find("Upgrade: websocket") != std::string::npos) break;
                // 计算需要接收的总长度（Content-Length 大小写不敏感匹配）
                std::string lower_hdr = s.substr(0, hdr_end);
                std::transform(lower_hdr.begin(), lower_hdr.end(), lower_hdr.begin(), ::tolower);
                size_t cl_pos = lower_hdr.find("content-length:");
                size_t total = hdr_end + 4;
                if (cl_pos != std::string::npos) {
                    size_t val_start = cl_pos + 15;
                    size_t val_end = s.find("\r\n", val_start);
                    if (val_end != std::string::npos) {
                        int need = std::atoi(s.substr(val_start, val_end - val_start).c_str());
                        if (need > 0) total += (size_t)need;
                    }
                }
                need_total = total;
            }
            if (need_total > 0 && data.size() >= need_total) break;  // 请求完整
        }
        if (data.empty()) { close_socket(fd); return; }

        HttpRequest req;
        if (!parse_http_request(data, req)) { close_socket(fd); return; }

        // CORS 预检
        if (req.method == "OPTIONS") {
            std::string resp = "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
                "Access-Control-Max-Age: 86400\r\n\r\n";
            Bytes rb(resp.begin(), resp.end());
            http_respond_and_close(fd, rb); return;
        }

        // WebSocket 升级
        if (req.headers.count("Upgrade") && req.headers.at("Upgrade") == "websocket") {
            route_websocket(fd, req); return;
        }

        // SSE 事件流
        if (req.path == "/api/events" || req.path == "/events") {
            std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\nX-Accel-Buffering: no\r\n\r\n";
            YB_SEND(fd, hdr.data(), hdr.size(), 0);

            SSEBroker::ClientCb client_cb = [fd](const std::string& msg) {
                YB_SEND(fd, msg.data(), msg.size(), 0);
            };
            SSEBroker sse_broker;
            sse_broker.add_client(client_cb);

            std::ostringstream init;
            init << "event: status\ndata: {\"connected\":true,\"cpp\":true}\n\n";
            YB_SEND(fd, init.str().data(), init.str().size(), 0);

            // 定期 ping，保持连接
            for (int i = 0; i < 300; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::string ping = "event: ping\ndata: {\"t\":" + std::to_string(util::now_ms()) + "}\n\n";
                if (YB_SEND(fd, ping.data(), ping.size(), 0) <= 0) break;
            }
            close_socket(fd); return;
        }

        // API 路由
        if (req.path.rfind("/api", 0) == 0 || req.path.rfind("/flood", 0) == 0) {
            auto resp = route_api(req);
            auto http_resp = build_http_response_bytes(resp);
            http_respond_and_close(fd, http_resp); return;
        }

        // 静态文件
        // ← 修复：加 Cache-Control: no-cache，防止浏览器缓存旧版 index.html/app.js/style.css
        //   （曾导致"主题切换无效"、改版不生效等问题；服务端每次从磁盘读最新文件）
        std::string fp = resolve_path(req.path);
        if (!fp.empty()) {
            std::ifstream f(fp, std::ios::binary);
            if (f.good()) {
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                HttpResponse r;
                r.body = content;
                r.headers["Content-Type"] = util::mime_type(fp);
                r.headers["Access-Control-Allow-Origin"] = "*";
                r.headers["Cache-Control"] = "no-cache, must-revalidate";
                r.headers["Pragma"] = "no-cache";
                r.headers["Connection"] = "close";
                auto http_resp = build_http_response_bytes(r);
                http_respond_and_close(fd, http_resp); return;
            }
        }

        // 根路径默认 -> index.html
        if (req.path == "/" || req.path.empty()) {
            std::ifstream f("index.html", std::ios::binary);
            if (f.good()) {
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                HttpResponse r;
                r.body = content;
                r.headers["Content-Type"] = "text/html; charset=utf-8";
                r.headers["Access-Control-Allow-Origin"] = "*";
                r.headers["Cache-Control"] = "no-cache, must-revalidate";
                r.headers["Pragma"] = "no-cache";
                r.headers["Connection"] = "close";
                auto http_resp = build_http_response_bytes(r);
                http_respond_and_close(fd, http_resp); return;
            }
        }

        HttpResponse r404; r404.not_found();
        auto resp = build_http_response_bytes(r404);
        http_respond_and_close(fd, resp);
    } catch (const std::exception& e) {
        std::cerr << "[HTTP] error: " << e.what() << std::endl;
        close_socket(fd);
    }
}

// =====================================================================
//  WebSocket 路由（前端控制台）
// =====================================================================
void YuanbaoServer::route_websocket(int fd, const HttpRequest& req) {
    auto it = req.headers.find("Sec-WebSocket-Key");
    if (it == req.headers.end()) { close_socket(fd); return; }
    std::string ws_key = it->second;
    std::string accept_str = ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    uint8_t sha_out[20];
    sha1_raw((uint8_t*)accept_str.data(), accept_str.size(), sha_out);
    std::string accept_b64 = proto::base64_encode(sha_out, 20);

    std::ostringstream resp;
    resp << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept_b64 << "\r\n\r\n";
    YB_SEND(fd, resp.str().data(), resp.str().size(), 0);
    add_fe_ws(fd);

    std::vector<char> buf(8192, 0);
    while (true) {
        int n = recv(fd, buf.data(), (int)buf.size(), 0);
        if (n <= 0) break;
        size_t pos = 0;
        while (pos < (size_t)n) {
            proto::WSFrame frame = proto::parse_ws_frame((uint8_t*)buf.data(), (size_t)n, pos);
            if (frame.opcode == 8) {
                auto close_frame = proto::build_ws_frame(8, {});
                YB_SEND(fd, close_frame.data(), close_frame.size(), 0);
                break;
            }
            if (frame.opcode == 1 && !frame.payload.empty()) {
                std::string text(frame.payload.begin(), frame.payload.end());
                JsonVal req_json;
                if (json_parse(text, req_json)) {
                    std::string action = req_json["action"].asString();
                    JsonVal out;
                    out["timestamp"] = (double)util::now_ms();

                    if (action == "ping") {
                        out["type"] = "pong";
                    } else if (action == "connect") {
                        bool ok = bot_connect();
                        out["type"] = "connecting";
                        out["ok"] = ok;
                    } else if (action == "disconnect") {
                        bot_disconnect();
                        out["type"] = "disconnected";
                    } else if (action == "send") {
                        std::string text2 = req_json["text"].asString();
                        std::string group = req_json["group"].asString();
                        if (group.empty()) group = config_.default_target();
                        bool ok = send_group_text(group, text2);
                        out["type"] = "sent"; out["ok"] = ok;
                    } else if (action == "flood_start") {
                        std::string id = flood_start(
                            req_json["text"].asString().empty() ? "test" : req_json["text"].asString(),
                            req_json["count"].asInt() > 0 ? req_json["count"].asInt() : 20,
                            req_json["delay"].asInt() > 0 ? req_json["delay"].asInt() : 50,
                            3, req_json["mode"].asString().empty() ? "random" : req_json["mode"].asString(),
                            req_json["group"].asString().empty() ? config_.default_target() : req_json["group"].asString()
                        );
                        out["type"] = "flood_started"; out["id"] = id;
                    } else if (action == "flood_cancel") {
                        flood_cancel(req_json["id"].asString());
                        out["type"] = "flood_cancelled";
                    } else if (action == "bot_status") {
                        out["type"] = "bot_status";
                        out["connected"] = bot_connected_.load();
                    } else {
                        out["type"] = "unknown_action";
                    }
                    std::string resp_str = json_compact(out);
                    auto frame_data = proto::build_ws_frame(1, Bytes(resp_str.begin(), resp_str.end()));
                    YB_SEND(fd, frame_data.data(), frame_data.size(), 0);
                }
            } else if (frame.opcode == 9) {
                auto pong = proto::build_ws_frame(10, frame.payload);
                YB_SEND(fd, pong.data(), pong.size(), 0);
            }
        }
    }
    remove_fe_ws(fd);
    close_socket(fd);
}

// =====================================================================
//  Flood 实现
// =====================================================================
std::string YuanbaoServer::flood_start(const std::string& text, int count, int delay,
                                        int batch, const std::string& mode,
                                        const std::string& group) {
    (void)batch;
    flood::FloodMode fm = flood::FloodMode::RANDOM;
    std::string m2 = mode;
    std::transform(m2.begin(), m2.end(), m2.begin(), ::tolower);
    if (m2 == "fullwidth") fm = flood::FloodMode::FULLWIDTH;
    else if (m2 == "mock" || m2 == "random") fm = flood::FloodMode::MOCK;
    else if (m2 == "zalgo") fm = flood::FloodMode::ZALGO;
    else if (m2 == "repeat") fm = flood::FloodMode::REPEAT;
    else if (m2 == "alternate") fm = flood::FloodMode::ALTERNATE;
    else if (m2 == "emoji") fm = flood::FloodMode::EMOJI;
    else if (m2 == "ghost") fm = flood::FloodMode::GHOST;
    else if (m2 == "matrix") fm = flood::FloodMode::MATRIX;
    else if (m2 == "earthquake") fm = flood::FloodMode::EARTHQUAKE;
    else if (m2 == "bubble") fm = flood::FloodMode::BUBBLE;
    else if (m2 == "firework") fm = flood::FloodMode::FIREWORK;
    else if (m2 == "rainbow") fm = flood::FloodMode::RAINBOW;
    else if (m2 == "storm") fm = flood::FloodMode::STORM;
    else if (m2 == "tsunami") fm = flood::FloodMode::TSUNAMI;
    else if (m2 == "quantum") fm = flood::FloodMode::QUANTUM;

    flood::FloodTask task;
    task.text = text; task.count = count; task.delay_ms = delay;
    task.batch_size = batch; task.mode = fm; task.group_code = group;
    return flood_engine_.create_task(task);
}

void YuanbaoServer::flood_cancel(const std::string& id) { flood_engine_.cancel_task(id); }

std::string YuanbaoServer::flood_stats() {
    auto fs = flood_engine_.get_stats();
    std::ostringstream oss;
    oss << "{\"total_tasks\":" << fs.total_tasks
        << ",\"total_sent\":" << fs.total_sent
        << ",\"active_tasks\":" << fs.active_tasks << "}";
    return oss.str();
}

// =====================================================================
//  构造 / 析构
// =====================================================================
YuanbaoServer::YuanbaoServer() {}
YuanbaoServer::~YuanbaoServer() { stop(); }

bool YuanbaoServer::load_config(const std::string& path) {
    if (!config_.load(path)) {
        std::cerr << "[Config] 加载失败: " << path << "\n";
        return false;
    }
    std::cout << "[Config] OK\n";
    std::cout << "  AppKey: " << config_.app_key.substr(0, 8) << "...\n";
    std::cout << "  API: " << config_.api_domain << "\n";
    std::cout << "  WS: " << config_.ws_url << "\n";
    std::cout << "  默认目标群: " << config_.default_target() << "\n";

    if (config_.llm.enabled) {
        std::cout << "  LLM 已启用: " << config_.llm.model << "\n";
        std::cout << "  LLM API: " << config_.llm.api_url << "\n";
    } else {
        std::cout << "  LLM: 未配置\n";
    }
    return true;
}

bool YuanbaoServer::start(int port) {
    if (running_.load()) return true;
    running_ = true;

    // 绑定刷屏引擎的发送回调（此前未设置，导致刷屏任务不发任何消息）
    flood_set_sender([this](const std::string& group, const std::string& text) {
        send_group_text(group, text);
    });

    // 加载 JSON 配置插件（plugins/*.json）
    load_plugins();

    // ← 补：扫描 ico 目录，建立贴纸名称 → 本地图标 映射（前端贴纸图标改用本地 ico）
    stickers_.scan_icons("ico");

    listen_fd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return false;
    yb_platform::set_reuseaddr(listen_fd_);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "WARNING: port " << port << " busy, trying +1\n";
        port++;
        addr.sin_port = htons((uint16_t)port);
        if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close_socket(listen_fd_); return false;
        }
    }
    if (listen(listen_fd_, 64) < 0) { close_socket(listen_fd_); return false; }

    // 多线程模型：单 acceptor 线程 accept + 每个连接一个独立线程处理
    // （避免多线程 accept 惊群，不受 CPU 线程数限制，阻塞请求互不影响）
    worker_threads_.emplace_back([this]() {
        while (running_.load()) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = (int)accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            int flag = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
            // 每连接一个线程：并发处理多个前端请求 / 长连接（SSE）
            std::thread([this, client_fd]() {
                try {
                    route_http(client_fd);
                } catch (const std::exception& e) {
                    std::cerr << "[Thread] route_http 异常: " << e.what() << "\n";
                } catch (...) {
                    std::cerr << "[Thread] route_http 未知异常\n";
                }
            }).detach();
        }
    });

    std::cout << "============================================================\n";
    std::cout << "  Yuanbao Bot Unified Server (C++)\n";
    std::cout << "============================================================\n";
    std::cout << "  Port:    " << port << "\n";
    std::cout << "  Frontend: http://127.0.0.1:" << port << "/\n";
    std::cout << "  WebSocket: ws://127.0.0.1:" << port << "\n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << "  No Python - Pure C++\n";
    std::cout << "  /api/flood/*     Flood engine (15 text transforms)\n";
    std::cout << "  /api/connect     Connect Yuanbao WebSocket\n";
    std::cout << "  /api/send        Send messages\n";
    std::cout << "  /api/stickers    Sticker library (80+ stickers)\n";
    std::cout << "  /api/messages    Message logs\n";
    std::cout << "============================================================\n";
    return true;
}

void YuanbaoServer::stop() {
    if (!running_.load()) return;
    running_ = false;
    bot_running_ = false;
    msg_logger_.stop();
    if (listen_fd_ >= 0) { close_socket(listen_fd_); listen_fd_ = -1; }
    for (auto& t : worker_threads_) if (t.joinable()) t.join();
    if (bot_ws_thread_.joinable()) bot_ws_thread_.join();
    if (bot_ws_fd_ >= 0) { close_socket(bot_ws_fd_); bot_ws_fd_ = -1; }
    std::cout << "[Server] Stopped\n";
}

// =====================================================================
//  main
// =====================================================================
int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 源码字符串为 UTF-8，Windows 控制台默认 GBK(CP936) 会显示乱码，切换到 UTF-8 代码页
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // 关闭 stdout/stderr 缓冲，保证输出顺序与实时刷新
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#endif

    std::cout << "============================================================\n";
    std::cout << "  Yuanbao Bot Unified Server\n";
    std::cout << "  C++ Only Backend (No Python)\n";
    std::cout << "============================================================\n";

    // 初始化网络
    if (!yb_platform::net_init()) {
        std::cerr << "FATAL: Network init failed\n";
        return 1;
    }

#ifdef YUANBAO_NO_OPENSSL
    std::cout << "  [INFO] OpenSSL disabled - using SHA1 fallback\n";
#else
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
#endif

    YuanbaoServer server;

    // 配置文件路径：优先命令行参数，然后当前目录
    std::string config_path = "config.json";
    if (argc > 2) config_path = argv[2];

    if (!server.load_config(config_path)) {
        std::cerr << "FATAL: Config load failed: " << config_path << "\n";
        return 1;
    }

    // 端口：命令行参数 > 配置文件 > 默认 5000
    int port = server.get_port();
    if (argc > 1) port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) port = server.get_port();

    if (!server.start(port)) {
        std::cerr << "FATAL: Server start failed\n";
        return 1;
    }

    std::cout << "  Running: http://127.0.0.1:" << port << "/\n\n";

#ifdef _WIN32
    signal(SIGINT, [](int) { std::cout << "\n[Exit]"; exit(0); });
#else
    signal(SIGINT, [](int) { std::cout << "\n[Exit]"; exit(0); });
    signal(SIGTERM, [](int) { std::cout << "\n[Exit]"; exit(0); });
#endif

    while (server.is_running())
        std::this_thread::sleep_for(std::chrono::seconds(1));

    yb_platform::net_cleanup();
    return 0;
}
