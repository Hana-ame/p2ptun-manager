#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>

namespace p2ptun {

namespace {

std::string RandomHex(size_t hex_chars) {
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    std::string out;
    out.reserve(hex_chars);
    for (size_t i = 0; i < hex_chars; ++i) {
        out.push_back(kHex[dist(gen)]);
    }
    return out;
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << int(c) << std::nouppercase;
        }
    }
    return escaped.str();
}

std::string UrlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            auto hexVal = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hexVal(value[i + 1]);
            int lo = hexVal(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (value[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(value[i]);
    }
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    oss << buf;
                } else {
                    oss << c;
                }
                break;
        }
    }
    return oss.str();
}

struct JsonValue {
    enum Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = kNull;
    bool bool_val = false;
    double num_val = 0.0;
    int64_t int_val = 0;
    std::string str_val;
    std::vector<JsonValue> arr_val;
    std::map<std::string, JsonValue> obj_val;

    const JsonValue* Get(const std::string& key) const {
        if (type != kObject) return nullptr;
        auto it = obj_val.find(key);
        return it == obj_val.end() ? nullptr : &it->second;
    }

    std::string GetString(const std::string& key, const std::string& def = "") const {
        const JsonValue* v = Get(key);
        if (!v) return def;
        if (v->type == kString) return v->str_val;
        if (v->type == kNumber) return std::to_string(v->int_val);
        return def;
    }

    int64_t GetInt64(const std::string& key, int64_t def = 0) const {
        const JsonValue* v = Get(key);
        if (!v) return def;
        if (v->type == kNumber) return v->int_val;
        if (v->type == kString && !v->str_val.empty()) {
            char* endp = nullptr;
            long long val = std::strtoll(v->str_val.c_str(), &endp, 10);
            if (endp != v->str_val.c_str()) return static_cast<int64_t>(val);
        }
        return def;
    }

    uint64_t GetUint64(const std::string& key, uint64_t def = 0) const {
        const JsonValue* v = Get(key);
        if (!v) return def;
        if (v->type == kNumber) return v->int_val < 0 ? 0 : static_cast<uint64_t>(v->int_val);
        return def;
    }

    bool GetBool(const std::string& key, bool def = false) const {
        const JsonValue* v = Get(key);
        if (!v) return def;
        if (v->type == kBool) return v->bool_val;
        if (v->type == kString) {
            std::string low = ToLower(Trim(v->str_val));
            return low == "true" || low == "1" || low == "yes";
        }
        return def;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : src_(src), pos_(0) {}

    bool Parse(JsonValue& out, std::string& err) {
        SkipWhitespaceAndComments();
        if (!ParseValue(out, err)) return false;
        SkipWhitespaceAndComments();
        return true;
    }

private:
    const std::string& src_;
    size_t pos_;

    void SkipWhitespaceAndComments() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                pos_ += 2;
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            if (c == '#') {
                ++pos_;
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
                continue;
            }
            break;
        }
    }

    bool ParseValue(JsonValue& out, std::string& err) {
        SkipWhitespaceAndComments();
        if (pos_ >= src_.size()) {
            err = "JSON 意外结束";
            return false;
        }
        char c = src_[pos_];
        if (c == '{') return ParseObject(out, err);
        if (c == '[') return ParseArray(out, err);
        if (c == '"') {
            out.type = JsonValue::kString;
            return ParseString(out.str_val, err);
        }
        if (c == 't' || c == 'f') return ParseBool(out, err);
        if (c == 'n') return ParseNull(out, err);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber(out, err);
        err = std::string("非法 JSON 字符: ") + c;
        return false;
    }

    bool ParseString(std::string& out, std::string& err) {
        if (src_[pos_] != '"') {
            err = "期望字符串引号";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= src_.size()) {
                    err = "字符串转义未闭合";
                    return false;
                }
                char esc = src_[pos_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (pos_ + 4 > src_.size()) {
                            err = "非法的 \\u 转义";
                            return false;
                        }
                        std::string hex = src_.substr(pos_, 4);
                        pos_ += 4;
                        unsigned int cp = static_cast<unsigned int>(std::strtoul(hex.c_str(), nullptr, 16));
                        if (cp <= 0x7F) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp <= 0x7FF) {
                            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        err = "字符串缺少闭合引号";
        return false;
    }

    bool ParseBool(JsonValue& out, std::string& err) {
        if (src_.compare(pos_, 4, "true") == 0) {
            out.type = JsonValue::kBool;
            out.bool_val = true;
            pos_ += 4;
            return true;
        }
        if (src_.compare(pos_, 5, "false") == 0) {
            out.type = JsonValue::kBool;
            out.bool_val = false;
            pos_ += 5;
            return true;
        }
        err = "非法的布尔值";
        return false;
    }

    bool ParseNull(JsonValue& out, std::string& err) {
        if (src_.compare(pos_, 4, "null") == 0) {
            out.type = JsonValue::kNull;
            pos_ += 4;
            return true;
        }
        err = "非法的 null 值";
        return false;
    }

    bool ParseNumber(JsonValue& out, std::string& err) {
        size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
        }
        std::string token = src_.substr(start, pos_ - start);
        if (token.empty() || token == "-") {
            err = "非法的数字";
            return false;
        }
        out.type = JsonValue::kNumber;
        out.num_val = std::strtod(token.c_str(), nullptr);
        out.int_val = static_cast<int64_t>(std::strtoll(token.c_str(), nullptr, 10));
        return true;
    }

    bool ParseArray(JsonValue& out, std::string& err) {
        out.type = JsonValue::kArray;
        out.arr_val.clear();
        ++pos_;
        SkipWhitespaceAndComments();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return true;
        }
        while (pos_ < src_.size()) {
            SkipWhitespaceAndComments();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            JsonValue elem;
            if (!ParseValue(elem, err)) return false;
            out.arr_val.push_back(std::move(elem));
            SkipWhitespaceAndComments();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }
            err = "数组中缺少 ',' 或 ']'";
            return false;
        }
        err = "数组未闭合";
        return false;
    }

    bool ParseObject(JsonValue& out, std::string& err) {
        out.type = JsonValue::kObject;
        out.obj_val.clear();
        ++pos_;
        SkipWhitespaceAndComments();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return true;
        }
        while (pos_ < src_.size()) {
            SkipWhitespaceAndComments();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            std::string key;
            if (!ParseString(key, err)) return false;
            SkipWhitespaceAndComments();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                err = "对象键后缺少 ':'";
                return false;
            }
            ++pos_;
            JsonValue val;
            if (!ParseValue(val, err)) return false;
            out.obj_val[key] = std::move(val);
            SkipWhitespaceAndComments();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }
            err = "对象中缺少 ',' 或 '}'";
            return false;
        }
        err = "对象未闭合";
        return false;
    }
};

TunnelRule RuleFromJsonValue(const JsonValue& v, const std::string& forced_mode, const std::string& def_proto, int def_bw) {
    TunnelRule r;
    r.id = v.GetString("id");
    r.mode = v.GetString("mode", forced_mode);
    r.remark = v.GetString("remark");
    r.name = v.GetString("name");
    if (r.name.empty()) {
        r.name = v.GetString("peer");
    }
    r.port = v.GetString("port");
    if (r.port.empty()) {
        std::string target = v.GetString("target");
        std::string listen = v.GetString("listen");
        r.port = !target.empty() ? target : listen;
    }
    r.secret = v.GetString("secret");
    r.proto = v.GetString("proto", def_proto);
    r.bandwidth_mbps = static_cast<int>(v.GetInt64("bandwidth_mbps", v.GetInt64("bandwidth", def_bw)));
    r.on_demand = v.GetBool("on_demand", false);
    r.enabled = v.GetBool("enabled", false);
    // 高级穿透参数 (均为可选；留空 = 不传，走 p2ptun 内置默认)
    r.broker = v.GetString("broker");
    r.stun = v.GetString("stun");
    r.turn = v.GetString("turn");
    r.proxy = v.GetString("proxy");
    r.peer_key = v.GetString("peer_key", v.GetString("key"));
    r.keepalive = v.GetString("keepalive");
    r.ice_ports = v.GetString("ice_ports");
    r.nat1to1 = v.GetString("nat1to1");
    r.idle_timeout = v.GetString("idle_timeout");
    r.keep_proxy = v.GetBool("keep_proxy", false);
    r.force_relay = v.GetBool("force_relay", false);
    r.max_peers = static_cast<int>(v.GetInt64("max_peers", 0));
    r.auto_restart = v.GetBool("auto_restart", true);
    r.serial_port = v.GetString("serial_port");
    r.serial_baud = static_cast<int>(v.GetInt64("serial_baud", v.GetInt64("baud", 115200)));
    r.serial_params = v.GetString("serial_params", "8N1");
    return NormalizeRule(r, forced_mode, def_proto, def_bw);
}

void WriteRuleJson(std::ostringstream& oss, const TunnelRule& r, int indent) {
    std::string pad(indent, ' ');
    std::string pad2(indent + 2, ' ');
    oss << pad << "{\n";
    oss << pad2 << "\"id\": \"" << JsonEscape(r.id) << "\",\n";
    oss << pad2 << "\"remark\": \"" << JsonEscape(r.remark) << "\",\n";
    oss << pad2 << "\"name\": \"" << JsonEscape(r.name) << "\",\n";
    oss << pad2 << "\"port\": \"" << JsonEscape(r.port) << "\",\n";
    if (!r.secret.empty()) {
        oss << pad2 << "\"secret\": \"" << JsonEscape(r.secret) << "\",\n";
    }
    oss << pad2 << "\"proto\": \"" << JsonEscape(NormalizeProto(r.proto)) << "\",\n";
    oss << pad2 << "\"bandwidth_mbps\": " << (r.bandwidth_mbps > 0 ? r.bandwidth_mbps : 100) << ",\n";
    if (r.mode == "connect" && r.on_demand) {
        oss << pad2 << "\"on_demand\": true,\n";
    }
    if (!Trim(r.broker).empty())    oss << pad2 << "\"broker\": \"" << JsonEscape(r.broker) << "\",\n";
    if (!Trim(r.stun).empty())      oss << pad2 << "\"stun\": \"" << JsonEscape(r.stun) << "\",\n";
    if (!Trim(r.turn).empty())      oss << pad2 << "\"turn\": \"" << JsonEscape(r.turn) << "\",\n";
    if (!Trim(r.proxy).empty())     oss << pad2 << "\"proxy\": \"" << JsonEscape(r.proxy) << "\",\n";
    if (!Trim(r.peer_key).empty())  oss << pad2 << "\"peer_key\": \"" << JsonEscape(r.peer_key) << "\",\n";
    if (!Trim(r.keepalive).empty()) oss << pad2 << "\"keepalive\": \"" << JsonEscape(r.keepalive) << "\",\n";
    if (!Trim(r.ice_ports).empty()) oss << pad2 << "\"ice_ports\": \"" << JsonEscape(r.ice_ports) << "\",\n";
    if (!Trim(r.nat1to1).empty())   oss << pad2 << "\"nat1to1\": \"" << JsonEscape(r.nat1to1) << "\",\n";
    if (!Trim(r.idle_timeout).empty()) oss << pad2 << "\"idle_timeout\": \"" << JsonEscape(r.idle_timeout) << "\",\n";
    if (r.keep_proxy)               oss << pad2 << "\"keep_proxy\": true,\n";
    if (r.force_relay)              oss << pad2 << "\"force_relay\": true,\n";
    if (r.max_peers > 0)            oss << pad2 << "\"max_peers\": " << r.max_peers << ",\n";
    if (!r.auto_restart)            oss << pad2 << "\"auto_restart\": false,\n";
    if (!Trim(r.serial_port).empty()) {
        oss << pad2 << "\"serial_port\": \"" << JsonEscape(r.serial_port) << "\",\n";
        oss << pad2 << "\"serial_baud\": " << (r.serial_baud > 0 ? r.serial_baud : 115200) << ",\n";
        oss << pad2 << "\"serial_params\": \"" << JsonEscape(Trim(r.serial_params).empty() ? "8N1" : r.serial_params) << "\",\n";
    }
    oss << pad2 << "\"enabled\": " << (r.enabled ? "true" : "false") << "\n";
    oss << pad << "}";
}

} // namespace

std::vector<ScenePreset> GetScenePresets() {
    return {
        {"自定义端口 (自由填写)", "暴露本机服务", "连接远程服务", "my-tunnel", "8080", "18080"},
        {"🖥 远程桌面 RDP (3389)", "本机远程桌面 (RDP)", "连接家里远程桌面", "home-rdp", "3389", "13389"},
        {"💻 SSH / WSL2 终端 (22)", "本机 SSH 服务", "连接远程 SSH", "home-ssh", "22", "2222"},
        {"🌐 SOCKS5 / HTTP 代理 (1080)", "本机代理端口 (1080)", "连接远程代理 (1080)", "my-proxy", "1080", "1080"},
        {"🚀 Clash 混合代理 (7890)", "本机 Clash 代理", "连接远程 Clash", "my-clash", "7890", "17890"},
        {"🏠 Web / NAS 面板 (8080)", "本机 Web 面板", "连接远程 Web 面板", "home-web", "8080", "18080"},
        {"🎮 Minecraft 联机 (25565)", "MC 联机服务端", "连接好友 MC 服", "mc-server", "25565", "25565"},
        {"🎮 幻兽帕鲁 / 游戏服 (8211)", "游戏联机端口", "连接好友游戏服", "game-server", "8211", "8211"}
    };
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string NormalizeProto(const std::string& raw) {
    std::string low = ToLower(Trim(raw));
    if (low == "hy2" || low == "hysteria2" || low == "hysteria") return "hy2";
    if (low == "quic") return "quic";
    if (low == "dc-quic" || low == "dcquic") return "dc-quic";
    if (low == "classic" || low == "sctp" || low == "dc" || low == "datachannel") return "classic";
    return "hy2";
}

std::string NormalizeEndpoint(const std::string& raw, const std::string& fallback_port) {
    std::string s = Trim(raw);
    std::string fb = Trim(fallback_port);
    if (fb.empty()) fb = "3389";
    if (s.empty()) {
        return "127.0.0.1:" + fb;
    }

    bool all_digits = !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
    if (all_digits) {
        long port = std::strtol(s.c_str(), nullptr, 10);
        if (port > 0 && port <= 65535) {
            return "127.0.0.1:" + std::to_string(port);
        }
    }

    if (s.size() > 1 && s[0] == ':') {
        std::string p = s.substr(1);
        bool p_digits = std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c); });
        if (p_digits) {
            long port = std::strtol(p.c_str(), nullptr, 10);
            if (port > 0 && port <= 65535) {
                return "127.0.0.1:" + std::to_string(port);
            }
        }
    }

    return s;
}

std::string ExtractPortOnly(const std::string& endpoint) {
    std::string s = Trim(endpoint);
    auto pos = s.rfind(':');
    if (pos != std::string::npos && pos + 1 < s.size()) {
        return s.substr(pos + 1);
    }
    return s;
}

std::string SuggestConnectPort(const std::string& expose_endpoint) {
    std::string p = ExtractPortOnly(expose_endpoint);
    if (p == "3389") return "13389";
    if (p == "22") return "2222";
    if (p == "80") return "18080";
    if (p == "443") return "18443";
    if (p == "445") return "10445";
    if (p == "3306") return "13306";
    if (p == "5432") return "15432";
    return p.empty() ? "1080" : p;
}

TunnelRule NormalizeRule(TunnelRule r, const std::string& forced_mode, const std::string& def_proto, int def_bw) {
    if (!forced_mode.empty()) {
        r.mode = forced_mode;
    }
    r.mode = ToLower(Trim(r.mode));
    if (r.mode != "expose" && r.mode != "connect") {
        r.mode = "expose";
    }
    r.name = Trim(r.name);
    if (r.name.empty()) {
        r.name = "tun-" + RandomHex(4);
    }
    r.id = Trim(r.id);
    if (r.id.empty()) {
        r.id = r.mode + "-" + r.name + "-" + RandomHex(4);
    }
    std::string def_port = (r.mode == "connect") ? "13389" : "3389";
    r.port = NormalizeEndpoint(r.port, def_port);
    r.secret = Trim(r.secret);
    r.proto = NormalizeProto(r.proto.empty() ? def_proto : r.proto);
    if (r.bandwidth_mbps <= 0) {
        r.bandwidth_mbps = (def_bw > 0) ? def_bw : 100;
    }
    r.remark = Trim(r.remark);
    if (r.remark.empty()) {
        r.remark = (r.mode == "expose" ? "暴露端口 " : "连接隧道 ") + ExtractPortOnly(r.port);
    }

    // 高级参数规范化：仅 trim + 合法性回退，留空即"不传该 flag"
    r.broker = Trim(r.broker);
    r.stun = Trim(r.stun);
    r.turn = Trim(r.turn);
    r.proxy = Trim(r.proxy);
    r.peer_key = Trim(r.peer_key);
    r.keepalive = Trim(r.keepalive);
    r.ice_ports = Trim(r.ice_ports);
    r.nat1to1 = Trim(r.nat1to1);
    r.idle_timeout = Trim(r.idle_timeout);
    if (r.max_peers < 0 || r.max_peers > 1024) {
        r.max_peers = 0;
    }
    if (r.serial_baud <= 0) {
        r.serial_baud = 115200;
    }
    r.serial_port = Trim(r.serial_port);
    r.serial_params = Trim(r.serial_params);
    if (r.serial_params.empty()) {
        r.serial_params = "8N1";
    }
    int db = 8; char parity = 'N'; int sb = 1;
    if (!ParseSerialParams(r.serial_params, db, parity, sb)) {
        r.serial_params = "8N1"; // 非法格式回退默认
    }
    return r;
}

void NormalizeAppConfig(AppConfig& cfg) {
    cfg.p2ptun_path = Trim(cfg.p2ptun_path);
    if (cfg.p2ptun_path.empty()) {
        cfg.p2ptun_path = "p2ptun.exe";
    }
    cfg.default_secret = Trim(cfg.default_secret);
    if (cfg.default_secret.empty()) {
        cfg.default_secret = "p2p-" + RandomHex(6);
    }
    cfg.default_proto = NormalizeProto(cfg.default_proto);
    if (cfg.default_bandwidth <= 0) {
        cfg.default_bandwidth = 100;
    }
    for (auto& r : cfg.expose) {
        r = NormalizeRule(r, "expose", cfg.default_proto, cfg.default_bandwidth);
    }
    for (auto& r : cfg.connect) {
        r = NormalizeRule(r, "connect", cfg.default_proto, cfg.default_bandwidth);
    }
}

AppConfig CreateDefaultAppConfig() {
    AppConfig cfg;
    cfg.p2ptun_path = "p2ptun.exe";
    cfg.default_secret = "p2p-" + RandomHex(6);
    cfg.default_proto = "hy2";
    cfg.default_bandwidth = 100;
    cfg.minimize_to_tray = true;

    TunnelRule exp_rdp;
    exp_rdp.id = "expose-rdp-default";
    exp_rdp.mode = "expose";
    exp_rdp.remark = "本机远程桌面 (RDP)";
    exp_rdp.name = "my-win-rdp";
    exp_rdp.port = "127.0.0.1:3389";
    exp_rdp.proto = "hy2";
    exp_rdp.bandwidth_mbps = 100;
    exp_rdp.enabled = false;

    TunnelRule exp_ssh;
    exp_ssh.id = "expose-ssh-default";
    exp_ssh.mode = "expose";
    exp_ssh.remark = "本机 SSH / WSL2";
    exp_ssh.name = "my-win-ssh";
    exp_ssh.port = "127.0.0.1:22";
    exp_ssh.proto = "hy2";
    exp_ssh.bandwidth_mbps = 100;
    exp_ssh.enabled = false;

    TunnelRule conn_rdp;
    conn_rdp.id = "connect-rdp-default";
    conn_rdp.mode = "connect";
    conn_rdp.remark = "连接远端桌面 (本地13389)";
    conn_rdp.name = "my-win-rdp";
    conn_rdp.port = "127.0.0.1:13389";
    conn_rdp.proto = "hy2";
    conn_rdp.bandwidth_mbps = 100;
    conn_rdp.enabled = false;

    TunnelRule conn_proxy;
    conn_proxy.id = "connect-proxy-default";
    conn_proxy.mode = "connect";
    conn_proxy.remark = "连接远端代理 (本地1080)";
    conn_proxy.name = "my-proxy";
    conn_proxy.port = "127.0.0.1:1080";
    conn_proxy.proto = "hy2";
    conn_proxy.bandwidth_mbps = 100;
    conn_proxy.enabled = false;

    cfg.expose = {exp_rdp, exp_ssh};
    cfg.connect = {conn_rdp, conn_proxy};
    return cfg;
}

std::string SerializeAppConfigToJson(const AppConfig& input_cfg) {
    AppConfig cfg = input_cfg;
    NormalizeAppConfig(cfg);

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"p2ptun_path\": \"" << JsonEscape(cfg.p2ptun_path) << "\",\n";
    oss << "  \"default_secret\": \"" << JsonEscape(cfg.default_secret) << "\",\n";
    oss << "  \"default_proto\": \"" << JsonEscape(cfg.default_proto) << "\",\n";
    oss << "  \"default_bandwidth\": " << cfg.default_bandwidth << ",\n";
    oss << "  \"minimize_to_tray\": " << (cfg.minimize_to_tray ? "true" : "false") << ",\n";

    oss << "  \"expose\": [";
    if (!cfg.expose.empty()) oss << "\n";
    for (size_t i = 0; i < cfg.expose.size(); ++i) {
        WriteRuleJson(oss, cfg.expose[i], 4);
        if (i + 1 < cfg.expose.size()) oss << ",";
        oss << "\n";
    }
    oss << (cfg.expose.empty() ? "],\n" : "  ],\n");

    oss << "  \"connect\": [";
    if (!cfg.connect.empty()) oss << "\n";
    for (size_t i = 0; i < cfg.connect.size(); ++i) {
        WriteRuleJson(oss, cfg.connect[i], 4);
        if (i + 1 < cfg.connect.size()) oss << ",";
        oss << "\n";
    }
    oss << (cfg.connect.empty() ? "]\n" : "  ]\n");
    oss << "}\n";
    return oss.str();
}

bool ParseAppConfigFromJson(const std::string& json_text, AppConfig& out_cfg, std::string& out_err) {
    JsonParser parser(json_text);
    JsonValue root;
    if (!parser.Parse(root, out_err)) {
        return false;
    }
    if (root.type != JsonValue::kObject) {
        out_err = "配置文件根节点必须是 JSON 对象 {...}";
        return false;
    }

    AppConfig cfg;
    cfg.p2ptun_path = root.GetString("p2ptun_path", "p2ptun.exe");
    cfg.default_secret = root.GetString("default_secret", "p2p-123456");
    cfg.default_proto = NormalizeProto(root.GetString("default_proto", "hy2"));
    cfg.default_bandwidth = static_cast<int>(root.GetInt64("default_bandwidth", 100));
    cfg.minimize_to_tray = root.GetBool("minimize_to_tray", true);

    if (const JsonValue* exp = root.Get("expose")) {
        if (exp->type == JsonValue::kArray) {
            for (const auto& item : exp->arr_val) {
                if (item.type == JsonValue::kObject) {
                    cfg.expose.push_back(RuleFromJsonValue(item, "expose", cfg.default_proto, cfg.default_bandwidth));
                }
            }
        }
    }

    if (const JsonValue* conn = root.Get("connect")) {
        if (conn->type == JsonValue::kArray) {
            for (const auto& item : conn->arr_val) {
                if (item.type == JsonValue::kObject) {
                    cfg.connect.push_back(RuleFromJsonValue(item, "connect", cfg.default_proto, cfg.default_bandwidth));
                }
            }
        }
    }

    if (const JsonValue* flat = root.Get("tunnels")) {
        if (flat->type == JsonValue::kArray) {
            for (const auto& item : flat->arr_val) {
                if (item.type == JsonValue::kObject) {
                    TunnelRule r = RuleFromJsonValue(item, "", cfg.default_proto, cfg.default_bandwidth);
                    if (r.mode == "connect") {
                        cfg.connect.push_back(r);
                    } else {
                        cfg.expose.push_back(r);
                    }
                }
            }
        }
    }

    NormalizeAppConfig(cfg);
    out_cfg = std::move(cfg);
    return true;
}

bool LoadAppConfigFromFile(const std::string& path, AppConfig& out_cfg, std::string& out_err) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        out_cfg = CreateDefaultAppConfig();
        std::string save_err;
        SaveAppConfigToFile(path, out_cfg, save_err);
        return true;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ParseAppConfigFromJson(ss.str(), out_cfg, out_err);
}

bool SaveAppConfigToFile(const std::string& path, const AppConfig& cfg, std::string& out_err) {
    std::string content = SerializeAppConfigToJson(cfg);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        out_err = "无法写入配置文件: " + path;
        return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs.good()) {
        out_err = "写入配置文件时发生 IO 错误: " + path;
        return false;
    }
    return true;
}

bool ParseStatusSnapshotJson(const std::string& json_text, StatusSnapshot& out_snap) {
    JsonParser parser(json_text);
    JsonValue root;
    std::string err;
    if (!parser.Parse(root, err) || root.type != JsonValue::kObject) {
        return false;
    }
    out_snap.reachable = true;
    out_snap.version = root.GetString("version");
    out_snap.pid = static_cast<int>(root.GetInt64("pid"));
    out_snap.mode = root.GetString("mode");
    out_snap.peer_id = root.GetString("peer_id");
    out_snap.remote_peer = root.GetString("remote_peer");
    out_snap.endpoint = root.GetString("endpoint");
    out_snap.proto = root.GetString("proto");
    out_snap.udp_endpoint = root.GetString("udp_endpoint");
    out_snap.broker = root.GetString("broker");
    out_snap.uptime_sec = root.GetInt64("uptime_sec");
    out_snap.uptime = root.GetString("uptime");
    out_snap.signaling_up = root.GetBool("signaling_up");
    out_snap.active_peers = root.GetInt64("active_peers");
    out_snap.active_streams = root.GetInt64("active_streams");
    out_snap.reconnects = root.GetUint64("reconnects");
    out_snap.bytes_tx = root.GetUint64("bytes_tx");
    out_snap.bytes_rx = root.GetUint64("bytes_rx");
    out_snap.last_rtt_ms = root.GetInt64("last_rtt_ms");
    out_snap.last_error = root.GetString("last_error");
    return true;
}

std::string BuildPairingUri(const TunnelRule& r, const std::string& default_secret) {
    std::string secret = Trim(r.secret).empty() ? Trim(default_secret) : Trim(r.secret);
    std::string port = (r.mode == "expose") ? SuggestConnectPort(r.port) : ExtractPortOnly(r.port);
    std::string proto = NormalizeProto(r.proto);
    int bw = r.bandwidth_mbps > 0 ? r.bandwidth_mbps : 100;

    std::ostringstream oss;
    oss << "p2ptun://connect?name=" << UrlEncode(r.name)
        << "&secret=" << UrlEncode(secret)
        << "&port=" << UrlEncode(port)
        << "&proto=" << UrlEncode(proto)
        << "&bw=" << bw;
    if (!r.remark.empty()) {
        oss << "&remark=" << UrlEncode(r.remark);
    }
    if (!Trim(r.broker).empty())    oss << "&broker=" << UrlEncode(Trim(r.broker));
    if (!Trim(r.stun).empty())      oss << "&stun=" << UrlEncode(Trim(r.stun));
    if (!Trim(r.turn).empty())      oss << "&turn=" << UrlEncode(Trim(r.turn));
    if (!Trim(r.proxy).empty())     oss << "&proxy=" << UrlEncode(Trim(r.proxy));
    if (!Trim(r.peer_key).empty() && Trim(r.peer_key) != "peerjs") oss << "&key=" << UrlEncode(Trim(r.peer_key));
    if (!Trim(r.keepalive).empty()) oss << "&keepalive=" << UrlEncode(Trim(r.keepalive));
    if (!Trim(r.ice_ports).empty()) oss << "&iceports=" << UrlEncode(Trim(r.ice_ports));
    if (!Trim(r.nat1to1).empty())   oss << "&nat1to1=" << UrlEncode(Trim(r.nat1to1));
    if (r.force_relay)              oss << "&forcerelay=1";
    if (r.keep_proxy)               oss << "&keepproxy=1";
    return oss.str();
}

std::string BuildCliCommand(const TunnelRule& r, const std::string& default_secret) {
    std::string secret = Trim(r.secret).empty() ? Trim(default_secret) : Trim(r.secret);
    std::string proto = NormalizeProto(r.proto);
    int bw = r.bandwidth_mbps > 0 ? r.bandwidth_mbps : 100;
    std::string listen_ep = (r.mode == "expose")
                                ? NormalizeEndpoint(SuggestConnectPort(r.port), "1080")
                                : NormalizeEndpoint(r.port, "1080");
    std::ostringstream oss;
    oss << "p2ptun connect " << r.name
        << " -secret " << secret
        << " -listen " << listen_ep
        << " -proto " << proto
        << " -bandwidth " << bw;
    if (!Trim(r.broker).empty())    oss << " -broker " << Trim(r.broker);
    if (!Trim(r.stun).empty())      oss << " -stun " << Trim(r.stun);
    if (!Trim(r.turn).empty())      oss << " -turn " << Trim(r.turn);
    if (!Trim(r.proxy).empty())     oss << " -proxy " << Trim(r.proxy);
    if (!Trim(r.peer_key).empty() && Trim(r.peer_key) != "peerjs") oss << " -key " << Trim(r.peer_key);
    if (!Trim(r.keepalive).empty()) oss << " -keepalive " << Trim(r.keepalive);
    if (!Trim(r.ice_ports).empty()) oss << " -ice-ports " << Trim(r.ice_ports);
    if (!Trim(r.nat1to1).empty())   oss << " -nat1to1 " << Trim(r.nat1to1);
    if (r.on_demand) {
        oss << " -on-demand";
        if (!Trim(r.idle_timeout).empty()) oss << " -idle-timeout " << Trim(r.idle_timeout);
    }
    if (r.force_relay)              oss << " -force-relay";
    if (r.keep_proxy)               oss << " -keep-proxy";
    return oss.str();
}

bool ParsePairingInput(const std::string& raw_input, TunnelRule& out_rule, std::string& out_err) {
    std::string s = Trim(raw_input);
    if (s.empty()) {
        out_err = "剪贴板或输入内容为空";
        return false;
    }

    std::string low = ToLower(s);
    if (low.rfind("p2ptun://", 0) == 0) {
        std::string rest = s.substr(9);
        std::string path_part = rest;
        std::string query_part;
        auto qpos = rest.find('?');
        if (qpos != std::string::npos) {
            path_part = rest.substr(0, qpos);
            query_part = rest.substr(qpos + 1);
        }

        std::map<std::string, std::string> kv;
        std::stringstream ss(query_part);
        std::string pair;
        while (std::getline(ss, pair, '&')) {
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                kv[ToLower(UrlDecode(pair.substr(0, eq)))] = UrlDecode(pair.substr(eq + 1));
            }
        }

        TunnelRule r;
        r.mode = "connect";
        r.name = Trim(kv["name"]);
        if (r.name.empty()) {
            while (!path_part.empty() && path_part.front() == '/') path_part.erase(path_part.begin());
            while (!path_part.empty() && path_part.back() == '/') path_part.pop_back();
            if (ToLower(path_part) != "connect" && ToLower(path_part) != "pair") {
                r.name = path_part;
            }
        }
        if (r.name.empty()) {
            out_err = "p2ptun:// 配对码缺少隧道名称 (name)";
            return false;
        }
        r.secret = Trim(kv["secret"]);
        r.port = Trim(kv["port"]);
        if (r.port.empty()) r.port = "1080";
        r.proto = NormalizeProto(kv["proto"]);
        int bw = std::atoi(kv["bw"].c_str());
        r.bandwidth_mbps = bw > 0 ? bw : 100;
        r.remark = Trim(kv["remark"]);
        r.broker = Trim(kv["broker"]);
        r.stun = Trim(kv["stun"]);
        r.turn = Trim(kv["turn"]);
        r.proxy = Trim(kv["proxy"]);
        r.peer_key = Trim(kv["key"]);
        r.keepalive = Trim(kv["keepalive"]);
        r.ice_ports = Trim(kv["iceports"]);
        r.nat1to1 = Trim(kv["nat1to1"]);
        r.force_relay = (kv["forcerelay"] == "1" || ToLower(kv["forcerelay"]) == "true");
        r.keep_proxy = (kv["keepproxy"] == "1" || ToLower(kv["keepproxy"]) == "true");
        if (r.remark.empty()) {
            r.remark = "导入连接 " + r.name;
        }
        r.enabled = true;
        out_rule = NormalizeRule(r, "connect", r.proto, r.bandwidth_mbps);
        return true;
    }

    {
        std::istringstream iss(s);
        std::vector<std::string> tokens;
        std::string tok;
        while (iss >> tok) tokens.push_back(tok);

        if (tokens.size() >= 2) {
            size_t idx = 0;
            if (ToLower(tokens[0]).find("p2ptun") != std::string::npos) {
                idx = 1;
            }
            if (idx < tokens.size() && (tokens[idx] == "connect" || tokens[idx] == "expose")) {
                std::string mode = tokens[idx];
                TunnelRule r;
                r.mode = mode;
                r.enabled = true;
                for (size_t i = idx + 1; i < tokens.size(); ++i) {
                    const std::string& arg = tokens[i];
                    if ((arg == "-id" || arg == "-peer" || arg == "-name" || arg == "--id" || arg == "--peer" || arg == "--name") && i + 1 < tokens.size()) {
                        r.name = tokens[++i];
                    } else if ((arg == "-secret" || arg == "--secret") && i + 1 < tokens.size()) {
                        r.secret = tokens[++i];
                    } else if ((arg == "-target" || arg == "-listen" || arg == "--target" || arg == "--listen") && i + 1 < tokens.size()) {
                        r.port = tokens[++i];
                    } else if ((arg == "-proto" || arg == "-transport" || arg == "--proto") && i + 1 < tokens.size()) {
                        r.proto = tokens[++i];
                    } else if ((arg == "-bandwidth" || arg == "--bandwidth") && i + 1 < tokens.size()) {
                        r.bandwidth_mbps = std::atoi(tokens[++i].c_str());
                    } else if (arg == "-on-demand" || arg == "--on-demand") {
                        r.on_demand = true;
                    } else if ((arg == "-broker" || arg == "--broker") && i + 1 < tokens.size()) {
                        r.broker = tokens[++i];
                    } else if ((arg == "-stun" || arg == "--stun") && i + 1 < tokens.size()) {
                        r.stun = tokens[++i];
                    } else if ((arg == "-turn" || arg == "--turn") && i + 1 < tokens.size()) {
                        r.turn = tokens[++i];
                    } else if ((arg == "-proxy" || arg == "--proxy") && i + 1 < tokens.size()) {
                        r.proxy = tokens[++i];
                    } else if ((arg == "-key" || arg == "--key") && i + 1 < tokens.size()) {
                        r.peer_key = tokens[++i];
                    } else if ((arg == "-keepalive" || arg == "--keepalive") && i + 1 < tokens.size()) {
                        r.keepalive = tokens[++i];
                    } else if ((arg == "-ice-ports" || arg == "--ice-ports") && i + 1 < tokens.size()) {
                        r.ice_ports = tokens[++i];
                    } else if ((arg == "-nat1to1" || arg == "--nat1to1") && i + 1 < tokens.size()) {
                        r.nat1to1 = tokens[++i];
                    } else if ((arg == "-idle-timeout" || arg == "--idle-timeout") && i + 1 < tokens.size()) {
                        r.idle_timeout = tokens[++i];
                    } else if ((arg == "-max-peers" || arg == "--max-peers") && i + 1 < tokens.size()) {
                        r.max_peers = std::atoi(tokens[++i].c_str());
                    } else if (arg == "-force-relay" || arg == "--force-relay") {
                        r.force_relay = true;
                    } else if (arg == "-keep-proxy" || arg == "--keep-proxy") {
                        r.keep_proxy = true;
                    } else if (!arg.empty() && arg[0] != '-' && r.name.empty()) {
                        r.name = arg;
                    }
                }
                if (r.name.empty()) {
                    out_err = "命令行缺少隧道名称";
                    return false;
                }
                r.remark = "命令行导入 " + r.name;
                out_rule = NormalizeRule(r, mode, r.proto, r.bandwidth_mbps);
                return true;
            }
        }
    }

    if (s.find(':') != std::string::npos && s.find(' ') == std::string::npos) {
        std::string left = s;
        std::string port_part = "1080";
        auto at_pos = s.rfind('@');
        if (at_pos != std::string::npos && at_pos > 0) {
            left = s.substr(0, at_pos);
            port_part = s.substr(at_pos + 1);
        }
        auto colon_pos = left.find(':');
        if (colon_pos != std::string::npos && colon_pos > 0) {
            std::string name = Trim(left.substr(0, colon_pos));
            std::string secret = Trim(left.substr(colon_pos + 1));
            if (!name.empty() && !secret.empty()) {
                TunnelRule r;
                r.mode = "connect";
                r.name = name;
                r.secret = secret;
                r.port = port_part;
                r.remark = "快捷导入 " + name;
                r.enabled = true;
                out_rule = NormalizeRule(r, "connect", "hy2", 100);
                return true;
            }
        }
    }

    out_err = "无法识别配对码格式（支持 p2ptun:// 链接、p2ptun connect 命令、或 隧道名:密钥@端口）";
    return false;
}

std::string EffectiveSecret(const TunnelRule& rule, const AppConfig& global_cfg) {
    std::string sec = Trim(rule.secret);
    if (!sec.empty()) return sec;
    return Trim(global_cfg.default_secret);
}

std::vector<std::string> BuildSubprocessArgs(const TunnelRule& rule, const AppConfig& global_cfg, int status_port) {
    std::string mode = (ToLower(Trim(rule.mode)) == "connect") ? "connect" : "expose";
    std::string proto = NormalizeProto(rule.proto.empty() ? global_cfg.default_proto : rule.proto);
    int bw = rule.bandwidth_mbps > 0 ? rule.bandwidth_mbps : (global_cfg.default_bandwidth > 0 ? global_cfg.default_bandwidth : 100);
    std::string ep = NormalizeEndpoint(rule.port, mode == "connect" ? "13389" : "3389");

    std::vector<std::string> args;
    args.push_back(mode);
    args.push_back(rule.name);
    if (mode == "expose") {
        args.push_back("-target");
        args.push_back(ep);
    } else {
        args.push_back("-listen");
        args.push_back(ep);
        if (rule.on_demand) {
            args.push_back("-on-demand");
        }
    }
    args.push_back("-proto");
    args.push_back(proto);
    args.push_back("-bandwidth");
    args.push_back(std::to_string(bw));

    // 高级穿透参数：留空不传，由 p2ptun 使用内置默认 (上游 main.go flag 全集)
    if (!Trim(rule.broker).empty()) {
        args.push_back("-broker");
        args.push_back(Trim(rule.broker));
    }
    if (!Trim(rule.stun).empty()) {
        args.push_back("-stun");
        args.push_back(Trim(rule.stun));
    }
    if (!Trim(rule.turn).empty()) {
        args.push_back("-turn");
        args.push_back(Trim(rule.turn));
    }
    if (!Trim(rule.proxy).empty()) {
        args.push_back("-proxy");
        args.push_back(Trim(rule.proxy));
    }
    if (!Trim(rule.peer_key).empty() && Trim(rule.peer_key) != "peerjs") {
        args.push_back("-key");
        args.push_back(Trim(rule.peer_key));
    }
    if (!Trim(rule.keepalive).empty()) {
        args.push_back("-keepalive");
        args.push_back(Trim(rule.keepalive));
    }
    if (!Trim(rule.ice_ports).empty()) {
        args.push_back("-ice-ports");
        args.push_back(Trim(rule.ice_ports));
    }
    if (!Trim(rule.nat1to1).empty()) {
        args.push_back("-nat1to1");
        args.push_back(Trim(rule.nat1to1));
    }
    if (mode == "expose" && rule.max_peers > 0) {
        args.push_back("-max-peers");
        args.push_back(std::to_string(rule.max_peers));
    }
    if (mode == "connect") {
        if (rule.on_demand) {
            args.push_back("-on-demand");
            if (!Trim(rule.idle_timeout).empty() && Trim(rule.idle_timeout) != "10m") {
                args.push_back("-idle-timeout");
                args.push_back(Trim(rule.idle_timeout));
            }
        }
        if (!Trim(rule.idle_timeout).empty() && !rule.on_demand) {
            // idle-timeout 仅在 on-demand 模式下有意义，静默忽略
        }
    }
    if (rule.force_relay) {
        args.push_back("-force-relay");
    }
    if (rule.keep_proxy) {
        args.push_back("-keep-proxy");
    }
    if (status_port > 0) {
        args.push_back("-status-addr");
        args.push_back("127.0.0.1:" + std::to_string(status_port));
    }
    return args;
}

bool ParseSerialParams(const std::string& params, int& out_databits, char& out_parity, int& out_stopbits) {
    std::string s = Trim(params);
    if (s.size() < 3) return false;
    // 格式: <数据位 7|8><校验 N|O|E><停止位 1|2>，如 "8N1"
    char db = s[0];
    char parity = static_cast<char>(std::toupper(static_cast<unsigned char>(s[1])));
    char sb = s[2];
    if (db != '7' && db != '8') return false;
    if (parity != 'N' && parity != 'O' && parity != 'E') return false;
    if (sb != '1' && sb != '2') return false;
    out_databits = db - '0';
    out_parity = parity;
    out_stopbits = sb - '0';
    return true;
}

std::string FormatBytes(uint64_t bytes) {
    const double kUnit = 1024.0;
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }
    const char* units[] = {"KB", "MB", "GB", "TB"};
    double val = static_cast<double>(bytes) / kUnit;
    int exp = 0;
    while (val >= kUnit && exp < 3) {
        val /= kUnit;
        ++exp;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", val, units[exp]);
    return std::string(buf);
}

} // namespace p2ptun
