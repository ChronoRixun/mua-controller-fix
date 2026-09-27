#pragma once

#include <cstdint>
#include <vector>

namespace xinput_pad
{
	constexpr int max_pads = 4;

	struct raw_state
	{
		std::uint16_t buttons; // XINPUT_GAMEPAD_* bits
		std::uint8_t left_trigger, right_trigger;
		std::int16_t left_x, left_y, right_x, right_y;
	};

	// Unprocessed XInput state (no deadzones); false if the pad is not connected.
	bool read_raw(int index, raw_state& out);

	// Indices of the currently connected XInput pads, in slot order.
	std::vector<int> connected_indices();
}
