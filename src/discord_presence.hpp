#pragma once

#include <string_view>

namespace discord_presence
{
	// Shows what the game is doing on Discord (Rich Presence; [Discord] in mua-controller-fix.ini, on
	// unless switched off), through the Discord app's local pipe on this PC: the game's own status line
	// (steam_presence.hpp) as discord_rules.hpp words it. Starts a thread of its own for Marvel.exe and
	// Alliance.exe; anything else (the test) gets none.
	void install();

	// Player 1's status line, as the game set it ("" when it cleared its presence). Called from the
	// game's thread: it only stores the text.
	void set_status(std::string_view status);
}
