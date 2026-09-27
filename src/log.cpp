#include "log.hpp"

#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

namespace logger
{
	namespace
	{
		std::mutex mutex;
		std::FILE* file = nullptr;
		std::unordered_set<std::string> seen_keys;

		void write_line(const char* fmt, va_list args)
		{
			if (!file)
			{
				file = _wfopen((module_dir() / L"mua-controller-fix.log").c_str(), L"w");
				if (!file)
				{
					return;
				}
			}

			SYSTEMTIME time{};
			GetLocalTime(&time);
			std::fprintf(file, "[%02u:%02u:%02u.%03u] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
			std::vfprintf(file, fmt, args);
			std::fputc('\n', file);
			std::fflush(file);
		}
	}

	const std::filesystem::path& module_dir()
	{
		static const std::filesystem::path dir = []
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			                   reinterpret_cast<LPCWSTR>(&module_dir), &self);

			std::wstring buffer(MAX_PATH, L'\0');
			const auto length = GetModuleFileNameW(self, buffer.data(), static_cast<DWORD>(buffer.size()));
			buffer.resize(length);
			return std::filesystem::path(buffer).parent_path();
		}();
		return dir;
	}

	void write(const char* fmt, ...)
	{
		std::lock_guard lock(mutex);
		va_list args;
		va_start(args, fmt);
		write_line(fmt, args);
		va_end(args);
	}

	void write_once(const std::string_view key, const char* fmt, ...)
	{
		std::lock_guard lock(mutex);
		if (!seen_keys.emplace(key).second)
		{
			return;
		}

		va_list args;
		va_start(args, fmt);
		write_line(fmt, args);
		va_end(args);
	}
}
