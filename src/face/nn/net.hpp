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

// Our own small engine for the face models: it reads a TensorFlow Lite
// model file (Google's MediaPipe face models, data/face) and works it out
// on the CPU, with no TensorFlow or any other library.
//
// It knows only the steps those models use - convolutions (normal and
// depthwise), PReLU and ReLU, adding and multiplying (with broadcasting),
// padding, max pooling, reshaping, joining, slicing, transposing, means
// and sums, and a few maths ones. Anything else in a file and it says so
// rather than guessing. Every shape is fixed when the file is read, so a
// run allocates nothing.
//
// Convolutions run on the fastest code the CPU has (AVX2 or SSE2,
// kernels.hpp). Next to a convolution, the PReLU or ReLU after it and a
// skip-connection added to it are done in the same pass.
//
// Tested against Google's own engine on the same inputs (tests/unit_face.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace direbot::face::nn {

class Net {
public:
	Net();
	~Net();
	Net(const Net &) = delete;
	Net &operator=(const Net &) = delete;
	Net(Net &&) noexcept;
	Net &operator=(Net &&) noexcept;

	// Reads a .tflite file (its bytes are copied: they needn't stay).
	// False, with what went wrong, for a file it can't run.
	bool load(const uint8_t *bytes, size_t size, std::string *problem = nullptr);
	bool loaded() const;

	size_t inputs() const;
	size_t outputs() const;
	const std::vector<int> &input_shape(size_t i) const;
	const std::vector<int> &output_shape(size_t i) const;
	float *input(size_t i);
	const float *output(size_t i) const;

	void run();

	// For tests: off, each step runs as the file has it (no steps done
	// together) on the plain C++ code. Set before load.
	void set_plain(bool plain);
	// Multiply-adds in one run, for the speed report.
	double work() const;
	// For the speed check: how long each kind of step took over the runs
	// since this was switched on, in milliseconds ("CONV_2D 1x1", ...).
	void set_timing(bool on);
	std::vector<std::pair<std::string, double>> timing() const;
	// The tensor the file calls `name`, after a run (only kept tensors:
	// inputs, outputs, and every one when plain); null if there's none.
	const float *tensor(const std::string &name, std::vector<int> *shape = nullptr) const;

private:
	struct Impl;
	Impl *impl_;
};

// IEEE half-precision bits to a float.
float half_to_float(uint16_t h);

} // namespace direbot::face::nn
