#pragma once

namespace steam_online
{
	// Watches and adjusts the game's online play through the ISteamClient it got from
	// SteamInternal_CreateInterface (steam_presence hooks that import and hands the client over).
	// Hooks the client's GetISteamUtils, GetISteamMatchmaking, GetISteamGameServer and
	// GetISteamNetworking, and through them the lobby search, lobby creation and data, the game server's
	// logon and the P2P sessions' state: logged as they happen (no IDs, names or addresses), and, under
	// an app ID shared with other games, the lobby changes online_rules.hpp describes.
	void watch_client(void* client);
}
