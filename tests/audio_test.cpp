// rk_audio_test: self-contained checks of rel.h, awc.h and audio_occlusion.h (no game files needed).
// rk_audio_test <file.rel>...: also every door, emitter and sound item the library types in those files, written back.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include "ragekit/ragekit.h"
#include "ragekit/rsc.h"

using namespace rk;

static int failures = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(int argc, char** argv) {
    // the proxy hash of the game's v_apart_midspaz (342.81573, -997.42883, -100) is 2860953274
    check(occlusion_proxy_hash("v_apart_midspaz", Vec3(342.81573f, -997.42883f, -100.f)) == 2860953274u, "proxy hash of v_apart_midspaz");
    check(rel_data_file_key("dlcLuxe_game.dat151.rel") == "dlcluxe", "data file key is cut at the first underscore");
    check(rel_data_file_key("rat_apt01_game.dat151.rel") == "rat", "data file key of rat_apt01 is 'rat' (two rat_ files would clash)");

    check(interior_dat151_file_name("rat_apt01") == "ratapt01_game.dat151.rel" && rel_data_file_key(interior_dat151_file_name("rat_apt01")) == "ratapt01", "unique rel file name");

    // a three room interior: limbo - hall - bath, a door between hall and bath
    AudioOcclusion m;
    m.name = "test_int"; m.position = Vec3(10.f, 20.f, -50.f);
    m.rooms = {"limbo", "rm_hall", "rm_bath"};
    OcclusionPortal p0; p0.room_from = 0; p0.room_to = 1;                  // front door (no entity)
    OcclusionPortal p1; p1.room_from = 1; p1.room_to = 2;
    OcclusionEntity door; door.model = "v_ilev_test_door"; p1.entities.push_back(door);
    m.portals = {p0, p1};
    OcclusionData d = build_occlusion(m);
    check(d.portals.size() == 4, "two non-mirror portals give four portal infos");
    check(!d.nodes.empty(), "paths exist");
    size_t doors = 0;
    for (auto& pi : d.portals) for (auto& e : pi.entities) { doors++; check(e.model_hash == joaat_lower("v_ilev_test_door") && e.is_door, "door entity"); }
    check(doors == 2, "the door sits in both directions of its portal");
    Bytes ymt = write_occlusion_ymt(d);
    OcclusionData back = read_occlusion_ymt(ymt);
    check(back.portals.size() == d.portals.size() && back.nodes.size() == d.nodes.size(), "ymt reads back");
    check(write_occlusion_ymt(back) == ymt, "ymt read -> write is stable");
    Pso pso = read_pso(ymt);
    check(pso.schema.elements.size() == 5 && pso.map.root_id == 5, "5 structure infos, root block 5 (entities, portals, children, nodes, root)");
    // no entities anywhere: no entity block and no entity struct
    OcclusionData plain = build_occlusion([&] { AudioOcclusion a = m; for (auto& p : a.portals) p.entities.clear(); return a; }());
    Pso pp = read_pso(write_occlusion_ymt(plain));
    check(pp.schema.elements.size() == 4 && pp.map.root_id == 4, "without entities: 4 structure infos, root block 4");

    // dat151
    Rel rel = build_interior_dat151(m);
    check(rel.index.size() == 3 && rel.validate().empty(), "dat151 has settings + 2 rooms");
    Bytes rb = write_rel(rel);
    Rel r2 = read_rel(rb);
    check(write_rel(r2) == rb, "rel read -> write is byte-identical");
    RelItem it;
    check(r2.find(joaat_lower("test_int"), it) && it.type_id == dat151::InteriorSettings, "InteriorSettings found by name hash");
    Dat151InteriorSettings is = Dat151InteriorSettings::read(it);
    check(is.rooms.size() == 2 && is.rooms[0] == joaat_lower("test_int_rm_hall"), "settings list the rooms");
    bool refs = true;
    for (size_t i = 0; i < is.rooms.size(); i++) { uint32_t h = 0; refs = refs && r2.hash_at(r2.hash_table[i], h) && h == is.rooms[i]; }
    check(refs, "the hash table points at the room hash fields");
    RelItem room;
    check(r2.find(joaat_lower("test_int_rm_bath"), room) && Dat151InteriorRoom::read(room).room_name == joaat_lower("rm_bath"), "room item");
    // index order: hashes rotated right by 8 ascending
    bool sorted = true;
    for (size_t i = 1; i < r2.index.size(); i++) {
        auto rot = [](uint32_t h) { return (h >> 8) | (h << 24); };
        sorted = sorted && rot(r2.index[i - 1].hash) < rot(r2.index[i].hash);
    }
    check(sorted, "index sorted like the game's");
    check(rel_type_name(r2.type) == std::string("dat151"), "type name");

    // ---- wave packs from PCM
    std::vector<int16_t> tone(10000);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = (int16_t)(12000.0 * std::sin(i * 0.05) + 3000.0 * std::sin(i * 0.31));
    AwcSoundSource pcm_src{"door_open", 22050, tone, 0, -1};
    AwcSoundSource adpcm_src{"hum_loop", 32000, std::vector<int16_t>(tone.begin(), tone.begin() + 9001), 4, 0};
    const Bytes pack = write_awc(make_awc({pcm_src, adpcm_src}));
    const AwcFile pa = read_awc(pack);
    check(pa.flags == 0xFF01 && pa.streams.size() == 2 && !pa.whole_file_encrypted, "a pack of two streams, chunk indices, not encrypted");
    check(pa.streams[0].id < pa.streams[1].id, "streams in ascending id order");
    check(write_awc(pa) == pack, "the pack reads and writes back byte for byte");
    const auto sounds = awc_decode(pa);
    bool found_pcm = false, found_adpcm = false;
    for (const AwcAudio& s : sounds) {
        if (s.id == (joaat_lower("door_open") & 0x1FFFFFFF)) found_pcm = s.codec == 0 && s.sample_rate == 22050 && s.pcm == tone;
        if (s.id == (joaat_lower("hum_loop") & 0x1FFFFFFF)) {
            double err = 0, sig = 0;
            for (size_t i = 0; i < s.pcm.size(); i++) { err += std::pow(s.pcm[i] - tone[i], 2.0); sig += std::pow((double)tone[i], 2.0); }
            const double snr = 10 * std::log10(sig / std::max(err, 1.0));
            std::printf("     ADPCM round trip: %zu samples, SNR %.1f dB\n", s.pcm.size(), snr);
            found_adpcm = s.codec == 4 && s.pcm.size() == 9001 && snr > 25;
        }
    }
    check(found_pcm, "PCM comes back sample for sample");
    check(found_adpcm, "ADPCM comes back within 25 dB SNR (blocks of 4,088 samples, an odd count)");
    for (const AwcStream& s : pa.streams) {
        const AwcFormat f = AwcFormat::read(s.chunk(awc_chunk::Format)->data);
        if (f.codec == 0) check(s.chunks.size() == 3 && s.chunks[0].type == awc_chunk::Peak && s.chunks[1].type == awc_chunk::Data && f.loop_point == -1 && f.has_peak,
                                "PCM: peak, data, format (the game's order), plays once");
        else check(s.chunks.size() == 3 && s.chunks[0].type == awc_chunk::Data && s.chunks[2].type == awc_chunk::Peak && f.loop_point == 0,
                   "ADPCM: data, format, peak, loops");
    }
    {
        int16_t mx = 0;
        for (size_t i = 0; i < 4096; i++) mx = std::max<int16_t>(mx, (int16_t)std::abs(tone[i]));
        check(awc_peaks(tone).size() == 2 && awc_peaks(tone)[0] == std::min(65535, 2 * mx), "peaks: the first 4,096 samples and one more block");
    }
    check(awc_encode_adpcm(std::vector<int16_t>(4088)).size() == 2048 && awc_encode_adpcm(std::vector<int16_t>(4089)).size() == 2048 + 5, "ADPCM blocks of 2,048 bytes, the last cut to its samples");
    bool threw = false;
    try { make_awc({{"a", 22050, tone, 0, -1}, {"A", 22050, tone, 0, -1}}); } catch (const Error&) { threw = true; }
    check(threw, "two names of one id are refused");

    // ---- .wav files
    {
        Bytes wav = awc_wav(AwcAudio{0, 44100, 4, 0, {100, -100, 200, -200}});
        const WavAudio w = read_wav(wav);
        check(w.channels == 1 && w.sample_rate == 44100 && w.samples.size() == 4 && w.samples[2] == 200, "a mono 16-bit .wav");
        Bytes st = wav;   // stereo: two frames of (100, -100), (200, -200)
        st[22] = 2; st[32] = 4;
        const WavAudio s2 = read_wav(st);
        check(s2.channels == 2 && s2.frames() == 2 && s2.mono() == std::vector<int16_t>{0, 0}, "stereo mixed to mono");
        Bytes f32 = wav; f32[20] = 3; f32[34] = 32;
        threw = false;
        try { read_wav(f32); } catch (const Error& e) { threw = std::string(e.what()).find("float") != std::string::npos; }
        check(threw, "a float .wav is refused with a plain message");
        Bytes b24 = wav; b24[34] = 24;
        threw = false;
        try { read_wav(b24); } catch (const Error& e) { threw = std::string(e.what()).find("24-bit") != std::string::npos; }
        check(threw, "a 24-bit .wav is refused");
    }

    // ---- typed door and emitter items
    {
        Rel game = Rel::make(reltype::Dat151, 1);
        Dat151DoorTuningParams tp;
        rel_add(game, joaat_lower("my_dtp"), tp);
        Dat151DoorAudioSettings ds;
        ds.sounds = joaat_lower("my_door_sounds"); ds.tuning = joaat_lower("my_dtp");
        rel_add(game, joaat_lower("my_door"), ds);
        Dat151DoorAudioSettingsLink link;
        link.door = joaat_lower("my_door");
        rel_add(game, joaat_lower(door_audio_link_name(joaat_lower("my_door_model"))), link);
        Dat151EntityEmitter em;
        em.sound = joaat_lower("my_hum"); em.max_distance = 25;
        rel_add(game, joaat_lower("my_emitter"), em);
        const Rel g2 = read_rel(write_rel(game));
        RelItem it;
        check(g2.find(joaat_lower("my_door"), it) && Dat151DoorAudioSettings::read(it).tuning == joaat_lower("my_dtp") && Dat151DoorAudioSettings::read(it).bytes() == Bytes(it.bytes.begin(), it.bytes.end()), "DoorAudioSettings");
        check(g2.find(joaat_lower("my_dtp"), it) && it.bytes.size() == 40 && Dat151DoorTuningParams::read(it).door_type == 4, "DoorTuningParams, 40 bytes");
        check(g2.find(joaat_lower("my_emitter"), it) && it.bytes.size() == 52 && Dat151EntityEmitter::read(it).max_distance == 25, "EntityEmitter, 52 bytes");
        check(door_audio_link_name(0xB7FF825D) == "dasl_b7ff825d" && joaat_lower("DASL_B7FF825D") == joaat_lower("dasl_b7ff825d"), "the link's name");
        check(g2.hash_table.size() == 2, "hash table: the link's door and the settings' tuning");

        Rel snd = Rel::make(reltype::Dat54, 1);
        Dat54SimpleSound ss;
        ss.container = joaat_lower("dlc_test/test"); ss.wave = joaat_lower("door_open");
        ss.header.set(dat54_field::Volume, (uint16_t)(int16_t)-300);
        rel_add(snd, joaat_lower("my_door_open"), ss);
        Dat54SoundSet set;
        set.sounds = {{joaat_lower("opening"), joaat_lower("my_door_open")}};
        rel_add(snd, joaat_lower("my_door_sounds"), set);
        Dat54LoopingSound loop;
        loop.child = joaat_lower("my_door_open");
        rel_add(snd, joaat_lower("my_loop"), loop);
        const Rel s2 = read_rel(write_rel(snd));
        check(s2.find(joaat_lower("my_door_open"), it) && it.bytes.size() == 1 + 6 + 9, "SimpleSound with a volume: type, flags and one field, the wave");
        const Dat54SimpleSound sb = Dat54SimpleSound::read(it);
        check(sb.header.flags == 0xAAAAAA04 && (int16_t)sb.header.get(dat54_field::Volume) == -300 && sb.wave == joaat_lower("door_open"), "the header's volume field");
        uint32_t h = 0;
        check(s2.pack_table.size() == 1 && s2.hash_at(s2.pack_table[0], h) && h == joaat_lower("dlc_test/test"), "the pack table points at the container");
        int set_refs = 0;
        for (uint32_t p : s2.hash_table) if (s2.hash_at(p, h) && h == joaat_lower("my_door_open")) set_refs++;
        check(set_refs == 2, "the hash table points at the set's sound and the loop's child");
        check(s2.find(joaat_lower("my_door_sounds"), it) && Dat54SoundSet::read(it).sounds.size() == 1, "SoundSet");
    }

    // ---- with arguments: every typed item of the game's files read and written back byte for byte, its refs in the hash table
    for (int a = 1; a < argc; a++) {
        const Rel g = read_rel(rsc::read_file(argv[a]));
        std::set<uint32_t> table(g.hash_table.begin(), g.hash_table.end()), packs(g.pack_table.begin(), g.pack_table.end());
        size_t n = 0, same = 0, refs_ok = 0;
        auto test = [&](const RelItem& it, const Bytes& b, const std::vector<uint32_t>& refs, const std::vector<uint32_t>& prefs) {
            n++;
            if (b == Bytes(it.bytes.begin(), it.bytes.end())) same++;
            bool ok = true;
            for (uint32_t r : refs) ok = ok && table.count(it.offset + 8 + r);
            for (uint32_t r : prefs) ok = ok && packs.count(it.offset + 8 + r);
            if (ok) refs_ok++;
        };
        for (const RelItem& it : g.items()) {
            if (g.type == reltype::Dat151) {
                if (it.type_id == dat151::DoorAudioSettings) { auto v = Dat151DoorAudioSettings::read(it); test(it, v.bytes(), v.refs(), {}); }
                if (it.type_id == dat151::DoorTuningParams) { auto v = Dat151DoorTuningParams::read(it); test(it, v.bytes(), v.refs(), {}); }
                if (it.type_id == dat151::EntityEmitter) { auto v = Dat151EntityEmitter::read(it); test(it, v.bytes(), v.refs(), {}); }
                if (it.type_id == dat151::DoorAudioSettingsLink) { auto v = Dat151DoorAudioSettingsLink::read(it); test(it, v.bytes(), v.refs(), {}); }
            } else if (g.type == reltype::Dat54) {
                if (it.type_id == dat54::SimpleSound) { auto v = Dat54SimpleSound::read(it); test(it, v.bytes(), v.refs(), v.pack_refs()); }
                if (it.type_id == dat54::SoundSet) { auto v = Dat54SoundSet::read(it); test(it, v.bytes(), v.refs(), {}); }
                if (it.type_id == dat54::LoopingSound) { auto v = Dat54LoopingSound::read(it); test(it, v.bytes(), v.refs(), {}); }
            }
        }
        std::printf("     %s: %zu typed items, %zu written back byte for byte, %zu with their refs in the tables\n", argv[a], n, same, refs_ok);
        check(same == n && refs_ok == n, "the game's typed items come back byte for byte with their refs");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
