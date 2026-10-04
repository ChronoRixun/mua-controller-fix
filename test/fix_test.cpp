// Runs next to the built dinput8.dll and imports DirectInput normally, so Windows loads
// the fix exactly the way it does for the game. Checks the forwarding exports and - with
// an Xbox-compatible pad connected - that the pad is presented as a wired Xbox 360
// controller through both the Unicode and ANSI interfaces (the games use ANSI). Also checks the
// Discord presence's rules (discord_rules.hpp) without Discord or the game, and the mod loader:
// the load order (mod_order.hpp) and, in a copy of this program set up like a game folder with
// mods, which files its reads get.
//
//   fix_test.exe          run the checks
//   fix_test.exe --live   also show live pad input, as the games see it, for 20 seconds

#include "../src/discord_rules.hpp"
#include "../src/mod_order.hpp"

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

	void write_file(const std::filesystem::path& path, const std::string& text)
	{
		std::error_code ignored;
		std::filesystem::create_directories(path.parent_path(), ignored);
		std::ofstream(path, std::ios::binary) << text;
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

	// What the presence makes of the games' status lines, as the probe logged them.
	void check_discord_rules()
	{
		using namespace discord_rules;
		std::printf("discord presence rules\n");

		CHECK(game_from_exe(L"Marvel.exe") == game::mua1 && game_from_exe(L"ALLIANCE.EXE") == game::mua2);
		CHECK(!game_from_exe(L"fix_test.exe") && !game_from_exe(L"Marvel.exe.bak"));
		CHECK(client_id_for(game::mua1) == "1556188950123511889" && client_id_for(game::mua2) == "1556189142092746874");

		const display both;
		CHECK((activity_for(game::mua2, "Playing Latveria: Urban Warfare As Wolverine.", both) == activity{"Latveria: Urban Warfare", "Playing as Wolverine"}));
		CHECK((activity_for(game::mua2, "Playing Latveria: Urban Warfare As Spider-Man.", both) == activity{"Latveria: Urban Warfare", "Playing as Spider-Man"}));
		CHECK((activity_for(game::mua1, "Playing Stark As Moonknight", both) == activity{"Stark Tower", "Playing as Moon Knight"}));
		CHECK((activity_for(game::mua1, "Playing Somewhere As Thor", both) == activity{"Somewhere", "Playing as Thor"})); // unknown names as they are
		CHECK((activity_for(game::mua2, "Playing Stark As Moonknight", both) == activity{"Stark", "Playing as Moonknight"})); // MUA1's names only for MUA1
		CHECK((activity_for(game::mua2, "In the Main Menu.", both) == activity{"In the Main Menu", ""}));
		CHECK((activity_for(game::mua2, "Watching someone else play.", both) == activity{"Watching someone else play", ""}));
		CHECK((activity_for(game::mua2, "", both) == activity{}));
		CHECK((activity_for(game::mua2, "Playing  As Wolverine.", both) == activity{"Playing As Wolverine", ""})); // no area: as it is
		CHECK((activity_for(game::mua2, utf8_from_game("Playing Latveria:\nUrban Warfare  As Iron Man."), both) == activity{"Latveria: Urban Warfare", "Playing as Iron Man"}));
		CHECK((activity_for(game::mua2, "Playing Latveria: Urban Warfare As Wolverine.", display{false, true}) == activity{"Playing as Wolverine", ""}));
		CHECK((activity_for(game::mua2, "Playing Latveria: Urban Warfare As Wolverine.", display{true, false}) == activity{"Latveria: Urban Warfare", "Playing"}));
		CHECK((activity_for(game::mua2, "Playing Latveria: Urban Warfare As Wolverine.", display{false, false}) == activity{"Playing", ""}));

		// One burst: player 1's line, then the other slots' in the same instant; the next burst 2 s on.
		first_of_burst bursts;
		CHECK(bursts.take(1000) && !bursts.take(1000) && !bursts.take(1016));
		CHECK(bursts.take(3016) && !bursts.take(3016));

		CHECK(utf8_from_game("Spider-Man") == "Spider-Man");
		CHECK(utf8_from_game("Fen\xC3\xADx") == "Fen\xC3\xADx");       // UTF-8 as it is
		CHECK(utf8_from_game("Fen\xEDx") == "Fen\xC3\xADx");            // Windows-1252 converted
		CHECK(utf8_from_game("a\nb") == "a b");
		CHECK(characters(clip(std::string(200, 'x'))) == 128 && clip(std::string(200, 'x')).ends_with("\xE2\x80\xA6"));

		CHECK(parse_switch(std::nullopt, true) && !parse_switch("0  ; off", true) && parse_switch("Yes", false) && parse_switch("maybe", true));
		CHECK(valid_client_id("1556189142092746874") && !valid_client_id("12345") && !valid_client_id("15561891420927468x4"));
		CHECK(valid_asset("logo") && !valid_asset("my logo") && !valid_asset(""));
		CHECK(choose_large_image(std::nullopt).key == "logo" && choose_large_image("  ; later").key == "logo");
		CHECK(choose_large_image("None").key.empty() && choose_large_image("art2").key == "art2");
		CHECK(choose_large_image("my logo").key == "logo" && choose_large_image("my logo").refused);

		const auto json = set_activity_json(42, {"Latveria: Urban Warfare", "Playing as \"Wolverine\""}, {1700000000, "", "Marvel: Ultimate Alliance 2"}, 7);
		CHECK(json == R"({"cmd":"SET_ACTIVITY","args":{"pid":42,"activity":{"details":"Latveria: Urban Warfare","state":"Playing as \"Wolverine\"","timestamps":{"start":1700000000}}},"nonce":"7"})");
		CHECK(set_activity_json(1, {}, {}, 1) == R"({"cmd":"SET_ACTIVITY","args":{"pid":1,"activity":{}},"nonce":"1"})");
		CHECK(set_activity_json(1, {"In the Main Menu", ""}, {0, "logo", "Marvel: Ultimate Alliance"}, 2).find(R"("assets":{"large_image":"logo","large_text":"Marvel: Ultimate Alliance"})") != std::string::npos);
		CHECK(json_value(R"({"evt":"ERROR","data":{"code":4000,"message":"bad \"id\""}})", "message") == "bad \"id\"");
		CHECK(json_value(R"({"evt":"READY"})", "evt") == "READY" && json_value(R"({"code":4000})", "code") == "4000");

		inbox in;
		const auto frame = encode(op_frame, R"({"evt":"READY"})");
		CHECK(in.take(frame.substr(0, 5)) && !in.pop() && in.take(frame.substr(5)) && in.pop()->json == R"({"evt":"READY"})");
		std::string huge(8, '\0');
		huge[0] = 1;
		huge[6] = 0x10; // a length of 1 MiB
		CHECK(!in.take(huge) && in.problem() != nullptr);
	}

	// mods\load-order.txt, as the mod loader reads it (mod_order.hpp): a mod it switches off, or a
	// leftover .staging folder, doesn't load; a folder it doesn't list loads after the listed ones.
	void check_mod_order()
	{
		namespace fs = std::filesystem;
		std::printf("mods: the load order (mod_order.hpp)\n");
		const auto root = fs::temp_directory_path() / ("fix_test-mods-" + std::to_string(GetCurrentProcessId()));
		std::error_code ignored;
		fs::remove_all(root, ignored);
		CHECK(mod_order::load_order(root).empty() && mod_order::enabled_mods(root).empty()); // no mods folder at all
		write_file(root / L"A/data/a.engb", "x");
		write_file(root / L".staging-00000001/data/b.engb", "x"); // a launcher import cut short
		write_file(root / L"B/data/c.engb", "x");
		fs::create_directories(root / L"C", ignored);
		write_file(root / L"load-order.txt", "\xEF\xBB\xBF# the launcher's list\r\n-A\r\n+C\r\n\r\nnot an entry\r\n");
		const auto order = mod_order::load_order(root);
		CHECK(order.size() == 3 && order[0].name == L"A" && !order[0].enabled && order[1].name == L"C" && order[1].enabled && order[2].name == L"B" && order[2].enabled);
		const auto mods = mod_order::enabled_mods(root);
		CHECK(mods.size() == 2 && mods[0].first == L"C" && mods[1].first == L"B" && mods[1].second == root / L"B");
		write_file(root / L"load-order.txt", "+b\n-c\n+Gone\n"); // names in any case; a listed folder that isn't there
		const auto listed = mod_order::enabled_mods(root);
		CHECK(listed.size() == 2 && listed[0].first == L"b" && listed[1].first == L"A"); // A unlisted now: after b
		fs::create_directories(root / L"Café", ignored);
		write_file(root / L"load-order.txt", "-Caf\xC3\xA9\n"); // UTF-8
		const auto named = mod_order::load_order(root);
		CHECK(named.size() == 4 && named[0].name == L"Café" && !named[0].enabled);
		fs::remove_all(root, ignored);
	}

	// What a read of `path` through Windows gets: the file's first bytes, or "(none)" when it can't be opened.
	std::string read_through(const HANDLE file)
	{
		if (file == INVALID_HANDLE_VALUE)
		{
			return "(none)";
		}
		char buffer[64]{};
		DWORD read = 0;
		ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
		CloseHandle(file);
		return {buffer, read};
	}

	std::string read_a(const char* path)
	{
		return read_through(CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
	}

	std::string read_w(const std::filesystem::path& path)
	{
		return read_through(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
	}

	// fix_test --mods-child: runs in the folder check_mod_loader sets up. The fix hooked this program's file
	// access as it hooks a game's: the program imports the same Windows functions and asks with the same
	// kinds of paths (relative to the game folder, '/' or '\', any case).
	int mods_child()
	{
		const auto game = module_path(nullptr).parent_path();
		CHECK(read_a("data/probe.engb") == "high");                // the later mod in load-order.txt wins
		CHECK(read_w(game / L"data" / L"probe.engb") == "high");   // full path, wide
		CHECK(read_a("DATA\\Only_Low.engb") == "low");             // a file only the earlier mod has, in another case
		CHECK(read_a("data/off.engb") == "(none)");                // its mod is switched off
		CHECK(read_a("data/staged.engb") == "(none)");             // .staging folders never load
		CHECK(read_a("readme.txt") == "(none)");                   // a mod's own readme isn't a game file
		CHECK(GetFileAttributesW((game / L"probe.txt").c_str()) != INVALID_FILE_ATTRIBUTES); // only a mod has it
		WIN32_FILE_ATTRIBUTE_DATA attributes{};
		CHECK(GetFileAttributesExA("probe.txt", GetFileExInfoStandard, &attributes) && attributes.nFileSizeLow == 4);
		WIN32_FIND_DATAW found{};
		const HANDLE search = FindFirstFileW((game / L"probe.txt").c_str(), &found);
		CHECK(search != INVALID_HANDLE_VALUE && std::wstring(found.cFileName) == L"probe.txt");
		if (search != INVALID_HANDLE_VALUE)
		{
			FindClose(search);
		}

		// A write goes to the game folder (check_mod_loader looks); reads still get the mod's copy.
		const HANDLE file = CreateFileW((game / L"probe.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		DWORD written = 0;
		CHECK(file != INVALID_HANDLE_VALUE && WriteFile(file, "game", 4, &written, nullptr) && written == 4);
		CloseHandle(file);
		CHECK(read_a("probe.txt") == "high");
		return failures;
	}

	// The mod loader in a process of its own: a copy of this program and the DLL in a folder laid out like a
	// game's, with mods. The copy runs mods_child() and reports its failures through its exit code.
	void check_mod_loader(const std::filesystem::path& dir)
	{
		namespace fs = std::filesystem;
		std::printf("mods: the loader, in a copy of this program set up like a game folder\n");
		const auto game = fs::temp_directory_path() / ("fix_test-game-" + std::to_string(GetCurrentProcessId()));
		std::error_code ignored;
		fs::remove_all(game, ignored);
		fs::create_directories(game, ignored);
		CHECK(fs::copy_file(dir / L"fix_test.exe", game / L"fix_test.exe", ignored) && fs::copy_file(dir / L"dinput8.dll", game / L"dinput8.dll", ignored));
		write_file(game / L"mods/Low/data/probe.engb", "low");
		write_file(game / L"mods/Low/data/only_low.engb", "low");
		write_file(game / L"mods/High/data/probe.engb", "high");
		write_file(game / L"mods/High/probe.txt", "high");
		write_file(game / L"mods/High/readme.txt", "about the mod");
		write_file(game / L"mods/Off/data/off.engb", "off");
		write_file(game / L"mods/.staging-00000001/data/staged.engb", "staged");
		write_file(game / L"mods/load-order.txt", "+Low\n-Off\n+High\n");

		std::fflush(stdout);
		STARTUPINFOW startup{sizeof(startup)};
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION process{};
		auto command = L"\"" + (game / L"fix_test.exe").wstring() + L"\" --mods-child";
		DWORD code = 1;
		if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, game.c_str(), &startup, &process))
		{
			WaitForSingleObject(process.hProcess, 30000);
			GetExitCodeProcess(process.hProcess, &code);
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		}
		CHECK(code == 0); // the copy's checks, above
		CHECK(read_file(game / L"probe.txt") == "game" && read_file(game / L"mods/High/probe.txt") == "high");
		const auto log = read_file(game / L"mua-controller-fix.log");
		CHECK(log.find("mods: Low - 2 file(s)") != std::string::npos && log.find("mods: High - 2 file(s)") != std::string::npos);
		CHECK(log.find("mods: Off") == std::string::npos && log.find("staging") == std::string::npos);
		CHECK(log.find("mods: 3 file(s) from mods") != std::string::npos && log.find("mods: CreateFileA data/probe.engb -> ") != std::string::npos);
		fs::remove_all(game, ignored);
	}
}

int main(int argc, char** argv)
{
	if (argc > 1 && std::strcmp(argv[1], "--mods-child") == 0)
	{
		return mods_child();
	}

	const bool live = argc > 1 && std::strcmp(argv[1], "--live") == 0;
	const auto dir = module_path(nullptr).parent_path();

	check_discord_rules();
	check_mod_order();
	check_mod_loader(dir);

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
