#include "ragekit/rel.h"
#include "ragekit/hash.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rk {

namespace {

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void put32(Bytes& b, uint32_t v) { size_t n = b.size(); b.resize(n + 4); for (size_t i = 0; i < 4; i++) b[n + i] = uint8_t(v >> (8 * i)); }
void put_float(Bytes& b, float f) { uint32_t v; std::memcpy(&v, &f, 4); put32(b, v); }
float get_float(const RelItem& it, size_t pos) { uint32_t v = it.u32(pos); float f; std::memcpy(&f, &v, 4); return f; }

struct Reader {
    std::span<const uint8_t> f;
    size_t pos = 0;
    void need(size_t n, const char* what) const {
        if (n > f.size() || pos > f.size() - n) throw Error(std::string("rel: truncated ") + what);
    }
    uint32_t u32(const char* what) { need(4, what); uint32_t v = rd32(f.data() + pos); pos += 4; return v; }
    uint8_t u8(const char* what) { need(1, what); return f[pos++]; }
};

bool rotated_index(uint32_t t) {
    switch (t) {
        case reltype::Dat10: case reltype::Dat15: case reltype::Dat16: case reltype::Dat22: case reltype::Dat54:
        case reltype::Dat149: case reltype::Dat150: case reltype::Dat151: return true;
        default: return false;
    }
}
uint32_t rot8(uint32_t h) { return (h >> 8) | (h << 24); }

} // namespace

const char* rel_type_name(uint32_t type) {
    switch (type) {
        case 4: return "dat4"; case 10: return "dat10"; case 15: return "dat15"; case 16: return "dat16"; case 22: return "dat22";
        case 54: return "dat54"; case 149: return "dat149"; case 150: return "dat150"; case 151: return "dat151";
        default: return "?";
    }
}

std::string rel_data_file_key(const std::string& file_name) {
    std::string s;
    size_t slash = file_name.find_last_of("/\\");
    for (size_t i = slash == std::string::npos ? 0 : slash + 1; i < file_name.size(); i++) s.push_back((char)tolower((unsigned char)file_name[i]));
    size_t u = s.find('_');
    return u == std::string::npos ? s : s.substr(0, u);
}

const char* dat151_type_name(uint8_t id) {
    switch (id) {
        case dat151::TrailerAudioSettings: return "TrailerAudioSettings";
        case dat151::CarAudioSettings: return "CarAudioSettings";
        case dat151::StaticEmitter: return "StaticEmitter";
        case dat151::StaticEmitterList: return "StaticEmitterList";
        case dat151::AmbientZone: return "AmbientZone";
        case dat151::AmbientRule: return "AmbientRule";
        case dat151::InteriorSettings: return "InteriorSettings";
        case dat151::InteriorWeaponMetrics: return "InteriorWeaponMetrics";
        case dat151::InteriorRoom: return "InteriorRoom";
        case dat151::DoorAudioSettingsLink: return "DoorAudioSettingsLink";
        case dat151::DoorAudioSettings: return "DoorAudioSettings";
        case dat151::DoorTuningParams: return "DoorTuningParams";
        case dat151::DoorList: return "DoorList";
        case dat151::EntityEmitter: return "EntityEmitter";
        default: return "";
    }
}

const char* dat54_type_name(uint8_t id) {
    static const char* const names[] = {"", "LoopingSound", "EnvelopeSound", "TwinLoopSound", "SpeechSound", "OnStopSound", "WrapperSound",
        "SequentialSound", "StreamingSound", "RetriggeredOverlappedSound", "CrossfadeSound", "CollapsingStereoSound", "SimpleSound",
        "MultitrackSound", "RandomizedSound", "EnvironmentSound", "DynamicEntitySound", "SequentialOverlapSound", "ModularSynthSound",
        "GranularSound", "DirectionalSound", "KineticSound", "SwitchSound", "VariableCurveSound", "VariablePrintValueSound",
        "VariableBlockSound", "IfSound", "MathOperationSound", "ParameterTransformSound", "FluctuatorSound", "AutomationSound",
        "ExternalStreamSound", "SoundSet", "AutomationNoteMapSound", "SoundSetList", "SoundHashList"};
    return id < sizeof names / sizeof names[0] ? names[id] : "";
}

// ---------------------------------------------------------------------------------------------------------------

uint32_t RelItem::u32(size_t pos) const { return pos + 4 <= bytes.size() ? rd32(bytes.data() + pos) : 0; }

Rel Rel::make(uint32_t type, uint32_t version) {
    Rel r;
    r.type = type;
    r.set_version(version);
    return r;
}
uint32_t Rel::version() const { return data.size() >= 4 ? rd32(data.data()) : 0; }
void Rel::set_version(uint32_t v) {
    if (data.size() < 4) data.assign(4, 0);
    for (int i = 0; i < 4; i++) data[(size_t)i] = uint8_t(v >> (8 * i));
}

RelItem Rel::item(const RelIndexEntry& e) const {
    RelItem it;
    it.hash = e.hash; it.name = e.name; it.offset = e.offset; it.length = e.length;
    if ((size_t)e.offset <= data.size() && (size_t)e.length <= data.size() - e.offset)
        it.bytes = std::span<const uint8_t>(data.data() + e.offset, e.length);
    if (!it.bytes.empty()) it.type_id = it.bytes[0];
    bool speech = type == reltype::Dat4 && !string_index;
    if (type != reltype::Dat54 && !speech && it.bytes.size() >= 4) {
        it.has_name_offset = true;
        it.name_offset = rd32(it.bytes.data()) >> 8;
    }
    return it;
}
std::vector<RelItem> Rel::items() const {
    std::vector<RelItem> v;
    v.reserve(index.size());
    for (auto& e : index) v.push_back(item(e));
    return v;
}
const RelIndexEntry* Rel::find_entry(uint32_t hash) const {
    for (auto& e : index) if (e.hash == hash) return &e;
    return nullptr;
}
bool Rel::find(uint32_t hash, RelItem& out) const {
    const RelIndexEntry* e = find_entry(hash);
    if (!e) return false;
    out = item(*e);
    return true;
}
bool Rel::hash_at(uint32_t file_position, uint32_t& hash) const {
    if (file_position < 8) return false;
    size_t p = file_position - 8;
    if (p + 4 > data.size()) return false;
    hash = rd32(data.data() + p);
    return true;
}
std::string Rel::table_name(const RelItem& it) const {
    if (!it.has_name_offset || names.empty()) return {};
    for (size_t i = 0; i < name_offsets.size() && i < names.size(); i++)
        if (name_offsets[i] == it.name_offset) return names[i];
    return {};
}

uint32_t Rel::item_alignment(uint32_t rel_type, uint8_t t, bool string_index) {
    switch (rel_type) {
        case reltype::Dat10: return 4;
        case reltype::Dat15:
            switch (t) { case 0: case 5: case 6: case 7: case 8: return 4; default: return 1; }
        case reltype::Dat149: case reltype::Dat150: case reltype::Dat151:
            switch (t) {
                case dat151::AmbientRule: case dat151::AmbientZone: case dat151::AlarmSettings: case dat151::ScannerSpecificLocation: return 16;
                case dat151::InteractiveMusicMood: case dat151::BarConstraint: case dat151::PedRaceToPedVoiceGroup: case dat151::SpeechParams:
                case dat151::TriggeredSpeechContext: case dat151::AmbientBankMap: case dat151::TrailerAudioSettings: case dat151::StaticEmitterList:
                case dat151::WeaponSettings: case dat151::CarAudioSettings: case dat151::StopTrackAction: return 4;
                default: return 1;
            }
        case reltype::Dat4:
            return (string_index && t == 5) ? 16 : 1;
        default: return 1;
    }
}

uint32_t Rel::add(uint32_t name_hash, std::span<const uint8_t> bytes, const std::vector<uint32_t>& hash_refs, const std::vector<uint32_t>& pack_refs) {
    if (bytes.empty()) throw Error("rel: empty item");
    if (data.size() < 4) data.assign(4, 0);
    uint32_t align = item_alignment(type, bytes[0], string_index);
    while (data.size() % align) data.push_back(0);
    uint32_t off = (uint32_t)data.size();
    data.insert(data.end(), bytes.begin(), bytes.end());
    for (uint32_t r : hash_refs) hash_table.push_back(off + 8 + r);
    for (uint32_t r : pack_refs) pack_table.push_back(off + 8 + r);
    RelIndexEntry e;
    e.hash = name_hash; e.offset = off; e.length = (uint32_t)bytes.size();
    index.push_back(e);
    sort_index();
    return off;
}

void Rel::sort_index() {
    if (string_index) return;
    if (rotated_index(type)) std::stable_sort(index.begin(), index.end(), [](const RelIndexEntry& a, const RelIndexEntry& b) { return rot8(a.hash) < rot8(b.hash); });
    else std::stable_sort(index.begin(), index.end(), [](const RelIndexEntry& a, const RelIndexEntry& b) { return a.hash < b.hash; });
}

void Rel::set_names(const std::vector<std::string>& n) {
    names = n;
    name_offsets.clear();
    uint32_t off = 0;
    for (auto& s : names) { name_offsets.push_back(off); off += (uint32_t)s.size() + 1; }
    name_table_length = names.empty() ? 4 : 4 + 4 * (uint32_t)names.size() + off;
}

std::string Rel::validate() const {
    char buf[160];
    if (data.size() < 4) return "data block shorter than 4 bytes";
    for (size_t i = 0; i < index.size(); i++) {
        auto& e = index[i];
        if ((size_t)e.offset > data.size() || (size_t)e.length > data.size() - e.offset) {
            std::snprintf(buf, sizeof buf, "index %zu (%08X) lies outside the data block", i, e.hash);
            return buf;
        }
    }
    for (uint32_t p : hash_table) if (p < 8 || p - 8 + 4 > data.size()) { std::snprintf(buf, sizeof buf, "hash table position %u outside the data block", p); return buf; }
    for (uint32_t p : pack_table) if (p < 8 || p - 8 + 4 > data.size()) { std::snprintf(buf, sizeof buf, "pack table position %u outside the data block", p); return buf; }
    return {};
}

// ---------------------------------------------------------------------------------------------------------------

Rel read_rel(std::span<const uint8_t> file) {
    Reader r{file};
    Rel rel;
    rel.type = r.u32("type");
    uint32_t dlen = r.u32("data length");
    r.need(dlen, "data block");
    rel.data.assign(file.begin() + (ptrdiff_t)r.pos, file.begin() + (ptrdiff_t)(r.pos + dlen));
    r.pos += dlen;

    rel.name_table_length = r.u32("name table length");
    uint32_t ncount = r.u32("name count");
    if (ncount > (file.size() - r.pos) / 4) throw Error("rel: implausible name count");
    for (uint32_t i = 0; i < ncount; i++) rel.name_offsets.push_back(r.u32("name offset"));
    for (uint32_t i = 0; i < ncount; i++) {
        std::string s;
        for (;;) { uint8_t c = r.u8("name"); if (!c) break; s.push_back((char)c); }
        rel.names.push_back(std::move(s));
    }

    uint32_t icount = r.u32("index count");
    if (icount) {
        // audioconfig.dat4.rel (the only dat4 without a name table) has a string index headed by a value (2524 / 2720)
        bool strings = rel.type == reltype::Dat4 && rel.name_table_length == 4;
        rel.string_index = strings;
        if (strings) {
            rel.string_index_flags = r.u32("index flags");
            for (uint32_t i = 0; i < icount; i++) {
                RelIndexEntry e;
                uint8_t sl = r.u8("index name length");
                r.need(sl, "index name");
                e.name.assign((const char*)file.data() + r.pos, sl);
                r.pos += sl;
                e.offset = r.u32("index offset"); e.length = r.u32("index length");
                e.hash = joaat_lower(e.name.substr(0, e.name.find('\0')));
                rel.index.push_back(std::move(e));
            }
        } else {
            if (icount > (file.size() - r.pos) / 12) throw Error("rel: implausible index count");
            for (uint32_t i = 0; i < icount; i++) {
                RelIndexEntry e;
                e.hash = r.u32("index hash"); e.offset = r.u32("index offset"); e.length = r.u32("index length");
                rel.index.push_back(e);
            }
        }
    }
    uint32_t hcount = r.u32("hash table count");
    if (hcount > (file.size() - r.pos) / 4) throw Error("rel: implausible hash table count");
    for (uint32_t i = 0; i < hcount; i++) rel.hash_table.push_back(r.u32("hash table"));
    uint32_t pcount = r.u32("pack table count");
    if (pcount > (file.size() - r.pos) / 4) throw Error("rel: implausible pack table count");
    for (uint32_t i = 0; i < pcount; i++) rel.pack_table.push_back(r.u32("pack table"));
    rel.trailing.assign(file.begin() + (ptrdiff_t)r.pos, file.end());
    return rel;
}

Bytes write_rel(const Rel& r) {
    Bytes b;
    b.reserve(r.data.size() + 256);
    put32(b, r.type);
    put32(b, (uint32_t)r.data.size());
    b.insert(b.end(), r.data.begin(), r.data.end());
    put32(b, r.name_table_length);
    put32(b, (uint32_t)r.names.size());
    for (uint32_t o : r.name_offsets) put32(b, o);
    for (auto& s : r.names) { b.insert(b.end(), s.begin(), s.end()); b.push_back(0); }
    put32(b, (uint32_t)r.index.size());
    if (!r.index.empty()) {
        if (r.string_index) {
            put32(b, r.string_index_flags);
            for (auto& e : r.index) {
                b.push_back((uint8_t)e.name.size());
                b.insert(b.end(), e.name.begin(), e.name.end());
                put32(b, e.offset); put32(b, e.length);
            }
        } else {
            for (auto& e : r.index) { put32(b, e.hash); put32(b, e.offset); put32(b, e.length); }
        }
    }
    put32(b, (uint32_t)r.hash_table.size());
    for (uint32_t p : r.hash_table) put32(b, p);
    put32(b, (uint32_t)r.pack_table.size());
    for (uint32_t p : r.pack_table) put32(b, p);
    b.insert(b.end(), r.trailing.begin(), r.trailing.end());
    return b;
}

std::string rel_describe(const Rel& r, size_t max_items) {
    std::string s;
    char buf[256];
    std::snprintf(buf, sizeof buf, "rel %s (type %u)  version %u  data %zu bytes  names %zu (table %u)  %s index %zu items  hash table %zu  pack table %zu%s\n",
                  rel_type_name(r.type), r.type, r.version(), r.data.size(), r.names.size(), r.name_table_length, r.string_index ? "string" : "hash",
                  r.index.size(), r.hash_table.size(), r.pack_table.size(), r.trailing.empty() ? "" : "  TRAILING BYTES");
    s += buf;
    size_t n = 0;
    for (auto& it : r.items()) {
        if (max_items && n++ >= max_items) { s += "  ...\n"; break; }
        std::string tn = r.type == reltype::Dat151 ? dat151_type_name(it.type_id) : r.type == reltype::Dat54 ? dat54_type_name(it.type_id) : "";
        std::snprintf(buf, sizeof buf, "  %08X  @%-8u %-6u type %-3u %s%s%s\n", it.hash, it.offset, it.length, it.type_id, tn.c_str(),
                      it.name.empty() ? "" : "  ", it.name.c_str());
        s += buf;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------
// typed items

namespace {
void put_header(Bytes& b, uint8_t type, uint32_t name_offset) { put32(b, (name_offset & 0xFFFFFF) << 8 | type); }
void need_type(const RelItem& it, uint8_t t, size_t min_len, const char* what) {
    if (it.type_id != t || it.bytes.size() < min_len) throw Error(std::string("rel: item is not a ") + what);
}
}

Dat151InteriorSettings Dat151InteriorSettings::read(const RelItem& it) {
    need_type(it, dat151::InteriorSettings, 20, "InteriorSettings");
    Dat151InteriorSettings v;
    v.name_offset = it.u32(0) >> 8;
    v.flags = it.u32(4); v.walla_sound_set = it.u32(8); v.reflections = it.u32(12);
    uint32_t n = it.u32(16);
    if (it.bytes.size() < 20 + size_t(n) * 4) throw Error("rel: InteriorSettings room list is cut off");
    for (uint32_t i = 0; i < n; i++) v.rooms.push_back(it.u32(20 + 4 * i));
    return v;
}
Bytes Dat151InteriorSettings::bytes() const {
    Bytes b;
    put_header(b, dat151::InteriorSettings, name_offset);
    put32(b, flags); put32(b, walla_sound_set); put32(b, reflections); put32(b, (uint32_t)rooms.size());
    for (uint32_t r : rooms) put32(b, r);
    return b;
}
std::vector<uint32_t> Dat151InteriorSettings::refs() const {
    std::vector<uint32_t> r;
    for (size_t i = 0; i < rooms.size(); i++) r.push_back(20 + 4 * (uint32_t)i);
    return r;
}

Dat151InteriorRoom Dat151InteriorRoom::read(const RelItem& it) {
    need_type(it, dat151::InteriorRoom, 68, "InteriorRoom");
    Dat151InteriorRoom v;
    v.name_offset = it.u32(0) >> 8;
    v.flags = it.u32(4); v.room_name = it.u32(8); v.ambient_zone = it.u32(12);
    uint32_t w = it.u32(16);
    v.interior_type = uint8_t(w); v.pad0 = uint8_t(w >> 8); v.pad1 = uint16_t(w >> 16);
    v.reverb_small = get_float(it, 20); v.reverb_medium = get_float(it, 24); v.reverb_large = get_float(it, 28);
    v.room_tone_sound = it.u32(32);
    w = it.u32(36);
    v.rain_type = uint8_t(w); v.pad2 = uint8_t(w >> 8); v.pad3 = uint16_t(w >> 16);
    v.exterior_audibility = get_float(it, 40); v.room_occlusion_damping = get_float(it, 44);
    v.non_marked_portal_occlusion = get_float(it, 48);
    v.distance_from_portal_for_occlusion = get_float(it, 52); v.distance_from_portal_fade_distance = get_float(it, 56);
    v.weapon_metrics = it.u32(60); v.walla_sound_set = it.u32(64);
    return v;
}
Bytes Dat151InteriorRoom::bytes() const {
    Bytes b;
    put_header(b, dat151::InteriorRoom, name_offset);
    put32(b, flags); put32(b, room_name); put32(b, ambient_zone);
    put32(b, uint32_t(interior_type) | uint32_t(pad0) << 8 | uint32_t(pad1) << 16);
    put_float(b, reverb_small); put_float(b, reverb_medium); put_float(b, reverb_large);
    put32(b, room_tone_sound);
    put32(b, uint32_t(rain_type) | uint32_t(pad2) << 8 | uint32_t(pad3) << 16);
    put_float(b, exterior_audibility); put_float(b, room_occlusion_damping); put_float(b, non_marked_portal_occlusion);
    put_float(b, distance_from_portal_for_occlusion); put_float(b, distance_from_portal_fade_distance);
    put32(b, weapon_metrics); put32(b, walla_sound_set);
    return b;
}
std::vector<uint32_t> Dat151InteriorRoom::refs() const { return {12}; } // AmbientZone

Dat151StaticEmitterList Dat151StaticEmitterList::read(const RelItem& it) {
    need_type(it, dat151::StaticEmitterList, 8, "StaticEmitterList");
    Dat151StaticEmitterList v;
    v.name_offset = it.u32(0) >> 8;
    uint32_t n = it.u32(4);
    if (it.bytes.size() < 8 + size_t(n) * 4) throw Error("rel: StaticEmitterList is cut off");
    for (uint32_t i = 0; i < n; i++) v.emitters.push_back(it.u32(8 + 4 * i));
    return v;
}
Bytes Dat151StaticEmitterList::bytes() const {
    Bytes b;
    put_header(b, dat151::StaticEmitterList, name_offset);
    put32(b, (uint32_t)emitters.size());
    for (uint32_t e : emitters) put32(b, e);
    return b;
}
// CodeWalker's list class declares no hash table offsets; the game files do list them (checked by the scan test).
std::vector<uint32_t> Dat151StaticEmitterList::refs() const { return {}; }

Dat151DoorAudioSettingsLink Dat151DoorAudioSettingsLink::read(const RelItem& it) {
    need_type(it, dat151::DoorAudioSettingsLink, 8, "DoorAudioSettingsLink");
    Dat151DoorAudioSettingsLink v;
    v.name_offset = it.u32(0) >> 8;
    v.door = it.u32(4);
    return v;
}
Bytes Dat151DoorAudioSettingsLink::bytes() const {
    Bytes b;
    put_header(b, dat151::DoorAudioSettingsLink, name_offset);
    put32(b, door);
    return b;
}
std::vector<uint32_t> Dat151DoorAudioSettingsLink::refs() const { return {4}; }

uint32_t rel_add(Rel& r, uint32_t h, const Dat151InteriorSettings& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }
uint32_t rel_add(Rel& r, uint32_t h, const Dat151InteriorRoom& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }
uint32_t rel_add(Rel& r, uint32_t h, const Dat151StaticEmitterList& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }
uint32_t rel_add(Rel& r, uint32_t h, const Dat151DoorAudioSettingsLink& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }

std::string door_audio_link_name(uint32_t model_hash) {
    char b[16];
    std::snprintf(b, sizeof b, "dasl_%08x", model_hash);
    return b;
}

Dat151DoorAudioSettings Dat151DoorAudioSettings::read(const RelItem& it) {
    need_type(it, dat151::DoorAudioSettings, 16, "DoorAudioSettings");
    Dat151DoorAudioSettings v;
    v.name_offset = it.u32(0) >> 8;
    v.sounds = it.u32(4); v.tuning = it.u32(8); v.max_occlusion = get_float(it, 12);
    return v;
}
Bytes Dat151DoorAudioSettings::bytes() const {
    Bytes b;
    put_header(b, dat151::DoorAudioSettings, name_offset);
    put32(b, sounds); put32(b, tuning); put_float(b, max_occlusion);
    return b;
}
std::vector<uint32_t> Dat151DoorAudioSettings::refs() const { return {8}; }

Dat151DoorTuningParams Dat151DoorTuningParams::read(const RelItem& it) {
    need_type(it, dat151::DoorTuningParams, 40, "DoorTuningParams");
    Dat151DoorTuningParams v;
    v.name_offset = it.u32(0) >> 8;
    v.flags = it.u32(4);
    v.open_threshold = get_float(it, 8); v.heading_threshold = get_float(it, 12); v.closed_threshold = get_float(it, 16);
    v.speed_threshold = get_float(it, 20); v.speed_scale = get_float(it, 24); v.heading_delta_threshold = get_float(it, 28);
    v.angular_velocity_threshold = get_float(it, 32); v.door_type = (int32_t)it.u32(36);
    return v;
}
Bytes Dat151DoorTuningParams::bytes() const {
    Bytes b;
    put_header(b, dat151::DoorTuningParams, name_offset);
    put32(b, flags);
    put_float(b, open_threshold); put_float(b, heading_threshold); put_float(b, closed_threshold); put_float(b, speed_threshold);
    put_float(b, speed_scale); put_float(b, heading_delta_threshold); put_float(b, angular_velocity_threshold);
    put32(b, (uint32_t)door_type);
    return b;
}

Dat151EntityEmitter Dat151EntityEmitter::read(const RelItem& it) {
    need_type(it, dat151::EntityEmitter, 52, "EntityEmitter");
    Dat151EntityEmitter v;
    v.name_offset = it.u32(0) >> 8;
    v.flags = it.u32(4); v.sound = it.u32(8); v.max_distance = get_float(it, 12);
    v.business_hours_prob = get_float(it, 16); v.evening_prob = get_float(it, 20); v.night_prob = get_float(it, 24);
    v.cone_inner_angle = get_float(it, 28); v.cone_outer_angle = get_float(it, 32); v.cone_max_atten = get_float(it, 36);
    v.stop_after_loud_sound = it.u32(40);
    const uint32_t w = it.u32(44);
    v.max_path_depth = uint8_t(w); v.pad0 = uint8_t(w >> 8); v.pad1 = uint16_t(w >> 16);
    v.broken_health = get_float(it, 48);
    return v;
}
Bytes Dat151EntityEmitter::bytes() const {
    Bytes b;
    put_header(b, dat151::EntityEmitter, name_offset);
    put32(b, flags); put32(b, sound); put_float(b, max_distance);
    put_float(b, business_hours_prob); put_float(b, evening_prob); put_float(b, night_prob);
    put_float(b, cone_inner_angle); put_float(b, cone_outer_angle); put_float(b, cone_max_atten);
    put32(b, stop_after_loud_sound);
    put32(b, uint32_t(max_path_depth) | uint32_t(pad0) << 8 | uint32_t(pad1) << 16);
    put_float(b, broken_health);
    return b;
}

uint32_t rel_add(Rel& r, uint32_t h, const Dat151DoorAudioSettings& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }
uint32_t rel_add(Rel& r, uint32_t h, const Dat151DoorTuningParams& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }
uint32_t rel_add(Rel& r, uint32_t h, const Dat151EntityEmitter& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }

// ---- dat54

namespace {
// the size of each header field by its bit (CodeWalker's RelSoundHeader)
constexpr uint8_t kHeaderFieldSize[32] = {4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 2, 2, 2, 4, 2, 2, 2, 2, 4, 2, 1, 1, 1, 4, 4, 2, 2, 2, 2, 2};

std::span<const uint8_t> sound_body(const RelItem& it, uint8_t type, const char* what, Dat54Header& h, size_t min_body) {
    if (it.bytes.empty() || it.type_id != type) throw Error(std::string("rel: item is not a ") + what);
    size_t pos = 1;
    h = Dat54Header::read(it.bytes, pos);
    if (it.bytes.size() - pos < min_body) throw Error(std::string("rel: ") + what + " is cut off");
    return it.bytes.subspan(pos);
}
uint32_t body_u32(std::span<const uint8_t> b, size_t pos) { return rd32(b.data() + pos); }
uint16_t body_u16(std::span<const uint8_t> b, size_t pos) { return uint16_t(b[pos] | b[pos + 1] << 8); }
void put16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
}

bool Dat54Header::has(uint8_t field) const {
    return field < 32 && ((flags >> (field & ~7u)) & 0xFF) != 0xAA && (flags >> field & 1);
}
void Dat54Header::set(uint8_t field, uint32_t value) {
    if (field >= 32) return;
    const uint32_t shift = field & ~7u;
    if (((flags >> shift) & 0xFF) == 0xAA) flags &= ~(0xFFu << shift);
    flags |= 1u << field;
    values[field] = value;
}
size_t Dat54Header::size() const {
    size_t n = 4;
    for (uint8_t f = 0; f < 32; f++) if (has(f)) n += kHeaderFieldSize[f];
    return n;
}
Dat54Header Dat54Header::read(std::span<const uint8_t> b, size_t& pos) {
    if (pos + 4 > b.size()) throw Error("rel: a sound header is cut off");
    Dat54Header h;
    h.flags = rd32(b.data() + pos);
    pos += 4;
    for (uint8_t f = 0; f < 32; f++) {
        if (!h.has(f)) continue;
        const size_t n = kHeaderFieldSize[f];
        if (pos + n > b.size()) throw Error("rel: a sound header is cut off");
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++) v |= uint32_t(b[pos + i]) << (8 * i);
        h.values[f] = v;
        pos += n;
    }
    return h;
}
void Dat54Header::write(Bytes& b) const {
    put32(b, flags);
    for (uint8_t f = 0; f < 32; f++) {
        if (!has(f)) continue;
        for (size_t i = 0; i < kHeaderFieldSize[f]; i++) b.push_back(uint8_t(values[f] >> (8 * i)));
    }
}

Dat54SimpleSound Dat54SimpleSound::read(const RelItem& it) {
    Dat54SimpleSound v;
    const auto b = sound_body(it, dat54::SimpleSound, "SimpleSound", v.header, 9);
    v.container = body_u32(b, 0); v.wave = body_u32(b, 4); v.wave_slot = b[8];
    return v;
}
Bytes Dat54SimpleSound::bytes() const {
    Bytes b{dat54::SimpleSound};
    header.write(b);
    put32(b, container); put32(b, wave); b.push_back(wave_slot);
    return b;
}
std::vector<uint32_t> Dat54SimpleSound::pack_refs() const { return {uint32_t(1 + header.size())}; }

Dat54SoundSet Dat54SoundSet::read(const RelItem& it) {
    Dat54SoundSet v;
    const auto b = sound_body(it, dat54::SoundSet, "SoundSet", v.header, 4);
    const uint32_t n = body_u32(b, 0);
    if ((b.size() - 4) / 8 < n) throw Error("rel: SoundSet is cut off");
    for (uint32_t i = 0; i < n; i++) v.sounds.emplace_back(body_u32(b, 4 + 8 * i), body_u32(b, 8 + 8 * i));
    return v;
}
Bytes Dat54SoundSet::bytes() const {
    Bytes b{dat54::SoundSet};
    header.write(b);
    put32(b, (uint32_t)sounds.size());
    for (auto& [name, sound] : sounds) { put32(b, name); put32(b, sound); }
    return b;
}
std::vector<uint32_t> Dat54SoundSet::refs() const {
    std::vector<uint32_t> r;
    const uint32_t base = uint32_t(1 + header.size());
    for (size_t i = 0; i < sounds.size(); i++) r.push_back(base + 8 + 8 * (uint32_t)i);
    return r;
}

Dat54LoopingSound Dat54LoopingSound::read(const RelItem& it) {
    Dat54LoopingSound v;
    const auto b = sound_body(it, dat54::LoopingSound, "LoopingSound", v.header, 14);
    v.loop_count = (int16_t)body_u16(b, 0); v.loop_count_variance = (int16_t)body_u16(b, 2); v.loop_point = (int16_t)body_u16(b, 4);
    v.child = body_u32(b, 6); v.loop_count_variable = body_u32(b, 10);
    return v;
}
Bytes Dat54LoopingSound::bytes() const {
    Bytes b{dat54::LoopingSound};
    header.write(b);
    put16(b, (uint16_t)loop_count); put16(b, (uint16_t)loop_count_variance); put16(b, (uint16_t)loop_point);
    put32(b, child); put32(b, loop_count_variable);
    return b;
}
std::vector<uint32_t> Dat54LoopingSound::refs() const { return {uint32_t(1 + header.size() + 6)}; }

uint32_t rel_add(Rel& r, uint32_t h, const Dat54SimpleSound& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs(), v.pack_refs()); }
uint32_t rel_add(Rel& r, uint32_t h, const Dat54SoundSet& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }
uint32_t rel_add(Rel& r, uint32_t h, const Dat54LoopingSound& v) { Bytes b = v.bytes(); return r.add(h, b, v.refs()); }

std::vector<std::string> read_nametable(std::span<const uint8_t> file) {
    std::vector<std::string> names;
    size_t start = 0;
    for (size_t i = 0; i < file.size(); i++)
        if (file[i] == 0) { names.emplace_back((const char*)file.data() + start, i - start); start = i + 1; }
    if (start != file.size()) throw Error("nametable: the last name has no terminating NUL");
    return names;
}

Bytes write_nametable(const std::vector<std::string>& names) {
    Bytes b;
    for (const std::string& n : names) {
        if (n.find('\0') != std::string::npos) throw Error("nametable: a name with a NUL in it");
        b.insert(b.end(), n.begin(), n.end());
        b.push_back(0);
    }
    return b;
}

} // namespace rk
