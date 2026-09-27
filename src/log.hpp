#pragma once

#include <filesystem>
#include <string_view>

namespace logger
{
	// Directory containing this DLL (the game folder once installed).
	const std::filesystem::path& module_dir();

	void write(const char* fmt, ...);

	// Logs the message only the first time `key` is seen. Used for per-frame calls.
	void write_once(std::string_view key, const char* fmt, ...);
}
