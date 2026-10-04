#pragma once

// Discord Rich Presence's decisions, kept apart from the pipe and the game so fix_test can check them
// without either: which game this is and so which Discord application, the [Discord] keys of
// mua-controller-fix.ini, the activity's text for the status line the game publishes, the pacing of
// updates, and the frames and JSON of Discord's local RPC pipe (as XML2 Fix has them).
//
// The game's side: both games set the Steam rich presence key "status" (steam_presence.hpp) to a
// line from their own data (data/RichPresence.xmlb): "In the Main Menu.", or "Playing <area> As
// <hero>" - MUA2 with the names it shows ("Playing Latveria: Urban Warfare As Wolverine."), MUA1 with
// short ones ("Playing Stark As Moonknight"). They build that line for each of the four player slots
// in turn (Marvel.exe 0x5f14c4), as the consoles did for each signed-in profile, but Steam keeps one
// per game: a slot without a hero ("Watching someone else play.") overwrites player 1's line in the
// same instant. So the first line of each burst is player 1's. Nothing else the game sets is shown
// ("connect": its online lobby).
//
// Discord's side: a named pipe \\.\pipe\discord-ipc-0..9 of the running Discord client; each frame is
// [uint32 opcode][uint32 length][JSON], little-endian. HANDSHAKE (0) {"v":1,"client_id":...} answered
// by a READY dispatch, then FRAME (1) commands such as SET_ACTIVITY with a nonce, answered in kind;
// CLOSE (2) ends it, PING (3) wants a PONG (4) with the same body. Discord clears a connection's
// activity when its pipe closes, and allows about five updates in 20 seconds. Whatever is on the
// other end of the pipe is held to what Discord sends: frames of at most 64 KiB, a few at a time.

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace discord_rules
{
	// ---- Which game -------------------------------------------------------------------------------

	enum class game
	{
		mua1, // Marvel.exe
		mua2, // Alliance.exe
	};

	// The two Discord applications (public ids), named after the games.
	inline constexpr std::string_view mua1_client_id = "1556188950123511889";
	inline constexpr std::string_view mua2_client_id = "1556189142092746874";

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

	inline std::string_view client_id_for(const game which)
	{
		return which == game::mua1 ? mua1_client_id : mua2_client_id;
	}

	inline std::string_view title_of(const game which)
	{
		return which == game::mua1 ? "Marvel: Ultimate Alliance" : "Marvel: Ultimate Alliance 2";
	}

	// ---- [Discord] in mua-controller-fix.ini ------------------------------------------------------

	inline std::string lowercase(const std::string_view text)
	{
		std::string out(text);
		for (auto& c : out)
		{
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		}
		return out;
	}

	inline std::string_view trim(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
		return text;
	}

	// An ini value without a comment after it ("1   ; on by default" -> "1") or the spaces around it:
	// GetPrivateProfileString hands the comment over with the value.
	inline std::string_view value_text(const std::string_view text)
	{
		const auto comment = text.find_first_of(";#");
		return trim(comment == std::string_view::npos ? text : text.substr(0, comment));
	}

	// A yes/no key (Enabled, ShowZone, ShowHero): 1/0, true/false, yes/no, on/off, in any case. Absent,
	// empty or anything else: `fallback`.
	inline bool parse_switch(const std::optional<std::string_view> value, const bool fallback)
	{
		if (!value)
		{
			return fallback;
		}
		const auto text = lowercase(value_text(*value));
		if (text == "1" || text == "true" || text == "yes" || text == "on") return true;
		if (text == "0" || text == "false" || text == "no" || text == "off") return false;
		return fallback;
	}

	// [Discord] ClientId, for testing with an application of one's own: a snowflake, 15 to 20 digits.
	inline bool valid_client_id(const std::string_view id)
	{
		return id.size() >= 15 && id.size() <= 20 && std::all_of(id.begin(), id.end(), [](const char c) { return c >= '0' && c <= '9'; });
	}

	// An art asset's key in the Discord application (or an image URL): printable ASCII without spaces or
	// quotes, at most 256 characters.
	inline bool valid_asset(const std::string_view key)
	{
		return !key.empty() && key.size() <= 256 &&
		       std::all_of(key.begin(), key.end(), [](const char c) { return c > ' ' && c < 0x7f && c != '"' && c != '\\'; });
	}

	// Both applications' picture (docs/discord-art), shown large in the activity.
	inline constexpr std::string_view large_image_key = "logo";

	struct image_choice
	{
		std::string key;      // "": no picture
		bool refused = false; // the value isn't an asset key: the logo stays, and the log says so
	};

	// [Discord] LargeImage: not set, the application's logo; none, no picture; else that asset.
	inline image_choice choose_large_image(const std::optional<std::string_view> value)
	{
		const auto text = value ? value_text(*value) : std::string_view();
		if (text.empty()) return {std::string(large_image_key)};
		if (lowercase(text) == "none") return {""};
		if (valid_asset(text)) return {std::string(text)};
		return {std::string(large_image_key), true};
	}

	// ---- Text -------------------------------------------------------------------------------------

	inline void append_utf8(std::string& out, const std::uint32_t code_point)
	{
		if (code_point < 0x80)
		{
			out += static_cast<char>(code_point);
		}
		else if (code_point < 0x800)
		{
			out += static_cast<char>(0xc0 | (code_point >> 6));
			out += static_cast<char>(0x80 | (code_point & 0x3f));
		}
		else if (code_point < 0x10000)
		{
			out += static_cast<char>(0xe0 | (code_point >> 12));
			out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (code_point & 0x3f));
		}
		else
		{
			out += static_cast<char>(0xf0 | (code_point >> 18));
			out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
			out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
			out += static_cast<char>(0x80 | (code_point & 0x3f));
		}
	}

	inline bool is_utf8(const std::string_view text)
	{
		for (std::size_t i = 0; i < text.size();)
		{
			const auto lead = static_cast<unsigned char>(text[i]);
			std::size_t extra = 0;
			std::uint32_t code = 0;
			std::uint32_t lowest = 0;
			if (lead < 0x80)
			{
				++i;
				continue;
			}
			if ((lead & 0xe0) == 0xc0) { extra = 1; code = lead & 0x1f; lowest = 0x80; }
			else if ((lead & 0xf0) == 0xe0) { extra = 2; code = lead & 0x0f; lowest = 0x800; }
			else if ((lead & 0xf8) == 0xf0) { extra = 3; code = lead & 0x07; lowest = 0x10000; }
			else return false;
			if (text.size() - i <= extra)
			{
				return false;
			}
			for (std::size_t k = 1; k <= extra; ++k)
			{
				const auto next = static_cast<unsigned char>(text[i + k]);
				if ((next & 0xc0) != 0x80)
				{
					return false;
				}
				code = (code << 6) | (next & 0x3f);
			}
			if (code < lowest || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
			{
				return false;
			}
			i += extra + 1;
		}
		return true;
	}

	// The game's text as UTF-8, for Discord: as it is when it already is UTF-8, else read as Windows-1252
	// (the five codes Windows-1252 leaves undefined become '?'). Control characters (MUA2's template has
	// a line break) become spaces.
	inline std::string utf8_from_game(const std::string_view text)
	{
		std::string out;
		if (is_utf8(text))
		{
			for (const char c : text)
			{
				const auto byte = static_cast<unsigned char>(c);
				out += byte < 0x20 || byte == 0x7f ? ' ' : c;
			}
			return out;
		}
		static constexpr std::array<std::uint16_t, 32> high{
			0x20ac, 0,      0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017d, 0,
			0,      0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0,      0x017e, 0x0178};
		for (const char c : text)
		{
			const auto byte = static_cast<unsigned char>(c);
			if (byte < 0x20 || byte == 0x7f)
			{
				out += ' ';
			}
			else if (byte >= 0x80 && byte < 0xa0)
			{
				const auto mapped = high[byte - 0x80];
				append_utf8(out, mapped ? mapped : '?');
			}
			else
			{
				append_utf8(out, byte);
			}
		}
		return out;
	}

	// The characters (code points) in UTF-8 text.
	inline std::size_t characters(const std::string_view utf8)
	{
		return static_cast<std::size_t>(std::count_if(utf8.begin(), utf8.end(), [](const char c) { return (static_cast<unsigned char>(c) & 0xc0) != 0x80; }));
	}

	// At most `limit` characters of UTF-8 text; a longer one is cut and ends with an ellipsis. Discord
	// takes 128 in details and state.
	inline std::string clip(const std::string_view utf8, const std::size_t limit = 128)
	{
		if (characters(utf8) <= limit || limit == 0)
		{
			return std::string(utf8);
		}
		std::size_t kept = 0;
		std::size_t end = 0;
		for (std::size_t i = 0; i < utf8.size(); ++i)
		{
			if ((static_cast<unsigned char>(utf8[i]) & 0xc0) != 0x80)
			{
				if (kept == limit - 1)
				{
					end = i;
					break;
				}
				++kept;
			}
		}
		return std::string(utf8.substr(0, end)) + "\xE2\x80\xA6"; // …
	}

	// A JSON string's body for UTF-8 text.
	inline std::string json_escape(const std::string_view utf8)
	{
		std::string out;
		for (const char c : utf8)
		{
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
				{
					static constexpr char digits[] = "0123456789abcdef";
					out += "\\u00";
					out += digits[(c >> 4) & 0xf];
					out += digits[c & 0xf];
				}
				else
				{
					out += c;
				}
			}
		}
		return out;
	}

	// ---- What the activity says -------------------------------------------------------------------

	// Player 1's status line out of the game's bursts (above): a line that comes within burst_gap_ms of
	// the one before belongs to the same burst - another player slot's - and is passed over. The game
	// sends a burst at most every two seconds.
	inline constexpr std::uint64_t burst_gap_ms = 250;

	class first_of_burst
	{
	public:
		bool take(const std::uint64_t now_ms)
		{
			const bool first = !seen_ || now_ms - last_ms_ >= burst_gap_ms;
			last_ms_ = now_ms;
			seen_ = true;
			return first;
		}

	private:
		std::uint64_t last_ms_ = 0;
		bool seen_ = false;
	};

	struct display
	{
		bool zone = true; // [Discord] ShowZone: the area
		bool hero = true; // [Discord] ShowHero: player 1's hero
	};

	struct activity
	{
		std::string details; // Discord's first line under the game's name
		std::string state;   // the second
		bool operator==(const activity&) const = default;
	};

	// MUA1's status line has the short names of its data, MUA2's the ones it shows. MUA1's names seen
	// so far, as the game shows them; any other comes through as it is.
	struct name_change
	{
		std::string_view from;
		std::string_view to;
	};

	inline constexpr std::array<name_change, 3> mua1_names{{
		{"Moonknight", "Moon Knight"},
		{"Omega", "Omega Base"},
		{"Stark", "Stark Tower"},
	}};

	inline std::string shown_name(const game which, const std::string_view name)
	{
		if (which == game::mua1)
		{
			for (const auto& change : mua1_names)
			{
				if (change.from == name) return std::string(change.to);
			}
		}
		return std::string(name);
	}

	// Spaces one at a time and none at either end.
	inline std::string squeeze(const std::string_view text)
	{
		std::string out;
		for (const char c : trim(text))
		{
			if (c != ' ' || out.empty() || out.back() != ' ') out += c;
		}
		return out;
	}

	inline constexpr std::string_view playing_word = "Playing ";
	inline constexpr std::string_view as_word = " As ";

	// The activity for the game's status line (UTF-8): "Playing <area> As <hero>" becomes the area over
	// "Playing as <hero>" ([Discord] ShowZone and ShowHero take either away); any other line ("In the
	// Main Menu.") shows as it is, without its full stop. No line yet (the intro movies): no text, only
	// the game's name and time.
	inline activity activity_for(const game which, const std::string_view status, const display& show)
	{
		std::string line = squeeze(status);
		if (!line.empty() && line.back() == '.')
		{
			line = squeeze(std::string_view(line).substr(0, line.size() - 1));
		}
		const auto as = line.rfind(as_word);
		if (line.starts_with(playing_word) && as != std::string::npos && as >= playing_word.size())
		{
			const auto area = shown_name(which, trim(std::string_view(line).substr(playing_word.size(), as - playing_word.size())));
			const auto hero = shown_name(which, trim(std::string_view(line).substr(as + as_word.size())));
			if (!area.empty() && !hero.empty())
			{
				const std::string playing = show.hero ? "Playing as " + hero : "Playing";
				return show.zone ? activity{clip(area), clip(playing)} : activity{clip(playing), ""};
			}
		}
		return {clip(line), ""};
	}

	inline std::string describe(const activity& a)
	{
		std::string text = a.details.empty() ? "(no details)" : a.details;
		if (!a.state.empty()) text += " | " + a.state;
		return text;
	}

	// ---- Pacing -------------------------------------------------------------------------------------

	inline constexpr std::uint64_t sample_ms = 1000;        // the status is looked at once a second,
	inline constexpr std::uint64_t min_update_ms = 5000;    // an update sent at most every 5 s (Discord: ~5 per 20 s),
	inline constexpr std::uint64_t retry_ms = 20000;        // and Discord looked for every 20 s while it isn't there
	inline constexpr std::uint64_t first_connect_ms = 2000; // after the game has started

	// Updates at most one per min_update_ms: the latest activity waits its turn and replaces any older
	// one still waiting (the caller keeps only the latest).
	class gate
	{
	public:
		bool may_send(const std::uint64_t now_ms) const
		{
			return !sent_ || now_ms - last_ms_ >= min_update_ms;
		}
		void sent(const std::uint64_t now_ms)
		{
			last_ms_ = now_ms;
			sent_ = true;
		}
		void reset()
		{
			sent_ = false;
		}

	private:
		std::uint64_t last_ms_ = 0;
		bool sent_ = false;
	};

	// ---- Frames and JSON ----------------------------------------------------------------------------

	enum opcode : std::uint32_t
	{
		op_handshake = 0,
		op_frame = 1,
		op_close = 2,
		op_ping = 3,
		op_pong = 4,
	};

	inline constexpr std::uint32_t max_frame = 64 * 1024;       // no frame of Discord's is near this long
	inline constexpr std::size_t max_read_per_poll = 64 * 1024; // bytes read from the pipe in one go; the rest waits for the next
	inline constexpr std::size_t max_queued = 64;               // frames waiting to be handled: Discord answers each command
	                                                            // once and pings now and then - more is a flood, not Discord

	inline std::string encode(const std::uint32_t op, const std::string_view json)
	{
		std::string out(8, '\0');
		const auto length = static_cast<std::uint32_t>(json.size());
		for (int i = 0; i < 4; ++i)
		{
			out[i] = static_cast<char>((op >> (8 * i)) & 0xff);
			out[4 + i] = static_cast<char>((length >> (8 * i)) & 0xff);
		}
		out.append(json);
		return out;
	}

	struct message
	{
		std::uint32_t op = 0;
		std::string json;
	};

	// Frames out of the bytes read from the pipe, however they were split. Each frame costs its own
	// length, not the buffer's: what was handed out is dropped once per feed.
	class frame_reader
	{
	public:
		void feed(const std::string_view bytes)
		{
			if (start_ > 0)
			{
				buffer_.erase(0, start_);
				start_ = 0;
			}
			buffer_.append(bytes);
		}
		std::optional<message> next()
		{
			if (broken_ || buffer_.size() - start_ < 8)
			{
				return std::nullopt;
			}
			const auto u32 = [&](const std::size_t at)
			{
				std::uint32_t value = 0;
				for (int i = 3; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(buffer_[start_ + at + static_cast<std::size_t>(i)]);
				return value;
			};
			const auto op = u32(0);
			const auto length = u32(4);
			if (length > max_frame)
			{
				broken_ = true;
				return std::nullopt;
			}
			if (buffer_.size() - start_ < 8 + static_cast<std::size_t>(length))
			{
				return std::nullopt;
			}
			message m{op, buffer_.substr(start_ + 8, length)};
			start_ += 8 + static_cast<std::size_t>(length);
			if (start_ == buffer_.size())
			{
				buffer_.clear();
				start_ = 0;
			}
			return m;
		}
		bool broken() const
		{
			return broken_;
		}
		void clear()
		{
			buffer_.clear();
			start_ = 0;
			broken_ = false;
		}

	private:
		std::string buffer_;
		std::size_t start_ = 0; // the first byte not handed out yet
		bool broken_ = false;
	};

	// The frames read from the pipe, waiting to be handled - held to what Discord sends. A frame longer
	// than max_frame, or more than max_queued frames waiting at once, means whatever is on the other end
	// isn't Discord (or is broken): the stream can't be trusted any more, everything waiting is dropped,
	// and nothing more is taken or handed out until clear() - the caller closes the pipe.
	class inbox
	{
	public:
		bool take(const std::string_view bytes)
		{
			if (problem_)
			{
				return false;
			}
			reader_.feed(bytes);
			while (auto m = reader_.next())
			{
				if (queued_.size() >= max_queued)
				{
					fail("more than 64 frames at once");
					return false;
				}
				queued_.push_back(std::move(*m));
			}
			if (reader_.broken())
			{
				fail("a frame longer than 64 KiB");
				return false;
			}
			return true;
		}
		std::optional<message> pop()
		{
			if (problem_ || queued_.empty())
			{
				return std::nullopt;
			}
			auto m = std::move(queued_.front());
			queued_.pop_front();
			return m;
		}
		// Why the stream can't be trusted; null while it can.
		const char* problem() const
		{
			return problem_;
		}
		void clear()
		{
			reader_.clear();
			queued_.clear();
			problem_ = nullptr;
		}

	private:
		void fail(const char* why)
		{
			problem_ = why;
			queued_.clear();
			reader_.clear();
		}

		frame_reader reader_;
		std::deque<message> queued_;
		const char* problem_ = nullptr;
	};

	inline std::string handshake_json(const std::string_view client_id)
	{
		return "{\"v\":1,\"client_id\":\"" + json_escape(client_id) + "\"}";
	}

	struct extras
	{
		std::int64_t start = 0;  // the elapsed timer's start, Unix seconds; 0: none
		std::string large_image; // choose_large_image; "": no picture
		std::string large_text;  // its tooltip: the game's title
	};

	inline std::string set_activity_json(const DWORD pid, const activity& a, const extras& x, const unsigned nonce)
	{
		std::string json = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) + ",\"activity\":{";
		std::string fields;
		const auto add = [&](const std::string& field)
		{
			if (!fields.empty()) fields += ',';
			fields += field;
		};
		// Discord wants at least two characters in details and state.
		if (characters(a.details) >= 2) add("\"details\":\"" + json_escape(a.details) + "\"");
		if (characters(a.state) >= 2) add("\"state\":\"" + json_escape(a.state) + "\"");
		if (x.start > 0) add("\"timestamps\":{\"start\":" + std::to_string(x.start) + "}");
		if (!x.large_image.empty())
		{
			std::string assets = "\"large_image\":\"" + json_escape(x.large_image) + "\"";
			if (characters(x.large_text) >= 2) assets += ",\"large_text\":\"" + json_escape(x.large_text) + "\"";
			add("\"assets\":{" + assets + "}");
		}
		json += fields + "}},\"nonce\":\"" + std::to_string(nonce) + "\"}";
		return json;
	}

	// The first value of `key` in Discord's JSON: a string unescaped, anything else as written (null,
	// numbers). Enough for evt, cmd, code and message; not a JSON parser.
	inline std::optional<std::string> json_value(const std::string_view json, const std::string_view key)
	{
		const std::string quoted = "\"" + std::string(key) + "\"";
		std::size_t at = 0;
		while ((at = json.find(quoted, at)) != std::string_view::npos)
		{
			auto i = at + quoted.size();
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
			if (i >= json.size() || json[i] != ':')
			{
				at = i; // "key" was a value, not a key
				continue;
			}
			++i;
			while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
			if (i >= json.size())
			{
				return std::nullopt;
			}
			if (json[i] != '"')
			{
				const auto end = json.find_first_of(",}] \t\r\n", i);
				return std::string(json.substr(i, end == std::string_view::npos ? json.size() - i : end - i));
			}
			std::string out;
			for (++i; i < json.size(); ++i)
			{
				const char c = json[i];
				if (c == '"')
				{
					return out;
				}
				if (c != '\\' || i + 1 >= json.size())
				{
					out += c;
					continue;
				}
				const char e = json[++i];
				switch (e)
				{
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u':
					if (i + 4 < json.size())
					{
						std::uint32_t code = 0;
						bool ok = true;
						for (int d = 1; d <= 4; ++d)
						{
							const char h = json[i + static_cast<std::size_t>(d)];
							code <<= 4;
							if (h >= '0' && h <= '9') code |= static_cast<std::uint32_t>(h - '0');
							else if (h >= 'a' && h <= 'f') code |= static_cast<std::uint32_t>(h - 'a' + 10);
							else if (h >= 'A' && h <= 'F') code |= static_cast<std::uint32_t>(h - 'A' + 10);
							else ok = false;
						}
						if (ok)
						{
							append_utf8(out, code);
							i += 4;
						}
					}
					break;
				default: out += e; break; // \" \\ \/
				}
			}
			return std::nullopt; // unterminated
		}
		return std::nullopt;
	}
}
