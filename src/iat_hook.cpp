#include "iat_hook.hpp"

#include <cstddef>
#include <cstring>

namespace iat_hook
{
	void* hook(const HMODULE module, const char* dll, const char* function, const WORD ordinal, void* replacement)
	{
		if (!module)
		{
			return nullptr;
		}

		auto* base = reinterpret_cast<std::byte*>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!directory.VirtualAddress)
		{
			return nullptr;
		}

		for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); import->Name; ++import)
		{
			if (_stricmp(reinterpret_cast<const char*>(base + import->Name), dll) != 0 || !import->OriginalFirstThunk)
			{
				continue;
			}

			const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk);
			auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++addresses)
			{
				bool match = false;
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
				{
					match = ordinal && IMAGE_ORDINAL(names->u1.Ordinal) == ordinal;
				}
				else if (function)
				{
					const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					match = std::strcmp(by_name->Name, function) == 0;
				}
				if (!match)
				{
					continue;
				}

				DWORD old_protect = 0;
				VirtualProtect(&addresses->u1.Function, sizeof(void*), PAGE_READWRITE, &old_protect);
				auto* previous = reinterpret_cast<void*>(addresses->u1.Function);
				addresses->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
				VirtualProtect(&addresses->u1.Function, sizeof(void*), old_protect, &old_protect);
				return previous;
			}
		}
		return nullptr;
	}
}
