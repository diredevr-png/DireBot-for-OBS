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

// The mouth from your mic: the face avatar listens to your mic in OBS and
// finds the vowel you're saying - A, I, U, E or O, the five mouth shapes
// every VRM has - and how loud, so its mouth makes the right shape as you
// talk. Webcam mouths look muted (the most common complaint in every
// tracking app); Warudo, VSeeFace, VMagicMirror and VTube Studio all fix it
// this way, the camera doing smiles, brows and eyes and the mic the
// talking.
//
// How (our own code; uLipSync's steps - MIT - were the recipe for the
// first ones): the mic's loudest channel is filtered and brought down to
// about 12,000 samples a second, and every 10 ms the last 30 ms are
// looked at:
//
//   - how loud it is (on the whole sound, as OBS's meter has it), against
//     where the mouth starts to open and where it's open all the way - a
//     noise gate below that;
//   - whether it's a voice at all: a voice repeats itself (its pitch)
//     and most of it is low - an "s" or a keyboard's click is neither;
//   - where its two lowest resonances are (its "formants", from linear
//     prediction): the first is how open the mouth is (A high, I and U
//     low), the second how far forward the tongue is (I high, U and O
//     low). Each vowel sits in its own place, the same for most voices -
//     the starting places are the averages of men's and women's voices
//     measured by Hillenbrand et al. (1995) - and "say each vowel" moves
//     them to yours.
//
// A sound that's loud but not a voice ("s", "t", "f") opens the mouth a
// little, teeth together. Then it's all smoothed so it reads as talking,
// not twitching, and held a moment between words.
//
// Plain code over plain numbers (tests/unit_voice.cpp feeds it made-up
// vowels).

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "tracker.hpp"

namespace direbot::face {

enum Vowel { VOWEL_A, VOWEL_I, VOWEL_U, VOWEL_E, VOWEL_O, VOWELS };
// "a", "i", "u", "e", "o" (the settings, FaceGetState), and VRM's names
// for their expressions ("aa", "ih", "ou", "ee", "oh").
extern const char *const VOWEL_NAMES[VOWELS];
extern const char *const VOWEL_VRM[VOWELS];

// Where each vowel is in a voice: its first and second formants (Hz).
struct VoiceProfile {
	float f1[VOWELS] = {848, 387, 417, 578, 620};
	float f2[VOWELS] = {1438, 2532, 1050, 2112, 1018};
	bool own = false; // from "say each vowel"
};
// A profile as text ({"a": [f1, f2], ...}) and back; false (and `p` as it
// was) if the text isn't one, or any of its places isn't where a voice's
// vowel can be (sensible_place).
bool sensible_place(float f1, float f2);
std::string profile_text(const VoiceProfile &p);
bool profile_from(const std::string &text, VoiceProfile &p);

struct VoiceOptions {
	// Quieter than `start_db` (dBFS, as the mic's level): its mouth stays
	// shut - the gate. From there it opens, all the way at `full_db`.
	float start_db = -50, full_db = -30;
	// How wide it opens: 1 as heard, up to 2.
	float amount = 1;
	VoiceProfile profile;
};

// One look at the sound (every 10 ms), for tests and for Voice.
struct Heard {
	float level_db = -100; // loudness (RMS, dBFS)
	bool voiced = false;   // a voice (not a hiss, a click or quiet)
	float f1 = 0, f2 = 0;  // its formants (Hz), 0 when not found
	// How each vowel matches it (adding up to 1), when f1 and f2 are found.
	float match[VOWELS] = {};
	bool matched = false;
};
// How each vowel matches formants `f1`, `f2` in profile `p`, into `out`.
void vowel_match(float f1, float f2, const VoiceProfile &p, float out[VOWELS]);

// What the mouth does from the mic, frame by frame.
struct VoiceOut {
	float level_db = -100; // how loud now
	float peak_db = -100;  // the loudest lately (falling back slowly), for a level bar
	bool heard = false;    // past where the mouth starts to open
	bool voiced = false;
	float talking = 0;     // 0 quiet to 1 talking (held a moment between words)
	float open = 0;        // how open its mouth is, 0 to 1
	float vowel[VOWELS] = {}; // each vowel's strength (its share times how open)
	float f1 = 0, f2 = 0;  // the voice's formants lately (Hz), 0 none
	bool hearing = false;  // sound has come in lately (the mic's there)
	bool muted = false;    // the mic's muted in OBS
};

// "Say each vowel": a second to get ready, then a second and a half of
// listening, for each of A, I, U, E and O in turn.
struct Calibration {
	static constexpr float READY = 1.0f, LISTEN = 1.5f;
	bool running = false;
	int vowel = 0;      // the one being said now (0 to VOWELS - 1)
	bool listening = false;
	float left = 0;     // seconds left of this part
	// Done: what it heard of each (false: it didn't hear it, so that one
	// keeps its place), and the profile made (`own` if it heard 3 or more).
	bool done = false;
	bool heard[VOWELS] = {};
	VoiceProfile result;
};

// Safe on any thread: feed() on OBS's audio thread, step() on the
// graphics one, the rest from the UI thread.
class Voice {
public:
	Voice();
	void set(const VoiceOptions &o);
	// Only the gate, the opening and the amount: a voice's own places
	// (from "say each vowel") stay as they are.
	void set_levels(const VoiceOptions &o);
	VoiceOptions options() const;

	// The mic's sound, as OBS has it (each channel's own floats, `frames`
	// of them, `rate` a second) - on OBS's audio thread - times `gain`
	// (its volume slider, so it's as loud as OBS's meter shows). Muted:
	// silence.
	void feed(const float *const *planes, int channels, size_t frames, uint32_t rate, bool muted,
		  float gain = 1);
	// `dt` seconds on, on the graphics thread: what the mouth does now.
	VoiceOut step(float dt);
	// No mic: quiet from now on.
	void forget();

	// "Say each vowel".
	void calibrate();
	void stop_calibrating();
	Calibration calibration() const;
	// A finished calibration's result, once (then forgotten).
	bool take_calibrated(VoiceProfile &out);

	// The looks since the last step (for tests: feed, then look()).
	std::vector<Heard> look();

private:
	void analyse();
	std::vector<Heard> take_heard();

	// The audio thread's side.
	mutable std::mutex lock_;
	uint32_t rate_ = 0;
	int down_ = 4;           // samples kept: one in `down_`
	float low_[3][5] = {};   // the low-pass filter: 3 biquads, b0 b1 b2 a1 a2
	float low_z_[3][2] = {}; // their memory
	int phase_ = 0;
	std::vector<float> ring_; // the last window, brought down
	size_t ring_at_ = 0, since_hop_ = 0, have_ = 0;
	float hop_energy_[3] = {}; // the whole sound's energy, each of the last 3 hops
	float hop_low_[3] = {};    // the brought-down sound's, the same
	double energy_now_ = 0, low_now_ = 0;
	size_t raw_now_ = 0;
	int hop_slot_ = 0;
	std::vector<Heard> heard_;
	bool muted_ = false;
	float since_sound_ = 1e9f; // seconds since sound came in (graphics thread)
	bool fed_ = false;

	// The graphics thread's side (under `state_lock_`, taken before
	// `lock_` when both are).
	mutable std::mutex state_lock_;
	VoiceOptions options_;
	VoiceOut out_;
	float share_[VOWELS] = {1, 0, 0, 0, 0};
	float open_target_ = 0;
	float quiet_for_ = 1e9f;
	Heard last_;
	Calibration cal_;
	std::vector<float> cal_f1_, cal_f2_;
	bool cal_ready_ = false;
};

// The mouth from the mic as the face avatar mixes it in: each vowel's
// strength, and how much the mic has the mouth (0 the camera's, 1 the
// mic's) - "both": the mic while you talk, the camera between.
struct MicMouth {
	float vowel[VOWELS] = {};
	float share = 0;
};
// Your 52 expressions' mouth (jaw open, round lips, wide lips) taken over
// by the mic's vowels, as much as `m.share` - for a character whose own
// face shapes are named as Apple's ARKit names them, and a picture
// character's drawn mouth.
void mic_into_moves(const MicMouth &m, float moves[EXPRESSIONS]);

} // namespace direbot::face
