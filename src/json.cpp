#include "ragekit/json.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "ragekit/error.h"

namespace rk {

double JsonValue::number() const { return type == Type::Number ? std::strtod(text.c_str(), nullptr) : 0.0; }

const JsonValue* JsonValue::get(std::string_view key) const {
    for (const auto& [k, v] : members) if (k == key) return &v;
    return nullptr;
}

namespace {

constexpr size_t kMaxDepth = 512;

struct Parser {
    std::string_view s;
    size_t p = 0;

    [[noreturn]] void fail(const std::string& what) const { throw Error("json: " + what + " at byte " + std::to_string(p)); }
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { p++; return true; } return false; }
    void expect(char c) { if (!eat(c)) fail(std::string("expected '") + c + "'"); }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | cp >> 6); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | (cp >> 12 & 0x3F)); out += (char)(0x80 | (cp >> 6 & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }
    uint32_t hex4() {
        if (p + 4 > s.size()) fail("a \\u escape cut short");
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = s[p++];
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (fail("a bad \\u escape"), 0));
        }
        return v;
    }
    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            if (p >= s.size()) fail("a string without its closing quote");
            const char c = s[p++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (p >= s.size()) fail("an escape cut short");
            const char e = s[p++];
            switch (e) {
                case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
                case 'r': out += '\r'; break; case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        p += 2;
                        const uint32_t lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(out, cp);
                    break;
                }
                default: fail(std::string("an unknown escape \\") + e);
            }
        }
    }
    JsonValue value(size_t depth) {
        if (depth > kMaxDepth) fail("nesting deeper than 512");
        ws();
        if (p >= s.size()) fail("the document ends where a value should be");
        JsonValue v;
        const char c = s[p];
        if (c == '{') {
            p++;
            v.type = JsonValue::Type::Object;
            if (eat('}')) return v;
            do {
                ws();
                std::string k = string();
                expect(':');
                v.members.emplace_back(std::move(k), value(depth + 1));
            } while (eat(','));
            expect('}');
        } else if (c == '[') {
            p++;
            v.type = JsonValue::Type::Array;
            if (eat(']')) return v;
            do v.items.push_back(value(depth + 1)); while (eat(','));
            expect(']');
        } else if (c == '"') {
            v.type = JsonValue::Type::String;
            v.text = string();
        } else if (s.substr(p, 4) == "true") { v.type = JsonValue::Type::Bool; v.boolean = true; p += 4; }
        else if (s.substr(p, 5) == "false") { v.type = JsonValue::Type::Bool; p += 5; }
        else if (s.substr(p, 4) == "null") p += 4;
        else if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = p;
            while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '-' || s[p] == '+' || s[p] == '.' || s[p] == 'e' || s[p] == 'E')) p++;
            v.type = JsonValue::Type::Number;
            v.text = std::string(s.substr(start, p - start));
        } else fail(std::string("an unexpected '") + c + "'");
        return v;
    }
};

void quote(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break; case '\r': out += "\\r"; break; case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break; case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04X", c); out += b; }
                else out += (char)c;
        }
    }
    out += '"';
}

void write(std::string& out, const JsonValue& v, bool indent, size_t depth) {
    auto nl = [&](size_t d) { if (indent) { out += '\n'; out.append(d, '\t'); } };
    switch (v.type) {
        case JsonValue::Type::Null: out += "null"; break;
        case JsonValue::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case JsonValue::Type::Number: out += v.text; break;
        case JsonValue::Type::String: quote(out, v.text); break;
        case JsonValue::Type::Array:
            out += '[';
            for (size_t i = 0; i < v.items.size(); i++) { if (i) out += ','; nl(depth + 1); write(out, v.items[i], indent, depth + 1); }
            if (!v.items.empty()) nl(depth);
            out += ']';
            break;
        case JsonValue::Type::Object:
            out += '{';
            for (size_t i = 0; i < v.members.size(); i++) {
                if (i) out += ',';
                nl(depth + 1);
                quote(out, v.members[i].first);
                out += ':';
                write(out, v.members[i].second, indent, depth + 1);
            }
            if (!v.members.empty()) nl(depth);
            out += '}';
            break;
    }
}

}  // namespace

JsonValue parse_json(std::string_view text) {
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.remove_prefix(3);
    Parser ps{text};
    JsonValue v = ps.value(0);
    ps.ws();
    while (ps.p < text.size() && text[ps.p] == '\0') ps.p++;   // a NUL the game's files may end with
    ps.ws();
    if (ps.p != text.size()) ps.fail("text after the document");
    return v;
}

std::string write_json(const JsonValue& v, bool indent) {
    std::string out;
    write(out, v, indent, 0);
    return out;
}

}  // namespace rk
