#pragma once

// Marvel: Ultimate Alliance (2016) and its sequel read gamepads through DirectInput and
// pick a button layout from a short table of known product IDs. Pads missing from it -
// e.g. an Xbox Wireless Controller over Bluetooth (045E:0B22) - fall back to a generic
// layout with the face buttons scrambled.
//
// The fix presents every XInput-capable pad the game doesn't already recognise as a
// wired Xbox 360 controller (045E:028E), and builds its DirectInput state from XInput in
// that controller's exact layout, so buttons and on-screen prompts line up.

namespace gamepad_fix
{
	// Hooks a freshly created IDirectInput8 (A or W) object and the devices it creates.
	void hook_direct_input(void* direct_input);
}
