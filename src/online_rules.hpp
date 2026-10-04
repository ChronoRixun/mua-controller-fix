#pragma once

// Online play's decisions, kept apart from the hooks (steam_online.cpp) so fix_test can check them.
//
// Both games' online play is Valve's SpaceWar sample's design (Marvel.exe/Alliance.exe still carry
// its CP2PAuthPlayer): the host creates a lobby (up to 4 players), starts a Steam game server inside
// the game (anonymous logon, players' auth tickets checked), and the players send P2P packets to the
// game server's SteamID. The lobby search filters by the game's own keys but sets no distance filter,
// so on Steam it lists only lobbies in the same or nearby regions.
//
// Under the game's own app ID nothing here changes anything. Under one it shares with other games
// (Steam's free test app 480, which players use to play these delisted games online), every search
// covers all regions and asks for this fix's tag, and every lobby the game sets data on gets the tag:
// MUA1, MUA2 and the other games on that app ID never see each other's lobbies.

#include "game_id.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace online_rules
{
	using game_id::game;

	// Running under an app ID that isn't the game's own (0: Steam didn't say).
	inline bool shared_app(const game which, const std::uint32_t app_id)
	{
		return app_id != 0 && app_id != game_id::steam_app_id(which);
	}

	// The lobby key and value that mark this game's lobbies on a shared app ID.
	inline constexpr std::string_view tag_key = "mua_fix_game";

	inline std::string_view tag_value(const game which)
	{
		return which == game::mua1 ? "mua1" : "mua2";
	}

	inline constexpr int distance_worldwide = 3; // k_ELobbyDistanceFilterWorldwide
	inline constexpr int comparison_equal = 0;   // k_ELobbyComparisonEqual

	inline std::string_view lobby_type_name(const int type)
	{
		switch (type)
		{
		case 0: return "private";
		case 1: return "friends only";
		case 2: return "public";
		case 3: return "invisible";
		default: return "unknown type";
		}
	}

	inline std::string_view comparison_name(const int comparison)
	{
		switch (comparison)
		{
		case -2: return "<=";
		case -1: return "<";
		case 0: return "=";
		case 1: return ">";
		case 2: return ">=";
		case 3: return "!=";
		default: return "?";
		}
	}

	// A P2P session as ISteamNetworking::GetP2PSessionState fills P2PSessionState_t's first four bytes.
	struct p2p_state
	{
		bool active = false;
		bool connecting = false;
		int error = 0; // EP2PSessionError
		bool relay = false;
		bool operator==(const p2p_state&) const = default;
	};

	inline p2p_state p2p_state_from(const std::uint8_t* bytes)
	{
		return {bytes[0] != 0, bytes[1] != 0, bytes[2], bytes[3] != 0};
	}

	inline std::string describe(const p2p_state& s)
	{
		if (s.error != 0)
		{
			switch (s.error)
			{
			case 2: return "failed (no rights to the app)";
			case 4: return "failed (timed out)";
			default: return "failed (error " + std::to_string(s.error) + ")";
			}
		}
		if (s.active) return s.relay ? "connected through Steam's relay" : "connected directly";
		if (s.connecting) return "connecting";
		return "not connected";
	}
}
