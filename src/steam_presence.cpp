#include "steam_presence.hpp"

#include "iat_hook.hpp"
#include "log.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace steam_presence
{
	namespace
	{
		// The interface versions both games ask for, and the vtable slots used here, from the
		// Steamworks SDK headers isteamclient017.h and isteamfriends015.h.
		constexpr const char* client_version = "SteamClient017";
		constexpr const char* friends_version = "SteamFriends015";
		constexpr std::size_t client_get_friends_slot = 8;      // ISteamClient017::GetISteamFriends
		constexpr std::size_t friends_set_presence_slot = 43;   // ISteamFriends015::SetRichPresence
		constexpr std::size_t friends_clear_presence_slot = 44; // ISteamFriends015::ClearRichPresence

		using create_interface_fn = void*(__cdecl*)(const char* version);
		using get_friends_fn = void* (*)(void* self, int user, int pipe, const char* version);
		using set_presence_fn = bool (*)(void* self, const char* key, const char* value);
		using clear_presence_fn = void (*)(void* self);

		create_interface_fn real_create_interface = nullptr;
		get_friends_fn real_get_friends = nullptr;
		set_presence_fn real_set_presence = nullptr;
		clear_presence_fn real_clear_presence = nullptr;

		std::mutex mutex;
		std::map<std::string, std::string> published; // what the game last set, by key

		// Points `slot` of `object`'s vtable at `replacement` and stores the previous entry in `real`.
		// The vtable belongs to the Steam client (the emulator, or Steam itself), not the game.
		template <typename T>
		bool patch_slot(void* object, const std::size_t slot, void* replacement, T& real)
		{
			std::lock_guard lock(mutex);
			auto** vtable = *static_cast<void***>(object);
			if (vtable[slot] == replacement)
			{
				return true; // already patched (the game asked again)
			}

			DWORD old_protect = 0;
			if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old_protect))
			{
				return false;
			}
			real = reinterpret_cast<T>(vtable[slot]);
			InterlockedExchangePointer(&vtable[slot], replacement); // `real` is stored before the hook can run
			VirtualProtect(&vtable[slot], sizeof(void*), old_protect, &old_protect);
			return true;
		}

		// The status line holds a newline (MUA2's template is "{MISSION}\n{CHARACTER}..."): keep the log one line per call.
		std::string printable(const char* text)
		{
			if (!text)
			{
				return "(null)";
			}

			std::string out;
			for (const char* c = text; *c; ++c)
			{
				switch (*c)
				{
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default: out += *c; break;
				}
			}
			return out;
		}

		bool hook_set_presence(void* self, const char* key, const char* value)
		{
			const std::string k = key ? key : "";
			const std::string v = value ? value : "";
			bool changed = false;
			{
				std::lock_guard lock(mutex);
				auto [entry, added] = published.try_emplace(k, v);
				changed = added || entry->second != v;
				entry->second = v;
			}
			if (changed)
			{
				logger::write("steam: SetRichPresence(\"%s\", \"%s\")", printable(key).c_str(), printable(value).c_str());
			}
			return real_set_presence(self, key, value);
		}

		void hook_clear_presence(void* self)
		{
			bool had_any = false;
			{
				std::lock_guard lock(mutex);
				had_any = !published.empty();
				published.clear();
			}
			if (had_any)
			{
				logger::write("steam: ClearRichPresence()");
			}
			real_clear_presence(self);
		}

		void* hook_get_friends(void* self, const int user, const int pipe, const char* version)
		{
			void* friends = real_get_friends(self, user, pipe, version);
			if (!friends)
			{
				logger::write("steam: GetISteamFriends(\"%s\") returned nothing", printable(version).c_str());
			}
			else if (!version || std::strcmp(version, friends_version) != 0)
			{
				logger::write_once("friends-version", "steam: the game asked for %s, not %s - rich presence not watched", printable(version).c_str(), friends_version);
			}
			else if (patch_slot(friends, friends_set_presence_slot, reinterpret_cast<void*>(&hook_set_presence), real_set_presence) &&
			         patch_slot(friends, friends_clear_presence_slot, reinterpret_cast<void*>(&hook_clear_presence), real_clear_presence))
			{
				logger::write_once("friends", "steam: watching the game's rich presence (%s)", friends_version);
			}
			else
			{
				logger::write("steam: ERROR: could not hook %s (error %lu)", friends_version, GetLastError());
			}
			return friends;
		}

		void* __cdecl hook_create_interface(const char* version)
		{
			void* result = real_create_interface(version);
			logger::write_once(std::string("interface ") + printable(version), "steam: SteamInternal_CreateInterface(\"%s\") -> %p", printable(version).c_str(), result);
			if (result && version && std::strcmp(version, client_version) == 0 &&
			    !patch_slot(result, client_get_friends_slot, reinterpret_cast<void*>(&hook_get_friends), real_get_friends))
			{
				logger::write("steam: ERROR: could not hook %s (error %lu)", client_version, GetLastError());
			}
			return result;
		}
	}

	void install()
	{
		real_create_interface = static_cast<create_interface_fn>(
			iat_hook::hook(GetModuleHandleW(nullptr), "steam_api64.dll", "SteamInternal_CreateInterface", 0, reinterpret_cast<void*>(&hook_create_interface)));
		if (!real_create_interface)
		{
			logger::write("steam: the game doesn't import SteamInternal_CreateInterface from steam_api64.dll - rich presence not watched");
		}
	}
}
