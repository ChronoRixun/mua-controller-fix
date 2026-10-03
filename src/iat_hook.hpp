#pragma once

#include <Windows.h>

namespace iat_hook
{
	// Points `module`'s import of `function` (by name, or by `ordinal` when non-zero) from `dll`
	// at `replacement`. Returns the previous target, or nullptr if the module doesn't import it.
	void* hook(HMODULE module, const char* dll, const char* function, WORD ordinal, void* replacement);
}
