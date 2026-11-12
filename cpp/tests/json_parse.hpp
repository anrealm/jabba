// SPDX-License-Identifier: MIT
// A small JSON reader for the conformance case files (spec/case.schema.json).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include <jabba/json.hpp>

namespace json_parse {

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    jabba::Json parse() {
        auto v = value();
        ws();
        if (i_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) {
        throw std::runtime_error("json: " + what + " at offset " + std::to_string(i_));
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

    jabba::Json value() {
        ws();
        if (i_ >= s_.size()) fail("unexpected end");
        char c = s_[i_];
        if (c == '{') return object();
        if (c == '[') return array();
        if (c == '"') return jabba::Json(string());
        if (literal("true")) return jabba::Json(true);
        if (literal("false")) return jabba::Json(false);
        if (literal("null")) return jabba::Json(nullptr);
        return number();
    }

    jabba::Json object() {
        expect('{');
        jabba::JsonObject obj;
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

    jabba::Json array() {
        expect('[');
        jabba::JsonArray arr;
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

    jabba::Json number() {
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
        if (is_float) return jabba::Json(std::strtod(text.c_str(), nullptr));
        return jabba::Json(static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10)));
    }

    std::string_view s_;
    std::size_t i_ = 0;
};

inline jabba::Json parse(std::string_view text) { return Parser(text).parse(); }

}  // namespace json_parse
