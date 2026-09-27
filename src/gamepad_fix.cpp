#include "gamepad_fix.hpp"
#include "log.hpp"
#include "xinput_pad.hpp"

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <dinput.h>
#include <Xinput.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gamepad_fix
{
	namespace
	{
		constexpr std::uint16_t microsoft_vid = 0x045E;
		constexpr std::uint16_t xbox360_wired_pid = 0x028E;

		// Xbox pads the games already map correctly; left untouched.
		constexpr std::array<std::uint16_t, 4> known_xbox_pids{0x028E, 0x02A1, 0x02D1, 0x02FF};

		// COM vtable slots (identical for the A and W interfaces).
		constexpr int slot_create_device = 3;
		constexpr int slot_enum_devices = 4;
		constexpr int slot_get_property = 5;
		constexpr int slot_get_device_state = 9;
		constexpr int slot_get_device_info = 15;

		// Axis offsets in DIJOYSTATE: X, Y, Z, Rx, Ry, Rz.
		constexpr std::array<DWORD, 6> axis_offsets{0, 4, 8, 12, 16, 20};

		struct axis_setup
		{
			LONG min = 0;
			LONG max = 65535;
			DWORD deadzone = 0; // 0..10000, as DIPROP_DEADZONE
		};

		// Per spoofed DirectInput device.
		struct device_record
		{
			bool spoofed = false;
			int ordinal = -1; // n-th spoofed device -> n-th connected XInput pad
			bool axes_loaded = false;
			std::array<axis_setup, 6> axes{};
		};

		using create_device_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, void**, LPUNKNOWN);
		using enum_devices_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIENUMDEVICESCALLBACKW, LPVOID, DWORD);
		using get_property_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, LPDIPROPHEADER);
		using get_device_info_t = HRESULT(STDMETHODCALLTYPE*)(void*, LPDIDEVICEINSTANCEW);
		using get_device_state_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);

		std::mutex mutex;

		// Original function pointers for each vtable we patched, by slot.
		std::unordered_map<void**, std::unordered_map<int, void*>> originals;

		std::mutex devices_mutex;
		std::unordered_map<void*, device_record> device_records;
		int spoofed_count = 0;

		std::uint16_t vid_of(const GUID& product) { return static_cast<std::uint16_t>(product.Data1 & 0xFFFF); }
		std::uint16_t pid_of(const GUID& product) { return static_cast<std::uint16_t>(product.Data1 >> 16); }

		// VID/PID pairs of XInput-capable HID devices ("IG_" in the device path), found via Raw Input.
		std::set<std::pair<std::uint16_t, std::uint16_t>> xinput_devices()
		{
			std::set<std::pair<std::uint16_t, std::uint16_t>> result;

			UINT count = 0;
			GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
			std::vector<RAWINPUTDEVICELIST> devices(count);
			count = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
			if (count == static_cast<UINT>(-1))
			{
				return result;
			}

			for (UINT i = 0; i < count; ++i)
			{
				if (devices[i].dwType != RIM_TYPEHID)
				{
					continue;
				}

				UINT length = 0;
				GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICENAME, nullptr, &length);
				std::wstring name(length, L'\0');
				if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICENAME, name.data(), &length) == static_cast<UINT>(-1))
				{
					continue;
				}

				std::ranges::transform(name, name.begin(), [](const wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
				if (name.find(L"IG_") == std::wstring::npos)
				{
					continue;
				}

				const auto vid_pos = name.find(L"VID_");
				const auto pid_pos = name.find(L"PID_");
				if (vid_pos != std::wstring::npos && pid_pos != std::wstring::npos)
				{
					const auto vid = static_cast<std::uint16_t>(std::wcstoul(name.substr(vid_pos + 4, 4).c_str(), nullptr, 16));
					const auto pid = static_cast<std::uint16_t>(std::wcstoul(name.substr(pid_pos + 4, 4).c_str(), nullptr, 16));
					result.emplace(vid, pid);
				}
			}

			return result;
		}

		bool should_spoof(const GUID& product)
		{
			const auto vid = vid_of(product);
			const auto pid = pid_of(product);
			if (vid == microsoft_vid && std::ranges::find(known_xbox_pids, pid) != known_xbox_pids.end())
			{
				return false;
			}

			return xinput_devices().contains({vid, pid});
		}

		// Rewrites the product GUID in a DIDEVICEINSTANCE (A or W; the field is at the same offset).
		void spoof_instance(LPDIDEVICEINSTANCEW instance, const char* where)
		{
			if (!instance || !should_spoof(instance->guidProduct))
			{
				return;
			}

			logger::write_once(std::string("spoof:") + where, "dinput: %s reports %04X:%04X -> presenting as wired Xbox 360 pad (045E:028E)",
			                   where, vid_of(instance->guidProduct), pid_of(instance->guidProduct));
			instance->guidProduct.Data1 = (static_cast<DWORD>(xbox360_wired_pid) << 16) | microsoft_vid;
		}

		void* original(void* self, const int slot)
		{
			std::lock_guard lock(mutex);
			return originals[*static_cast<void***>(self)][slot];
		}

		void patch_vtable(void* object, const int slot, void* replacement)
		{
			std::lock_guard lock(mutex);
			auto** vtable = *static_cast<void***>(object);
			auto& saved = originals[vtable];
			if (saved.contains(slot))
			{
				return;
			}

			DWORD old_protect = 0;
			VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old_protect);
			saved[slot] = vtable[slot];
			vtable[slot] = replacement;
			VirtualProtect(&vtable[slot], sizeof(void*), old_protect, &old_protect);
		}

		HRESULT STDMETHODCALLTYPE get_device_info(void* self, LPDIDEVICEINSTANCEW instance)
		{
			const auto result = reinterpret_cast<get_device_info_t>(original(self, slot_get_device_info))(self, instance);
			if (SUCCEEDED(result))
			{
				spoof_instance(instance, "GetDeviceInfo");
			}
			return result;
		}

		HRESULT STDMETHODCALLTYPE get_property(void* self, REFGUID property, LPDIPROPHEADER header)
		{
			const auto result = reinterpret_cast<get_property_t>(original(self, slot_get_property))(self, property, header);
			if (FAILED(result) || &property != &DIPROP_VIDPID)
			{
				return result;
			}

			auto* value = reinterpret_cast<LPDIPROPDWORD>(header);
			GUID product{};
			product.Data1 = value->dwData;
			if (should_spoof(product))
			{
				value->dwData = (static_cast<DWORD>(xbox360_wired_pid) << 16) | microsoft_vid;
			}
			return result;
		}

		// The real device info, whichever of the A or W interfaces `device` is.
		bool real_device_info(void* device, DIDEVICEINSTANCEW& info)
		{
			const auto real_info = reinterpret_cast<get_device_info_t>(original(device, slot_get_device_info));
			for (const DWORD size : {static_cast<DWORD>(sizeof(DIDEVICEINSTANCEW)), static_cast<DWORD>(sizeof(DIDEVICEINSTANCEA))})
			{
				info = {};
				info.dwSize = size;
				if (SUCCEEDED(real_info(device, &info)))
				{
					return true;
				}
			}
			return false;
		}

		device_record& record_for(void* device)
		{
			static std::unordered_map<std::string, int> ordinals; // by instance GUID: one per physical pad

			std::lock_guard lock(devices_mutex);
			const auto [it, inserted] = device_records.try_emplace(device);
			if (inserted)
			{
				DIDEVICEINSTANCEW info{};
				if (real_device_info(device, info) && should_spoof(info.guidProduct))
				{
					const std::string key(reinterpret_cast<const char*>(&info.guidInstance), sizeof(GUID));
					const auto [ordinal, first_time] = ordinals.try_emplace(key, spoofed_count);
					if (first_time)
					{
						++spoofed_count;
						logger::write("dinput: building %04X:%04X state from XInput as wired Xbox 360 pad #%d",
						              vid_of(info.guidProduct), pid_of(info.guidProduct), ordinal->second + 1);
					}
					it->second.spoofed = true;
					it->second.ordinal = ordinal->second;
				}
			}
			return it->second;
		}

		// Reads the ranges and deadzones the game configured, so synthesized values match what DirectInput would report.
		void load_axes(void* device, device_record& record)
		{
			const auto real_get_property = reinterpret_cast<get_property_t>(original(device, slot_get_property));
			for (size_t i = 0; i < axis_offsets.size(); ++i)
			{
				DIPROPRANGE range{{sizeof(DIPROPRANGE), sizeof(DIPROPHEADER), axis_offsets[i], DIPH_BYOFFSET}};
				if (SUCCEEDED(real_get_property(device, DIPROP_RANGE, &range.diph)) && range.lMax > range.lMin)
				{
					record.axes[i].min = range.lMin;
					record.axes[i].max = range.lMax;
				}

				DIPROPDWORD deadzone{{sizeof(DIPROPDWORD), sizeof(DIPROPHEADER), axis_offsets[i], DIPH_BYOFFSET}};
				if (SUCCEEDED(real_get_property(device, DIPROP_DEADZONE, &deadzone.diph)))
				{
					record.axes[i].deadzone = std::min<DWORD>(deadzone.dwData, 10000);
				}
			}
			record.axes_loaded = true;
		}

		// value in [-1, 1] -> the axis range, applying the axis deadzone like DirectInput does.
		LONG to_axis(float value, const axis_setup& axis)
		{
			const float deadzone = axis.deadzone / 10000.0f;
			const float magnitude = std::fabs(value);
			value = magnitude <= deadzone ? 0.0f : std::copysign((magnitude - deadzone) / (1.0f - deadzone), value);
			value = std::clamp(value, -1.0f, 1.0f);

			const double center = (static_cast<double>(axis.min) + axis.max) / 2.0;
			const double half = (static_cast<double>(axis.max) - axis.min) / 2.0;
			return static_cast<LONG>(std::lround(center + value * half));
		}

		float stick(const std::int16_t raw)
		{
			return std::max(-1.0f, raw / 32767.0f);
		}

		DWORD dpad_pov(const std::uint16_t buttons)
		{
			const bool up = buttons & XINPUT_GAMEPAD_DPAD_UP, down = buttons & XINPUT_GAMEPAD_DPAD_DOWN;
			const bool left = buttons & XINPUT_GAMEPAD_DPAD_LEFT, right = buttons & XINPUT_GAMEPAD_DPAD_RIGHT;
			if (up && right) return 4500;
			if (right && down) return 13500;
			if (down && left) return 22500;
			if (left && up) return 31500;
			if (up) return 0;
			if (right) return 9000;
			if (down) return 18000;
			if (left) return 27000;
			return static_cast<DWORD>(-1);
		}

		// Fills DIJOYSTATE exactly as a wired Xbox 360 pad reports it through DirectInput.
		void fill_xbox360_state(DIJOYSTATE& state, const xinput_pad::raw_state& pad, const device_record& record)
		{
			const auto& axes = record.axes;
			state.lX = to_axis(stick(pad.left_x), axes[0]);
			state.lY = to_axis(-stick(pad.left_y), axes[1]); // DirectInput Y grows downwards
			state.lZ = to_axis((pad.left_trigger - pad.right_trigger) / 255.0f, axes[2]); // both triggers share Z
			state.lRx = to_axis(stick(pad.right_x), axes[3]);
			state.lRy = to_axis(-stick(pad.right_y), axes[4]);
			state.lRz = 0;
			state.rglSlider[0] = state.rglSlider[1] = 0;

			state.rgdwPOV[0] = dpad_pov(pad.buttons);
			state.rgdwPOV[1] = state.rgdwPOV[2] = state.rgdwPOV[3] = static_cast<DWORD>(-1);

			constexpr std::array<WORD, 10> button_order{
				XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
				XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
				XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
				XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
			};
			std::memset(state.rgbButtons, 0, sizeof(state.rgbButtons));
			for (size_t i = 0; i < button_order.size(); ++i)
			{
				state.rgbButtons[i] = (pad.buttons & button_order[i]) ? 0x80 : 0;
			}
		}

		HRESULT STDMETHODCALLTYPE get_device_state(void* self, const DWORD size, LPVOID data)
		{
			const auto result = reinterpret_cast<get_device_state_t>(original(self, slot_get_device_state))(self, size, data);
			if (FAILED(result) || !data || (size != sizeof(DIJOYSTATE) && size != sizeof(DIJOYSTATE2)))
			{
				return result;
			}

			auto& record = record_for(self);
			if (!record.spoofed)
			{
				return result;
			}
			if (!record.axes_loaded)
			{
				load_axes(self, record);
			}

			const auto connected = xinput_pad::connected_indices();
			xinput_pad::raw_state pad{};
			if (record.ordinal < 0 || record.ordinal >= static_cast<int>(connected.size()) || !xinput_pad::read_raw(connected[record.ordinal], pad))
			{
				return result;
			}

			fill_xbox360_state(*static_cast<DIJOYSTATE*>(data), pad, record);
			return result;
		}

		// Calls `patch` on both the A and W interfaces of a COM object, so the hooks apply
		// whichever one the game uses (or QueryInterfaces to later).
		template <typename Patch>
		void for_both_interfaces(void* object, const IID& iid_a, const IID& iid_w, Patch patch)
		{
			for (const auto* iid : {&iid_a, &iid_w})
			{
				void* view = nullptr;
				if (SUCCEEDED(static_cast<IUnknown*>(object)->QueryInterface(*iid, &view)) && view)
				{
					patch(view);
					static_cast<IUnknown*>(view)->Release();
				}
			}
		}

		HRESULT STDMETHODCALLTYPE create_device(void* self, REFGUID instance, void** device, LPUNKNOWN outer)
		{
			const auto result = reinterpret_cast<create_device_t>(original(self, slot_create_device))(self, instance, device, outer);
			if (SUCCEEDED(result) && device && *device)
			{
				for_both_interfaces(*device, IID_IDirectInputDevice8A, IID_IDirectInputDevice8W, [](void* view)
				{
					patch_vtable(view, slot_get_device_info, reinterpret_cast<void*>(&get_device_info));
					patch_vtable(view, slot_get_property, reinterpret_cast<void*>(&get_property));
					patch_vtable(view, slot_get_device_state, reinterpret_cast<void*>(&get_device_state));
				});
			}
			return result;
		}

		struct enum_context
		{
			LPDIENUMDEVICESCALLBACKW callback;
			LPVOID user;
		};

		BOOL CALLBACK enum_callback(LPCDIDEVICEINSTANCEW instance, LPVOID ref)
		{
			const auto* context = static_cast<enum_context*>(ref);
			if (!instance || instance->dwSize < offsetof(DIDEVICEINSTANCEW, guidProduct) + sizeof(GUID))
			{
				return context->callback(instance, context->user);
			}

			// DIDEVICEINSTANCEA is smaller than the W version; copy exactly what the caller was given.
			alignas(DIDEVICEINSTANCEW) std::byte copy[sizeof(DIDEVICEINSTANCEW)]{};
			std::memcpy(copy, instance, std::min<size_t>(instance->dwSize, sizeof(copy)));
			auto* modified = reinterpret_cast<LPDIDEVICEINSTANCEW>(copy);

			if (GET_DIDEVICE_TYPE(instance->dwDevType) == DI8DEVTYPE_GAMEPAD || GET_DIDEVICE_TYPE(instance->dwDevType) == DI8DEVTYPE_JOYSTICK)
			{
				logger::write_once(std::string("enum:") + std::to_string(instance->guidProduct.Data1),
				                   "dinput: game sees device %04X:%04X", vid_of(instance->guidProduct), pid_of(instance->guidProduct));
			}

			spoof_instance(modified, "EnumDevices");
			return context->callback(modified, context->user);
		}

		HRESULT STDMETHODCALLTYPE enum_devices(void* self, DWORD type, LPDIENUMDEVICESCALLBACKW callback, LPVOID user, DWORD flags)
		{
			const auto real = reinterpret_cast<enum_devices_t>(original(self, slot_enum_devices));
			if (!callback)
			{
				return real(self, type, callback, user, flags);
			}

			enum_context context{callback, user};
			return real(self, type, &enum_callback, &context, flags);
		}
	}

	void hook_direct_input(void* direct_input)
	{
		for_both_interfaces(direct_input, IID_IDirectInput8A, IID_IDirectInput8W, [](void* view)
		{
			patch_vtable(view, slot_create_device, reinterpret_cast<void*>(&create_device));
			patch_vtable(view, slot_enum_devices, reinterpret_cast<void*>(&enum_devices));
		});
		logger::write("dinput: hooked a DirectInput instance");
	}
}
