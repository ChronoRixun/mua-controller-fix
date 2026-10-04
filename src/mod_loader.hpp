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
// Only a file the game asks Windows for by name can be replaced. The games read most of their
// files straight out of their .bin archives; loose_first.hpp makes them skip the archive for a
// file a mod has.

namespace mod_loader
{
	// Reads the load order and hooks file access in the given modules (the game and the
	// engine DLLs that read game data). Logs every access when `trace` is set. Returns whether it
	// hooked anything (it doesn't when there are no mod files and no tracing).
	bool install(std::initializer_list<const char*> modules, bool trace);

	// Whether an enabled mod has the file at `path`: relative to the game folder or a full path
	// under it, '/' or '\', any case.
	bool provides(const char* path);
}
