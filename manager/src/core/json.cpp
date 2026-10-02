// json.cpp - see json.h.
#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace atmt {

namespace {

const Json& NullJson() {
    static const Json n;
    return n;
}

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool Run(Json* out, std::string* error) {
        Ws();
        if (!Value(out, 0)) return Fail(error);
        Ws();
        if (pos_ != s_.size()) {
            why_ = "trailing characters";
            return Fail(error);
        }
        return true;
    }

private:
    bool Fail(std::string* error) {
        if (error != nullptr) {
            size_t line = 1;
            for (size_t i = 0; i < pos_ && i < s_.size(); ++i) {
                if (s_[i] == '\n') ++line;
            }
            *error = why_ + " at line " + std::to_string(line);
        }
        return false;
    }

    void Ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) {
            ++pos_;
        }
    }

    bool Literal(const char* word) {
        size_t n = 0;
        while (word[n] != '\0') ++n;
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }

    static void Utf8(std::string& out, uint32_t c) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    bool Hex4(uint32_t* v) {
        if (pos_ + 4 > s_.size()) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool StringBody(std::string* out) {
        ++pos_;   // the opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                *out += c;
                continue;
            }
            if (pos_ >= s_.size()) break;
            const char e = s_[pos_++];
            switch (e) {
                case '"': *out += '"'; break;
                case '\\': *out += '\\'; break;
                case '/': *out += '/'; break;
                case 'b': *out += '\b'; break;
                case 'f': *out += '\f'; break;
                case 'n': *out += '\n'; break;
                case 'r': *out += '\r'; break;
                case 't': *out += '\t'; break;
                case 'u': {
                    uint32_t c1 = 0;
                    if (!Hex4(&c1)) {
                        why_ = "bad \\u escape";
                        return false;
                    }
                    if (c1 >= 0xD800 && c1 < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0) {
                        pos_ += 2;
                        uint32_t c2 = 0;
                        if (!Hex4(&c2)) {
                            why_ = "bad \\u escape";
                            return false;
                        }
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    Utf8(*out, c1);
                    break;
                }
                default:
                    why_ = "bad escape";
                    return false;
            }
        }
        why_ = "unterminated string";
        return false;
    }

    bool Value(Json* out, int depth) {
        if (depth > 64) {
            why_ = "nested too deep";
            return false;
        }
        if (pos_ >= s_.size()) {
            why_ = "unexpected end";
            return false;
        }
        const char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            *out = Json::MakeObject();
            Ws();
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                Ws();
                if (pos_ >= s_.size() || s_[pos_] != '"') {
                    why_ = "expected a key";
                    return false;
                }
                std::string key;
                if (!StringBody(&key)) return false;
                Ws();
                if (pos_ >= s_.size() || s_[pos_] != ':') {
                    why_ = "expected ':'";
                    return false;
                }
                ++pos_;
                Ws();
                Json v;
                if (!Value(&v, depth + 1)) return false;
                (*out)[key] = std::move(v);
                Ws();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                why_ = "expected ',' or '}'";
                return false;
            }
        }
        if (c == '[') {
            ++pos_;
            *out = Json::MakeArray();
            Ws();
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                Ws();
                Json v;
                if (!Value(&v, depth + 1)) return false;
                out->Push(std::move(v));
                Ws();
                if (pos_ < s_.size() && s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < s_.size() && s_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                why_ = "expected ',' or ']'";
                return false;
            }
        }
        if (c == '"') {
            std::string str;
            if (!StringBody(&str)) return false;
            *out = Json(std::move(str));
            return true;
        }
        if (Literal("true")) {
            *out = Json(true);
            return true;
        }
        if (Literal("false")) {
            *out = Json(false);
            return true;
        }
        if (Literal("null")) {
            *out = Json();
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = s_.c_str() + pos_;
            char* end = nullptr;
            const double v = std::strtod(begin, &end);
            if (end == begin) {
                why_ = "bad number";
                return false;
            }
            pos_ += static_cast<size_t>(end - begin);
            *out = Json(v);
            return true;
        }
        why_ = std::string("unexpected '") + c + "'";
        return false;
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string why_;
};

void Quote(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

}  // namespace

bool Json::Parse(const std::string& text, Json* out, std::string* error) {
    Parser p(text);
    Json result;
    if (!p.Run(&result, error)) return false;
    *out = std::move(result);
    return true;
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ == Object) {
        for (const auto& kv : obj_) {
            if (kv.first == key) return kv.second;
        }
    }
    return NullJson();
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Object) {
        *this = MakeObject();
    }
    for (auto& kv : obj_) {
        if (kv.first == key) return kv.second;
    }
    obj_.emplace_back(key, Json());
    return obj_.back().second;
}

bool Json::Has(const std::string& key) const {
    if (type_ != Object) return false;
    for (const auto& kv : obj_) {
        if (kv.first == key) return true;
    }
    return false;
}

void Json::Erase(const std::string& key) {
    for (size_t i = 0; i < obj_.size(); ++i) {
        if (obj_[i].first == key) {
            obj_.erase(obj_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void Json::Push(Json v) {
    if (type_ != Array) *this = MakeArray();
    arr_.push_back(std::move(v));
}

std::string Json::Dump(bool pretty) const {
    std::string out;
    DumpTo(out, pretty, 0);
    if (pretty) out += '\n';
    return out;
}

void Json::DumpTo(std::string& out, bool pretty, int indent) const {
    const std::string pad = pretty ? std::string(static_cast<size_t>(indent + 2), ' ') : std::string();
    const std::string end_pad = pretty ? std::string(static_cast<size_t>(indent), ' ') : std::string();
    switch (type_) {
        case Null: out += "null"; break;
        case Bool: out += bool_ ? "true" : "false"; break;
        case Number: {
            char buf[64];
            if (std::isfinite(num_) && num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
            } else {
                std::snprintf(buf, sizeof(buf), "%.17g", num_);
            }
            out += buf;
            break;
        }
        case String: Quote(out, str_); break;
        case Array:
            if (arr_.empty()) {
                out += "[]";
                break;
            }
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (pretty) out += "\n" + pad;
                arr_[i].DumpTo(out, pretty, indent + 2);
                if (i + 1 < arr_.size()) out += ',';
            }
            if (pretty) out += "\n" + end_pad;
            out += ']';
            break;
        case Object:
            if (obj_.empty()) {
                out += "{}";
                break;
            }
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (pretty) out += "\n" + pad;
                Quote(out, obj_[i].first);
                out += pretty ? ": " : ":";
                obj_[i].second.DumpTo(out, pretty, indent + 2);
                if (i + 1 < obj_.size()) out += ',';
            }
            if (pretty) out += "\n" + end_pad;
            out += '}';
            break;
    }
}

}  // namespace atmt
