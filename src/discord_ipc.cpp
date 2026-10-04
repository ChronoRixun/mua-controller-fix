#include "discord_ipc.hpp"

#include <algorithm>
#include <string>

namespace discord_ipc
{
	namespace
	{
		// Waits for an overlapped operation; cancels it after `timeout_ms`.
		bool finish(const HANDLE pipe, OVERLAPPED& overlapped, const BOOL started, DWORD& transferred, const DWORD timeout_ms)
		{
			if (started)
			{
				return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
			}
			if (GetLastError() != ERROR_IO_PENDING)
			{
				return false;
			}
			if (WaitForSingleObject(overlapped.hEvent, timeout_ms) != WAIT_OBJECT_0)
			{
				CancelIoEx(pipe, &overlapped);
				GetOverlappedResult(pipe, &overlapped, &transferred, TRUE); // the cancel's completion
				return false;
			}
			return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
		}

		// A pipe's name for the log: what follows \\.\pipe\ (ASCII, as the names here are).
		std::string short_name(const std::wstring& name)
		{
			const auto slash = name.find_last_of(L'\\');
			std::string out;
			for (const wchar_t c : name.substr(slash == std::wstring::npos ? 0 : slash + 1))
			{
				out += c < 128 ? static_cast<char>(c) : '?';
			}
			return out;
		}
	}

	connection::~connection()
	{
		close();
		if (event_)
		{
			CloseHandle(event_);
		}
	}

	bool connection::is_open() const
	{
		return pipe_ != INVALID_HANDLE_VALUE;
	}

	void connection::cancel_on(const HANDLE event)
	{
		cancel_ = event;
	}

	bool connection::cancelled() const
	{
		return cancel_ && WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0;
	}

	const std::string& connection::problem() const
	{
		return problem_;
	}

	bool connection::open(const std::string_view client_id, std::string& pipe, std::string& error, const DWORD timeout_ms, const std::wstring_view pipes)
	{
		close();
		inbox_.clear(); // nothing from an earlier connection
		problem_.clear();
		if (cancelled())
		{
			error = "the game is quitting";
			return false;
		}
		if (!event_)
		{
			event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!event_)
			{
				error = "can't create an event (error " + std::to_string(GetLastError()) + ")";
				return false;
			}
		}
		DWORD last_error = ERROR_FILE_NOT_FOUND;
		for (int i = 0; i < 10 && pipe_ == INVALID_HANDLE_VALUE; ++i)
		{
			const std::wstring name = std::wstring(pipes) + std::to_wstring(i);
			pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
			if (pipe_ != INVALID_HANDLE_VALUE)
			{
				pipe = short_name(name);
			}
			else if (GetLastError() != ERROR_FILE_NOT_FOUND)
			{
				last_error = GetLastError(); // busy, or not ours to open: try the next
			}
		}
		if (pipe_ == INVALID_HANDLE_VALUE)
		{
			error = last_error == ERROR_FILE_NOT_FOUND ? "Discord isn't running (no \\\\.\\pipe\\discord-ipc-0..9)"
			                                           : "Discord's pipe can't be opened (error " + std::to_string(last_error) + ")";
			return false;
		}

		if (!send(discord_rules::op_handshake, discord_rules::handshake_json(client_id)))
		{
			error = "Discord didn't take the handshake on " + pipe;
			close();
			return false;
		}
		// The deadline holds however many frames arrive meanwhile: only READY, an error or a CLOSE ends
		// the handshake before it.
		const DWORD start = GetTickCount();
		for (;;)
		{
			const DWORD elapsed = GetTickCount() - start;
			if (elapsed >= timeout_ms)
			{
				error = "Discord didn't answer the handshake on " + pipe;
				close();
				return false;
			}
			const auto reply = wait(timeout_ms - elapsed);
			if (!reply)
			{
				if (!problem_.empty())
				{
					error = pipe + " " + problem_;
				}
				else if (cancelled())
				{
					error = "the game is quitting";
				}
				else
				{
					error = is_open() ? "Discord didn't answer the handshake on " + pipe : "Discord closed " + pipe + " during the handshake";
				}
				close();
				return false;
			}
			if (reply->op == discord_rules::op_close)
			{
				const auto code = discord_rules::json_value(reply->json, "code").value_or("?");
				const auto message = discord_rules::json_value(reply->json, "message").value_or("no reason given");
				error = "Discord refused the connection (" + code + ": " + message + ")";
				close();
				return false;
			}
			if (reply->op == discord_rules::op_ping)
			{
				send(discord_rules::op_pong, reply->json);
				continue;
			}
			if (reply->op == discord_rules::op_frame && discord_rules::json_value(reply->json, "evt") == "READY")
			{
				return true;
			}
			if (reply->op == discord_rules::op_frame && discord_rules::json_value(reply->json, "evt") == "ERROR")
			{
				error = "Discord answered the handshake with an error (" + discord_rules::json_value(reply->json, "message").value_or("no message") + ")";
				close();
				return false;
			}
		}
	}

	bool connection::send(const std::uint32_t op, const std::string_view json, const DWORD timeout_ms)
	{
		if (!is_open())
		{
			return false;
		}
		const auto bytes = discord_rules::encode(op, json);
		std::size_t written = 0;
		while (written < bytes.size())
		{
			OVERLAPPED overlapped{};
			overlapped.hEvent = event_;
			ResetEvent(event_);
			DWORD done = 0;
			const BOOL started = WriteFile(pipe_, bytes.data() + written, static_cast<DWORD>(bytes.size() - written), &done, &overlapped);
			if (!finish(pipe_, overlapped, started, done, timeout_ms) || done == 0)
			{
				close();
				return false;
			}
			written += done;
		}
		return true;
	}

	// At most max_read_per_poll bytes a call, so a peer that never stops writing can't hold the caller:
	// the rest waits for the next poll, and the inbox's cap ends it before long.
	connection::poll_result connection::read_available()
	{
		std::size_t taken = 0;
		while (taken < discord_rules::max_read_per_poll)
		{
			DWORD available = 0;
			if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr))
			{
				return poll_result::gone;
			}
			if (available == 0)
			{
				return poll_result::open;
			}
			std::string chunk(std::min<std::size_t>({static_cast<std::size_t>(available), std::size_t{16 * 1024}, discord_rules::max_read_per_poll - taken}), '\0');
			OVERLAPPED overlapped{};
			overlapped.hEvent = event_;
			ResetEvent(event_);
			DWORD done = 0;
			const BOOL started = ReadFile(pipe_, chunk.data(), static_cast<DWORD>(chunk.size()), &done, &overlapped);
			if (!finish(pipe_, overlapped, started, done, 1000))
			{
				return poll_result::gone;
			}
			if (done == 0)
			{
				return poll_result::open;
			}
			taken += done;
			if (!inbox_.take(std::string_view(chunk.data(), done)))
			{
				return poll_result::untrusted;
			}
		}
		return poll_result::open;
	}

	void connection::poll()
	{
		if (!is_open())
		{
			return;
		}
		switch (read_available())
		{
		case poll_result::open:
			break;
		case poll_result::gone:
			close(); // what Discord sent before its end closed (a CLOSE and its reason) is still handed out
			break;
		case poll_result::untrusted:
			problem_ = std::string("sent ") + inbox_.problem() + " - that isn't Discord";
			close();
			break;
		}
	}

	bool connection::receive(std::vector<discord_rules::message>& out)
	{
		poll();
		bool got = false;
		while (auto m = inbox_.pop())
		{
			out.push_back(std::move(*m));
			got = true;
		}
		return is_open() || got;
	}

	std::optional<discord_rules::message> connection::wait(const DWORD timeout_ms)
	{
		const DWORD start = GetTickCount();
		for (;;)
		{
			poll();
			if (auto m = inbox_.pop())
			{
				return m;
			}
			if (!is_open() || GetTickCount() - start >= timeout_ms)
			{
				return std::nullopt;
			}
			if (cancel_ ? WaitForSingleObject(cancel_, 15) == WAIT_OBJECT_0 : (Sleep(15), false))
			{
				return std::nullopt;
			}
		}
	}

	// Closes the pipe; what was read from it stays in the inbox until it's handed out or the next open().
	void connection::close()
	{
		if (pipe_ != INVALID_HANDLE_VALUE)
		{
			CancelIoEx(pipe_, nullptr);
			CloseHandle(pipe_);
			pipe_ = INVALID_HANDLE_VALUE;
		}
	}
}
