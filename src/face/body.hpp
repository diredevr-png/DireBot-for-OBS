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
// them done as MediaPipe does them, so the answers are theirs. The body
// as its Holistic Landmarker has it:
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
//
// Your hands as its Hand Landmarker has them - each hand found on its
// own, not only where the body model thinks your wrist is (in a dim room,
// with an arm raised, the body model often has your wrist somewhere else):
//
//   4. A hand found last look is followed from where it was: a square
//      round its points, cut out at 224 x 224, through the hand model: 21
//      points on it in the picture and in metres, how sure it is a hand is
//      there, and whether it's a left or a right hand.
//   5. A hand not followed is looked for at your wrist (MediaPipe
//      Holistic's square from the body's wrist and fingers, made better by
//      the hand-square model), and - when that doesn't find it - by the
//      palm finder on the whole picture (shrunk to 192 x 192): when a hand
//      has just been lost, and twice a second while one isn't followed.
//   6. Left and right from the hand itself (the hand model's own left or
//      right, over a few looks), checked against your body's wrists when
//      the body model is sure.
//   7. Smoothing (a One Euro filter on each point, as the face's): steady
//      while you're still, straight there when you move. How much is a
//      slider.
//
// The body model can run less often than the hands look (set_body_every):
// hands keep up with quick moves, and the body (where your shoulders and
// elbows are) doesn't need to.
//
// Then our own checks and fixes:
//   - A hand that isn't yours (a "ghost" hand) never counts: the hand
//     model must be sure, a hand found twice counts once, and - when the
//     body model is sure of you - a hand must be within your arm's reach.
//   - The body model sometimes has you turned round (your back to the
//     camera) when you raise both hands: its left and right swapped. You
//     face the camera to stream, so they're swapped back.
//   - Your arm follows your hand: where a hand is found away from where
//     the body model has that wrist, the wrist is put at the hand, and its
//     elbow worked out from your shoulder (it bends the way the body model
//     had it).
//
// Plain code over plain numbers, so it's tested without OBS
// (tests/unit_body.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
	NotLooked, // not looked for at its wrist (no body, the wrist out of the picture or out of sight,
		   // hands off: see looks_for_hand), and not found anywhere else
	Unsure,    // looked for at its wrist (or followed until now), and the hand model isn't sure a hand is there
	Elsewhere, // out of your reach: a hand that isn't yours
	Twice,     // the other hand's: your hands together, one hand found twice
};

// Where a hand found came from.
enum class HandFrom {
	None,
	Followed, // from where it was last look
	Wrist,    // looked for at your body's wrist
	Palm,     // the palm finder, anywhere in the picture
};

struct HandSeen {
	bool found = false;
	HandGate gate = HandGate::NotLooked;
	float presence = 0;   // how sure the hand model is (0 to 1)
	float handedness = 0; // how sure the hand model is that it's a right hand - the body model's right (0 to 1)
	// In the picture's pixels (z: how far from the camera against the
	// wrist, on x's scale).
	Point points[HAND_POINTS];
	// In metres, the camera's way (x right, y down, z away from it), the
	// wrist at the body's wrist (Body::world).
	Point world[HAND_POINTS];
	// The square it was cut from (picture pixels, radians).
	float box[5] = {};
	HandFrom from = HandFrom::None;
	bool tracked = false; // the square came from where it was last look (from == Followed)
	// How lit the hand is (its palm, 0 black to 1 white, as the picture's
	// own brightness), and the picture just round it.
	float light = 0, around = 0;
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
	bool fresh = false;    // this look ran the body model (else the body is from an earlier look)
	bool turned = false;   // the body model had you turned round: its left and right were swapped back
	bool palm = false;     // this look ran the palm finder
	float ms = 0;          // how long the look took (milliseconds)
};

class BodyTracker {
public:
	BodyTracker();
	~BodyTracker();
	BodyTracker(const BodyTracker &) = delete;
	BodyTracker &operator=(const BodyTracker &) = delete;

	// Google's holistic_landmarker.task (its body finder, body model,
	// hand-square model and hand model); for light mode, a lighter hand
	// model (hand_landmark_lite.tflite; none: light mode uses the full
	// one); and the palm finder (palm_detection_full.tflite; none: a hand
	// is only looked for at your wrist). From memory or files. False, with
	// why, if it can't.
	bool load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size,
		  std::string *problem = nullptr, const uint8_t *palm = nullptr, size_t palm_size = 0);
	bool load_files(const std::string &task_path, const std::string &lite_path, std::string *problem = nullptr,
			const std::string &palm_path = "");
	bool ready() const;
	bool has_palm() const;

	// Looks at a new picture. `seconds`: when the camera took it (any
	// clock that only goes forward), for the smoothing.
	const Body &look(const Frame &picture, double seconds);
	const Body &body() const;

	// The next look starts with the body finder. Hands too.
	void forget();

	// Hands: looked for (else only the body, and a hand only at a wrist
	// where looks_for_hand says). Light: the lighter hand model.
	void set_hands(bool on);
	// The body model runs at most this often (seconds; 0: every look) -
	// hands are looked at on every look.
	void set_body_every(float seconds);
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
	// ...and the palm finder's finds on the last look that ran it (picture
	// pixels: each box, how sure, and its 7 points - the wrist first, then
	// the knuckles from the first finger's, the thumb's two).
	struct Palm {
		float x = 0, y = 0, w = 0, h = 0, score = 0;
		float keys[14] = {};
	};
	const std::vector<Palm> &palms() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// Whether the tracker looks for a hand at a wrist (in sight, in the
// picture) whose elbow is `elbow` in a picture w x h: hands on, always;
// hands off (arms only), only where the elbow is out of the picture - only
// a hand found there tells your arm from the body model's guess at one
// (ArmSolver).
bool looks_for_hand(bool hands_on, const Point &elbow, int w, int h);

// MediaPipe's SSD anchors (shares of a finder's square, x then y): for
// each stride, a grid of size / stride spots, 2 anchors a spot for each
// stride the same as it (the body finder: 8, 16, 32, 32, 32; the palm
// finder: 8, 16, 16, 16).
std::vector<float> ssd_anchors(int size, const std::vector<int> &strides);

// The palm finder's answers (its 2016 boxes, 18 numbers each, and how
// sure each is) on a picture w x h fitted into its size x size square with
// black bars, as MediaPipe reads them: the sure ones, overlapping ones
// merged (weighted by how sure), surest first - in the picture's pixels.
std::vector<BodyTracker::Palm> read_palms(const float *boxes, const float *scores, const std::vector<float> &anchors,
					  int size, int w, int h);
// MediaPipe's hand square from a palm found (picture pixels: middle x, y,
// width, height, radians): turned so its wrist-to-middle-knuckle way
// points up, out to the whole hand.
void palm_square(const BodyTracker::Palm &palm, float out[5]);

// The body model had you turned round (your back to the camera: your
// left shoulder on the picture's left of your right one, well past
// side-on): its left and right points swapped back. True if it did.
bool turn_back_round(Point points[BODY_POINTS], Point world[BODY_POINTS], float seen[BODY_POINTS]);

// Your arm to your hand: a found hand away from where the body model has
// that wrist (the body model wrong about your arm, as it often is in a
// dim room or with an arm raised) puts the wrist at the hand - in the
// picture, and in metres as far as your shoulders' size there says - and
// works its elbow out from your shoulder (bent the way the body model
// had it). A hand at the body's wrist only nudges the wrist to it.
// `side`: the person's left (0) or right (1).
void arm_to_hand(Body &body, int side);

} // namespace direbot::face
