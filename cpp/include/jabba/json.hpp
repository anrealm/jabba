// SPDX-License-Identifier: MIT
// A minimal JSON value for request-record fields and the record itself.
#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// std::to_chars for double needs macOS 13.3 / iOS 16.3 on Apple platforms.
#ifndef JABBA_FLOAT_TO_CHARS
#if (defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__) &&            \
     __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 130300) ||          \
    (defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) &&          \
     __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__ < 160300)
#define JABBA_FLOAT_TO_CHARS 0
#else
#define JABBA_FLOAT_TO_CHARS 1
#endif
#endif

namespace jabba {

class Json;
using JsonArray = std::vector<Json>;
/// Objects keep insertion order.
using JsonObject = std::vector<std::pair<std::string, Json>>;

class Json {
public:
    using Value =
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, JsonArray, JsonObject>;

    Json() : value_(nullptr) {}
    Json(std::nullptr_t) : value_(nullptr) {}
    Json(bool v) : value_(v) {}
    Json(double v) : value_(v) {}
    Json(float v) : value_(static_cast<double>(v)) {}
    Json(const char* v) : value_(std::string(v)) {}
    /// A character is a one-character string, not a number.
    Json(char v) : value_(std::string(1, v)) {}
    Json(std::string v) : value_(std::move(v)) {}
    Json(std::string_view v) : value_(std::string(v)) {}
    Json(JsonArray v) : value_(std::move(v)) {}
    Json(JsonObject v) : value_(std::move(v)) {}

    /// Integers that fit in int64 stay integers; larger unsigned values become floats.
    template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool> &&
                                            !std::is_same_v<T, char>,
                                        int> = 0>
    Json(T v) {
        if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t)) {
            if (v > static_cast<std::uint64_t>(INT64_MAX)) {
                value_ = static_cast<double>(v);
                return;
            }
        }
        value_ = static_cast<std::int64_t>(v);
    }

    const Value& value() const { return value_; }

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
    template <class T>
    const T* get_if() const {
        return std::get_if<T>(&value_);
    }

    /// Value of `key` if this is an object that has it.
    const Json* find(std::string_view key) const {
        if (auto* obj = get_if<JsonObject>()) {
            for (auto& [k, v] : *obj) {
                if (k == key) return &v;
            }
        }
        return nullptr;
    }

    /// Compact JSON text.
    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

    void write(std::string& out) const {
        std::visit([&](const auto& v) { write_value(out, v); }, value_);
    }

    friend bool operator==(const Json& a, const Json& b) { return a.value_ == b.value_; }

private:
    static void write_value(std::string& out, std::nullptr_t) { out += "null"; }
    static void write_value(std::string& out, bool v) { out += v ? "true" : "false"; }
    static void write_value(std::string& out, std::int64_t v) { out += std::to_string(v); }
    static void write_value(std::string& out, double v) {
        if (!std::isfinite(v)) {
            out += "null";
            return;
        }
        char buf[64];
#if JABBA_FLOAT_TO_CHARS
        auto end = std::to_chars(buf, buf + sizeof buf, v).ptr;
        std::string_view text(buf, static_cast<std::size_t>(end - buf));
#else
        // The fewest digits (15..17) that read back as the same value. snprintf and
        // strtod share the C locale, which may localize the point; JSON wants '.'.
        int n = 0;
        for (int precision = 15; precision <= 17; ++precision) {
            n = std::snprintf(buf, sizeof buf, "%.*g", precision, v);
            if (std::strtod(buf, nullptr) == v) break;
        }
        for (int i = 0; i < n; ++i) {
            if (buf[i] == ',') buf[i] = '.';
        }
        std::string_view text(buf, static_cast<std::size_t>(n));
#endif
        out += text;
        // Keep it a float in the eyes of a JSON reader: 1 -> 1.0.
        if (text.find_first_of(".eE") == std::string_view::npos) out += ".0";
    }
    static void write_value(std::string& out, const std::string& s) { write_string(out, s); }
    static void write_value(std::string& out, const JsonArray& a) {
        out += '[';
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            a[i].write(out);
        }
        out += ']';
    }
    static void write_value(std::string& out, const JsonObject& o) {
        out += '{';
        for (std::size_t i = 0; i < o.size(); ++i) {
            if (i) out += ',';
            write_string(out, o[i].first);
            out += ':';
            o[i].second.write(out);
        }
        out += '}';
    }

    static void write_string(std::string& out, std::string_view s) {
        out += '"';
        for (char ch : s) {
            auto c = static_cast<unsigned char>(ch);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char esc[8];
                        std::snprintf(esc, sizeof esc, "\\u%04x", static_cast<unsigned>(c));
                        out += esc;
                    } else {
                        out += ch;
                    }
            }
        }
        out += '"';
    }

    Value value_;
};

namespace detail {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Json parse() {
        auto v = value();
        ws();
        if (i_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) {
        throw std::invalid_argument("json: " + what + " at offset " + std::to_string(i_));
    }

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }

    bool eat(char c) {
        ws();
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!eat(c)) fail(std::string("expected '") + c + "'");
    }

    bool literal(std::string_view word) {
        if (s_.substr(i_, word.size()) == word) {
            i_ += word.size();
            return true;
        }
        return false;
    }

    Json value() {
        ws();
        if (i_ >= s_.size()) fail("unexpected end");
        char c = s_[i_];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return Json(string());
        if (literal("true")) return Json(true);
        if (literal("false")) return Json(false);
        if (literal("null")) return Json(nullptr);
        return number();
    }

    Json object() {
        expect('{');
        JsonObject obj;
        if (eat('}')) return obj;
        do {
            ws();
            auto key = string();
            expect(':');
            obj.emplace_back(std::move(key), value());
        } while (eat(','));
        expect('}');
        return obj;
    }

    Json array() {
        expect('[');
        JsonArray arr;
        if (eat(']')) return arr;
        do {
            arr.push_back(value());
        } while (eat(','));
        expect(']');
        return arr;
    }

    std::string string() {
        if (s_[i_] != '"') fail("expected a string");
        ++i_;
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail("unterminated string");
            char c = s_[i_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) fail("bad escape");
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (i_ + 4 > s_.size()) fail("bad \\u escape");
                    auto cp = static_cast<unsigned>(std::stoul(std::string(s_.substr(i_, 4)), nullptr, 16));
                    i_ += 4;
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    Json number() {
        auto start = i_;
        bool is_float = false;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == '.' || c == 'e' || c == 'E') {
                is_float = true;
            } else if (!(c == '-' || c == '+' || (c >= '0' && c <= '9'))) {
                break;
            }
            ++i_;
        }
        if (start == i_) fail("unexpected character");
        std::string text(s_.substr(start, i_ - start));
        if (is_float) return Json(std::strtod(text.c_str(), nullptr));
        return Json(static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

}  // namespace detail

/// Parse JSON text (RFC 8259); throws std::invalid_argument on malformed input.
inline Json parse_json(std::string_view text) { return detail::Parser(text).parse(); }

}  // namespace jabba
