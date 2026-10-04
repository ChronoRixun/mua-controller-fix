#include "loose_first.hpp"

#include "discord_rules.hpp"
#include "iat_hook.hpp"
#include "log.hpp"
#include "mod_loader.hpp"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string_view>

namespace loose_first
{
	namespace
	{
		using namespace std::string_view_literals;

		// The 2016 ports keep their files in encrypted .bin archives, a layer of their own under the engine's
		// file system: when the engine opens a file natively, it first asks a lookup function whether one of
		// the archives has it (archive by the path's first folder, entry by name). Only if not does it open
		// the loose path - through the exe's imports, which the mod loader redirects. In both games that
		// lookup has one caller; redirecting the call makes it report "not in the archives" for every file a
		// mod has. Its arguments: rcx = the path, rdx = the archive found, r8 = its entry.
		//
		// MUA1 (Marvel.exe): igStandardFile's open 0x1400a21c0 calls the lookup 0x1400a1700 at 0x1400a23d5;
		// on a miss it calls fopen (MSVCR110) on the same path at 0x1400a2402.
		// MUA2 (Alliance.exe): the native device's open 0x140274070 calls the lookup 0x140273570 at
		// 0x1402740fd; on a miss it calls CreateFileA at 0x140274188.
		// Addresses are the retail exes' at their preferred base; RVAs below.
		struct code_check
		{
			std::uint32_t rva;
			std::string_view bytes;
		};

		struct game_code
		{
			std::span<const code_check> checks;
			std::uint32_t call; // the `call lookup` to redirect
			std::uint32_t lookup;
		};

		constexpr code_check mua1_checks[] = {
			{0x0a23cd, "\xC7\x44\x24\x24\xFF\xFF\xFF\xFF\xE8\x26\xF3\xFF\xFF\x84\xC0\x74\x1E"sv}, // mov [rsp+24],-1; call lookup; test al,al; je miss
			{0x0a1700, "\x41\x56\x41\x57\x48\x81\xEC\x68\x02\x00\x00"sv},                         // the lookup's prologue
			{0x0a23fc, "\x49\x8B\xD6\x48\x8B\xCD\xFF\x15\xA0\xF1\x6C\x00"sv},                     // miss: fopen(path, mode)
		};
		constexpr game_code mua1{mua1_checks, 0x0a23d5, 0x0a1700};

		constexpr code_check mua2_checks[] = {
			{0x2740f2, "\xC7\x84\x24\x80\x00\x00\x00\xFF\xFF\xFF\xFF\xE8\x6E\xF4\xFF\xFF\x84\xC0\x74\x5D"sv}, // mov [rsp+80],-1; call lookup; test al,al; je miss
			{0x273570, "\x41\x56\x41\x57\x48\x81\xEC\x68\x02\x00\x00"sv},                                     // the lookup's prologue
			{0x274188, "\xFF\x15\xCA\x41\x95\x00"sv},                                                         // miss: call [CreateFileA]
		};
		constexpr game_code mua2{mua2_checks, 0x2740fd, 0x273570};

		// The game's code is checked until it matches: on the Steam release it is encrypted until the Steam
		// stub has run, so the first check that can pass is at the game's own first file access. A build that
		// still differs after this many is another build.
		constexpr int checks_before_giving_up = 1000;

		enum class state
		{
			waiting,
			done,
			off
		};

		std::atomic<state> current{state::off};
		std::mutex mutex;
		int checks = 0;
		const game_code* code = nullptr;
		std::byte* image = nullptr;
		std::size_t image_size = 0;

		using lookup_t = bool (*)(const char* path, void* archive, void* entry);
		lookup_t original_lookup = nullptr;

		bool lookup(const char* path, void* archive, void* entry)
		{
			if (mod_loader::provides(path))
			{
				return false; // the game opens the loose file instead, and the mod loader hands it the mod's
			}
			return original_lookup(path, archive, entry);
		}

		// Executable memory within a rel32 call's reach of `site`.
		std::byte* allocate_near(const std::byte* site)
		{
			SYSTEM_INFO system{};
			GetSystemInfo(&system);
			const std::uintptr_t granularity = system.dwAllocationGranularity;
			const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(site) & ~(granularity - 1);
			for (std::uintptr_t distance = granularity; distance < 0x7FF00000; distance += granularity)
			{
				for (const std::uintptr_t address : {start - distance, start + distance})
				{
					if (void* memory = VirtualAlloc(reinterpret_cast<void*>(address), system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))
					{
						return static_cast<std::byte*>(memory);
					}
				}
			}
			return nullptr;
		}

		bool matches(const code_check& check)
		{
			return check.rva + check.bytes.size() <= image_size && std::memcmp(image + check.rva, check.bytes.data(), check.bytes.size()) == 0;
		}

		void give_up(const char* why, const std::uint32_t rva)
		{
			logger::write("mods: models, textures and data stay in the archives - %s (%#x)", why, rva);
			current.store(state::off, std::memory_order_release);
		}

		void apply()
		{
			if (current.load(std::memory_order_acquire) != state::waiting)
			{
				return;
			}
			std::lock_guard lock(mutex);
			if (current.load(std::memory_order_relaxed) != state::waiting)
			{
				return;
			}

			for (const auto& check : code->checks)
			{
				if (!matches(check))
				{
					if (++checks >= checks_before_giving_up)
					{
						give_up("the game's code isn't the retail build's", check.rva);
					}
					return;
				}
			}

			// A stub near the game (a call reaches +-2 GB) jumps to lookup(); the call is pointed at the stub.
			auto* const stub = allocate_near(image + code->call);
			if (!stub)
			{
				give_up("no memory near the game's code", code->call);
				return;
			}
			const auto target = reinterpret_cast<std::uint64_t>(&lookup);
			const unsigned char jump[] = {0x48, 0xB8}; // mov rax, imm64; jmp rax
			std::memcpy(stub, jump, sizeof(jump));
			std::memcpy(stub + 2, &target, sizeof(target));
			stub[10] = std::byte{0xFF};
			stub[11] = std::byte{0xE0};
			FlushInstructionCache(GetCurrentProcess(), stub, 12);

			original_lookup = reinterpret_cast<lookup_t>(image + code->lookup);
			auto* const call = image + code->call;
			const auto displacement = static_cast<std::int32_t>(stub - (call + 5));
			DWORD protection = 0;
			if (!VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &protection))
			{
				give_up("the game's code can't be changed", code->call);
				return;
			}
			std::memcpy(call + 1, &displacement, sizeof(displacement));
			VirtualProtect(call, 5, protection, &protection);
			FlushInstructionCache(GetCurrentProcess(), call, 5);

			logger::write("mods: loose files first - a file a mod has is read from the mod, not the game's .bin archives");
			current.store(state::done, std::memory_order_release);
		}

		using create_file_a_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		using create_file_w_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
		create_file_a_t next_create_file_a = &CreateFileA;
		create_file_w_t next_create_file_w = &CreateFileW;

		HANDLE WINAPI create_file_a(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templ)
		{
			apply();
			return next_create_file_a(name, access, share, security, disposition, flags, templ);
		}

		HANDLE WINAPI create_file_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templ)
		{
			apply();
			return next_create_file_w(name, access, share, security, disposition, flags, templ);
		}
	}

	void install()
	{
		wchar_t exe[MAX_PATH]{};
		const std::wstring_view path(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
		const auto slash = path.find_last_of(L"\\/");
		const auto game = discord_rules::game_from_exe(path.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
		if (!game)
		{
			return;
		}
		code = *game == discord_rules::game::mua1 ? &mua1 : &mua2;

		const HMODULE module = GetModuleHandleW(nullptr);
		image = reinterpret_cast<std::byte*>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
		image_size = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew)->OptionalHeader.SizeOfImage;
		current.store(state::waiting, std::memory_order_release);

		// An exe without the Steam stub can be changed now, before any of the game's threads run. The Steam
		// release's code is still encrypted here: then it's changed at the game's first file access, from
		// these hooks (after mod_loader's, so they run first and pass the call on to the loader's).
		apply();
		if (current.load(std::memory_order_acquire) == state::waiting)
		{
			if (auto* previous = iat_hook::hook(module, "KERNEL32.dll", "CreateFileA", 0, reinterpret_cast<void*>(&create_file_a)))
			{
				next_create_file_a = reinterpret_cast<create_file_a_t>(previous);
			}
			if (auto* previous = iat_hook::hook(module, "KERNEL32.dll", "CreateFileW", 0, reinterpret_cast<void*>(&create_file_w)))
			{
				next_create_file_w = reinterpret_cast<create_file_w_t>(previous);
			}
		}
	}
}
