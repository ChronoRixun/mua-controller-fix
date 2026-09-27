#include "xinput_pad.hpp"
#include "log.hpp"

#include <Windows.h>
#include <Xinput.h>

namespace xinput_pad
{
	namespace
	{
		using get_state_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

		get_state_t load_get_state()
		{
			for (const auto* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
			{
				if (const auto module = LoadLibraryW(dll))
				{
					if (const auto proc = GetProcAddress(module, "XInputGetState"))
					{
						logger::write("xinput: using %ls", dll);
						return reinterpret_cast<get_state_t>(proc);
					}
				}
			}

			logger::write("ERROR: no XInput DLL available - controllers will not work");
			return nullptr;
		}

		get_state_t get_state_function()
		{
			static const auto get_state = load_get_state();
			return get_state;
		}
	}

	bool read_raw(const int index, raw_state& out)
	{
		const auto get_state = get_state_function();
		XINPUT_STATE raw{};
		if (!get_state || index < 0 || index >= max_pads || get_state(static_cast<DWORD>(index), &raw) != ERROR_SUCCESS)
		{
			return false;
		}

		const auto& pad = raw.Gamepad;
		out = {pad.wButtons, pad.bLeftTrigger, pad.bRightTrigger, pad.sThumbLX, pad.sThumbLY, pad.sThumbRX, pad.sThumbRY};
		return true;
	}

	std::vector<int> connected_indices()
	{
		static bool was_connected[max_pads]{};

		std::vector<int> result;
		raw_state ignored{};
		for (int index = 0; index < max_pads; ++index)
		{
			const bool connected = read_raw(index, ignored);
			if (connected != was_connected[index])
			{
				logger::write("xinput: pad %d %s", index + 1, connected ? "connected" : "disconnected");
				was_connected[index] = connected;
			}
			if (connected)
			{
				result.push_back(index);
			}
		}
		return result;
	}
}
