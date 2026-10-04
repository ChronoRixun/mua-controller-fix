#pragma once

namespace steam_presence
{
	// Watches the rich presence the game itself publishes to Steam. Both games build a status line
	// ("status": "Playing <area> As <hero>", "In the Main Menu.") and hand it to
	// ISteamFriends::SetRichPresence; this hooks the executable's import of SteamInternal_CreateInterface
	// and, through the ISteamClient it returns, the ISteamFriends the game asks for, so every
	// SetRichPresence/ClearRichPresence is seen here before being passed on unchanged. Player 1's line
	// (discord_rules.hpp) goes to discord_presence and the log when it changes; nothing else is shown.
	void install();
}
