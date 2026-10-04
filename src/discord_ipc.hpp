#pragma once

// A connection to the running Discord client's local RPC pipe (\\.\pipe\discord-ipc-0..9), as its
// SDKs make one, without them: frames of [opcode][length][JSON] (discord_rules.hpp). Every read and
// write has a timeout, so a Discord that stops answering never holds its caller for long, and what
// the other end sends is held to what Discord sends (discord_rules::inbox): a peer that floods the
// pipe, or sends a frame no Discord does, is dropped at once. Used by the presence's own thread in the
// game (discord_presence.cpp). As XML2 Fix has it (src/discord_ipc.hpp there).

#include "discord_rules.hpp"

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace discord_ipc
{
	inline constexpr std::wstring_view discord_pipes = L"\\\\.\\pipe\\discord-ipc-";

	class connection
	{
	public:
		connection() = default;
		~connection();
		connection(const connection&) = delete;
		connection& operator=(const connection&) = delete;

		// Opens the first of Discord's pipes (`pipes` 0..9) that is there and introduces itself as the
		// application `client_id`, waiting up to `timeout_ms` in all for Discord's READY - however much
		// else arrives meanwhile. On success `pipe` names the pipe; otherwise `error` says why (no
		// Discord, Discord's refusal - e.g. an unknown application, with its code and message - or a
		// peer that isn't Discord).
		bool open(std::string_view client_id, std::string& pipe, std::string& error, DWORD timeout_ms = 5000, std::wstring_view pipes = discord_pipes);

		bool is_open() const;

		// Sends one frame, waiting at most `timeout_ms` for the pipe to take it.
		bool send(std::uint32_t op, std::string_view json, DWORD timeout_ms = 2000);

		// The frames Discord has sent since the last call, without waiting - those it sent before its
		// end closed too (a CLOSE with its reason comes right before). False once the pipe is gone and
		// nothing it sent is left.
		bool receive(std::vector<discord_rules::message>& out);

		// The next frame, waiting up to `timeout_ms`; nullopt on a timeout, when the pipe is gone and
		// nothing it sent is left (is_open() says which), or when the cancel event is set.
		std::optional<discord_rules::message> wait(DWORD timeout_ms);

		// An event that, once set, cuts wait() and open() short (the game quitting); null for none.
		void cancel_on(HANDLE event);

		// Why this side dropped the connection - the peer flooded the pipe or sent a frame no Discord
		// does - or "" (Discord closed it, or it broke).
		const std::string& problem() const;

		void close();

	private:
		enum class poll_result
		{
			open,      // read what there was
			gone,      // the pipe broke; what was read before stays in the inbox
			untrusted, // the peer isn't Discord: the inbox is dropped
		};

		HANDLE pipe_ = INVALID_HANDLE_VALUE;
		HANDLE event_ = nullptr;
		HANDLE cancel_ = nullptr;
		discord_rules::inbox inbox_;
		std::string problem_;

		poll_result read_available();
		void poll(); // reads what there is; closes the pipe when it's gone or can't be trusted
		bool cancelled() const;
	};
}
