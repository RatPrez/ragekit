#include "ragekit/game.h"
#include "ragekit/rsc.h"
#include <atomic>

namespace rk {

namespace {
std::atomic<uint8_t> g_default{(uint8_t)Game::Legacy};

std::string lower_type(std::string_view t) {
    if (!t.empty() && t[0] == '.') t.remove_prefix(1);
    std::string s(t);
    for (auto& c : s) c = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}
} // namespace

const char* game_name(Game g) { return g == Game::Enhanced ? "enhanced" : "legacy"; }

std::optional<Game> game_from_name(std::string_view name) {
    const std::string n = lower_type(name);
    if (n == "legacy" || n == "gen8") return Game::Legacy;
    if (n == "enhanced" || n == "gen9") return Game::Enhanced;
    return std::nullopt;
}

Game default_game() { return (Game)g_default.load(); }
void set_default_game(Game g) { g_default.store((uint8_t)g); }

namespace rsc {

Game game_of(std::string_view type, int version) {
    const std::string t = lower_type(type);
    if (t == "ytd") return version == kYtdGen9 ? Game::Enhanced : Game::Legacy;
    if (t == "ydr" || t == "ydd") return version == kYdrGen9 || version == 154 ? Game::Enhanced : Game::Legacy;
    if (t == "yft") return version == kYftGen9 ? Game::Enhanced : Game::Legacy;
    if (t == "ypt") return version == kYptGen9 ? Game::Enhanced : Game::Legacy;
    return Game::Legacy;
}

int version_for(std::string_view type, Game g) {
    const bool e = g == Game::Enhanced;
    const std::string t = lower_type(type);
    if (t == "ytd") return e ? kYtdGen9 : kYtd;
    if (t == "ydr") return e ? kYdrGen9 : kYdr;
    if (t == "ydd") return e ? kYddGen9 : kYdd;
    if (t == "yft") return e ? kYftGen9 : kYft;
    if (t == "ypt") return e ? kYptGen9 : kYpt;
    if (t == "ybn") return kYbn;
    if (t == "ycd") return kYcd;
    if (t == "ymap" || t == "ytyp" || t == "ymt") return kYmap;
    if (t == "ynv") return kYnv;
    if (t == "yed") return kYed;
    if (t == "yld") return kYld;
    if (t == "yfd") return kYfd;
    return 0;
}

} // namespace rsc
} // namespace rk
