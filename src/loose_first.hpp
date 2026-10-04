#pragma once

// Makes the game read a mod's copy of a file it keeps in its .bin archives (models, textures, data):
// the 2016 ports look a file up in their archives before they ever ask Windows for it, so the mod loader
// (mod_loader.hpp) would never see the request. For a file a mod has, the archive lookup reports "not
// here" and the game opens the loose path, which the mod loader redirects to the mod. Done for MUA2.
//
// Nothing is changed until every byte of the game's code it relies on matches the retail build. On the
// Steam release that code is still encrypted (SteamStub) while the DLL loads, so the check is repeated
// at the game's file opens until the game itself is running.

namespace loose_first
{
	// Call after mod_loader::install has hooked the game, and only when it did.
	void install();
}
