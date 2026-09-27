// Runs next to the built dinput8.dll and imports DirectInput normally, so Windows loads
// the fix exactly the way it does for the game. Checks the forwarding exports and - with
// an Xbox-compatible pad connected - that the pad is presented as a wired Xbox 360
// controller through both the Unicode and ANSI interfaces (the games use ANSI).
//
//   fix_test.exe          run the checks
//   fix_test.exe --live   also show live pad input, as the games see it, for 20 seconds

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	int failures = 0;

#define CHECK(expr)                                                              \
	do                                                                           \
	{                                                                            \
		if (expr) std::printf("  ok    %s\n", #expr);                            \
		else { std::printf("  FAIL  %s  (line %d)\n", #expr, __LINE__); ++failures; } \
	} while (false)

	constexpr DWORD xbox360_wired = 0x028E045E; // PID << 16 | VID, as in guidProduct.Data1

	std::filesystem::path module_path(HMODULE module)
	{
		wchar_t buffer[MAX_PATH]{};
		GetModuleFileNameW(module, buffer, MAX_PATH);
		return buffer;
	}

	std::string read_file(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	int connected_xinput_pads()
	{
		int count = 0;
		for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index)
		{
			XINPUT_STATE state{};
			count += XInputGetState(index, &state) == ERROR_SUCCESS;
		}
		return count;
	}

	// Reads the pad the way the MUA games do: c_dfDIJoystick through GetDeviceState.
	template <typename Device>
	void check_state(Device* device, const bool live)
	{
		CHECK(SUCCEEDED(device->SetDataFormat(&c_dfDIJoystick)));
		CHECK(SUCCEEDED(device->SetCooperativeLevel(GetConsoleWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)));
		CHECK(SUCCEEDED(device->Acquire()));

		DIJOYSTATE state{};
		CHECK(SUCCEEDED(device->GetDeviceState(sizeof(state), &state)));
		std::printf("  info  idle state: X=%ld Y=%ld Z=%ld Rx=%ld Ry=%ld POV=%ld buttons=", state.lX, state.lY, state.lZ,
		            state.lRx, state.lRy, static_cast<long>(state.rgdwPOV[0]));
		for (int b = 0; b < 10; ++b) std::printf("%d", state.rgbButtons[b] ? 1 : 0);
		std::printf("\n");

		// Wired Xbox 360 layout: no Rz, a single POV hat, buttons 0-9 only.
		CHECK(state.lRz == 0 && state.rgdwPOV[1] == static_cast<DWORD>(-1));
		CHECK(std::all_of(state.rgbButtons + 10, state.rgbButtons + 32, [](BYTE b) { return b == 0; }));

		if (live)
		{
			std::printf("\n  live input for 20 seconds (A B X Y LB RB Back Start LS RS)\n");
			for (int tick = 0; tick < 200; ++tick)
			{
				device->Poll();
				device->GetDeviceState(sizeof(state), &state);
				std::printf("\r  X=%6ld Y=%6ld Z=%6ld Rx=%6ld Ry=%6ld POV=%6ld buttons=", state.lX, state.lY, state.lZ,
				            state.lRx, state.lRy, static_cast<long>(state.rgdwPOV[0]));
				for (int b = 0; b < 10; ++b) std::printf("%d", state.rgbButtons[b] ? 1 : 0);
				Sleep(100);
			}
			std::printf("\n");
		}
		device->Unacquire();
	}
}

int main(int argc, char** argv)
{
	const bool live = argc > 1 && std::strcmp(argv[1], "--live") == 0;
	const auto dir = module_path(nullptr).parent_path();

	std::printf("the fix is the dinput8.dll this program loaded\n");
	const auto fix = GetModuleHandleW(L"dinput8.dll");
	CHECK(fix != nullptr && module_path(fix).parent_path() == dir);
	const auto version = reinterpret_cast<const char* (*)()>(GetProcAddress(fix, "MuaControllerFix_Version"));
	CHECK(version != nullptr && std::strncmp(version(), "MUA Controller Fix", 18) == 0);

	std::printf("forwarded exports behave like Windows' own dinput8.dll\n");
	CHECK(GetdfDIJoystick() != nullptr && GetdfDIJoystick()->dwNumObjs == c_dfDIJoystick.dwNumObjs);
	const auto can_unload = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(fix, "DllCanUnloadNow"));
	CHECK(can_unload != nullptr && (can_unload() == S_OK || can_unload() == S_FALSE));

	const int connected = connected_xinput_pads();
	std::printf("  info  %d XInput pad(s) connected%s\n", connected, connected ? "" : " - pad checks skipped");

	std::printf("Unicode interface\n");
	{
		IDirectInput8W* input = nullptr;
		CHECK(SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
		                                   reinterpret_cast<void**>(&input), nullptr)));

		struct found
		{
			GUID instance;
			GUID product;
			std::wstring name;
		};
		std::vector<found> devices;
		input->EnumDevices(DI8DEVCLASS_GAMECTRL, [](LPCDIDEVICEINSTANCEW device, LPVOID ref) -> BOOL
		{
			static_cast<std::vector<found>*>(ref)->push_back({device->guidInstance, device->guidProduct, device->tszProductName});
			return DIENUM_CONTINUE;
		}, &devices, DIEDFL_ATTACHEDONLY);

		for (const auto& device : devices)
		{
			std::printf("  info  %ls enumerates as %04lX:%04lX\n", device.name.c_str(), device.product.Data1 & 0xFFFF, device.product.Data1 >> 16);

			IDirectInputDevice8W* handle = nullptr;
			if (FAILED(input->CreateDevice(device.instance, &handle, nullptr)))
			{
				continue;
			}

			DIDEVICEINSTANCEW info{sizeof(info)};
			handle->GetDeviceInfo(&info);
			CHECK(info.guidProduct.Data1 == device.product.Data1); // GetDeviceInfo agrees with EnumDevices

			DIPROPDWORD vidpid{{sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), 0, DIPH_DEVICE}};
			if (SUCCEEDED(handle->GetProperty(DIPROP_VIDPID, &vidpid.diph)))
			{
				CHECK(vidpid.dwData == device.product.Data1); // and so does DIPROP_VIDPID
			}

			if (info.guidProduct.Data1 == xbox360_wired)
			{
				check_state(handle, live);
			}
			handle->Release();
		}

		if (connected > 0)
		{
			CHECK(std::ranges::any_of(devices, [](const auto& d) { return d.product.Data1 == xbox360_wired; }));
		}
		input->Release();
	}

	std::printf("ANSI interface (what the MUA games use)\n");
	{
		IDirectInput8A* input = nullptr;
		CHECK(SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A,
		                                   reinterpret_cast<void**>(&input), nullptr)));

		std::vector<GUID> instances;
		input->EnumDevices(DI8DEVCLASS_GAMECTRL, [](LPCDIDEVICEINSTANCEA device, LPVOID ref) -> BOOL
		{
			static_cast<std::vector<GUID>*>(ref)->push_back(device->guidInstance);
			return DIENUM_CONTINUE;
		}, &instances, DIEDFL_ATTACHEDONLY);

		for (const auto& instance : instances)
		{
			IDirectInputDevice8A* handle = nullptr;
			if (FAILED(input->CreateDevice(instance, &handle, nullptr)))
			{
				continue;
			}

			DIDEVICEINSTANCEA info{sizeof(info)};
			CHECK(SUCCEEDED(handle->GetDeviceInfo(&info)));
			std::printf("  info  %s reports %04lX:%04lX\n", info.tszProductName, info.guidProduct.Data1 & 0xFFFF, info.guidProduct.Data1 >> 16);
			if (info.guidProduct.Data1 == xbox360_wired)
			{
				check_state(handle, false);
			}
			handle->Release();
		}
		input->Release();
	}

	const auto log = read_file(dir / "mua-controller-fix.log");
	CHECK(log.find("hooked a DirectInput instance") != std::string::npos);
	if (connected > 0)
	{
		CHECK(log.find("building ") != std::string::npos);
		CHECK(log.find("wired Xbox 360 pad #2") == std::string::npos); // A and W views of one pad count once
	}

	std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
