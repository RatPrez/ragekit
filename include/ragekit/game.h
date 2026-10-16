// ragekit - the two editions of GTA V on PC, and the one a program works with.
//
// GTA V Legacy (2015, DirectX 11) and GTA V Enhanced (2025, DirectX 12: the "gen9" files) share their archives, keys,
// maps, collision, meta, PSO, RBF and audio files. Four resource types differ: texture dictionaries (.ytd version 13 /
// 5), drawables and drawable dictionaries (.ydr .ydd 165 / 159) and fragments (.yft 162 / 171). Their readers tell the
// edition from the resource version and read both into the same structs; their writers take the edition to write.
//
// A program picks its edition once, before it loads anything (CodeWalker asks the same at start up), and every writer
// and the game index follow it unless told otherwise:
//
//     rk::set_default_game(rk::Game::Enhanced);
//     Bytes ydr = rk::write_ydr(d);                       // an Enhanced drawable
//     Bytes old = rk::write_ydr(d, rk::Game::Legacy);     // or say which
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rk {

enum class Game : uint8_t {
    Legacy = 0,     // GTA V (2015), "gen8"
    Enhanced = 1,   // GTA V Enhanced (2025), "gen9"
};

/// "legacy" / "enhanced".
const char* game_name(Game g);
/// Parses "legacy", "enhanced", "gen8", "gen9" (any case); none for anything else.
std::optional<Game> game_from_name(std::string_view name);

/// The edition writers and GameIndex::default_game_dir() use when none is given. Legacy until set; set it once at start up.
Game default_game();
void set_default_game(Game g);

} // namespace rk
