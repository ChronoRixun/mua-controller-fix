#pragma once

#include <Windows.h>

#include <initializer_list>

// Loads mods without touching the game's files. A mod is a folder under <game>\mods laid out
// like the game folder (e.g. mods\Faster Combat\data\fightstyles\fightstyle_default.engb);
// mods\load-order.txt lists which mods are enabled and in what order (folders it doesn't list
// load after the listed ones, alphabetically, so a mod copied in by hand just works):
//
//   # later lines win when two mods contain the same file
//   +Better HUD
//   -Unused Mod           (disabled; kept in the list so its position is remembered)
//   +Faster Combat
//
// When the game opens, checks or searches for a file for reading, the path is redirected to
// the copy in the highest enabled mod that has one. Writes, and files no mod contains, go
// to the game folder as usual. Mods can also add files the game doesn't ship.
//
// Only a file the game asks Windows for by name can be replaced: both games read their models,
// textures and maps straight out of their .bin archives without asking for a loose copy, so
// those can't (the README lists what the games do look for).

namespace mod_loader
{
	// Reads the load order and hooks file access in the given modules (the game and the
	// engine DLLs that read game data). Logs every access when `trace` is set.
	void install(std::initializer_list<const char*> modules, bool trace);
}
