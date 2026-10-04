#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace game_id
{
	enum class game
	{
		mua1, // Marvel.exe
		mua2, // Alliance.exe
	};

	// The game from its executable's file name, in any case; nullopt for anything else (the test).
	inline std::optional<game> game_from_exe(const std::wstring_view file_name)
	{
		const auto is = [&](const std::wstring_view name)
		{
			return file_name.size() == name.size() && std::equal(file_name.begin(), file_name.end(), name.begin(), [](const wchar_t a, const wchar_t b)
			                                                     { return (a >= L'A' && a <= L'Z' ? a - L'A' + L'a' : a) == b; });
		};
		if (is(L"marvel.exe")) return game::mua1;
		if (is(L"alliance.exe")) return game::mua2;
		return std::nullopt;
	}

	// The game's own Steam app ID.
	inline constexpr std::uint32_t steam_app_id(const game which)
	{
		return which == game::mua1 ? 433300 : 433320;
	}
}
