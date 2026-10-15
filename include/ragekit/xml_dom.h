// ragekit - a small XML reader for the game's data files (content.xml, setup2.xml, *.meta) and CodeWalker XML.
//
// Elements, attributes, text; comments, processing instructions, the DOCTYPE and a byte order mark are skipped,
// CDATA is text, the five predefined entities and numeric character references are decoded. No namespaces, no
// DTD entities. Element and attribute names keep their case; lookups are case-sensitive unless noted.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rk {

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;                      // the element's own text (trimmed), children's text not included
    std::vector<XmlElement> children;

    /// First child with this name (nullptr if none).
    const XmlElement* child(std::string_view n) const;
    /// First child whose name equals n ignoring ASCII case.
    const XmlElement* child_ci(std::string_view n) const;
    /// Every child with this name.
    std::vector<const XmlElement*> children_named(std::string_view n) const;
    /// The attribute's value, `fallback` if absent.
    std::string attribute(std::string_view n, std::string_view fallback = {}) const;
    /// The text of the first child with this name ("" if none).
    std::string child_text(std::string_view n) const;
    /// `<n value="..."/>` as the game's data files write booleans and numbers: the value attribute of child n.
    std::string child_value(std::string_view n, std::string_view fallback = {}) const;
};

/// Parses a document and returns its root element. Throws rk::Error on malformed input.
XmlElement parse_xml(std::string_view text);

} // namespace rk
