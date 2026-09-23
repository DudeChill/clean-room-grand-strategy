// Minimal dependency-free JSON parser and serializer.
//
// The parser is a strict recursive-descent reader: the whole document must be a
// single value with no trailing bytes, strings must use the JSON escape set, and
// numbers must match the JSON grammar (no leading zeros, no bare '.' or 'e').
// Errors carry `line N, column M: reason` so content authors can fix data files
// without guessing where the parser stopped.

#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace hoi {
namespace {

// Integral doubles beyond 2^53 lose precision anyway; this bound keeps the
// int64 cast in dump() well inside the representable range.
constexpr double kMaxExactInt = 1.0e18;

const Json& null_json() {
    static const Json kNull;
    return kNull;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

void append_escaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20u) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

struct Parser {
    const std::string& text;
    size_t pos = 0;
    std::string* err = nullptr;
    bool failed = false;

    explicit Parser(const std::string& t) : text(t) {}

    [[nodiscard]] bool at_end() const { return pos >= text.size(); }
    [[nodiscard]] char peek() const { return pos < text.size() ? text[pos] : '\0'; }

    void error(const std::string& reason) {
        if (failed) return;
        failed = true;
        if (!err) return;
        size_t line = 1;
        size_t column = 1;
        for (size_t i = 0; i < pos && i < text.size(); ++i) {
            if (text[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        *err = "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " +
               reason;
    }

    void skip_ws() {
        while (!at_end()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    Json parse_value() {
        skip_ws();
        if (failed) return Json();
        if (at_end()) {
            error("unexpected end of input");
            return Json();
        }
        switch (text[pos]) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': {
                std::string s = parse_string();
                if (failed) return Json();
                return Json(std::move(s));
            }
            case 't': return parse_literal("true", Json(true));
            case 'f': return parse_literal("false", Json(false));
            case 'n': return parse_literal("null", Json(nullptr));
            default: break;
        }
        const char c = text[pos];
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        error(std::string("unexpected character '") + c + "'");
        return Json();
    }

    Json parse_literal(const char* literal, Json value) {
        const size_t n = std::strlen(literal);
        if (text.compare(pos, n, literal) != 0) {
            error(std::string("invalid literal, expected '") + literal + "'");
            return Json();
        }
        pos += n;
        return value;
    }

    uint32_t parse_hex4() {
        if (pos + 4 > text.size()) {
            error("truncated \\u escape");
            return 0;
        }
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = text[pos + static_cast<size_t>(k)];
            v <<= 4;
            if (h >= '0' && h <= '9') {
                v |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                v |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                v |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                error("invalid hex digit in \\u escape");
                return 0;
            }
        }
        pos += 4;
        return v;
    }

    std::string parse_string() {
        std::string out;
        ++pos;  // opening quote
        while (true) {
            if (at_end()) {
                error("unterminated string");
                return {};
            }
            const unsigned char c = static_cast<unsigned char>(text[pos]);
            if (c == '"') {
                ++pos;
                return out;
            }
            if (c < 0x20u) {
                error("unescaped control character in string");
                return {};
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos;
                continue;
            }
            ++pos;  // backslash
            if (at_end()) {
                error("unterminated escape sequence");
                return {};
            }
            const char e = text[pos++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = parse_hex4();
                    if (failed) return {};
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // High surrogate: a following low surrogate is mandatory.
                        if (pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
                            pos += 2;
                            const uint32_t lo = parse_hex4();
                            if (failed) return {};
                            if (lo < 0xDC00u || lo > 0xDFFFu) {
                                error("invalid low surrogate in \\u escape");
                                return {};
                            }
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        } else {
                            error("unpaired high surrogate in \\u escape");
                            return {};
                        }
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        error("unpaired low surrogate in \\u escape");
                        return {};
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    error(std::string("invalid escape '\\") + e + "'");
                    return {};
            }
        }
    }

    Json parse_number() {
        const size_t start = pos;
        if (peek() == '-') ++pos;
        if (at_end() || !(peek() >= '0' && peek() <= '9')) {
            error("expected digit in number");
            return Json();
        }
        if (peek() == '0') {
            ++pos;
            if (!at_end() && peek() >= '0' && peek() <= '9') {
                error("leading zeros are not allowed in numbers");
                return Json();
            }
        } else {
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos;
        }
        if (!at_end() && peek() == '.') {
            ++pos;
            if (at_end() || !(peek() >= '0' && peek() <= '9')) {
                error("expected digit after decimal point");
                return Json();
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos;
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            ++pos;
            if (!at_end() && (peek() == '+' || peek() == '-')) ++pos;
            if (at_end() || !(peek() >= '0' && peek() <= '9')) {
                error("expected digit in exponent");
                return Json();
            }
            while (!at_end() && peek() >= '0' && peek() <= '9') ++pos;
        }

        const std::string token = text.substr(start, pos - start);
        const double value = std::strtod(token.c_str(), nullptr);
        if (!std::isfinite(value)) {
            // Overflowing literals (e.g. 1e999) have no portable double value and
            // must never reach simulation state.
            error("number out of range");
            return Json();
        }
        return Json(value);
    }

    Json parse_array() {
        ++pos;  // '['
        Json arr = Json::array();
        skip_ws();
        if (peek() == ']') {
            ++pos;
            return arr;
        }
        while (true) {
            Json v = parse_value();
            if (failed) return Json();
            arr.push_back(std::move(v));
            skip_ws();
            if (at_end()) {
                error("unterminated array");
                return Json();
            }
            const char c = text[pos];
            if (c == ',') {
                ++pos;
                skip_ws();
                if (peek() == ']') {
                    error("trailing comma in array");
                    return Json();
                }
                continue;
            }
            if (c == ']') {
                ++pos;
                return arr;
            }
            error("expected ',' or ']' in array");
            return Json();
        }
    }

    Json parse_object() {
        ++pos;  // '{'
        Json obj = Json::object();
        skip_ws();
        if (peek() == '}') {
            ++pos;
            return obj;
        }
        while (true) {
            skip_ws();
            if (peek() != '"') {
                error("expected string key in object");
                return Json();
            }
            std::string key = parse_string();
            if (failed) return Json();
            skip_ws();
            if (peek() != ':') {
                error("expected ':' after object key");
                return Json();
            }
            ++pos;
            Json v = parse_value();
            if (failed) return Json();
            obj.set(key, std::move(v));
            skip_ws();
            if (at_end()) {
                error("unterminated object");
                return Json();
            }
            const char c = text[pos];
            if (c == ',') {
                ++pos;
                skip_ws();
                if (peek() == '}') {
                    error("trailing comma in object");
                    return Json();
                }
                continue;
            }
            if (c == '}') {
                ++pos;
                return obj;
            }
            error("expected ',' or '}' in object");
            return Json();
        }
    }
};

void dump_value(const Json& v, int indent, int depth, std::string& out) {
    switch (v.type()) {
        case Json::Type::Null:
            out += "null";
            return;
        case Json::Type::Bool:
            out += v.as_bool(false) ? "true" : "false";
            return;
        case Json::Type::Number: {
            const double d = v.as_double();
            if (!std::isfinite(d)) {
                // JSON has no NaN/Inf spelling; emitting null keeps files parseable.
                out += "null";
                return;
            }
            char buf[48];
            if (d == std::floor(d) && d >= -kMaxExactInt && d <= kMaxExactInt) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
            } else {
                std::snprintf(buf, sizeof(buf), "%.17g", d);
            }
            out += buf;
            return;
        }
        case Json::Type::String:
            append_escaped(out, v.as_string());
            return;
        case Json::Type::Array: {
            const auto& items = v.array_items();
            if (items.empty()) {
                out += "[]";
                return;
            }
            out.push_back('[');
            for (size_t i = 0; i < items.size(); ++i) {
                if (i != 0) out.push_back(',');
                if (indent >= 0) {
                    out.push_back('\n');
                    out.append(static_cast<size_t>(indent) * static_cast<size_t>(depth + 1), ' ');
                }
                dump_value(items[i], indent, depth + 1, out);
            }
            if (indent >= 0) {
                out.push_back('\n');
                out.append(static_cast<size_t>(indent) * static_cast<size_t>(depth), ' ');
            }
            out.push_back(']');
            return;
        }
        case Json::Type::Object: {
            const auto& items = v.object_items();
            if (items.empty()) {
                out += "{}";
                return;
            }
            out.push_back('{');
            for (size_t i = 0; i < items.size(); ++i) {
                if (i != 0) out.push_back(',');
                if (indent >= 0) {
                    out.push_back('\n');
                    out.append(static_cast<size_t>(indent) * static_cast<size_t>(depth + 1), ' ');
                }
                append_escaped(out, items[i].first);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                dump_value(items[i].second, indent, depth + 1, out);
            }
            if (indent >= 0) {
                out.push_back('\n');
                out.append(static_cast<size_t>(indent) * static_cast<size_t>(depth), ' ');
            }
            out.push_back('}');
            return;
        }
    }
}

}  // namespace

Json Json::object() {
    Json j;
    j.type_ = Type::Object;
    return j;
}

Json Json::array() {
    Json j;
    j.type_ = Type::Array;
    return j;
}

bool Json::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (const auto& kv : obj_) {
        if (kv.first == key) return true;
    }
    return false;
}

const Json& Json::at(const std::string& key) const {
    if (type_ == Type::Object) {
        for (const auto& kv : obj_) {
            if (kv.first == key) return kv.second;
        }
    }
    return null_json();
}

const Json& Json::at(size_t index) const {
    if (type_ == Type::Array && index < arr_.size()) return arr_[index];
    return null_json();
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

double Json::as_double(double fallback) const { return type_ == Type::Number ? num_ : fallback; }

int64_t Json::as_int(int64_t fallback) const {
    if (type_ != Type::Number || !std::isfinite(num_)) return fallback;
    return static_cast<int64_t>(num_);
}

bool Json::as_bool(bool fallback) const { return type_ == Type::Bool ? num_ != 0.0 : fallback; }

std::string Json::as_string(const std::string& fallback) const {
    return type_ == Type::String ? str_ : fallback;
}

void Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        arr_.clear();
        str_.clear();
        num_ = 0.0;
    }
    for (auto& kv : obj_) {
        if (kv.first == key) {
            kv.second = std::move(value);
            return;
        }
    }
    obj_.emplace_back(key, std::move(value));
}

void Json::push_back(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        obj_.clear();
        str_.clear();
        num_ = 0.0;
    }
    arr_.push_back(std::move(value));
}

Json Json::parse(const std::string& text, std::string* err) {
    Parser parser(text);
    parser.err = err;
    if (err) err->clear();

    Json value = parser.parse_value();
    if (!parser.failed) {
        parser.skip_ws();
        if (parser.pos != text.size()) parser.error("trailing characters after top-level value");
    }
    if (parser.failed) return Json();
    return value;
}

bool Json::parse_file(const std::string& path, Json* out, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open file '" + path + "'";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) {
        if (err) *err = "I/O error while reading file '" + path + "'";
        return false;
    }
    const std::string text = ss.str();

    std::string parse_err;
    Json value = parse(text, &parse_err);
    if (!parse_err.empty()) {
        if (err) *err = path + ": " + parse_err;
        return false;
    }
    if (out) *out = std::move(value);
    return true;
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_value(*this, indent, 0, out);
    return out;
}

bool Json::write_file(const std::string& path, int indent) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::string text = dump(indent);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return f.good();
}

}  // namespace hoi
