// dinput8.dll replacement: every export forwards to Windows' own dinput8.dll in the
// system folder. DirectInput8Create additionally installs the gamepad fix on the
// object it returns.

#include "gamepad_fix.hpp"
#include "log.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>

#include <string>

namespace
{
	HMODULE system_dinput8()
	{
		static const HMODULE module = []
		{
			wchar_t folder[MAX_PATH]{};
			const UINT length = GetSystemDirectoryW(folder, MAX_PATH);
			const auto path = std::wstring(folder, length) + L"\\dinput8.dll";

			const auto loaded = LoadLibraryW(path.c_str());
			if (loaded)
			{
				logger::write("loaded %ls", path.c_str());
			}
			else
			{
				logger::write("ERROR: could not load %ls (error %lu)", path.c_str(), GetLastError());
			}
			return loaded;
		}();
		return module;
	}

	template <typename T>
	T real(const char* name)
	{
		const auto module = system_dinput8();
		return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
	}
}

extern "C"
{
	const char* MuaControllerFix_Version()
	{
		return "MUA Controller Fix 1.1.0";
	}

	HRESULT WINAPI proxy_DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
	{
		static const auto target = real<decltype(&DirectInput8Create)>("DirectInput8Create");
		if (!target)
		{
			return DIERR_GENERIC;
		}

		const auto result = target(instance, version, riid, out, outer);
		if (SUCCEEDED(result) && out && *out)
		{
			logger::write_once("version", "%s", MuaControllerFix_Version());
			gamepad_fix::hook_direct_input(*out);
		}
		return result;
	}

	HRESULT WINAPI proxy_DllCanUnloadNow()
	{
		static const auto target = real<HRESULT(WINAPI*)()>("DllCanUnloadNow");
		return target ? target() : S_FALSE;
	}

	HRESULT WINAPI proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
	{
		static const auto target = real<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject");
		return target ? target(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
	}

	HRESULT WINAPI proxy_DllRegisterServer()
	{
		static const auto target = real<HRESULT(WINAPI*)()>("DllRegisterServer");
		return target ? target() : E_FAIL;
	}

	HRESULT WINAPI proxy_DllUnregisterServer()
	{
		static const auto target = real<HRESULT(WINAPI*)()>("DllUnregisterServer");
		return target ? target() : E_FAIL;
	}

	LPCDIDATAFORMAT WINAPI proxy_GetdfDIJoystick()
	{
		static const auto target = real<LPCDIDATAFORMAT(WINAPI*)()>("GetdfDIJoystick");
		return target ? target() : nullptr;
	}
}
