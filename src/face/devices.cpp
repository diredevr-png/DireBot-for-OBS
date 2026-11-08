/*
DireBot for OBS
Copyright (C) 2026 DireBot

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "devices.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace direbot::face {

namespace {

// A stick past its dead zone, round its middle (XInput's advice), the rest
// spread to 1.
void radial(int16_t x, int16_t y, float dead, float &ox, float &oy)
{
	const float fx = (float)x, fy = (float)y;
	const float m = std::sqrt(fx * fx + fy * fy);
	if (m <= dead) {
		ox = oy = 0;
		return;
	}
	const float k = std::min(1.0f, (m - dead) / (32767.0f - dead)) / m;
	ox = std::max(-1.0f, std::min(1.0f, fx * k));
	oy = std::max(-1.0f, std::min(1.0f, fy * k));
}

float trigger(uint8_t v)
{
	return v <= 30 ? 0.0f : (float)(v - 30) / 225.0f;
}

constexpr uint32_t PAD_KNOWN = PAD_DPAD | PAD_START | PAD_BACK | PAD_LEFT_STICK | PAD_RIGHT_STICK |
			       PAD_LEFT_BUMPER | PAD_RIGHT_BUMPER | PAD_FACE;

} // namespace

PadState pad_from_xinput(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t lx, int16_t ly,
			 int16_t rx, int16_t ry)
{
	PadState p;
	p.connected = true;
	p.buttons = buttons & PAD_KNOWN;
	radial(lx, ly, 7849, p.left_x, p.left_y);
	radial(rx, ry, 8689, p.right_x, p.right_y);
	p.left_trigger = trigger(left_trigger);
	p.right_trigger = trigger(right_trigger);
	return p;
}

bool same_pad(const PadState &a, const PadState &b)
{
	constexpr float CLOSE = 0.02f;
	return a.connected == b.connected && a.buttons == b.buttons && std::fabs(a.left_x - b.left_x) < CLOSE &&
	       std::fabs(a.left_y - b.left_y) < CLOSE && std::fabs(a.right_x - b.right_x) < CLOSE &&
	       std::fabs(a.right_y - b.right_y) < CLOSE && std::fabs(a.left_trigger - b.left_trigger) < CLOSE &&
	       std::fabs(a.right_trigger - b.right_trigger) < CLOSE;
}

// ---- What's been done --------------------------------------------------------

void DeviceTally::key(int slot, bool down, double now)
{
	if (slot < 0 || slot >= SLOTS)
		return;
	const size_t s = (size_t)slot;
	if (down) {
		if (held_[s])
			return; // a held key repeating
		held_[s] = true;
		state_.key_presses++;
		state_.keys_held++;
	} else {
		if (!held_[s])
			return;
		held_[s] = false;
		state_.keys_held = std::max(0, state_.keys_held - 1);
	}
	state_.key_at = now;
}

void DeviceTally::keys_up()
{
	held_.reset();
	state_.keys_held = 0;
}

void DeviceTally::mouse_move(double dx, double dy, double now)
{
	if (!std::isfinite(dx) || !std::isfinite(dy) || (dx == 0 && dy == 0))
		return;
	state_.mouse_x += dx;
	state_.mouse_y += dy;
	state_.mouse_at = now;
}

void DeviceTally::mouse_button(int which, bool down, double now)
{
	if (which < 0 || which > 4)
		return;
	const uint32_t bit = 1u << which;
	if (down) {
		if (!(state_.mouse_buttons & bit))
			state_.clicks++;
		state_.mouse_buttons |= bit;
	} else {
		state_.mouse_buttons &= ~bit;
	}
	state_.mouse_at = now;
}

void DeviceTally::wheel(double notches, double now)
{
	if (!std::isfinite(notches) || notches == 0)
		return;
	state_.wheel += notches;
	state_.mouse_at = now;
}

void DeviceTally::pad(const PadState &p, int plugged, double now)
{
	state_.pads_in = std::max(0, plugged);
	// Used: it moved since it was last used (a stick moved slowly still
	// counts, once it's gone far enough).
	if (p.connected && !same_pad(used_, p)) {
		used_ = p;
		state_.pad_at = now;
	}
	if (!p.connected)
		used_ = PadState();
	state_.pad = p;
}

void DeviceTally::set_reading(bool keyboard, bool mouse, bool pads, const std::string &keyboard_problem,
			      const std::string &mouse_problem, const std::string &pads_problem)
{
	state_.keyboard = keyboard;
	state_.mouse = mouse;
	state_.pads = pads;
	state_.keyboard_problem = keyboard_problem;
	state_.mouse_problem = mouse_problem;
	state_.pads_problem = pads_problem;
}

// ---- The reader ----------------------------------------------------------------

double DeviceReader::clock()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

namespace {
std::mutex readers_lock;
std::weak_ptr<DeviceReader> reader_now;
} // namespace

std::shared_ptr<DeviceReader> DeviceReader::open()
{
	std::lock_guard<std::mutex> l(readers_lock);
	if (auto r = reader_now.lock())
		return r;
	auto r = std::make_shared<DeviceReader>();
	reader_now = r;
	return r;
}

#ifdef _WIN32

namespace {

// XInput's state, as xinput.h has it (loaded from Windows' own DLL when
// it's there: the add-on doesn't need it to load).
struct XPad {
	WORD buttons;
	BYTE left_trigger, right_trigger;
	SHORT lx, ly, rx, ry;
};
struct XState {
	DWORD packet;
	XPad pad;
};
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XState *);

const wchar_t *const WINDOW_CLASS = L"DireBotForObsDevices";

// A held key is checked as still down this often (seconds), and let go
// of once Windows says it's up and it hasn't repeated for HELD_QUIET (a
// key held in a game repeats every 30 ms or so).
constexpr double CHECK_KEYS = 0.25, HELD_QUIET = 0.6;
// A pad plugged in is read this often; a slot with none, this often (asking
// an empty one is slow).
constexpr double PAD_EVERY = 1.0 / 60, EMPTY_EVERY = 2.0;

} // namespace

struct DeviceReader::Impl {
	std::thread thread;
	std::atomic<bool> stop{false};
	mutable std::mutex lock;
	DeviceTally tally; // under lock
	HWND window = nullptr;
	HINSTANCE instance = nullptr;
	bool own[2] = {false, false}; // registered: the keyboard, the mouse
	// Each held key's virtual key and when it last went down or repeated,
	// only to see it's still down (the reader's thread only; cleared as it
	// goes up).
	uint16_t vk[DeviceTally::SLOTS] = {};
	double seen[DeviceTally::SLOTS] = {};
	// An absolute mouse's last place (a remote desktop, a tablet).
	bool abs_known = false;
	double abs_x = 0, abs_y = 0;
	HMODULE xinput = nullptr;
	XInputGetStateFn get_state = nullptr;
	struct Slot {
		bool in = false;
		double next = 0;
		DWORD packet = 0;
		PadState pad;
		double used = -1;
	} pads[4];

	void run();
	void register_raw();
	void unregister_raw();
	void on_input(HRAWINPUT h);
	void poll_pads(double now);
	void check_keys(double now);
	static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

LRESULT CALLBACK DeviceReader::Impl::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_INPUT) {
		auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (self)
			self->on_input(reinterpret_cast<HRAWINPUT>(lp));
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

void DeviceReader::Impl::on_input(HRAWINPUT h)
{
	alignas(8) BYTE buffer[sizeof(RAWINPUT) + 64];
	UINT size = sizeof(buffer);
	const UINT got = GetRawInputData(h, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));
	if (got == (UINT)-1 || got < sizeof(RAWINPUTHEADER))
		return;
	const RAWINPUT *ri = reinterpret_cast<const RAWINPUT *>(buffer);
	const double now = DeviceReader::clock();
	if (ri->header.dwType == RIM_TYPEKEYBOARD && got >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) {
		const RAWKEYBOARD &k = ri->data.keyboard;
		// 255: a made-up key Windows sends as part of another's (Num Lock's
		// shifts, Pause's prefix).
		if (k.VKey == 0xFF)
			return;
		const int slot = (k.MakeCode & 0xFF) | ((k.Flags & RI_KEY_E0) ? 0x100 : 0) |
				 ((k.Flags & RI_KEY_E1) ? 0x1FF : 0);
		const bool down = !(k.Flags & RI_KEY_BREAK);
		const size_t s = (size_t)std::min(slot, DeviceTally::SLOTS - 1);
		vk[s] = down ? (uint16_t)k.VKey : (uint16_t)0;
		seen[s] = now;
		std::lock_guard<std::mutex> l(lock);
		tally.key((int)s, down, now);
	} else if (ri->header.dwType == RIM_TYPEMOUSE && got >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE)) {
		const RAWMOUSE &m = ri->data.mouse;
		double dx = 0, dy = 0;
		if (m.usFlags & MOUSE_MOVE_ABSOLUTE) {
			// 0 to 65535 across the screen (or all of them): in pixels, and
			// how far since the last.
			const bool all = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
			const double w = GetSystemMetrics(all ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
			const double hgt = GetSystemMetrics(all ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
			const double x = m.lLastX / 65535.0 * w, y = m.lLastY / 65535.0 * hgt;
			if (abs_known) {
				dx = x - abs_x;
				dy = y - abs_y;
			}
			abs_x = x;
			abs_y = y;
			abs_known = true;
		} else {
			dx = m.lLastX;
			dy = m.lLastY;
		}
		const USHORT f = m.usButtonFlags;
		std::lock_guard<std::mutex> l(lock);
		tally.mouse_move(dx, dy, now);
		static const USHORT DOWN[5] = {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_DOWN,
					       RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_BUTTON_4_DOWN,
					       RI_MOUSE_BUTTON_5_DOWN};
		static const USHORT UP[5] = {RI_MOUSE_LEFT_BUTTON_UP, RI_MOUSE_RIGHT_BUTTON_UP, RI_MOUSE_MIDDLE_BUTTON_UP,
					     RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
		for (int b = 0; b < 5; b++) {
			if (f & DOWN[b])
				tally.mouse_button(b, true, now);
			if (f & UP[b])
				tally.mouse_button(b, false, now);
		}
		if (f & RI_MOUSE_WHEEL)
			tally.wheel((SHORT)m.usButtonData / 120.0, now);
	}
}

// Raw Input for the keyboard and the mouse, unless another part of OBS
// reads them that way already (a process has one window for each; taking
// it would stop that one).
void DeviceReader::Impl::register_raw()
{
	bool taken[2] = {false, false};
	UINT count = 0;
	GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
	if (count > 0) {
		std::vector<RAWINPUTDEVICE> list(count);
		if (GetRegisteredRawInputDevices(list.data(), &count, sizeof(RAWINPUTDEVICE)) != (UINT)-1)
			for (UINT i = 0; i < count; i++) {
				const RAWINPUTDEVICE &d = list[i];
				if (d.usUsagePage == 1 && d.usUsage == 6 && d.hwndTarget != window)
					taken[0] = true;
				if (d.usUsagePage == 1 && d.usUsage == 2 && d.hwndTarget != window)
					taken[1] = true;
			}
	}
	std::string problem[2];
	const USHORT usage[2] = {6, 2};
	for (int i = 0; i < 2; i++) {
		if (taken[i]) {
			problem[i] = i == 0 ? "Another part of OBS reads the keyboard already, so it's left to that."
					    : "Another part of OBS reads the mouse already, so it's left to that.";
			continue;
		}
		RAWINPUTDEVICE d{};
		d.usUsagePage = 1;
		d.usUsage = usage[i];
		d.dwFlags = RIDEV_INPUTSINK;
		d.hwndTarget = window;
		if (RegisterRawInputDevices(&d, 1, sizeof(d))) {
			own[i] = true;
		} else {
			problem[i] = std::string("Windows wouldn't let it read the ") + (i == 0 ? "keyboard" : "mouse") +
				     " (error " + std::to_string(GetLastError()) + ").";
		}
	}
	std::lock_guard<std::mutex> l(lock);
	tally.set_reading(own[0], own[1], get_state != nullptr, problem[0], problem[1],
			  get_state ? "" : "Windows' game pad part (XInput) isn't on this PC.");
}

// Let go of them - only if they're still ours.
void DeviceReader::Impl::unregister_raw()
{
	UINT count = 0;
	GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
	std::vector<RAWINPUTDEVICE> list(count);
	if (count && GetRegisteredRawInputDevices(list.data(), &count, sizeof(RAWINPUTDEVICE)) == (UINT)-1)
		count = 0;
	const USHORT usage[2] = {6, 2};
	for (int i = 0; i < 2; i++) {
		if (!own[i])
			continue;
		bool ours = false;
		for (UINT k = 0; k < count; k++)
			if (list[k].usUsagePage == 1 && list[k].usUsage == usage[i] && list[k].hwndTarget == window)
				ours = true;
		if (!ours)
			continue;
		RAWINPUTDEVICE d{};
		d.usUsagePage = 1;
		d.usUsage = usage[i];
		d.dwFlags = RIDEV_REMOVE;
		d.hwndTarget = nullptr;
		RegisterRawInputDevices(&d, 1, sizeof(d));
		own[i] = false;
	}
}

void DeviceReader::Impl::poll_pads(double now)
{
	if (!get_state)
		return;
	int plugged = 0, newest = -1;
	for (int i = 0; i < 4; i++) {
		Slot &s = pads[i];
		if (!s.in && now < s.next)
			continue;
		XState x{};
		const DWORD r = get_state((DWORD)i, &x);
		if (r != ERROR_SUCCESS) {
			s = Slot();
			s.next = now + EMPTY_EVERY;
			continue;
		}
		const bool was_in = s.in;
		s.in = true;
		const PadState p = pad_from_xinput(x.pad.buttons, x.pad.left_trigger, x.pad.right_trigger, x.pad.lx,
						   x.pad.ly, x.pad.rx, x.pad.ry);
		if (!was_in || !same_pad(s.pad, p)) {
			if (was_in)
				s.used = now;
			s.pad = p;
		} else {
			s.pad = p;
		}
		s.packet = x.packet;
	}
	for (int i = 0; i < 4; i++)
		if (pads[i].in) {
			plugged++;
			if (newest < 0 || pads[i].used > pads[newest].used)
				newest = i;
		}
	std::lock_guard<std::mutex> l(lock);
	tally.pad(newest >= 0 ? pads[newest].pad : PadState(), plugged, now);
}

void DeviceReader::Impl::check_keys(double now)
{
	std::vector<int> up;
	for (int s = 0; s < DeviceTally::SLOTS; s++) {
		if (!vk[s] || now - seen[s] < HELD_QUIET)
			continue;
		if (!(GetAsyncKeyState(vk[s]) & 0x8000)) {
			vk[s] = 0;
			up.push_back(s);
		}
	}
	if (up.empty())
		return;
	std::lock_guard<std::mutex> l(lock);
	for (int s : up)
		tally.key(s, false, now);
}

void DeviceReader::Impl::run()
{
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&DeviceReader::Impl::proc), &instance);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = proc;
	wc.hInstance = instance;
	wc.lpszClassName = WINDOW_CLASS;
	RegisterClassExW(&wc); // (already there from before: fine)
	window = CreateWindowExW(0, WINDOW_CLASS, L"DireBot for OBS devices", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
				 instance, nullptr);
	if (!window) {
		std::lock_guard<std::mutex> l(lock);
		const std::string why = "Windows wouldn't make the hidden window it reads them with (error " +
					std::to_string(GetLastError()) + ").";
		tally.set_reading(false, false, false, why, why, why);
		return;
	}
	SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	for (const wchar_t *dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
		xinput = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (xinput) {
			get_state = reinterpret_cast<XInputGetStateFn>(
				reinterpret_cast<void *>(GetProcAddress(xinput, "XInputGetState")));
			if (get_state)
				break;
			FreeLibrary(xinput);
			xinput = nullptr;
		}
	}
	register_raw();
	double next_pads = 0, next_check = 0;
	while (!stop) {
		MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
		const double now = DeviceReader::clock();
		if (now >= next_pads) {
			poll_pads(now);
			next_pads = now + PAD_EVERY;
		}
		if (now >= next_check) {
			check_keys(now);
			next_check = now + CHECK_KEYS;
		}
	}
	unregister_raw();
	SetWindowLongPtrW(window, GWLP_USERDATA, 0);
	DestroyWindow(window);
	window = nullptr;
	UnregisterClassW(WINDOW_CLASS, instance);
	if (xinput)
		FreeLibrary(xinput);
	xinput = nullptr;
	get_state = nullptr;
	std::fill(std::begin(vk), std::end(vk), 0);
	std::lock_guard<std::mutex> l(lock);
	tally.keys_up();
}

DeviceReader::DeviceReader() : impl_(std::make_unique<Impl>())
{
	impl_->thread = std::thread([this] { impl_->run(); });
}

DeviceReader::~DeviceReader()
{
	impl_->stop = true;
	if (impl_->thread.joinable())
		impl_->thread.join();
}

DeviceState DeviceReader::state() const
{
	std::lock_guard<std::mutex> l(impl_->lock);
	return impl_->tally.state();
}

#else // not Windows: none of them can be read here

struct DeviceReader::Impl {
	DeviceTally tally;
};

DeviceReader::DeviceReader() : impl_(std::make_unique<Impl>())
{
	const std::string why = "Only on Windows.";
	impl_->tally.set_reading(false, false, false, why, why, why);
}

DeviceReader::~DeviceReader() = default;

DeviceState DeviceReader::state() const
{
	return impl_->tally.state();
}

#endif

} // namespace direbot::face
