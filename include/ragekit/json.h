// ragekit - JSON documents: the .ugc user-generated content (creator missions, the game's own jobs) and the few .json
// data files. A small DOM: objects keep their keys in the file's order and numbers their text, so a document reads back
// the same after write_json.
#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rk {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;                                        // a number as written ("-1", "0.5", "1e3"), or a string's value
    std::vector<JsonValue> items;                            // Array
    std::vector<std::pair<std::string, JsonValue>> members;  // Object, in the file's order

    double number() const;
    /// The member of an object with this key, nullptr when there is none.
    const JsonValue* get(std::string_view key) const;
    bool operator==(const JsonValue& o) const = default;
};

/// Parses a JSON document (UTF-8; a byte order mark is skipped). Throws rk::Error with the byte offset of a syntax error.
JsonValue parse_json(std::string_view text);
/// The document as text: compact, or indented with tabs (as the game's .ugc files are) when `indent`.
std::string write_json(const JsonValue& v, bool indent = true);

}  // namespace rk
