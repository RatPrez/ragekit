// ragekit - CodeWalker XML writers: .ydr.xml / .ydd.xml / .ybn.xml / .ytd.xml, text for text what
// CodeWalker's "Export XML" produces (YdrXml.GetXml, YddXml.GetXml, YbnXml.GetXml, YtdXml.GetXml),
// so CodeWalker's importers and Sollumz (Blender) accept it. Textures are not inside the XML: each
// one is `<name>.dds` in a folder next to it, `dds_files` makes those files.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "bounds.h"
#include "drawable.h"
#include "texture.h"

namespace rk {

struct XmlOptions {
    /// Names for hashes the library does not know (it knows every stock shader name and .sps file name):
    /// shader names, projected texture hashes of lights, ydd drawable names. Return "" for unknown;
    /// CodeWalker's "hash_XXXXXXXX" is written then.
    std::function<std::string(uint32_t)> hash_name;
    /// CodeWalker (a Windows program) writes CR LF line ends; false writes LF only.
    bool crlf = true;
    /// A shader parameter whose name is not in CodeWalker's table: CodeWalker prints its hash as a plain number, which
    /// its own importer then reads back as the hash of the text "2447005446" (the parameter is lost). Default: write
    /// "hash_XXXXXXXX", which the importer understands. true reproduces CodeWalker's text exactly.
    bool unknown_param_as_number = false;
};

/// `<Drawable>` document (a .ydr).
std::string write_drawable_xml(const Drawable& d, const XmlOptions& opt = {});
/// `<DrawableDictionary>` document (a .ydd): one `<Item>` per drawable.
std::string write_drawable_dictionary_xml(const DrawableDictionary& dd, const XmlOptions& opt = {});
/// `<BoundsFile>` document (a .ybn).
std::string write_bounds_xml(const Bound& b, const XmlOptions& opt = {});
/// `<TextureDictionary>` document (a .ytd). Each item's FileName is `<name>.dds`, relative to the folder the
/// importer is given (CodeWalker writes no folder into the XML); `dds_folder_name` is only for the caller's
/// bookkeeping and is not written.
std::string write_texture_dictionary_xml(const TextureDictionary& td, const std::string& dds_folder_name = "");

/// The DDS files the XML refers to: (`<name>.dds`, bytes). A drawable's are its embedded textures.
std::vector<std::pair<std::string, Bytes>> dds_files(const TextureDictionary& td);
std::vector<std::pair<std::string, Bytes>> dds_files(const Drawable& d);
std::vector<std::pair<std::string, Bytes>> dds_files(const DrawableDictionary& dd);

/// A float the way CodeWalker prints it (.NET shortest round-trip: "1E-05", "0.07499981", "NaN").
std::string xml_float(float f);

} // namespace rk
