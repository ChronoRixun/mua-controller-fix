#pragma once

#include <Windows.h>

#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

// One vtable slot pointed at a replacement, for every Steam interface object of the kind it's given:
// the vtables belong to the Steam client (the emulator, or Steam itself), not the game. Each patched
// vtable's own entry is kept, so objects of different classes behind one hook (the player's and the
// game server's networking, say) each reach their own function.
class vtable_hook
{
public:
	vtable_hook(const std::size_t slot, void* replacement) : slot_(slot), replacement_(replacement)
	{
	}

	// Patches `object`'s vtable (once per vtable). False if the page can't be made writable.
	bool patch(void* object)
	{
		auto** vtable = *static_cast<void***>(object);
		std::lock_guard lock(mutex_);
		if (vtable[slot_] == replacement_)
		{
			return true; // this vtable is done (the game asked for the interface again)
		}
		DWORD old_protect = 0;
		if (!VirtualProtect(&vtable[slot_], sizeof(void*), PAGE_READWRITE, &old_protect))
		{
			return false;
		}
		originals_.emplace_back(vtable, vtable[slot_]);
		InterlockedExchangePointer(&vtable[slot_], replacement_); // the original is stored before the hook can run
		VirtualProtect(&vtable[slot_], sizeof(void*), old_protect, &old_protect);
		return true;
	}

	// The entry `object`'s vtable had before patch().
	template <typename T>
	T original(void* object) const
	{
		auto** vtable = *static_cast<void***>(object);
		std::lock_guard lock(mutex_);
		for (const auto& [patched, entry] : originals_)
		{
			if (patched == vtable)
			{
				return reinterpret_cast<T>(entry);
			}
		}
		return reinterpret_cast<T>(vtable[slot_]); // not patched (never happens for a hooked call)
	}

private:
	std::size_t slot_;
	void* replacement_;
	mutable std::mutex mutex_;
	std::vector<std::pair<void**, void*>> originals_;
};
