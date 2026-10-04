#include "discord_presence.hpp"
#include "discord_ipc.hpp"
#include "discord_rules.hpp"
#include "log.hpp"

#include <Windows.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace discord_presence
{
	namespace
	{
		using namespace discord_rules;

		// Set before the thread starts; read only after.
		game which = game::mua2;
		std::string client_id;
		display show;
		extras presence_extras;
		DWORD pid = 0; // for the Discord client on this PC only (args.pid), never in the activity

		std::mutex status_mutex;
		std::string status; // player 1's status line, UTF-8

		std::string current_status()
		{
			std::lock_guard lock(status_mutex);
			return status;
		}

		// ---- mua-controller-fix.ini ---------------------------------------------------------------

		std::string utf8(const std::wstring_view text)
		{
			if (text.empty())
			{
				return {};
			}
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		// [section] key of mua-controller-fix.ini, next to the DLL; nullopt when it isn't there.
		std::optional<std::string> ini_text(const wchar_t* section, const wchar_t* key)
		{
			static const auto ini = (logger::module_dir() / L"mua-controller-fix.ini").wstring();
			constexpr const wchar_t* absent = L"\x01";
			wchar_t value[512]{};
			GetPrivateProfileStringW(section, key, absent, value, static_cast<DWORD>(std::size(value)), ini.c_str());
			if (std::wstring_view(value) == absent)
			{
				return std::nullopt;
			}
			return utf8(value);
		}

		std::optional<std::string_view> view(const std::optional<std::string>& text)
		{
			return text ? std::optional<std::string_view>(*text) : std::nullopt;
		}

		std::int64_t unix_now()
		{
			FILETIME now{};
			GetSystemTimeAsFileTime(&now);
			ULARGE_INTEGER ticks{};
			ticks.LowPart = now.dwLowDateTime;
			ticks.HighPart = now.dwHighDateTime;
			return static_cast<std::int64_t>((ticks.QuadPart - 116444736000000000ULL) / 10000000ULL);
		}

		// ---- The thread ---------------------------------------------------------------------------

		constexpr unsigned changes_to_log = 100;

		// Everything the presence's thread does. No C++ exception leaves it: a failure while talking to
		// Discord drops that one round - the connection is closed and Discord looked for again later -
		// instead of taking the game down. It runs until the game ends; Discord clears the activity
		// when the game's end closes the pipe.
		class presence
		{
		public:
			void run()
			{
				Sleep(static_cast<DWORD>(first_connect_ms));
				for (;;)
				{
					const std::uint64_t now = GetTickCount64();
					try
					{
						talk(now);
					}
					catch (...)
					{
						pipe_.close();
						sent_.reset();
						next_attempt_ = now + retry_ms;
						if (!talk_failed_)
						{
							logger::write("discord: ERROR: talking to Discord failed (out of memory?) - connection closed, looking again every 20 s");
							talk_failed_ = true;
						}
					}
					Sleep(static_cast<DWORD>(sample_ms));
				}
			}

		private:
			discord_ipc::connection pipe_;
			gate pacing_;
			std::optional<activity> sent_; // on this connection
			std::uint64_t next_attempt_ = 0;
			std::string last_problem_;
			std::string last_refusal_;
			unsigned nonce_ = 0;
			unsigned changes_ = 0;
			bool talk_failed_ = false;

			// Discord: connect when it's time, handle what it sent, send the activity when it changed.
			void talk(const std::uint64_t now)
			{
				const activity target = activity_for(which, current_status(), show);
				if (!pipe_.is_open() && now >= next_attempt_)
				{
					std::string name;
					std::string problem;
					if (pipe_.open(client_id, name, problem))
					{
						logger::write("discord: connected as %.*s (%s)", static_cast<int>(title_of(which).size()), title_of(which).data(), name.c_str());
						last_problem_.clear();
						last_refusal_.clear();
						sent_.reset();
						pacing_.reset();
					}
					else
					{
						next_attempt_ = now + retry_ms;
						if (problem != last_problem_)
						{
							logger::write("discord: %s - looking again every %u s", problem.c_str(), static_cast<unsigned>(retry_ms / 1000));
							last_problem_ = problem;
						}
					}
				}
				if (!pipe_.is_open())
				{
					return;
				}
				bool said_why = false;
				std::vector<message> incoming;
				if (pipe_.receive(incoming))
				{
					for (const auto& m : incoming)
					{
						if (m.op == op_ping)
						{
							pipe_.send(op_pong, m.json);
						}
						else if (m.op == op_close)
						{
							logger::write("discord: Discord closed the connection (%s: %s) - looking again every %u s", json_value(m.json, "code").value_or("?").c_str(),
							              json_value(m.json, "message").value_or("no reason given").c_str(), static_cast<unsigned>(retry_ms / 1000));
							pipe_.close();
							said_why = true;
							break;
						}
						else if (m.op == op_frame && json_value(m.json, "evt") == "ERROR")
						{
							const auto why = json_value(m.json, "message").value_or("no message");
							if (why != last_refusal_)
							{
								logger::write("discord: Discord refused the presence (%s)", why.c_str());
								last_refusal_ = why;
							}
						}
					}
				}
				if (pipe_.is_open() && (!sent_ || *sent_ != target) && pacing_.may_send(now))
				{
					if (pipe_.send(op_frame, set_activity_json(pid, target, presence_extras, ++nonce_)))
					{
						pacing_.sent(now);
						sent_ = target;
						if (changes_ < changes_to_log)
						{
							logger::write("discord: presence -> %s", describe(target).c_str());
						}
						else if (changes_ == changes_to_log)
						{
							logger::write("discord: (further presence changes aren't logged)");
						}
						++changes_;
					}
				}
				if (!pipe_.is_open())
				{
					if (!said_why)
					{
						if (!pipe_.problem().empty())
						{
							logger::write("discord: the pipe %s - closed, looking again every %u s", pipe_.problem().c_str(), static_cast<unsigned>(retry_ms / 1000));
						}
						else
						{
							logger::write("discord: the connection to Discord is gone - looking again every %u s", static_cast<unsigned>(retry_ms / 1000));
						}
					}
					last_problem_ = "gone";
					next_attempt_ = now + retry_ms;
				}
			}
		};

		DWORD WINAPI run(void*)
		{
			try
			{
				presence().run();
			}
			catch (...)
			{
				// Only a presence that couldn't be made (out of memory); its pipe, if any, is closed.
			}
			return 0;
		}
	}

	void set_status(const std::string_view text)
	{
		auto line = utf8_from_game(text);
		std::lock_guard lock(status_mutex);
		status = std::move(line);
	}

	void install()
	{
		wchar_t exe[MAX_PATH]{};
		const std::wstring_view path(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
		const auto slash = path.find_last_of(L"\\/");
		const auto found = game_from_exe(path.substr(slash == std::wstring_view::npos ? 0 : slash + 1));
		if (!found)
		{
			return; // not one of the games
		}
		which = *found;

		if (!parse_switch(view(ini_text(L"Discord", L"Enabled")), true))
		{
			logger::write("discord: off ([Discord] Enabled in mua-controller-fix.ini)");
			return;
		}

		client_id = std::string(client_id_for(which));
		if (const auto own = ini_text(L"Discord", L"ClientId"))
		{
			const auto id = value_text(*own);
			if (valid_client_id(id))
			{
				client_id = std::string(id);
			}
			else if (!id.empty())
			{
				logger::write("discord: [Discord] ClientId=%.*s isn't an application id (15 to 20 digits) - ignored", static_cast<int>(id.size()), id.data());
			}
		}
		if (const auto image = ini_text(L"Discord", L"LargeImage"))
		{
			const auto key = value_text(*image);
			if (valid_asset(key))
			{
				presence_extras.large_image = std::string(key);
			}
			else if (!key.empty())
			{
				logger::write("discord: [Discord] LargeImage isn't an asset key (no spaces or quotes, at most 256 characters) - ignored");
			}
		}
		show.zone = parse_switch(view(ini_text(L"Discord", L"ShowZone")), true);
		show.hero = parse_switch(view(ini_text(L"Discord", L"ShowHero")), true);
		presence_extras.large_text = std::string(title_of(which));
		presence_extras.start = unix_now();
		pid = GetCurrentProcessId();

		const HANDLE thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
		if (!thread)
		{
			logger::write("discord: ERROR: could not start the presence's thread (error %lu)", GetLastError());
			return;
		}
		CloseHandle(thread);
		logger::write("discord: presence on as %.*s (application %s; area %s, hero %s)", static_cast<int>(title_of(which).size()), title_of(which).data(), client_id.c_str(),
		              show.zone ? "shown" : "hidden", show.hero ? "shown" : "hidden");
	}
}
