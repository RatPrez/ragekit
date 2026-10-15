// ragekit - small XML reader. See include/ragekit/xml_dom.h.
#include "ragekit/xml_dom.h"
#include <cctype>
#include <cstdint>
#include <cstring>
#include "ragekit/error.h"

namespace rk {

namespace {

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

void append_utf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
}

std::string decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out += s[i]; continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string_view::npos) { out += s[i]; continue; }
        const std::string_view e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (!e.empty() && e[0] == '#') {
            const bool hex = e.size() > 1 && (e[1] == 'x' || e[1] == 'X');
            append_utf8(out, (uint32_t)std::strtoul(std::string(e.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10));
        } else { out += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
    return s;
}

struct Parser {
    std::string_view s;
    size_t p = 0;

    [[noreturn]] void fail(const char* what) const {
        size_t line = 1;
        for (size_t i = 0; i < p && i < s.size(); i++) line += s[i] == '\n';
        throw Error(std::string("xml: ") + what + " (line " + std::to_string(line) + ")");
    }
    bool at(std::string_view t) const { return s.compare(p, t.size(), t) == 0; }
    void skip_ws() { while (p < s.size() && std::isspace((unsigned char)s[p])) p++; }
    void skip_past(std::string_view t, const char* what) {
        const size_t e = s.find(t, p);
        if (e == std::string_view::npos) fail(what);
        p = e + t.size();
    }
    /// Comments, processing instructions and the DOCTYPE between elements.
    void skip_misc() {
        for (;;) {
            skip_ws();
            if (at("<!--")) skip_past("-->", "unterminated comment");
            else if (at("<?")) skip_past("?>", "unterminated processing instruction");
            else if (at("<!")) skip_past(">", "unterminated declaration");   // the DOCTYPE, and the game's own "<! --- note --- />"
            else return;
        }
    }
    std::string name() {
        const size_t b = p;
        while (p < s.size() && !std::isspace((unsigned char)s[p]) && s[p] != '>' && s[p] != '/' && s[p] != '=') p++;
        if (p == b) fail("expected a name");
        return std::string(s.substr(b, p - b));
    }
    XmlElement element() {
        if (!at("<")) fail("expected '<'");
        p++;
        XmlElement e;
        e.name = name();
        for (;;) {   // attributes
            skip_ws();
            if (p >= s.size()) fail("unterminated start tag");
            if (at("/>")) { p += 2; return e; }
            if (s[p] == '>') { p++; break; }
            std::string an = name();
            skip_ws();
            if (p >= s.size() || s[p] != '=') fail("expected '=' after an attribute name");
            p++;
            skip_ws();
            if (p >= s.size() || (s[p] != '"' && s[p] != '\'')) fail("expected a quoted attribute value");
            const char q = s[p++];
            const size_t end = s.find(q, p);
            if (end == std::string_view::npos) fail("unterminated attribute value");
            e.attributes.emplace_back(std::move(an), decode(s.substr(p, end - p)));
            p = end + 1;
        }
        std::string text;
        for (;;) {   // content
            if (p >= s.size()) fail("unterminated element");
            if (at("</")) {
                p += 2;
                const std::string n = name();
                if (n != e.name) fail("mismatched end tag");
                skip_ws();
                if (p >= s.size() || s[p] != '>') fail("expected '>'");
                p++;
                break;
            }
            if (at("<!--")) { skip_past("-->", "unterminated comment"); continue; }
            if (at("<![CDATA[")) {
                const size_t b = p + 9;
                skip_past("]]>", "unterminated CDATA");
                text.append(s.substr(b, p - 3 - b));
                continue;
            }
            if (at("<?")) { skip_past("?>", "unterminated processing instruction"); continue; }
            if (at("<!")) { skip_past(">", "unterminated declaration"); continue; }
            if (s[p] == '<') { e.children.push_back(element()); continue; }
            const size_t b = p;
            while (p < s.size() && s[p] != '<') p++;
            text += decode(s.substr(b, p - b));
        }
        e.text = std::string(trim(text));
        return e;
    }
};

}   // namespace

const XmlElement* XmlElement::child(std::string_view n) const {
    for (const auto& c : children) if (c.name == n) return &c;
    return nullptr;
}
const XmlElement* XmlElement::child_ci(std::string_view n) const {
    for (const auto& c : children) if (ieq(c.name, n)) return &c;
    return nullptr;
}
std::vector<const XmlElement*> XmlElement::children_named(std::string_view n) const {
    std::vector<const XmlElement*> out;
    for (const auto& c : children) if (c.name == n) out.push_back(&c);
    return out;
}
std::string XmlElement::attribute(std::string_view n, std::string_view fallback) const {
    for (const auto& a : attributes) if (a.first == n) return a.second;
    return std::string(fallback);
}
std::string XmlElement::child_text(std::string_view n) const {
    const XmlElement* c = child(n);
    return c ? c->text : std::string();
}
std::string XmlElement::child_value(std::string_view n, std::string_view fallback) const {
    const XmlElement* c = child(n);
    return c ? c->attribute("value", fallback) : std::string(fallback);
}

XmlElement parse_xml(std::string_view text) {
    Parser ps{text};
    if (ps.at("\xEF\xBB\xBF")) ps.p = 3;
    ps.skip_misc();
    if (ps.p >= text.size()) throw Error("xml: no root element");
    XmlElement root = ps.element();
    ps.skip_misc();
    return root;
}

namespace {

void escape_into(std::string& out, std::string_view s, bool attribute) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': if (attribute) out += "&quot;"; else out += c; break;
            case '\n': if (attribute) out += "&#10;"; else out += c; break;
            case '\r': out += "&#13;"; break;
            case '\t': if (attribute) out += "&#9;"; else out += c; break;
            default: out += c;
        }
    }
}

void write_element(std::string& out, const XmlElement& e, int depth, const XmlWriteOptions& o) {
    std::string pad;
    for (int i = 0; i < depth; i++) pad += o.indent;
    out += pad + "<" + e.name;
    for (auto& [n, v] : e.attributes) {
        out += " " + n + "=\"";
        escape_into(out, v, true);
        out += "\"";
    }
    if (e.text.empty() && e.children.empty()) {
        out += " />" + o.newline;
        return;
    }
    out += ">";
    escape_into(out, e.text, false);
    if (!e.children.empty()) {
        out += o.newline;
        for (const XmlElement& c : e.children) write_element(out, c, depth + 1, o);
        out += pad;
    }
    out += "</" + e.name + ">" + o.newline;
}

} // namespace

std::string write_xml(const XmlElement& root, const XmlWriteOptions& o) {
    std::string out;
    if (o.declaration) out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + o.newline;
    write_element(out, root, 0, o);
    return out;
}

} // namespace rk
