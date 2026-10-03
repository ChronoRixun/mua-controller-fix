#include "steam_presence.hpp"

#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, const DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(instance);
		steam_presence::install();
	}
	return TRUE;
}
