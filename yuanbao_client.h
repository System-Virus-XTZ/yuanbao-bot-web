/**
 * yuanbao_client.h - C++ 元宝 Bot 客户端（可选模块）
 *
 * 提供独立的 Bot 客户端功能，包含：
 *  - HMAC-SHA256 签名
 *  - WebSocket 客户端连接元宝服务器
 *  - ProtoBuf 消息编解码（扩展）
 *  - 消息日志
 *

 * 依赖：server.h（提供基础类型和 ProtoBuf）
 * 编译：需要 OpenSSL (-lssl -lcrypto)
 */
#pragma once

#include "server.h"

#ifndef YUANBAO_NO_OPENSSL
  #include <openssl/hmac.h>
  #include <openssl/sha.h>
#endif

namespace bot {

// ═══════════════════════════════════════════════
//  ProtoBuf 扩展（兼容 Python 实现的协议）
// ═══════════════════════════════════════════════
namespace proto {

inline Bytes encode_head(int cmd_type, const std::string& cmd, uint32_t seq_no,
                         const std::string& msg_id, const std::string& module) {
    Bytes data;
    bytes_append(data, encode_uint32(1, (uint32_t)cmd_type));
    bytes_append(data, encode_string(2, cmd));
    bytes_append(data, encode_uint32(3, seq_no));
    bytes_append(data, encode_string(4, msg_id));
    bytes_append(data, encode_string(5, module));
    return data;
}

inline Bytes encode_conn_msg(const Bytes& head, const Bytes& body) {
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

inline Bytes encode_send_group_msg(
    const std::string& msg_id, const std::string& group_code,
    const std::string& from_account, const std::string& text,
    const std::string& ref_msg_id = "", uint32_t rnd = 0) {
    Bytes data;
    bytes_append(data, encode_string(1, msg_id));
    bytes_append(data, encode_string(2, group_code));
    bytes_append(data, encode_string(3, from_account));
    if (rnd == 0)
        rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000);
    bytes_append(data, encode_uint32(5, rnd));
    bytes_append(data, encode_message(6, encode_text_elem(text)));
    if (!ref_msg_id.empty()) bytes_append(data, encode_string(7, ref_msg_id));
    return data;
}

inline Bytes encode_send_c2c_msg(
    const std::string& msg_id, const std::string& to_account,
    const std::string& from_account, const std::string& text,
    uint32_t rnd = 0) {
    Bytes data;
    bytes_append(data, encode_string(1, msg_id));
    bytes_append(data, encode_string(2, to_account));
    bytes_append(data, encode_string(3, from_account));
    if (rnd == 0)
        rnd = (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000);
    bytes_append(data, encode_uint32(4, rnd));
    bytes_append(data, encode_message(5, encode_text_elem(text)));
    return data;
}

inline Bytes encode_get_members_req(const std::string& group_code) {
    return encode_string(1, group_code);
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
    Bytes img_info;
    bytes_append(img_info, encode_uint32(1, 1));
    bytes_append(img_info, encode_uint32(2, (uint32_t)size));
    bytes_append(img_info, encode_uint32(3, (uint32_t)w));
    bytes_append(img_info, encode_uint32(4, (uint32_t)h));
    bytes_append(img_info, encode_string(5, url));
    Bytes mc;
    if (!uuid.empty()) bytes_append(mc, encode_string(2, uuid));
    bytes_append(mc, encode_uint32(3, 255));
    bytes_append(mc, encode_message(8, img_info));
    Bytes elem = encode_string(1, "TIMImageElem");
    bytes_append(elem, encode_message(2, mc));
    return elem;
}

} // namespace proto

// ═══════════════════════════════════════════════
//  HTTP 结果
// ═══════════════════════════════════════════════
struct HttpResult {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

// ═══════════════════════════════════════════════
//  SHA256 / HMAC 工具
// ═══════════════════════════════════════════════
#ifndef YUANBAO_NO_OPENSSL

inline std::string sha256_hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)data.data(), data.size(), hash);
    char buf[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(buf + i * 2, "%02x", hash[i]);
    return std::string(buf);
}

inline std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         (const unsigned char*)data.data(), data.size(),
         result, &result_len);
    char buf[EVP_MAX_MD_SIZE * 2 + 1];
    for (unsigned int i = 0; i < result_len; i++)
        sprintf(buf + i * 2, "%02x", result[i]);
    return std::string(buf, result_len * 2);
}

#endif // YUANBAO_NO_OPENSSL

// ═══════════════════════════════════════════════
//  WebSocket 客户端
// ═══════════════════════════════════════════════
class WSClient {
public:
    int fd = -1;

    bool connect_ws(const std::string& path, const std::string& host, int port) {
        fd = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;

        yb_platform::set_recv_timeout(fd, 10000);
        yb_platform::set_send_timeout(fd, 10000);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);

        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            struct hostent* he = gethostbyname(host.c_str());
            if (!he) { close_socket(fd); fd = -1; return false; }
            memcpy(&addr.sin_addr, he->h_addr, (size_t)he->h_length);
        }

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close_socket(fd); fd = -1; return false;
        }

        std::string raw_key;
        for (int i = 0; i < 16; i++) raw_key += (char)(rand() % 256);
        std::string ws_key = proto::base64_encode((const uint8_t*)raw_key.data(), raw_key.size());

        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << ":" << port << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << ws_key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n\r\n";

        std::string req_str = req.str();
        if (YB_SEND(fd, req_str.data(), req_str.size(), 0) < 0) {
            close_socket(fd); fd = -1; return false;
        }

        char buf[512];
        ssize_t n = YB_RECV(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close_socket(fd); fd = -1; return false; }
        buf[n] = '\0';

        if (std::string(buf).find("101") == std::string::npos) {
            close_socket(fd); fd = -1; return false;
        }
        return true;
    }

    bool send_frame(const Bytes& data) {
        if (fd < 0) return false;
        Bytes frame = proto::build_ws_frame(1, data);
        return YB_SEND(fd, frame.data(), frame.size(), 0) >= 0;
    }

    std::vector<Bytes> recv_frames() {
        std::vector<Bytes> results;
        if (fd < 0) return results;
        std::vector<char> buf(65536);
        ssize_t n = YB_RECV(fd, buf.data(), buf.size(), 0);
        if (n <= 0) return results;
        size_t pos = 0;
        while (pos < (size_t)n) {
            size_t p = 0;
            proto::WSFrame frm = proto::parse_ws_frame((uint8_t*)buf.data(), (size_t)n, p);
            if (frm.payload.empty()) break;
            if (frm.opcode == 1 || frm.opcode == 2)
                results.push_back(frm.payload);
            if (frm.opcode == 8) { close_socket(fd); fd = -1; return results; }
            pos = p;
        }
        return results;
    }

    void close_ws() {
        if (fd >= 0) {
            Bytes close_frame = proto::build_ws_frame(8, {});
            YB_SEND(fd, close_frame.data(), close_frame.size(), 0);
            close_socket(fd);
            fd = -1;
        }
    }
};

// ═══════════════════════════════════════════════
//  Bot 客户端（独立使用场景）
// ═══════════════════════════════════════════════
class YuanbaoBot {
public:
    BotConfig config;
    WSClient ws_client;
    MessageLogger msg_logger;

    std::atomic<bool> connected{false};
    std::string token;
    std::string bot_id;
    uint32_t seq_no = 0;
    std::atomic<bool> should_reconnect{false};

    std::vector<JsonVal> msg_cache;
    std::set<std::string> seen_msg_ids;

    std::map<std::string, JsonVal> stickers;
    std::function<void(const JsonVal& msg)> on_message;
    std::function<void(const std::string& group, const std::string& msg)> flood_sender;

    YuanbaoBot() : msg_logger("./logs") { init_stickers(); }

    void init_stickers() {
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
        for (auto& kv : data) {
            JsonVal s;
            s["sticker_id"] = kv.second;
            s["package_id"] = "1003";
            s["name"] = kv.first;
            s["width"] = 128;
            s["height"] = 128;
            s["formats"] = "png";
            stickers[kv.first] = s;
        }
    }

    // 鉴权
    bool sign_token() {
#ifndef YUANBAO_NO_OPENSSL
        std::string host = config.api_domain;
        std::string path = "/api/v5/robotLogic/sign-token";

        static const char chars[] = "0123456789abcdef";
        std::string nonce;
        for (int i = 0; i < 32; i++) nonce += chars[rand() % 16];

        time_t t = time(nullptr) + 8 * 3600;
        struct tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &t);
#else
        gmtime_r(&t, &tm_buf);
#endif
        char ts_buf[64];
        strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%S+08:00", &tm_buf);
        std::string timestamp = ts_buf;

        std::string plain = nonce + timestamp + config.app_key + config.app_secret;
        std::string sig = hmac_sha256_hex(config.app_secret, plain);

        JsonVal body;
        body["app_key"] = config.app_key;
        body["nonce"] = nonce;
        body["signature"] = sig;
        body["timestamp"] = timestamp;

        std::ostringstream cmd;
        cmd << "curl -s -k -X POST \"https://" << host << path << "\""
            << " -H \"Content-Type: application/json\""
            << " -H \"X-AppVersion: 1.0.11\""
            << " -H \"X-OperationSystem: linux\""
            << " -H \"X-Bot-Version: 2026.3.22\""
            << " -d '" << json_compact(body) << "'";

        FILE* fp = popen(cmd.str().c_str(), "r");
        if (!fp) return false;
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        pclose(fp);

        JsonVal rsp;
        if (!json_parse(std::string(buf, n), rsp) || rsp["code"].asInt() != 0)
            return false;
        token = rsp["data"]["token"].asString();
        bot_id = rsp["data"]["bot_id"].asString();
        return true;
#else
        return false;
#endif
    }

    // 连接
    bool connect() {
        if (token.empty() && !sign_token()) return false;

        std::string url = config.ws_url;
        std::string host;
        int port = 80;
        if (url.find("wss://") == 0) { url = url.substr(6); port = 443; }
        else if (url.find("ws://") == 0) { url = url.substr(5); }

        size_t pos = url.find('/');
        std::string hostport = (pos == std::string::npos) ? url : url.substr(0, pos);
        std::string path = (pos == std::string::npos) ? "/" : url.substr(pos);

        size_t colon = hostport.find(':');
        if (colon != std::string::npos) {
            host = hostport.substr(0, colon);
            port = std::stoi(hostport.substr(colon + 1));
        } else {
            host = hostport;
        }

        if (!ws_client.connect_ws(path, host, port)) return false;

        Bytes auth_data = proto::encode_auth_bind(bot_id, token);
        Bytes head = proto::encode_head(0, "auth-bind", seq_no++, util::random_id(32), "conn_access");
        Bytes msg = proto::encode_conn_msg(head, auth_data);
        if (!ws_client.send_frame(msg)) { ws_client.close_ws(); return false; }

        auto frames = ws_client.recv_frames();
        if (frames.empty()) { ws_client.close_ws(); return false; }

        connected = true;
        return true;
    }

    void disconnect() {
        connected = false;
        ws_client.close_ws();
    }

    std::string generate_msg_id() { return util::random_id(32); }

    bool send_group_msg(const std::string& group, const std::string& text,
                        const std::string& ref = "") {
        if (!connected) return false;
        std::string msg_id = generate_msg_id();
        Bytes data = proto::encode_send_group_msg(msg_id, group, bot_id, text, ref);
        Bytes head = proto::encode_head(0, "send_group_message", seq_no++, msg_id, "yuanbao_openclaw_proxy");
        Bytes msg = proto::encode_conn_msg(head, data);
        return ws_client.send_frame(msg);
    }

    bool send_c2c_msg(const std::string& to, const std::string& text) {
        if (!connected) return false;
        std::string msg_id = generate_msg_id();
        Bytes data = proto::encode_send_c2c_msg(msg_id, to, bot_id, text);
        Bytes head = proto::encode_head(0, "send_c2c_message", seq_no++, msg_id, "yuanbao_openclaw_proxy");
        Bytes msg = proto::encode_conn_msg(head, data);
        return ws_client.send_frame(msg);
    }

    bool send_sticker(const std::string& group, const std::string& sticker_id,
                      const std::string& package_id = "1003", const std::string& name = "sticker") {
        if (!connected) return false;
        std::string msg_id = generate_msg_id();
        JsonVal sd;
        sd["sticker_id"] = sticker_id; sd["package_id"] = package_id;
        sd["width"] = 128; sd["height"] = 128; sd["formats"] = "png"; sd["name"] = name;
        std::string sticker_json = json_compact(sd);
        Bytes elem = proto::encode_face_elem(sticker_json);
        Bytes body;
        bytes_append(body, proto::encode_string(1, msg_id));
        bytes_append(body, proto::encode_string(2, group));
        bytes_append(body, proto::encode_string(3, bot_id));
        bytes_append(body, proto::encode_uint32(5, (uint32_t)(rand() % 1000000000)));
        bytes_append(body, proto::encode_message(6, elem));
        Bytes head = proto::encode_head(0, "send_group_message", seq_no++, msg_id, "yuanbao_openclaw_proxy");
        Bytes msg = proto::encode_conn_msg(head, body);
        return ws_client.send_frame(msg);
    }

    bool send_image(const std::string& group, const std::string& url,
                    const std::string& uuid = "", int size = 0, int w = 0, int h = 0) {
        if (!connected) return false;
        std::string msg_id = generate_msg_id();
        Bytes elem = proto::encode_image_elem(url, uuid, size, w, h);
        Bytes body;
        bytes_append(body, proto::encode_string(1, msg_id));
        bytes_append(body, proto::encode_string(2, group));
        bytes_append(body, proto::encode_string(3, bot_id));
        bytes_append(body, proto::encode_uint32(5, (uint32_t)(rand() % 1000000000)));
        bytes_append(body, proto::encode_message(6, elem));
        Bytes head = proto::encode_head(0, "send_group_message", seq_no++, msg_id, "yuanbao_openclaw_proxy");
        Bytes msg = proto::encode_conn_msg(head, body);
        return ws_client.send_frame(msg);
    }

    // 接收循环
    void run_receiver() {
        std::thread([this]() {
            while (connected) {
                auto frames = ws_client.recv_frames();
                if (frames.empty()) {
                    if (connected) { connected = false; break; }
                    continue;
                }
                for (auto& frame : frames) {
                    handle_frame(frame);
                }
            }
        }).detach();
    }

    void handle_frame(const Bytes& data) {
        std::string text(data.begin(), data.end());
        JsonVal msg;
        if (!json_parse(text, msg)) return;

        std::string cmd = msg["cmd"].asString();
        if (cmd == "pong") return;

        std::string msg_id = msg["msg_id"].asString();
        if (seen_msg_ids.count(msg_id)) return;
        seen_msg_ids.insert(msg_id);
        if (seen_msg_ids.size() > 1000) {
            auto it = seen_msg_ids.begin();
            for (int i = 0; i < 100 && it != seen_msg_ids.end(); ++it, i++)
                seen_msg_ids.erase(seen_msg_ids.begin());
        }

        msg_cache.push_back(msg);
        if (msg_cache.size() > 500) msg_cache.erase(msg_cache.begin());

        msg_logger.log(msg);

        if (on_message) {
            on_message(msg);
        }
    }

    void do_send(const std::string& group, const std::string& msg) {
        send_group_msg(group, msg);
    }

    void load_from_config(const BotConfig& cfg) {
        config = cfg;
    }
};

} // namespace bot
