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

// Your body and hands in the camera's picture, for the 3D avatar's arms
// and fingers: 33 points on your body (shoulders, elbows, wrists...) and
// 21 on each hand, both in the picture and in 3D.
//
// Google's MediaPipe body and hand models do the seeing (data/face,
// Apache 2.0), run by our own engine (nn/net.hpp), with the steps around
// them done as MediaPipe's Holistic Landmarker does them, so the answers
// are theirs:
//
//   1. Find a body: the whole picture, shrunk to 224 x 224, through the
//      body finder; it follows the surest one.
//   2. Its 33 points: a square round the body (from the finder's points
//      at its hips and above its head), turned upright, cut out at 256 x
//      256, through the body model - which also says how sure it is a body
//      is there, how likely each point is to be in sight, and the points
//      in metres round your hips. A sharper guess at each point from the
//      model's heat map.
//   3. Next look: the square comes from where the body was (its two extra
//      points, smoothed), so the finder only runs again once it's lost.
//   4. Each hand: a square round it from the body's wrist, first finger
//      and little finger, made better by the hand-square model; or, if
//      the hand was found last look and hasn't moved far, a square round
//      where it was. Cut out at 224 x 224, through the hand model: 21
//      points on it in the picture and in metres, and how sure it is a
//      hand is there.
//   5. Smoothing (a One Euro filter on each point, as the face's): steady
//      while you're still, straight there when you move. How much is a
//      slider.
//
// Then our own checks, so a hand that isn't yours (a "ghost" hand) never
// moves the character: a hand is only looked for where your wrist is in
// the picture, the hand model must be sure, its wrist must be where your
// body's wrist is, and one hand found twice (your hands together) counts
// once.
//
// Plain code over plain numbers, so it's tested without OBS
// (tests/unit_body.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "tracker.hpp"

namespace direbot::face {

constexpr int BODY_POINTS = 33;
constexpr int HAND_POINTS = 21;

// Some of the body's points by number (MediaPipe's). "Left" and "right"
// are the person's own: their left side is on the picture's right in a
// camera that doesn't mirror.
enum BodyPointId {
	BODY_NOSE = 0,
	BODY_LEFT_EAR = 7,
	BODY_RIGHT_EAR = 8,
	BODY_LEFT_SHOULDER = 11,
	BODY_RIGHT_SHOULDER = 12,
	BODY_LEFT_ELBOW = 13,
	BODY_RIGHT_ELBOW = 14,
	BODY_LEFT_WRIST = 15,
	BODY_RIGHT_WRIST = 16,
	BODY_LEFT_PINKY = 17,
	BODY_RIGHT_PINKY = 18,
	BODY_LEFT_INDEX = 19,
	BODY_RIGHT_INDEX = 20,
	BODY_LEFT_THUMB = 21,
	BODY_RIGHT_THUMB = 22,
	BODY_LEFT_HIP = 23,
	BODY_RIGHT_HIP = 24,
};

// A hand's points (MediaPipe's): the wrist, then each digit from its
// root to its tip - thumb 1-4, first finger 5-8, middle 9-12, ring
// 13-16, little 17-20.
enum HandPointId {
	HAND_WRIST = 0,
	HAND_THUMB = 1,
	HAND_INDEX = 5,
	HAND_MIDDLE = 9,
	HAND_RING = 13,
	HAND_LITTLE = 17,
};

// Why a hand the hand model saw was turned down (or NotLooked: it
// wasn't looked for).
enum class HandGate {
	Ok,
	NotLooked, // no body, the wrist out of the picture or out of sight, or hands off
	Unsure,    // the hand model isn't sure a hand is there
	Elsewhere, // its wrist isn't where your body's wrist is
	Twice,     // the other hand's: your hands together, one hand found twice
};

struct HandSeen {
	bool found = false;
	HandGate gate = HandGate::NotLooked;
	float presence = 0;   // how sure the hand model is (0 to 1)
	float handedness = 0; // the model's own "a right hand, mirrored" (0 to 1)
	// In the picture's pixels (z: how far from the camera against the
	// wrist, on x's scale).
	Point points[HAND_POINTS];
	// In metres, the camera's way (x right, y down, z away from it), the
	// wrist at the body's wrist (Body::world).
	Point world[HAND_POINTS];
	// The square it was cut from (picture pixels, radians).
	float box[5] = {};
	bool tracked = false; // the square came from where it was last look
};

struct Body {
	bool found = false;
	int w = 0, h = 0; // the picture's size (the points are in its pixels)
	float presence = 0;
	// In the picture's pixels (0, 0 the top left pixel's middle); z: how
	// far from the camera against the hips, on x's scale.
	Point points[BODY_POINTS];
	// How likely each point is to be in sight in the picture (0 to 1).
	float seen[BODY_POINTS] = {};
	// In metres round the middle of the hips, the camera's way (x right,
	// y down, z away from it).
	Point world[BODY_POINTS];
	// The person's own left hand [0] and right [1].
	HandSeen hands[2];
	bool searched = false; // this look ran the body finder
	float ms = 0;          // how long the look took (milliseconds)
};

class BodyTracker {
public:
	BodyTracker();
	~BodyTracker();
	BodyTracker(const BodyTracker &) = delete;
	BodyTracker &operator=(const BodyTracker &) = delete;

	// Google's holistic_landmarker.task (its body finder, body model,
	// hand-square model and hand model), and - for light mode - a lighter
	// hand model (hand_landmark_lite.tflite; none: light mode uses the
	// full one). From memory or files. False, with why, if it can't.
	bool load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size,
		  std::string *problem = nullptr);
	bool load_files(const std::string &task_path, const std::string &lite_path, std::string *problem = nullptr);
	bool ready() const;

	// Looks at a new picture. `seconds`: when the camera took it (any
	// clock that only goes forward), for the smoothing.
	const Body &look(const Frame &picture, double seconds);
	const Body &body() const;

	// The next look starts with the body finder. Hands too.
	void forget();

	// Hands: looked for (else only the body). Light: the lighter hand model.
	void set_hands(bool on);
	void set_light(bool on);
	bool light() const;
	// Smoothing: 0 the steadiest to 1 the snappiest; 0.5 is MediaPipe's
	// own. Below 0: none at all (for tests).
	void set_smoothing(float amount);

	// For tests: the models step by step on the plain C++ code (set
	// before load), and the body finder's best find on the last look
	// that ran it (picture pixels: its box, then its 4 points).
	void set_plain(bool plain);
	struct Found {
		bool any = false;
		float x = 0, y = 0, w = 0, h = 0, score = 0;
		float keys[8] = {};
	};
	const Found &found() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace direbot::face
