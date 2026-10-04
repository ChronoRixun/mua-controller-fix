#include "discord_presence.hpp"
#include "log.hpp"
#include "loose_first.hpp"
#include "mod_loader.hpp"
#include "steam_presence.hpp"

#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, const DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(instance);

		// Mods from <game>\mods (see mod_loader.hpp). The game checks that a movie is there, then
		// Bink opens it by name; Bink's and FMOD's file access is hooked as well as the game's.
		// [Debug] LogFiles=1 in mua-controller-fix.ini logs every file the game opens.
		// loose_first.hpp makes the game use a mod's copy of a file it keeps in its .bin archives
		// (models, textures, data), where it would otherwise never ask Windows for the file.
		const auto ini = (logger::module_dir() / L"mua-controller-fix.ini").wstring();
		if (mod_loader::install({nullptr, "bink2w64.dll", "fmodex64.dll", "fmod_event64.dll"},
		                        GetPrivateProfileIntW(L"Debug", L"LogFiles", 0, ini.c_str()) != 0))
		{
			loose_first::install();
		}

		steam_presence::install();
		discord_presence::install();
	}
	return TRUE;
}
