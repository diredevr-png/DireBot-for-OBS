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

// Your keyboard, mouse and game pad, for the face avatar's gaming hands
// (0.9.0, gamehands.hpp): its hands on its own keyboard, mouse and pad
// while you play or type.
//
// Kept to as little as the hands need, so they can never give a password
// (or anything you type) away:
//
//   - The keyboard: only THAT a key went down or up - never which one. A
//     key's scan code is used for the moment it's held, only to tell one
//     held key from another (a key held down repeats; two held at once go
//     up one at a time), and is gone once it's up. What's kept is how many
//     keys have gone down since it started and how many are held now.
//   - The mouse: how far it moved (summed), its buttons held, clicks and
//     the wheel.
//   - A game pad (XInput: Xbox pads, and others through Steam or
//     DS4Windows): its sticks, triggers and buttons.
//
// Nothing is written anywhere, logged or sent: it stays in OBS's memory
// while the gaming hands are on, and is let go of when they're off.
//
// On Windows the keyboard and mouse are read with Raw Input on a hidden
// window of its own (RIDEV_INPUTSINK: while a game has the focus too), so
// a game that takes the mouse for itself still moves the avatar's; pads
// with XInput, a few dozen times a second. OBS reads raw input for a
// window of only one part of it at a time, so if another part of OBS
// already reads the keyboard or the mouse that way, that one is left as
// it is and the page says so. Elsewhere none of them can be read (the
// add-on is for Windows; the face avatar's "Show me" pretends).
//
// The counting is plain code over plain numbers (DeviceTally,
// tests/unit_game.cpp); the reading is one thread for all face avatars.

#pragma once

#include <bitset>
#include <cstdint>
#include <memory>
#include <string>

namespace direbot::face {

// A pad's buttons: XInput's own bits.
enum PadButton : uint32_t {
	PAD_UP = 0x0001,
	PAD_DOWN = 0x0002,
	PAD_LEFT = 0x0004,
	PAD_RIGHT = 0x0008,
	PAD_START = 0x0010,
	PAD_BACK = 0x0020,
	PAD_LEFT_STICK = 0x0040, // pressed in
	PAD_RIGHT_STICK = 0x0080,
	PAD_LEFT_BUMPER = 0x0100,
	PAD_RIGHT_BUMPER = 0x0200,
	PAD_A = 0x1000,
	PAD_B = 0x2000,
	PAD_X = 0x4000,
	PAD_Y = 0x8000,
};
constexpr uint32_t PAD_DPAD = PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT;
constexpr uint32_t PAD_FACE = PAD_A | PAD_B | PAD_X | PAD_Y;

struct PadState {
	bool connected = false;
	uint32_t buttons = 0;
	// Each stick past its dead zone, -1 to 1 (y up), and each trigger 0 to
	// 1 past its first touch.
	float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
	float left_trigger = 0, right_trigger = 0;
};
// XInput's numbers as a PadState: each stick's dead zone (XInput's own:
// 7849 the left, 8689 the right) taken off round its middle, the rest
// spread to 1; a trigger's first 30 of 255 taken off.
PadState pad_from_xinput(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, int16_t lx, int16_t ly,
			 int16_t rx, int16_t ry);
// The same as far as the hands go (a stick that moved less than 2% isn't).
bool same_pad(const PadState &a, const PadState &b);

struct DeviceState {
	// Whether each is read here, or why not (in plain words).
	bool keyboard = false, mouse = false, pads = false;
	std::string keyboard_problem, mouse_problem, pads_problem;
	// Keys gone down since it started (not a held key's repeats), and
	// held now.
	uint64_t key_presses = 0;
	int keys_held = 0;
	// The mouse: moved (its own counts, summed: x right, y towards you),
	// clicks, its buttons held (1 left, 2 right, 4 middle, 8 and 16 its
	// side buttons) and the wheel (notches, summed: + away from you).
	double mouse_x = 0, mouse_y = 0;
	uint64_t clicks = 0;
	uint32_t mouse_buttons = 0;
	double wheel = 0;
	// Pads plugged in, and the one used last.
	int pads_in = 0;
	PadState pad;
	// When each was last used (DeviceReader::clock()'s seconds; -1 not yet).
	double key_at = -1, mouse_at = -1, pad_at = -1;
};

// What's been done on them, from each thing that happens (the reader's
// thread; tests).
class DeviceTally {
public:
	// Held keys are told apart by `slot` (0 to 511: its scan code, and
	// 256 more for one with Windows' E0 prefix) only while they're held.
	static constexpr int SLOTS = 512;
	void key(int slot, bool down, double now);
	// Every held key up at once (the PC locked, or a key went up where it
	// couldn't be seen).
	void keys_up();
	bool held(int slot) const { return slot >= 0 && slot < SLOTS && held_[(size_t)slot]; }
	void mouse_move(double dx, double dy, double now);
	// Button 0 left, 1 right, 2 middle, 3 and 4 its side ones.
	void mouse_button(int which, bool down, double now);
	void wheel(double notches, double now);
	// The pad used last (`now`: when it changed, as far as the hands go)
	// and how many are plugged in.
	void pad(const PadState &p, int plugged, double now);
	void set_reading(bool keyboard, bool mouse, bool pads, const std::string &keyboard_problem,
			 const std::string &mouse_problem, const std::string &pads_problem);
	const DeviceState &state() const { return state_; }

private:
	DeviceState state_;
	std::bitset<SLOTS> held_;
	PadState used_; // the pad as it was when it was last used
};

// Reads them for every face avatar with gaming hands on, on a thread of
// its own: opened the first time it's asked for, shut once nobody holds
// it.
class DeviceReader {
public:
	static std::shared_ptr<DeviceReader> open();
	DeviceReader();
	~DeviceReader();
	DeviceReader(const DeviceReader &) = delete;
	DeviceReader &operator=(const DeviceReader &) = delete;

	DeviceState state() const;
	// Its clock (seconds).
	static double clock();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace direbot::face
