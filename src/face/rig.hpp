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
//     like a long thin one - the whole of it turns round its head.
//   - Its body (a VRM's spine and chest): some of your head's turn and
//     its own slow sway, its chest and shoulders rising with each breath
//     (alive.hpp); a character with no head bone grows a touch with each
//     breath instead. Its fingers rest relaxed (or as curled as asked).
//   - Your 52 expressions (through your face's tuning, alive.hpp): parts
//     named as Apple's ARKit names them ("jawOpen", "eyeBlinkLeft"...)
//     follow yours as they are; VRM's
//     expressions (blink, "aa", happy...) and the usual names other
//     tools give them ("Blink", "A", "Smile", "まばたき"...) are worked
//     out from yours. Your eyes' looks turn a VRM's eyes.
//   - Its bones that follow others (VRM's node constraints): twist bones
//     turning with its arms, sleeves aiming at its elbows... as its file
//     says, once everything else has moved it and before it swings.
//   - Swinging parts: VRM's spring bones as VRM says they move, bumping
//     into its colliders; a character's dangly parts (a chain of
//     links, a doll's ears and legs) swing, each hanging from
//     its first bone - or, set up (an imported Lens Studio project), the
//     chains it names, pulled as it says, and parts that stay upright and
//     trail after your head (a doll's body).
//   - Its own animation, if it has one and it's chosen (an idle, a tail's
//     wag...), over and over: your head, eyes and its swinging parts move
//     from where it has them.
//   - What it does on cue (react.hpp): a face for a while (VRM's happy,
//     angry... or the file's own; made from ARKit's shapes for a character
//     made for them; or any of its shapes by name), its eyes screwed shut,
//     a move played once - a wave or cheer with a VRM's arms, one of its
//     own animations, or a VRM animation file's (any VRM: its bones'
//     turns carried over to this one's, as VRM says) - and its swinging
//     parts thrown about by a hit.
//   - Where your real eyes and mouth go on it (face_spots).
//
// Then its points are moved by its bones and expressions ("skinned"),
// ready to draw. Plain code over plain numbers (tests/unit_model.cpp).

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "arms.hpp"
#include "math3d.hpp"
#include "model.hpp"
#include "react.hpp"
#include "setup.hpp"
#include "tracker.hpp"
#include "voice.hpp"

namespace direbot::face {

// The least a blink or mouth shape must move its face (a share of its
// height: about 1.5 mm on a person) for its own face to show it.
constexpr float FACE_SHOWS = 0.001f;

struct RigOptions {
	// Its front: the turn from the model as it is to facing +z (VRM 0.x
	// models face -z; some others face x).
	Quat facing;
	// Swinging parts: 0 stiff to 1 floppy (0.5 as the file says).
	float floppy = 0.5f;
	bool springs = true;
	// Which of its own animations plays (Model::animations), or -1 none.
	int animation = -1;
	// Its parts (by node name, or "#n" for its n-th node) whose own face
	// your face doesn't move: in a file of two characters, one talks.
	std::vector<std::string> still_faces;
	// How much of your head's turn the neck takes (the head the rest).
	float neck_share = 0.4f;
	// A character made standing in a T: its arms down by its sides.
	bool arms_down = true;
	// Its set-up (setup.hpp): the chains that swing and the parts that
	// stay upright, if it names any.
	std::shared_ptr<const CharacterSetup> setup;
	// Its fingers (a VRM's finger bones): 0 as the file has them, 0.5
	// relaxed as hands rest, 1 curled. Fingers its file already bends are
	// only bent the rest of the way.
	float fingers = 0.5f;
};

// A body move on it (react.hpp's, found): a VRM's wave or cheer, one of
// its own animations, or a VRM animation file's; how far into it (seconds)
// and how much it shows (0 to 1).
struct MoveNow {
	BuiltIn builtin = BuiltIn::None;
	int animation = -1;
	std::shared_ptr<const Motion> motion;
	float time = 0, amount = 0;
};

struct Drive {
	// Your head's turn (facing +z, y up: as it's seen in the picture,
	// already mirrored if the picture is), against facing straight ahead.
	Quat head;
	// The model -> the world it's drawn in (any size; the swinging parts
	// swing in it, with gravity down -y).
	M4 place;
	// Your expressions (the tracker's moves through your face's tuning,
	// alive.hpp's tune_moves: 0 at rest to 1 all the way), with left and
	// right already the character's own.
	float moves[EXPRESSIONS] = {};
	float dt = 0;     // seconds since the last frame
	bool look = true; // turn its eyes with yours
	bool expressions = true;
	// The model -> the world with your head held straight, for parts that
	// stay upright (`place` if not given).
	M4 upright;
	bool has_upright = false;
	// Your real eyes and mouth are on it: its own drawn ones (its set-up's
	// "own_face") aren't drawn.
	bool hide_own_face = false;
	// Alive when you're still (alive.hpp): its breath (0 breathed out to
	// 1 in, already times how much), how much of your head's turn its body
	// takes (0 none to 1 the most: 40% of it), and its body's own sway
	// (degrees: a turn about up, a lean to the side). A VRM breathes with
	// its chest and shoulders; one with no head bone (it turns whole)
	// grows a little with each breath, if `breathe_whole`.
	float breath = 0;
	float body_follow = 0;
	float body_yaw = 0, body_roll = 0;
	bool breathe_whole = true;
	// The mouth from your mic (voice.hpp): its vowels take over the
	// mouth's shapes as much as `mic.share` - VRM's A I U E O as they
	// are, ARKit-named shapes through mic_into_moves. `moves` stay the
	// camera's.
	MicMouth mic;
	// On cue (react.hpp): faces (each by name, as faces() lists them, or a
	// shape's own name; a face takes over from yours as much as it's on,
	// your blinks and mouth staying yours), its eyes shut this much (0 to
	// 1), body moves (oldest first, each over the last as much as it
	// shows) and its swinging parts thrown, once (the world's units a
	// second).
	std::vector<FaceNow> faces;
	float squint = 0;
	std::vector<MoveNow> body;
	V3 knock;
	// Its head squished by a headpat (react.hpp's ReactOut::squish: 0 not
	// at all, 1 the most): its head and all on it 10% shorter along its
	// own up and 5% wider, from where it sits on its neck (one with no
	// head bone: all of it, from its bottom, half as much).
	float squish = 0;
	// Your arms, hands and fingers (arms.hpp), if followed: a VRM's arms
	// as yours (a move on cue takes over from them as much as it shows),
	// its hand by its head where yours is by yours.
	bool follow_arms = false;
	ArmsNow arms;
};

// A box round what a character takes up (model space).
struct Bounds {
	V3 low, high;
	bool found = false;
};

class Rig {
public:
	Rig(std::shared_ptr<const Model> model, const RigOptions &options);

	void pose(const Drive &drive);
	// Its box as it stands at rest (0.8.1): its arms down (a character
	// made standing in a T, RigOptions::arms_down), its own animation
	// where it is, nothing on cue, your face not on it. Posed once to
	// measure; its swinging parts start again at rest after. Not found:
	// no points.
	Bounds rest_bounds();
	// Another of its own animations (or -1 none), from its start: no need
	// to start again.
	void set_animation(int which);
	int animation() const { return options_.animation; }
	// Swinging parts back at rest (when it's put somewhere new).
	void settle();

	struct Part {
		int node = -1, part = -1;
		const Primitive *prim = nullptr;
		std::vector<V3> pos, normal; // in the world
		// The way its picture's right is, in the world, for a part with a
		// normal map (empty: none); which way its up is: prim->tangent's w,
		// times `handed` (-1 for a part mirrored by its node or bones).
		std::vector<V3> tangent;
		float handed = 1;
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
	// Whether its own face can blink and open its mouth where you'd see
	// it: a blink (or mouth) shape that moves it at least FACE_SHOWS of
	// its height. A shape that barely moves (a marker for where its eyes
	// and mouth are) doesn't count.
	bool blinks() const { return blinks_; }
	bool talks() const { return talks_; }
	const std::vector<std::vector<float>> &weights() const { return weights_; }
	int springs() const { return (int)springs_.size(); }
	// How many of its finger joints relax (RigOptions::fingers), and
	// whether it has a body that breathes and follows your head.
	int finger_joints() const { return (int)fingers_.size(); }
	// How many of its bones follow others (VRM's node constraints: twist
	// bones, sleeves...).
	int following_bones() const { return (int)model_->constraints.size(); }
	bool has_body() const { return !torso_.empty(); }
	bool auto_chain() const { return auto_chain_; }
	int upright_parts() const { return (int)upright_.size(); }
	// What your eyes and mouth ride on: the model -> the world as its head
	// is now (its head bone's, or all of it turned) - squished too, if a
	// headpat squishes it (Drive::squish).
	M4 head_frame() const;
	// The squish on it now (the world -> the world; none: as it is).
	const M4 &squish() const { return squish_; }

	// The faces it makes on cue, for the page: VRM's own (happy, angry,
	// sad, relaxed, surprised - those it has) and its file's other
	// expressions; or, made for ARKit, the five made from its shapes (and
	// "tongueOut" if it has one); or the usual-named ones it has. Then
	// every shape's own name (any can be a face).
	std::vector<std::string> faces() const;
	std::vector<std::string> shapes() const;
	// Whether it has arms to wave and cheer with, and plays VRM animation
	// files (a VRM, with hips).
	bool has_arms() const { return arm_bone_[0][0] >= 0 || arm_bone_[1][0] >= 0; }
	// Its arms can follow yours (Drive::follow_arms): a VRM with arms.
	bool follows_arms() const { return arm_rest_[0].ok || arm_rest_[1].ok; }
	bool plays_motions() const { return model_->is_vrm() && model_->bone("hips") >= 0; }
	// Its head at rest, the facing model's way (x its right as the picture
	// shows it, y up, z out of its face): the box round what its head
	// bone moves (its skull and face, and what hangs off it - hair, ears
	// - up to `top`). Not found: no head bone, or nothing on it.
	struct HeadBox {
		V3 low, high;
		float top = 0;
		bool found = false;
		// How high it all reaches over a grid across it (x) and from its
		// back to its front (z): TOP_GRID x TOP_GRID cells over [top_low,
		// top_high], each its highest point's y (or below `low` if none) -
		// where a hand on its head rests.
		static constexpr int TOP_GRID = 16;
		V3 top_low, top_high;
		std::vector<float> tops;
		// The highest it reaches within `reach` of (x, z) (`top` if it
		// knows no better).
		float top_at(float x, float z, float reach) const;
	};
	const HeadBox &head_box() const { return head_box_; }

private:
	struct Joint {
		SpringJoint s;
		Quat rest; // its own turn at rest (as its animation has it now)
		V3 tail;   // in its own space
		V3 axis;   // the tail's way, in its own space
		V3 now, before;
		// A set-up chain (as Lens Studio's swing): pulled by `pull` (the
		// model's units a second, each second; turned with the head, or
		// not), never back to how it was made.
		bool pulled = false, with_head = true;
		V3 pull;
	};
	struct Upright {
		int node = -1;
		float follow = 5;
		V3 at;
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
	void apply_constraints();
	void turn_in_rest(int node, Quat by);
	void find_fingers();
	void curl_fingers(const Drive &d);
	void find_arms();
	void drive_arms(const Drive &d);
	void drive_expressions(const Drive &d);
	void drive_head(const Drive &d);
	void drive_eyes(const Drive &d);
	void step_springs(float dt, float scale);
	void animate(float dt);
	void play_moves(const Drive &d);
	void pose_bone(int node, Quat world_turn, float amount);
	void play_motion(const Motion &mo, float t, float amount);
	void play_builtin(BuiltIn b, float t, float amount);
	void play_own(int animation, float t, float amount);
	void measure_head();
	void squish_head(const Drive &d);
	void stand_upright(const Drive &d);
	Quat head_turn_now() const;
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
	// A finger joint: bent about `axis` (the model at rest) by `most` at
	// a full curl, less what the file already bends it (`has`; radians).
	// Which it is: its side (0 its left), digit (0 the thumb, then first
	// to little) and joint (0 its root).
	struct Finger {
		int node = -1;
		V3 axis;
		float most = 0, has = 0;
		int side = 0, digit = 0, joint = 0;
	};
	std::vector<Finger> fingers_;
	// Its spine, chest and upper chest (as it has them, lowest first) and
	// its shoulders: they breathe and follow your head.
	std::vector<int> torso_;
	int shoulder_[2] = {-1, -1}, upper_arm_[2] = {-1, -1};
	std::vector<Chain> springs_;
	std::vector<Upright> upright_;
	std::vector<char> held_;     // a node standing upright this frame
	std::vector<char> own_face_; // a node of its own drawn face
	bool hide_face_ = false;
	bool auto_chain_ = false;
	bool settled_ = true;
	float spring_time_ = 0;
	float anim_time_ = 0;          // seconds into its animation
	std::vector<char> anim_turns_; // a node its animation turns
	// How much a body move holds each node this frame (0 to 1): what the
	// rig itself does to it (arms down, fingers relaxed) is that much less.
	std::vector<float> moved_;
	// The VRM humanoid's bones by VRM 1.0's names (a 0.x's thumbs too).
	std::map<std::string, int> bones1_;
	int arm_bone_[2][3] = {{-1, -1, -1}, {-1, -1, -1}}; // each side's upper arm, lower arm, hand
	// Each arm at rest, the facing model's way, for following yours: its
	// shoulder (where the upper arm turns), its parts' lengths and ways,
	// its elbow's hinge (bending its forearm forwards) and its hand's
	// ways (wrist to middle knuckle, little knuckle to first).
	struct ArmRest {
		bool ok = false, hand = false;
		V3 shoulder;
		// Its shoulder bone (collarbone), when the arm hangs from it: where
		// it turns from, and the axis that lifts the arm's root straight up.
		bool lifts = false;
		V3 root, lift_axis;
		float upper_len = 0, fore_len = 0;
		V3 upper, fore, hinge, fore_hinge;
		V3 along, across;
	};
	ArmRest arm_rest_[2];
	// Its head's middle and width (ear to ear, about), the facing model's
	// way, for a hand by its head (`head_known_`: it has them).
	V3 head_mid_;
	float head_width_ = 0;
	bool head_known_ = false;
	// A VRM animation's expressions this frame (name -> strength).
	std::map<std::string, float> move_faces_;
	HeadBox head_box_;
	// Its head bone and all that hangs off it (squished with it).
	std::vector<int> head_tree_;
	M4 squish_;
	float floor_ = 0; // its lowest, the facing model's way
	// Its shapes by name (lower case): mesh and morph.
	std::map<std::string, std::vector<std::pair<int, int>>> morph_by_name_;
	V3 last_root_;
	std::vector<std::vector<std::vector<Sparse>>> sparse_; // mesh, part, morph
	std::vector<std::vector<float>> weights_;
	std::vector<Driver> drivers_;
	bool perfect_sync_ = false;
	int arkit_count_ = 0;
	bool blinks_ = false, talks_ = false;
	std::vector<char> still_mesh_; // a mesh of a face your face doesn't move
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
	M4 upright; // the model -> the world, your head held straight (Drive::upright)
	// In its own spot: the picture's width and height in the world, and
	// the world's ways that are its right and up (a camera view turns
	// them).
	float seen_w = 1, seen_h = 1;
	V3 right{1, 0, 0}, up{0, 1, 0};
};

// What's framed in its own spot, turned to face +z: its middle and how
// big it is each way. `head`: round its head and shoulders (a VRM with a
// head bone), else all of it - as it stands (`stands`, Rig::rest_bounds,
// from 0.8.1; with a little room round it), or its file's box if not
// given (a character made standing in a T is as wide as its arms reach
// then).
void framed_box(const Model &m, Quat facing, bool head, const Bounds *stands, V3 &middle, V3 &extent);
// In its own spot: the picture `w` x `h`, the character `size` of it
// (1: as tall or as wide as the picture; up to 3, bigger than the picture,
// its top kept in it, from 0.8.1), its middle moved by `dx`, `dy` (shares
// of the picture, + right and down) and grown by `grow`. `head` and
// `stands`: as framed_box.
Framing frame_own_spot(const Model &m, Quat facing, uint32_t w, uint32_t h, float size, bool head, float dx, float dy,
		       float grow, const Bounds *stands = nullptr);
// Moves it on in its spot by `dx`, `dy` more (shares of the picture).
void move_own_spot(Framing &f, float dx, float dy);

// A saved camera view of it in its own spot: the camera gone round it
// (`yaw`, degrees, + to the picture's right, so you see more of its left
// side) and up (`pitch`, + looking down on it), closer (`zoom`: 1 as it's
// framed, 2 twice as big) and where it sits in the picture (`x`, `y`:
// shares of the picture, + right and down). The default is as it's framed.
struct CameraView {
	float yaw = 0, pitch = 0, zoom = 1, x = 0, y = 0;
};
// Each kept sensible (yaw -120 to 120, pitch -45 to 60, zoom 0.3 to 4,
// x and y -0.5 to 0.5).
CameraView sensible_view(const CameraView &v);
// A share `t` (0 to 1) of the way from `a` to `b`.
CameraView view_between(const CameraView &a, const CameraView &b, float t);
// The framing seen from the camera view (before move_own_spot).
void view_own_spot(Framing &f, const CameraView &v);

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

// Where your eyes and mouth go on it: the character's own left eye,
// right eye and mouth, each its middle in the model at rest and its size
// (the model's units) on a card facing its front. As its set-up says;
// else found from its blink and mouth expressions, its eye bones, or (if
// none) a guess at the front of its top half - each worked out again only
// when something changes.
struct FaceSpot {
	V3 at;
	float w = 0, h = 0;
};
struct FaceSpots {
	FaceSpot spot[3];
	// How each was found: "set up", "expressions", "eye bones", "guessed".
	const char *from[3] = {"guessed", "guessed", "guessed"};
	// Its front picture's box (the facing model's left, top, width,
	// height), for spots given as shares of it.
	float box[4] = {0, 0, 1, 1};
	// How deep it is there: its front and back.
	float front = 0.5f, back = -0.5f;
};
FaceSpots face_spots(const std::shared_ptr<const Model> &m, Quat facing, const CharacterSetup &setup);
// The part of its front box round its face, for a close look at its eyes
// and mouth: left, top, width, height, as shares of the box (left and
// width of its width, top and height of its height), as square as the
// box lets it be. All of it (0, 0, 1, 1) when its face is most of it.
struct FaceRegion {
	float x = 0, y = 0, w = 1, h = 1;
};
FaceRegion face_region(const FaceSpots &fs, Quat facing);

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
