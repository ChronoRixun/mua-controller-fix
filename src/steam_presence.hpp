#pragma once

namespace steam_presence
{
	// Watches the rich presence the game itself publishes to Steam. Both games build a status line
	// ("status": mission, character, level) and hand it to ISteamFriends::SetRichPresence; this hooks
	// the executable's import of SteamInternal_CreateInterface and, through the ISteamClient it
	// returns, the ISteamFriends the game asks for, so every SetRichPresence/ClearRichPresence is
	// seen here (and logged when it changes) before being passed on unchanged.
	void install();
}
