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

// A 3D character (model.hpp) moved by your face, frame by frame:
//
//   - Your head's turn: a VRM's neck and head bones turn (the neck some
//     of the way, the head the rest), or - a character with no head bone,
//     like the Sausage - the whole of it turns round its head.
//   - Your 52 expressions: parts named as Apple's ARKit names them
//     ("jawOpen", "eyeBlinkLeft"...) follow yours as they are; VRM's
//     expressions (blink, "aa", happy...) and the usual names other
//     tools give them ("Blink", "A", "Smile", "まばたき"...) are worked
//     out from yours. Your eyes' looks turn a VRM's eyes.
//   - Swinging parts: VRM's spring bones as VRM says they move, bumping
//     into its colliders; a character whose bones are one chain (the
//     Sausage's string of sausages) swings on it, hanging from its first.
//
// Then its points are moved by its bones and expressions ("skinned"),
// ready to draw. Plain code over plain numbers (tests/unit_model.cpp).

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "math3d.hpp"
#include "model.hpp"
#include "tracker.hpp"

namespace direbot::face {

struct RigOptions {
	// Its front: the turn from the model as it is to facing +z (VRM 0.x
	// models face -z; the Sausage faces x).
	Quat facing;
	// Swinging parts: 0 stiff to 1 floppy (0.5 as the file says).
	float floppy = 0.5f;
	bool springs = true;
	// How much of your head's turn the neck takes (the head the rest).
	float neck_share = 0.4f;
	// A character made standing in a T: its arms down by its sides.
	bool arms_down = true;
};

struct Drive {
	// Your head's turn (facing +z, y up: as it's seen in the picture,
	// already mirrored if the picture is), against facing straight ahead.
	Quat head;
	// The model -> the world it's drawn in (any size; the swinging parts
	// swing in it, with gravity down -y).
	M4 place;
	// Your expressions (the tracker's moves, 0 at rest to 1), with left
	// and right already the character's own.
	float moves[EXPRESSIONS] = {};
	float dt = 0;     // seconds since the last frame
	bool look = true; // turn its eyes with yours
	bool expressions = true;
};

class Rig {
public:
	Rig(std::shared_ptr<const Model> model, const RigOptions &options);

	void pose(const Drive &drive);
	// Swinging parts back at rest (when it's put somewhere new).
	void settle();

	struct Part {
		int node = -1, part = -1;
		const Primitive *prim = nullptr;
		std::vector<V3> pos, normal; // in the world
	};
	const std::vector<Part> &parts() const { return parts_; }
	const std::vector<M4> &world() const { return world_; }
	const Model &model() const { return *model_; }
	// Whether it turns whole (no head bone) and round where (model space).
	bool turns_whole() const { return head_ < 0; }
	V3 pivot() const { return pivot_; }
	void set_pivot(V3 p) { pivot_ = p; }

	// What moved it last frame, for FaceGetState and tests: each VRM (or
	// usual-named) expression's strength, how many ARKit-named parts it
	// has, and each mesh's expression strengths.
	const std::map<std::string, float> &expression_values() const { return values_; }
	int arkit_parts() const { return arkit_count_; }
	const std::vector<std::vector<float>> &weights() const { return weights_; }
	int springs() const { return (int)springs_.size(); }
	bool auto_chain() const { return auto_chain_; }

private:
	struct Joint {
		SpringJoint s;
		Quat rest; // its own turn at rest
		V3 tail;   // in its own space
		V3 axis;   // the tail's way, in its own space
		V3 now, before;
	};
	struct Chain {
		std::vector<Joint> joints;
		std::vector<int> colliders;
	};
	struct Sparse {
		std::vector<uint32_t> at;
		std::vector<V3> pos, normal;
	};
	struct Driver {
		int mesh = -1, morph = -1;
		int move = -1; // an ARKit move, or -1
		std::string expression;
		float weight = 1;
	};

	void world_from(int node);
	void drive_expressions(const Drive &d);
	void drive_head(const Drive &d);
	void drive_eyes(const Drive &d);
	void step_springs(float dt, float scale);
	void skin();

	std::shared_ptr<const Model> model_;
	RigOptions options_;
	std::vector<V3> t_;
	std::vector<Quat> r_;
	std::vector<V3> s_;
	std::vector<M4> rest_world_; // model space
	std::vector<M4> world_;      // the drive's world
	M4 root_;
	int head_ = -1, neck_ = -1, left_eye_ = -1, right_eye_ = -1;
	V3 pivot_;
	float height_ = 1;
	struct Arm {
		int node;
		Quat turn; // as the model at rest has it
	};
	std::vector<Arm> arms_;
	std::vector<Chain> springs_;
	bool auto_chain_ = false;
	bool settled_ = true;
	float spring_time_ = 0;
	V3 last_root_;
	std::vector<std::vector<std::vector<Sparse>>> sparse_; // mesh, part, morph
	std::vector<std::vector<float>> weights_;
	std::vector<Driver> drivers_;
	bool perfect_sync_ = false;
	int arkit_count_ = 0;
	std::map<std::string, float> values_;
	std::vector<Part> parts_;
};

// Where a character goes and how it's seen, frame by frame.
struct Framing {
	M4 place;          // the model -> the world (Drive::place)
	M4 view;           // the world -> the camera's view (looking along -z, y up)
	float fov = 0.35f; // radians, top to bottom
	float near = 0.1f, far = 100;
	float world_scale = 1; // world units a model unit
	float height = 1;      // its height in the world
	V3 pivot;              // what it turns round, whole (model space)
	bool has_pivot = false;
};

// In its own spot: the picture `w` x `h`, the character `size` of it
// (0 to 1), its middle moved by `dx`, `dy` (shares of the picture, + right
// and down) and grown by `grow`. `head`: framed round its head and
// shoulders (a VRM with a head bone), else all of it.
Framing frame_own_spot(const Model &m, Quat facing, uint32_t w, uint32_t h, float size, bool head, float dx, float dy,
		       float grow);

// Worn on your head: where it sits on it (centimetres from the middle of
// your head - MediaPipe's face at rest - x right, y up, z out of your
// face, and how many centimetres a model unit is), or worked out (`auto`):
// its head bone where yours is (a VRM, at its own size), else its middle
// over your face, 24 cm across. `size` grows it from there.
struct WornPlace {
	bool automatic = true;
	V3 at;
	float units = 1;
	float size = 1;
};
// `pose`: your face (its centimetres -> the camera's, MediaPipe's
// facial transformation, as a matrix with points as columns); the camera
// sees 63 degrees top to bottom, `aspect` wide.
Framing frame_worn(const Model &m, Quat facing, const WornPlace &place, const M4 &pose, float aspect);
// Where the middle of your head is in the model, as worn (for turning it
// whole round your head in its own spot).
V3 head_in_model(const Model &m, Quat facing, const WornPlace &place);

// A row-after-row 4 x 4 (the tracker's pose) as a matrix.
M4 matrix_of_rows(const float rows[16]);
// Your head's turn from the glide (degrees; tilt in radians, clockwise as
// the picture shows it), mirrored or not.
Quat head_turn(float yaw, float pitch, float tilt, bool mirror);

// Which way it most likely faces, as a turn (degrees about y) to face
// the camera: a VRM as its version says; any other the way glTF says
// (+z), unless it's much deeper than it's wide - then it was made facing
// +x (as the AR VTuber characters are).
int guess_turn(const Model &m);

// An ARKit move's number by its name (any case, with or without a
// prefix like "blendShape1."), or -1.
int arkit_move(const std::string &name);
// Left and right swapped (eyeBlinkLeft <-> eyeBlinkRight...), for a
// mirrored picture.
void swap_sides(float moves[EXPRESSIONS]);

} // namespace direbot::face
