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

// Where your head is in 3D and which way it's turned, from the face's
// points - the same sums as MediaPipe's "face geometry", so the answer
// is the same as theirs.
//
// Google's file (in the face models, data/face) has a face at rest: 468
// points in centimetres, and how much each of 33 steady ones (not the
// mouth or eyes) counts. That face is turned, sized and moved to fit the
// points best (weighted Procrustes), looking through a camera that sees
// 63 degrees from top to bottom. Twice: once to find how far away the
// face is, once more for the answer.
//
// The answer is a 4 x 4 matrix, row after row: the face at rest -> the
// camera's space, in centimetres, the camera at 0 looking along -z with
// y up (OpenGL's way). MediaPipe calls it the facial transformation
// matrix.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace direbot::face {

class Geometry {
public:
	// geometry_pipeline_metadata_landmarks.binarypb, from the models file.
	bool load(const uint8_t *bytes, size_t size, std::string *problem = nullptr);
	bool loaded() const { return !rest_.empty(); }

	// `xyz`: the face's points, 3 floats each (the first 468 count): x and
	// y in the picture's pixels, z the model's depth on x's scale. False
	// for points all in one spot.
	bool pose(const float *xyz, int w, int h, float pose[16]) const;

	// The face at rest (x, y, z each, centimetres) and its triangles (3
	// point numbers each), for drawing a face on later.
	const std::vector<float> &rest() const { return rest_out_; }
	const std::vector<uint32_t> &triangles() const { return triangles_; }

private:
	std::vector<double> rest_;   // x, y, z each
	std::vector<int> used_;      // the points that count...
	std::vector<double> weight_; // ...and how much
	std::vector<float> rest_out_;
	std::vector<uint32_t> triangles_;
};

// Which way a head is turned, in degrees: yaw + towards the picture's
// right, pitch + up, roll + clockwise as the picture shows it. From a
// pose's turn (any size it has is taken out first).
struct Turn {
	float yaw = 0, pitch = 0, roll = 0;
};
Turn turn_of(const float pose[16]);
// The turn since `rest`, in the head's own terms: a camera off to one
// side still reads 0 when you look the way you did at rest.
Turn turn_since(const float pose[16], const float rest[16]);

// A 3 x 3 turn (row after row) that turns `from` onto `to` best, each
// point counting `weight`, and how much bigger and where it moves to:
// to ~ scale * turn * from + move. For tests and pose.
bool fit(const double *from, const double *to, const double *weight, size_t n, double turn[9], double &scale,
	 double move[3]);

} // namespace direbot::face
