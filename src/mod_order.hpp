#pragma once

// Which mods load, and in what order (mod_loader.hpp describes the layout): mods\load-order.txt's
// "+Name" lines (enabled) and "-Name" lines (disabled, kept for their place), then the folders it
// doesn't list, alphabetically - but not folders whose names start with '.', the launcher's staging
// folders among them. The mod loader indexes these mods' files; fix_test checks it on folders of its own.
// Same rules as XML2 Fix's mod_order.hpp.

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mod_order
{
	inline std::wstring lower(std::wstring text)
	{
		for (auto& c : text)
		{
			c = static_cast<wchar_t>(std::towlower(c));
		}
		return text;
	}

	struct entry
	{
		std::wstring name;
		bool enabled = true;
	};

	// load-order.txt's lines (UTF-8, a byte order mark allowed): "+Name" or "-Name"; '#' comments,
	// blank lines and anything else skipped. No file: no lines.
	inline std::vector<entry> read_load_order(const std::filesystem::path& file)
	{
		std::vector<entry> entries;
		std::ifstream in(file);
		std::string line;
		while (std::getline(in, line))
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			{
				line.pop_back();
			}
			if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) // UTF-8 byte order mark
			{
				line.erase(0, 3);
			}
			if (line.size() < 2 || line[0] == '#' || (line[0] != '+' && line[0] != '-'))
			{
				continue;
			}

			const std::string name = line.substr(1);
			const int length = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), nullptr, 0);
			std::wstring wide(length > 0 ? static_cast<std::size_t>(length) : 0, L'\0');
			if (length > 0)
			{
				MultiByteToWideChar(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), wide.data(), length);
			}
			entries.push_back({wide, line[0] == '+'});
		}
		return entries;
	}

	// The whole order under `root` (the mods folder): the listed entries as listed, then the unlisted
	// folders (copied in by hand) alphabetically, enabled - the launcher adds them to the list the same way.
	inline std::vector<entry> load_order(const std::filesystem::path& root)
	{
		auto order = read_load_order(root / L"load-order.txt");
		std::vector<std::wstring> unlisted;
		std::error_code error;
		for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
		{
			const auto name = it->path().filename().wstring();
			std::error_code ignored;
			const auto listed = std::any_of(order.begin(), order.end(), [&](const auto& e) { return lower(e.name) == lower(name); });
			if (it->is_directory(ignored) && !name.empty() && name.front() != L'.' && !listed)
			{
				unlisted.push_back(name);
			}
		}
		std::sort(unlisted.begin(), unlisted.end(), [](const auto& a, const auto& b) { return lower(a) < lower(b); });
		for (const auto& name : unlisted)
		{
			order.push_back({name, true});
		}
		return order;
	}

	// The mods that load, in order: each enabled entry whose folder is there, as {name, folder}.
	inline std::vector<std::pair<std::wstring, std::filesystem::path>> enabled_mods(const std::filesystem::path& root)
	{
		std::vector<std::pair<std::wstring, std::filesystem::path>> mods;
		for (const auto& [name, enabled] : load_order(root))
		{
			const auto folder = root / name;
			std::error_code error;
			if (enabled && std::filesystem::is_directory(folder, error))
			{
				mods.emplace_back(name, folder);
			}
		}
		return mods;
	}
}
