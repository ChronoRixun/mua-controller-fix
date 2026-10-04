#include "steam_online.hpp"

#include "log.hpp"
#include "online_rules.hpp"
#include "vtable_hook.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace steam_online
{
	namespace
	{
		using namespace online_rules;

		// The vtable slots used here, from the Steamworks SDK headers for the versions both games ask
		// for: isteamclient017.h, isteamutils008.h, isteammatchmaking.h (SteamMatchMaking009),
		// isteamgameserver012.h and isteamnetworking005.h.
		constexpr std::size_t client_get_game_server_slot = 6;   // ISteamClient017::GetISteamGameServer
		constexpr std::size_t client_get_utils_slot = 9;         // ISteamClient017::GetISteamUtils
		constexpr std::size_t client_get_matchmaking_slot = 10;  // ISteamClient017::GetISteamMatchmaking
		constexpr std::size_t client_get_networking_slot = 16;   // ISteamClient017::GetISteamNetworking
		constexpr std::size_t utils_get_app_id_slot = 9;         // ISteamUtils008::GetAppID
		constexpr std::size_t lobbies_request_list_slot = 4;     // ISteamMatchmaking::RequestLobbyList
		constexpr std::size_t lobbies_string_filter_slot = 5;    // ::AddRequestLobbyListStringFilter
		constexpr std::size_t lobbies_number_filter_slot = 6;    // ::AddRequestLobbyListNumericalFilter
		constexpr std::size_t lobbies_distance_filter_slot = 9;  // ::AddRequestLobbyListDistanceFilter
		constexpr std::size_t lobbies_create_slot = 13;          // ::CreateLobby
		constexpr std::size_t lobbies_join_slot = 14;            // ::JoinLobby
		constexpr std::size_t lobbies_set_data_slot = 20;        // ::SetLobbyData
		constexpr std::size_t server_log_on_anonymous_slot = 6;  // ISteamGameServer012::LogOnAnonymous
		constexpr std::size_t server_logged_on_slot = 8;         // ::BLoggedOn
		constexpr std::size_t server_secure_slot = 9;            // ::BSecure
		constexpr std::size_t network_session_state_slot = 6;    // ISteamNetworking005::GetP2PSessionState

		constexpr const char* utils_version = "SteamUtils008";
		constexpr const char* lobbies_version = "SteamMatchMaking009";
		constexpr const char* server_version = "SteamGameServer012";
		constexpr const char* network_version = "SteamNetworking005";

		// A CSteamID passed by value travels as its 64 bits in a register.
		using get_interface_fn = void* (*)(void* self, int user, int pipe, const char* version);
		using get_utils_fn = void* (*)(void* self, int pipe, const char* version);
		using get_app_id_fn = std::uint32_t (*)(void* self);
		using request_list_fn = std::uint64_t (*)(void* self);
		using string_filter_fn = void (*)(void* self, const char* key, const char* value, int comparison);
		using number_filter_fn = void (*)(void* self, const char* key, int value, int comparison);
		using distance_filter_fn = void (*)(void* self, int filter);
		using create_lobby_fn = std::uint64_t (*)(void* self, int type, int max_members);
		using join_lobby_fn = std::uint64_t (*)(void* self, std::uint64_t lobby);
		using set_lobby_data_fn = bool (*)(void* self, std::uint64_t lobby, const char* key, const char* value);
		using void_fn = void (*)(void* self);
		using bool_fn = bool (*)(void* self);
		using session_state_fn = bool (*)(void* self, std::uint64_t remote, std::uint8_t* state);

		constexpr unsigned searches_to_log = 10;

		std::optional<game> which; // from the executable's name; none: not one of the games
		std::mutex mutex;
		void* utils = nullptr;            // the game's own ISteamUtils (its app ID)
		std::optional<bool> shared;       // decided once the app ID is known
		std::unordered_set<std::uint64_t> tagged_lobbies;
		std::unordered_map<std::uint64_t, std::pair<unsigned, p2p_state>> sessions; // by remote SteamID: its number in the log, its last state
		std::optional<std::pair<bool, bool>> server_state;                         // logged on, secure

		std::uintptr_t entry(void* object, const std::size_t slot)
		{
			return reinterpret_cast<std::uintptr_t>((*static_cast<void***>(object))[slot]);
		}

		// Whether the game runs under an app ID it shares with other games (online_rules.hpp).
		bool shared_app_id()
		{
			std::lock_guard lock(mutex);
			if (shared)
			{
				return *shared;
			}
			if (!which || !utils)
			{
				return false;
			}
			const auto app_id = reinterpret_cast<get_app_id_fn>(entry(utils, utils_get_app_id_slot))(utils);
			shared = shared_app(*which, app_id);
			if (*shared)
			{
				logger::write("online: the game runs under Steam app %u, not its own (%u): lobby searches cover every region and show only lobbies tagged %.*s=%.*s, and its lobbies get that tag",
				              app_id, game_id::steam_app_id(*which), static_cast<int>(tag_key.size()), tag_key.data(), static_cast<int>(tag_value(*which).size()), tag_value(*which).data());
			}
			else
			{
				logger::write("online: the game runs under its own Steam app ID (%u): its lobbies are left as they are", app_id);
			}
			return *shared;
		}

		// ---- Lobbies ------------------------------------------------------------------------------

		std::uint64_t hook_request_list(void* self);
		void hook_string_filter(void* self, const char* key, const char* value, int comparison);
		void hook_number_filter(void* self, const char* key, int value, int comparison);
		std::uint64_t hook_create_lobby(void* self, int type, int max_members);
		std::uint64_t hook_join_lobby(void* self, std::uint64_t lobby);
		bool hook_set_lobby_data(void* self, std::uint64_t lobby, const char* key, const char* value);

		vtable_hook request_list_hook(lobbies_request_list_slot, reinterpret_cast<void*>(&hook_request_list));
		vtable_hook string_filter_hook(lobbies_string_filter_slot, reinterpret_cast<void*>(&hook_string_filter));
		vtable_hook number_filter_hook(lobbies_number_filter_slot, reinterpret_cast<void*>(&hook_number_filter));
		vtable_hook create_lobby_hook(lobbies_create_slot, reinterpret_cast<void*>(&hook_create_lobby));
		vtable_hook join_lobby_hook(lobbies_join_slot, reinterpret_cast<void*>(&hook_join_lobby));
		vtable_hook set_lobby_data_hook(lobbies_set_data_slot, reinterpret_cast<void*>(&hook_set_lobby_data));

		const char* text(const char* s)
		{
			return s ? s : "(null)";
		}

		std::uint64_t hook_request_list(void* self)
		{
			if (shared_app_id())
			{
				reinterpret_cast<distance_filter_fn>(entry(self, lobbies_distance_filter_slot))(self, distance_worldwide);
				const std::string key(tag_key);
				const std::string value(tag_value(*which));
				string_filter_hook.original<string_filter_fn>(self)(self, key.c_str(), value.c_str(), comparison_equal);
			}
			static unsigned searches = 0; // the lobby browser refreshes: the first few are enough
			if (++searches <= searches_to_log)
			{
				logger::write("online: the game searches for lobbies%s", searches == searches_to_log ? " (further searches aren't logged)" : "");
			}
			return request_list_hook.original<request_list_fn>(self)(self);
		}

		void hook_string_filter(void* self, const char* key, const char* value, const int comparison)
		{
			logger::write_once(std::string("string filter ") + text(key) + text(value), "online: the game's lobby search wants %s %.*s \"%s\"", text(key),
			                   static_cast<int>(comparison_name(comparison).size()), comparison_name(comparison).data(), text(value));
			string_filter_hook.original<string_filter_fn>(self)(self, key, value, comparison);
		}

		void hook_number_filter(void* self, const char* key, const int value, const int comparison)
		{
			logger::write_once(std::string("number filter ") + text(key) + std::to_string(value), "online: the game's lobby search wants %s %.*s %d", text(key),
			                   static_cast<int>(comparison_name(comparison).size()), comparison_name(comparison).data(), value);
			number_filter_hook.original<number_filter_fn>(self)(self, key, value, comparison);
		}

		std::uint64_t hook_create_lobby(void* self, const int type, const int max_members)
		{
			logger::write("online: the game creates a lobby (%.*s, up to %d players)", static_cast<int>(lobby_type_name(type).size()), lobby_type_name(type).data(), max_members);
			return create_lobby_hook.original<create_lobby_fn>(self)(self, type, max_members);
		}

		std::uint64_t hook_join_lobby(void* self, const std::uint64_t lobby)
		{
			logger::write("online: the game joins a lobby");
			return join_lobby_hook.original<join_lobby_fn>(self)(self, lobby);
		}

		// The game's own lobby data, then the tag once per lobby (only the owner's SetLobbyData works).
		bool hook_set_lobby_data(void* self, const std::uint64_t lobby, const char* key, const char* value)
		{
			const auto real = set_lobby_data_hook.original<set_lobby_data_fn>(self);
			const bool done = real(self, lobby, key, value);
			logger::write_once(std::string("lobby data ") + text(key), "online: the game sets lobby data \"%s\"%s", text(key), done ? "" : " (refused: not the lobby's owner?)");
			if (done && shared_app_id())
			{
				bool first = false;
				{
					std::lock_guard lock(mutex);
					first = tagged_lobbies.insert(lobby).second;
				}
				if (first)
				{
					const std::string tag(tag_key);
					const std::string own(tag_value(*which));
					const bool tagged = real(self, lobby, tag.c_str(), own.c_str());
					logger::write("online: %s the lobby as %s=%s", tagged ? "tagged" : "could not tag", tag.c_str(), own.c_str());
				}
			}
			return done;
		}

		// ---- The game server ----------------------------------------------------------------------

		void hook_log_on_anonymous(void* self);
		bool hook_secure(void* self);

		vtable_hook log_on_anonymous_hook(server_log_on_anonymous_slot, reinterpret_cast<void*>(&hook_log_on_anonymous));
		vtable_hook secure_hook(server_secure_slot, reinterpret_cast<void*>(&hook_secure));

		void hook_log_on_anonymous(void* self)
		{
			logger::write("online: the game server logs on to Steam (anonymous)");
			log_on_anonymous_hook.original<void_fn>(self)(self);
		}

		// The game asks now and then; its answer and BLoggedOn are logged when they change.
		bool hook_secure(void* self)
		{
			const bool secure = secure_hook.original<bool_fn>(self)(self);
			const bool logged_on = reinterpret_cast<bool_fn>(entry(self, server_logged_on_slot))(self);
			bool changed = false;
			{
				std::lock_guard lock(mutex);
				changed = !server_state || *server_state != std::pair{logged_on, secure};
				server_state = std::pair{logged_on, secure};
			}
			if (changed)
			{
				logger::write("online: game server %s, %s", logged_on ? "logged on" : "not logged on", secure ? "secure" : "not secure");
			}
			return secure;
		}

		// ---- P2P sessions ---------------------------------------------------------------------------

		bool hook_session_state(void* self, std::uint64_t remote, std::uint8_t* state);

		vtable_hook session_state_hook(network_session_state_slot, reinterpret_cast<void*>(&hook_session_state));

		// Each peer is a number in the log, never its SteamID or address.
		bool hook_session_state(void* self, const std::uint64_t remote, std::uint8_t* state)
		{
			const bool known = session_state_hook.original<session_state_fn>(self)(self, remote, state);
			if (known && state)
			{
				const auto now = p2p_state_from(state);
				unsigned number = 0;
				bool changed = false;
				{
					std::lock_guard lock(mutex);
					auto [entry_it, added] = sessions.try_emplace(remote, static_cast<unsigned>(sessions.size() + 1), p2p_state{});
					changed = added || entry_it->second.second != now;
					entry_it->second.second = now;
					number = entry_it->second.first;
				}
				if (changed)
				{
					logger::write("online: peer %u: %s", number, describe(now).c_str());
				}
			}
			return known;
		}

		// ---- The client's interfaces --------------------------------------------------------------

		void* hook_get_utils(void* self, int pipe, const char* version);
		void* hook_get_lobbies(void* self, int user, int pipe, const char* version);
		void* hook_get_server(void* self, int user, int pipe, const char* version);
		void* hook_get_network(void* self, int user, int pipe, const char* version);

		vtable_hook get_utils_hook(client_get_utils_slot, reinterpret_cast<void*>(&hook_get_utils));
		vtable_hook get_lobbies_hook(client_get_matchmaking_slot, reinterpret_cast<void*>(&hook_get_lobbies));
		vtable_hook get_server_hook(client_get_game_server_slot, reinterpret_cast<void*>(&hook_get_server));
		vtable_hook get_network_hook(client_get_networking_slot, reinterpret_cast<void*>(&hook_get_network));

		bool is(const char* version, const char* wanted)
		{
			return version && std::strcmp(version, wanted) == 0;
		}

		void patch_all(void* object, std::initializer_list<vtable_hook*> hooks, const char* what)
		{
			for (auto* hook : hooks)
			{
				if (!hook->patch(object))
				{
					logger::write("online: ERROR: could not hook %s (error %lu)", what, GetLastError());
					return;
				}
			}
			logger::write_once(std::string("watching ") + what, "online: watching %s", what);
		}

		void* hook_get_utils(void* self, const int pipe, const char* version)
		{
			void* result = get_utils_hook.original<get_utils_fn>(self)(self, pipe, version);
			if (result && is(version, utils_version))
			{
				std::lock_guard lock(mutex);
				if (!utils)
				{
					utils = result; // the player's (the game asks before the game server's)
				}
			}
			return result;
		}

		void* hook_get_lobbies(void* self, const int user, const int pipe, const char* version)
		{
			void* result = get_lobbies_hook.original<get_interface_fn>(self)(self, user, pipe, version);
			if (result && is(version, lobbies_version))
			{
				patch_all(result, {&request_list_hook, &string_filter_hook, &number_filter_hook, &create_lobby_hook, &join_lobby_hook, &set_lobby_data_hook}, "the lobbies (SteamMatchMaking009)");
			}
			return result;
		}

		void* hook_get_server(void* self, const int user, const int pipe, const char* version)
		{
			void* result = get_server_hook.original<get_interface_fn>(self)(self, user, pipe, version);
			if (result && is(version, server_version))
			{
				patch_all(result, {&log_on_anonymous_hook, &secure_hook}, "the game server (SteamGameServer012)");
			}
			return result;
		}

		void* hook_get_network(void* self, const int user, const int pipe, const char* version)
		{
			void* result = get_network_hook.original<get_interface_fn>(self)(self, user, pipe, version);
			if (result && is(version, network_version))
			{
				patch_all(result, {&session_state_hook}, "the P2P sessions (SteamNetworking005)");
			}
			return result;
		}
	}

	void watch_client(void* client)
	{
		{
			std::lock_guard lock(mutex);
			if (!which)
			{
				wchar_t exe[MAX_PATH]{};
				const std::wstring_view path(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
				const auto slash = path.find_last_of(L"\\/");
				which = game_id::game_from_exe(path.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
			}
			if (!which)
			{
				return; // not one of the games
			}
		}
		for (auto* hook : {&get_utils_hook, &get_lobbies_hook, &get_server_hook, &get_network_hook})
		{
			if (!hook->patch(client))
			{
				logger::write("online: ERROR: could not hook the Steam client (error %lu)", GetLastError());
				return;
			}
		}
	}
}
