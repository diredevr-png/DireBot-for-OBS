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

#include "net.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <cstring>
#include <limits>

#include "kernels.hpp"

namespace direbot::face::nn {

float half_to_float(uint16_t h)
{
	const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
	uint32_t exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
	uint32_t bits;
	if (exponent == 0) {
		if (mantissa == 0) {
			bits = sign;
		} else {
			// Too small for a normal half: move the point along.
			int shifts = 0;
			while (!(mantissa & 0x400u)) {
				mantissa <<= 1;
				shifts++;
			}
			bits = sign | ((uint32_t)(113 - shifts) << 23) | ((mantissa & 0x3FFu) << 13);
		}
	} else if (exponent == 31) {
		bits = sign | 0x7F800000u | (mantissa << 13);
	} else {
		bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
	}
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

namespace {

// ---- Reading a FlatBuffers file (what a .tflite is) ----------------------
//
// A table starts with where its list of fields is (a "vtable"); each field
// is found through that list, and is missing (its default) if it isn't
// there. Every read is checked against the file's size.

struct Flat {
	const uint8_t *b = nullptr;
	size_t n = 0;
	bool bad = false;

	template<typename T> T get(size_t at)
	{
		T v{};
		if (at > n || n - at < sizeof(T)) {
			bad = true;
			return v;
		}
		std::memcpy(&v, b + at, sizeof(T));
		return v;
	}
	size_t field(size_t table, int id)
	{
		if (!table || bad)
			return 0;
		const int64_t vt = (int64_t)table - get<int32_t>(table);
		if (vt < 0 || (uint64_t)vt >= n) {
			bad = true;
			return 0;
		}
		const uint16_t size = get<uint16_t>((size_t)vt);
		const size_t slot = 4 + 2 * (size_t)id;
		if (slot + 2 > size)
			return 0;
		const uint16_t off = get<uint16_t>((size_t)vt + slot);
		return off ? table + off : 0;
	}
	// What an offset at `at` points to.
	size_t follow(size_t at)
	{
		if (!at)
			return 0;
		const uint32_t o = get<uint32_t>(at);
		if (!o || bad)
			return 0;
		if ((uint64_t)at + o >= n) {
			bad = true;
			return 0;
		}
		return at + o;
	}
	size_t table(size_t t, int id) { return follow(field(t, id)); }
	// A vector: where its first item is, and how many (each `item` bytes).
	size_t vec(size_t t, int id, uint32_t &count, size_t item)
	{
		count = 0;
		const size_t v = table(t, id);
		if (!v)
			return 0;
		count = get<uint32_t>(v);
		if (bad || (uint64_t)count * item > n - (v + 4)) {
			bad = true;
			count = 0;
			return 0;
		}
		return v + 4;
	}
	// Item i of a vector of tables.
	size_t item(size_t first, uint32_t i) { return follow(first + 4 * (size_t)i); }
	template<typename T> T scalar(size_t t, int id, T otherwise)
	{
		const size_t f = field(t, id);
		return f ? get<T>(f) : otherwise;
	}
	std::vector<int> ints(size_t t, int id)
	{
		uint32_t count;
		const size_t first = vec(t, id, count, 4);
		std::vector<int> out(count);
		for (uint32_t i = 0; i < count; i++)
			out[i] = get<int32_t>(first + 4 * (size_t)i);
		return out;
	}
	std::string text(size_t t, int id)
	{
		uint32_t count;
		const size_t first = vec(t, id, count, 1);
		return first ? std::string((const char *)b + first, count) : std::string();
	}
};

// TensorFlow Lite's numbers for things (its schema.fbs).
enum TfOp {
	TF_ADD = 0,
	TF_CONCATENATION = 2,
	TF_CONV_2D = 3,
	TF_DEPTHWISE_CONV_2D = 4,
	TF_DEQUANTIZE = 6,
	TF_LOGISTIC = 14,
	TF_MAX_POOL_2D = 17,
	TF_MUL = 18,
	TF_RELU = 19,
	TF_RELU6 = 21,
	TF_RESHAPE = 22,
	TF_PAD = 34,
	TF_TRANSPOSE = 39,
	TF_MEAN = 40,
	TF_SUB = 41,
	TF_DIV = 42,
	TF_STRIDED_SLICE = 45,
	TF_PRELU = 54,
	TF_NEG = 59,
	TF_SUM = 74,
	TF_SQRT = 75,
	TF_RSQRT = 76,
	TF_SQUARED_DIFFERENCE = 99,
};
enum TfType { TF_FLOAT32 = 0, TF_FLOAT16 = 1, TF_INT32 = 2 };
enum TfActivation { TF_ACT_NONE = 0, TF_ACT_RELU = 1, TF_ACT_RELU_N1_TO_1 = 2, TF_ACT_RELU6 = 3 };
enum TfPadding { TF_SAME = 0, TF_VALID = 1 };

const char *op_name(int code)
{
	switch (code) {
	case TF_ADD:
		return "ADD";
	case TF_CONCATENATION:
		return "CONCATENATION";
	case TF_CONV_2D:
		return "CONV_2D";
	case TF_DEPTHWISE_CONV_2D:
		return "DEPTHWISE_CONV_2D";
	case TF_DEQUANTIZE:
		return "DEQUANTIZE";
	case TF_LOGISTIC:
		return "LOGISTIC";
	case TF_MAX_POOL_2D:
		return "MAX_POOL_2D";
	case TF_MUL:
		return "MUL";
	case TF_RELU:
		return "RELU";
	case TF_RELU6:
		return "RELU6";
	case TF_RESHAPE:
		return "RESHAPE";
	case TF_PAD:
		return "PAD";
	case TF_TRANSPOSE:
		return "TRANSPOSE";
	case TF_MEAN:
		return "MEAN";
	case TF_SUB:
		return "SUB";
	case TF_DIV:
		return "DIV";
	case TF_STRIDED_SLICE:
		return "STRIDED_SLICE";
	case TF_PRELU:
		return "PRELU";
	case TF_NEG:
		return "NEG";
	case TF_SUM:
		return "SUM";
	case TF_SQRT:
		return "SQRT";
	case TF_RSQRT:
		return "RSQRT";
	case TF_SQUARED_DIFFERENCE:
		return "SQUARED_DIFFERENCE";
	default:
		return nullptr;
	}
}

size_t count_of(const std::vector<int> &shape)
{
	size_t n = 1;
	for (int d : shape)
		n *= (size_t)std::max(0, d);
	return n;
}

std::string shape_text(const std::vector<int> &shape)
{
	std::string s = "[";
	for (size_t i = 0; i < shape.size(); i++)
		s += (i ? "," : "") + std::to_string(shape[i]);
	return s + "]";
}

void clamp_range(int act, float &lo, float &hi)
{
	lo = -std::numeric_limits<float>::infinity();
	hi = std::numeric_limits<float>::infinity();
	if (act == TF_ACT_RELU) {
		lo = 0;
	} else if (act == TF_ACT_RELU6) {
		lo = 0;
		hi = 6;
	} else if (act == TF_ACT_RELU_N1_TO_1) {
		lo = -1;
		hi = 1;
	}
}

// TensorFlow Lite's "SAME": the padding before (the extra one goes after).
int same_pad(int in, int out, int stride, int filter, int dilation)
{
	const int effective = (filter - 1) * dilation + 1;
	const int total = std::max(0, (out - 1) * stride + effective - in);
	return total / 2;
}

int out_size(int padding, int in, int stride, int filter, int dilation)
{
	const int effective = (filter - 1) * dilation + 1;
	if (padding == TF_SAME)
		return (in + stride - 1) / stride;
	return (in - effective + stride) / stride;
}

constexpr int MOST_DIMS = 6;

// Shapes made the same length (ones in front), for broadcasting.
void align(const std::vector<int> &shape, int rank, int *out)
{
	const int pad = rank - (int)shape.size();
	for (int i = 0; i < rank; i++)
		out[i] = i < pad ? 1 : shape[i - pad];
}

} // namespace

struct Net::Impl {
	struct Tensor {
		std::string name;
		std::vector<int> shape;
		int type = TF_FLOAT32;
		bool constant = false;
		std::vector<float> values; // a constant's (floats, halves made floats)
		std::vector<int> ints;     // a constant's (int32)
		float *data = nullptr;     // where it is during a run
		int slot = -1;
		size_t count() const { return count_of(shape); }
	};
	struct Step {
		int code = 0;
		std::vector<int> in, out;
		// From the file.
		int padding = TF_SAME, stride_w = 1, stride_h = 1, dil_w = 1, dil_h = 1, act = TF_ACT_NONE;
		int filter_w = 1, filter_h = 1, multiplier = 1, axis = 0;
		bool keep_dims = false;
		int begin_mask = 0, end_mask = 0, ellipsis_mask = 0, new_axis_mask = 0, shrink_mask = 0;
		bool offset = false;
		// Done by another step.
		bool skip = false;
		// A convolution's: what's added and the PReLU done with it, and
		// the tensor it writes in the end.
		int residual = -1, slope = -1, result = -1;
		int run_at = -1; // where it runs, if not where the file has it
		float lo = 0, hi = 0;
		// Got ready once.
		std::vector<float> packed, bias, slopes;
		int pad_top = 0, pad_left = 0;
	};

	bool plain = false;
	bool ready = false;
	std::vector<Tensor> tensors;
	std::vector<Step> steps;
	std::vector<int> ins, outs;
	std::vector<std::vector<float>> slots;
	std::vector<float> zeros;
	const kernels::Table *table = nullptr;
	double macs = 0;
	bool timing = false;
	std::map<std::string, double> times;
	std::string problem;

	bool fail(const std::string &why)
	{
		if (problem.empty())
			problem = why;
		return false;
	}

	bool read(const uint8_t *bytes, size_t size);
	bool check(Step &s);
	void fuse();
	bool prepare(Step &s);
	void plan();
	void exec(const Step &s);

	Tensor &t(int i) { return tensors[(size_t)i]; }
	bool is_float(int i) const { return i >= 0 && tensors[(size_t)i].type == TF_FLOAT32; }
	// A constant int32 tensor's values (empty if it isn't one).
	const std::vector<int> &ints(int i)
	{
		static const std::vector<int> none;
		return i >= 0 && tensors[(size_t)i].constant && tensors[(size_t)i].type == TF_INT32
			       ? tensors[(size_t)i].ints
			       : none;
	}
};

bool Net::Impl::read(const uint8_t *bytes, size_t size)
{
	Flat f;
	f.b = bytes;
	f.n = size;
	if (size < 8)
		return fail("the model file is empty");
	if (std::memcmp(bytes + 4, "TFL3", 4) != 0)
		return fail("it isn't a TensorFlow Lite model");
	// The root table: where the first four bytes say.
	const size_t model = f.get<uint32_t>(0) < size ? f.get<uint32_t>(0) : 0;
	uint32_t n_codes, n_graphs, n_buffers;
	const size_t codes = f.vec(model, 1, n_codes, 4);
	const size_t graphs = f.vec(model, 2, n_graphs, 4);
	const size_t buffers = f.vec(model, 4, n_buffers, 4);
	if (f.bad || !model || n_graphs < 1)
		return fail("the model file is damaged");
	std::vector<int> op_codes(n_codes);
	for (uint32_t i = 0; i < n_codes; i++) {
		const size_t c = f.item(codes, i);
		const int old = f.scalar<int8_t>(c, 0, 0);
		const int code = f.scalar<int32_t>(c, 3, 0);
		op_codes[i] = std::max(old, code);
	}
	const size_t graph = f.item(graphs, 0);
	uint32_t n_tensors, n_ops;
	const size_t tensor_list = f.vec(graph, 0, n_tensors, 4);
	const size_t op_list = f.vec(graph, 3, n_ops, 4);
	ins = f.ints(graph, 1);
	outs = f.ints(graph, 2);
	if (f.bad)
		return fail("the model file is damaged");

	tensors.resize(n_tensors);
	for (uint32_t i = 0; i < n_tensors; i++) {
		const size_t tt = f.item(tensor_list, i);
		Tensor &x = tensors[i];
		x.shape = f.ints(tt, 0);
		x.type = f.scalar<int8_t>(tt, 1, 0);
		x.name = f.text(tt, 3);
		const uint32_t buffer = f.scalar<uint32_t>(tt, 2, 0);
		if (x.type != TF_FLOAT32 && x.type != TF_FLOAT16 && x.type != TF_INT32)
			return fail("tensor " + x.name + " is of a kind this engine doesn't know");
		for (int d : x.shape)
			if (d < 0 || d > (1 << 24))
				return fail("tensor " + x.name + " has a size that isn't fixed");
		if (buffer == 0 || buffer >= n_buffers)
			continue;
		const size_t buf = f.item(buffers, buffer);
		uint32_t bytes_n;
		const size_t data = f.vec(buf, 0, bytes_n, 1);
		if (!data || !bytes_n) {
			if (f.scalar<uint64_t>(buf, 1, 0))
				return fail("the model keeps its weights outside the file");
			continue;
		}
		const size_t want = x.count() * (x.type == TF_FLOAT16 ? 2 : 4);
		if (bytes_n != want)
			return fail("tensor " + x.name + " has the wrong amount of data");
		x.constant = true;
		const uint8_t *p = bytes + data;
		if (x.type == TF_FLOAT32) {
			x.values.resize(x.count());
			std::memcpy(x.values.data(), p, want);
		} else if (x.type == TF_FLOAT16) {
			x.values.resize(x.count());
			for (size_t k = 0; k < x.values.size(); k++) {
				uint16_t h;
				std::memcpy(&h, p + 2 * k, 2);
				x.values[k] = half_to_float(h);
			}
		} else {
			x.ints.resize(x.count());
			std::memcpy(x.ints.data(), p, want);
		}
	}
	for (int i : ins)
		if (i < 0 || i >= (int)n_tensors || !is_float(i))
			return fail("the model's input isn't a list of floats");
	for (int i : outs)
		if (i < 0 || i >= (int)n_tensors)
			return fail("the model's outputs are damaged");

	for (uint32_t i = 0; i < n_ops; i++) {
		const size_t o = f.item(op_list, i);
		const uint32_t index = f.scalar<uint32_t>(o, 0, 0);
		if (index >= op_codes.size())
			return fail("the model file is damaged");
		Step s;
		s.code = op_codes[index];
		s.in = f.ints(o, 1);
		s.out = f.ints(o, 2);
		for (int k : s.in)
			if (k < -1 || k >= (int)n_tensors)
				return fail("the model file is damaged");
		for (int k : s.out)
			if (k < 0 || k >= (int)n_tensors)
				return fail("the model file is damaged");
		if (!op_name(s.code))
			return fail("the model has a step (" + std::to_string(s.code) + ") this engine can't do");
		const size_t opt = f.table(o, 4);
		switch (s.code) {
		case TF_CONV_2D:
			s.padding = f.scalar<int8_t>(opt, 0, 0);
			s.stride_w = f.scalar<int32_t>(opt, 1, 1);
			s.stride_h = f.scalar<int32_t>(opt, 2, 1);
			s.act = f.scalar<int8_t>(opt, 3, 0);
			s.dil_w = f.scalar<int32_t>(opt, 4, 1);
			s.dil_h = f.scalar<int32_t>(opt, 5, 1);
			break;
		case TF_DEPTHWISE_CONV_2D:
			s.padding = f.scalar<int8_t>(opt, 0, 0);
			s.stride_w = f.scalar<int32_t>(opt, 1, 1);
			s.stride_h = f.scalar<int32_t>(opt, 2, 1);
			s.multiplier = f.scalar<int32_t>(opt, 3, 1);
			s.act = f.scalar<int8_t>(opt, 4, 0);
			s.dil_w = f.scalar<int32_t>(opt, 5, 1);
			s.dil_h = f.scalar<int32_t>(opt, 6, 1);
			break;
		case TF_MAX_POOL_2D:
			s.padding = f.scalar<int8_t>(opt, 0, 0);
			s.stride_w = f.scalar<int32_t>(opt, 1, 1);
			s.stride_h = f.scalar<int32_t>(opt, 2, 1);
			s.filter_w = f.scalar<int32_t>(opt, 3, 1);
			s.filter_h = f.scalar<int32_t>(opt, 4, 1);
			s.act = f.scalar<int8_t>(opt, 5, 0);
			break;
		case TF_ADD:
		case TF_SUB:
		case TF_MUL:
		case TF_DIV:
			s.act = f.scalar<int8_t>(opt, 0, 0);
			break;
		case TF_CONCATENATION:
			s.axis = f.scalar<int32_t>(opt, 0, 0);
			s.act = f.scalar<int8_t>(opt, 1, 0);
			break;
		case TF_MEAN:
		case TF_SUM:
			s.keep_dims = f.scalar<uint8_t>(opt, 0, 0) != 0;
			break;
		case TF_STRIDED_SLICE:
			s.begin_mask = f.scalar<int32_t>(opt, 0, 0);
			s.end_mask = f.scalar<int32_t>(opt, 1, 0);
			s.ellipsis_mask = f.scalar<int32_t>(opt, 2, 0);
			s.new_axis_mask = f.scalar<int32_t>(opt, 3, 0);
			s.shrink_mask = f.scalar<int32_t>(opt, 4, 0);
			s.offset = f.scalar<uint8_t>(opt, 5, 0) != 0;
			break;
		default:
			break;
		}
		if (f.bad)
			return fail("the model file is damaged");
		if (s.code == TF_DEQUANTIZE) {
			// Half-size weights: made floats now, once.
			if (s.in.size() != 1 || s.out.size() != 1 || s.in[0] < 0 || !t(s.in[0]).constant ||
			    t(s.in[0]).type != TF_FLOAT16 || t(s.out[0]).type != TF_FLOAT32)
				return fail(
					"the model works out half-size numbers as it goes, which this engine can't do");
			Tensor &to = t(s.out[0]);
			if (to.count() != t(s.in[0]).count())
				return fail("the model file is damaged");
			to.constant = true;
			to.values = t(s.in[0]).values;
			continue;
		}
		steps.push_back(std::move(s));
	}
	return true;
}

// Each step's inputs are the kind it takes, and its answer the shape the
// file says.
bool Net::Impl::check(Step &s)
{
	const std::string what = op_name(s.code);
	auto need = [&](size_t n) {
		if (s.in.size() < n || s.out.size() != 1)
			return fail(what + " has the wrong number of inputs");
		for (size_t i = 0; i < n; i++)
			if (s.in[i] < 0)
				return fail(what + " is missing an input");
		return true;
	};
	auto expect = [&](const std::vector<int> &shape) {
		if (shape != t(s.out[0]).shape)
			return fail(what + " gives " + shape_text(shape) + " where the file says " +
				    shape_text(t(s.out[0]).shape));
		return true;
	};
	auto floats = [&](std::initializer_list<int> which) {
		for (int i : which)
			if (!is_float(i))
				return fail(what + " needs floats");
		return true;
	};
	switch (s.code) {
	case TF_CONV_2D:
	case TF_DEPTHWISE_CONV_2D: {
		if (!need(2) || !floats({s.in[0], s.in[1], s.out[0]}))
			return false;
		const auto &x = t(s.in[0]).shape, &w = t(s.in[1]).shape;
		if (x.size() != 4 || w.size() != 4 || x[0] != 1 || !t(s.in[1]).constant)
			return fail(what + " has shapes this engine can't do");
		if (s.in.size() > 2 && s.in[2] >= 0 && (!t(s.in[2]).constant || !is_float(s.in[2])))
			return fail(what + "'s bias isn't fixed");
		if (s.stride_w < 1 || s.stride_h < 1 || s.dil_w < 1 || s.dil_h < 1 || s.stride_w > 8 ||
		    s.stride_h > 8 || s.dil_w > 8 || s.dil_h > 8)
			return fail(what + " has steps this engine can't do");
		int out_c;
		if (s.code == TF_CONV_2D) {
			if (w[3] != x[3])
				return fail("CONV_2D's filter doesn't fit its input");
			s.filter_h = w[1];
			s.filter_w = w[2];
			out_c = w[0];
		} else {
			if (w[0] != 1 || s.multiplier != 1 || w[3] != x[3])
				return fail("DEPTHWISE_CONV_2D with a multiplier isn't something this engine can do");
			s.filter_h = w[1];
			s.filter_w = w[2];
			out_c = w[3];
		}
		if (s.filter_h * s.filter_w > kernels::MOST_TAPS || s.filter_h < 1 || s.filter_w < 1)
			return fail(what + "'s filter is too big for this engine");
		if (s.in.size() > 2 && s.in[2] >= 0 && t(s.in[2]).count() != (size_t)out_c)
			return fail(what + "'s bias doesn't fit");
		const int oh = out_size(s.padding, x[1], s.stride_h, s.filter_h, s.dil_h);
		const int ow = out_size(s.padding, x[2], s.stride_w, s.filter_w, s.dil_w);
		if (oh < 1 || ow < 1)
			return fail(what + "'s input is too small");
		return expect({1, oh, ow, out_c});
	}
	case TF_MAX_POOL_2D: {
		if (!need(1) || !floats({s.in[0], s.out[0]}))
			return false;
		const auto &x = t(s.in[0]).shape;
		if (x.size() != 4 || x[0] != 1 || s.filter_w < 1 || s.filter_h < 1 || s.stride_w < 1 || s.stride_h < 1)
			return fail("MAX_POOL_2D has shapes this engine can't do");
		return expect({1, out_size(s.padding, x[1], s.stride_h, s.filter_h, 1),
			       out_size(s.padding, x[2], s.stride_w, s.filter_w, 1), x[3]});
	}
	case TF_ADD:
	case TF_SUB:
	case TF_MUL:
	case TF_DIV:
	case TF_SQUARED_DIFFERENCE:
	case TF_PRELU: {
		if (!need(2) || !floats({s.in[0], s.in[1], s.out[0]}))
			return false;
		const auto &a = t(s.in[0]).shape, &b = t(s.in[1]).shape;
		const int rank = (int)std::max(a.size(), b.size());
		if (rank > MOST_DIMS)
			return fail(what + " has too many dimensions");
		int da[MOST_DIMS], db[MOST_DIMS];
		align(a, rank, da);
		align(b, rank, db);
		std::vector<int> shape((size_t)rank);
		for (int i = 0; i < rank; i++) {
			if (da[i] != db[i] && da[i] != 1 && db[i] != 1)
				return fail(what + "'s inputs don't fit together");
			shape[(size_t)i] = da[i] == 1 ? db[i] : da[i];
		}
		if (s.code == TF_PRELU && count_of(shape) != count_of(a))
			return fail("PRELU's slopes are bigger than its input");
		return expect(shape);
	}
	case TF_RELU:
	case TF_RELU6:
	case TF_LOGISTIC:
	case TF_NEG:
	case TF_SQRT:
	case TF_RSQRT:
		if (!need(1) || !floats({s.in[0], s.out[0]}))
			return false;
		return expect(t(s.in[0]).shape);
	case TF_RESHAPE:
		if (!need(1) || !floats({s.in[0], s.out[0]}))
			return false;
		if (t(s.in[0]).count() != t(s.out[0]).count())
			return fail("RESHAPE changes the size");
		return true;
	case TF_PAD: {
		if (!need(2) || !floats({s.in[0], s.out[0]}))
			return false;
		const auto &x = t(s.in[0]).shape;
		const auto &p = ints(s.in[1]);
		if (x.size() > 4 || p.size() != 2 * x.size())
			return fail("PAD's padding isn't fixed");
		std::vector<int> shape = x;
		for (size_t i = 0; i < x.size(); i++) {
			if (p[2 * i] < 0 || p[2 * i + 1] < 0)
				return fail("PAD takes away, which this engine can't do");
			shape[i] += p[2 * i] + p[2 * i + 1];
		}
		return expect(shape);
	}
	case TF_CONCATENATION: {
		if (s.in.empty() || s.out.size() != 1 || !floats({s.out[0]}))
			return fail("CONCATENATION has the wrong inputs");
		const auto &first = t(s.in[0]).shape;
		const int rank = (int)first.size();
		int axis = s.axis < 0 ? s.axis + rank : s.axis;
		if (axis < 0 || axis >= rank)
			return fail("CONCATENATION's axis is off the end");
		s.axis = axis;
		std::vector<int> shape = first;
		shape[(size_t)axis] = 0;
		for (int i : s.in) {
			if (!is_float(i) || (int)t(i).shape.size() != rank)
				return fail("CONCATENATION's inputs don't fit together");
			for (int d = 0; d < rank; d++)
				if (d != axis && t(i).shape[(size_t)d] != first[(size_t)d])
					return fail("CONCATENATION's inputs don't fit together");
			shape[(size_t)axis] += t(i).shape[(size_t)axis];
		}
		return expect(shape);
	}
	case TF_MEAN:
	case TF_SUM: {
		if (!need(2) || !floats({s.in[0], s.out[0]}))
			return false;
		const auto &x = t(s.in[0]).shape;
		const auto &axes = ints(s.in[1]);
		if (axes.empty() || x.size() > MOST_DIMS)
			return fail(what + "'s axes aren't fixed");
		std::vector<bool> gone(x.size(), false);
		for (int a : axes) {
			const int k = a < 0 ? a + (int)x.size() : a;
			if (k < 0 || k >= (int)x.size())
				return fail(what + "'s axis is off the end");
			gone[(size_t)k] = true;
		}
		std::vector<int> shape;
		for (size_t i = 0; i < x.size(); i++)
			if (!gone[i])
				shape.push_back(x[i]);
			else if (s.keep_dims)
				shape.push_back(1);
		return expect(shape);
	}
	case TF_TRANSPOSE: {
		if (!need(2) || !floats({s.in[0], s.out[0]}))
			return false;
		const auto &x = t(s.in[0]).shape;
		const auto &perm = ints(s.in[1]);
		if (perm.size() != x.size() || x.size() > MOST_DIMS)
			return fail("TRANSPOSE's order isn't fixed");
		std::vector<int> shape(x.size());
		std::vector<bool> used(x.size(), false);
		for (size_t i = 0; i < x.size(); i++) {
			if (perm[i] < 0 || perm[i] >= (int)x.size() || used[(size_t)perm[i]])
				return fail("TRANSPOSE's order is damaged");
			used[(size_t)perm[i]] = true;
			shape[i] = x[(size_t)perm[i]];
		}
		return expect(shape);
	}
	case TF_STRIDED_SLICE: {
		if (!need(4) || !floats({s.in[0], s.out[0]}))
			return false;
		const auto &x = t(s.in[0]).shape;
		const auto &b = ints(s.in[1]), &e = ints(s.in[2]), &st = ints(s.in[3]);
		if (b.size() != x.size() || e.size() != x.size() || st.size() != x.size() || x.size() > MOST_DIMS)
			return fail("STRIDED_SLICE's numbers aren't fixed");
		if (s.ellipsis_mask || s.new_axis_mask || s.offset)
			return fail("STRIDED_SLICE with an ellipsis or new axes isn't something this engine can do");
		for (int v : st)
			if (v != 1)
				return fail("STRIDED_SLICE with steps isn't something this engine can do");
		size_t n = 1;
		for (size_t i = 0; i < x.size(); i++) {
			int from = (s.begin_mask >> i) & 1 ? 0 : b[i];
			if (from < 0)
				from += x[i];
			from = std::clamp(from, 0, x[i]);
			int to;
			if ((s.shrink_mask >> i) & 1) {
				to = from + 1;
			} else {
				to = (s.end_mask >> i) & 1 ? x[i] : e[i];
				if (to < 0)
					to += x[i];
				to = std::clamp(to, 0, x[i]);
			}
			n *= (size_t)std::max(0, to - from);
		}
		if (n != t(s.out[0]).count())
			return fail("STRIDED_SLICE gives a different size from the file's");
		return true;
	}
	default:
		return fail(what + " isn't something this engine can do");
	}
}

// Steps done together: a convolution, then a skip-connection added to it,
// then a PReLU or ReLU - one pass over the answer instead of three.
void Net::Impl::fuse()
{
	std::vector<int> users(tensors.size(), 0), made_by(tensors.size(), -1);
	for (size_t k = 0; k < steps.size(); k++) {
		for (int i : steps[k].in)
			if (i >= 0)
				users[(size_t)i]++;
		for (int o : steps[k].out)
			made_by[(size_t)o] = (int)k;
	}
	for (int o : outs)
		users[(size_t)o] += 1000; // never folded away
	auto only_user = [&](int tensor, size_t after) -> int {
		if (users[(size_t)tensor] != 1)
			return -1;
		for (size_t k = after + 1; k < steps.size(); k++)
			for (int i : steps[k].in)
				if (i == tensor)
					return steps[k].skip ? -1 : (int)k;
		return -1;
	};
	for (size_t k = 0; k < steps.size(); k++) {
		Step &s = steps[k];
		if (s.skip || (s.code != TF_CONV_2D && s.code != TF_DEPTHWISE_CONV_2D))
			continue;
		s.result = s.out[0];
		bool clamped = s.act != TF_ACT_NONE;
		// + a skip-connection (convolutions only).
		int next = only_user(s.result, k);
		if (next >= 0 && !clamped && s.code == TF_CONV_2D && steps[(size_t)next].code == TF_ADD) {
			Step &add = steps[(size_t)next];
			const int other = add.in[0] == s.result ? add.in[1] : add.in[0];
			// What's added must be worked out before the add; if that's
			// after the convolution, the convolution waits for it.
			const bool ready_before = other >= 0 && !t(other).constant &&
						  (made_by[(size_t)other] < next ||
						   std::find(ins.begin(), ins.end(), other) != ins.end());
			if (add.in[0] != add.in[1] && ready_before && t(other).shape == t(s.result).shape) {
				if (made_by[(size_t)other] > (int)k)
					s.run_at = next;
				s.residual = other;
				s.act = add.act;
				clamped = add.act != TF_ACT_NONE;
				add.skip = true;
				s.result = add.out[0];
				next = only_user(s.result, (size_t)next);
			}
		}
		// Then a PReLU (slopes per channel) or a ReLU.
		if (next >= 0 && !clamped) {
			Step &after = steps[(size_t)next];
			const int channels = t(s.result).shape.back();
			if (after.code == TF_PRELU && after.in[0] == s.result && t(after.in[1]).constant &&
			    t(after.in[1]).count() == (size_t)channels && t(after.in[1]).shape.back() == channels) {
				s.slope = after.in[1];
				after.skip = true;
				s.result = after.out[0];
			} else if ((after.code == TF_RELU || after.code == TF_RELU6) && after.in[0] == s.result) {
				s.act = after.code == TF_RELU ? TF_ACT_RELU : TF_ACT_RELU6;
				after.skip = true;
				s.result = after.out[0];
			}
		}
	}
	// A convolution that waits runs just before the add it took over.
	std::vector<std::pair<int, int>> order;
	for (size_t k = 0; k < steps.size(); k++)
		order.push_back({steps[k].run_at >= 0 ? steps[k].run_at : (int)k, (int)k});
	std::stable_sort(order.begin(), order.end());
	std::vector<Step> sorted;
	sorted.reserve(steps.size());
	for (const auto &o : order)
		sorted.push_back(std::move(steps[(size_t)o.second]));
	steps.swap(sorted);
}

// Weights packed for the kernels, padding worked out.
bool Net::Impl::prepare(Step &s)
{
	if (s.result < 0)
		s.result = s.out[0];
	clamp_range(s.act, s.lo, s.hi);
	if (s.code != TF_CONV_2D && s.code != TF_DEPTHWISE_CONV_2D) {
		if (s.code == TF_MAX_POOL_2D) {
			const auto &x = t(s.in[0]).shape, &y = t(s.out[0]).shape;
			s.pad_top = s.padding == TF_SAME ? same_pad(x[1], y[1], s.stride_h, s.filter_h, 1) : 0;
			s.pad_left = s.padding == TF_SAME ? same_pad(x[2], y[2], s.stride_w, s.filter_w, 1) : 0;
		}
		return true;
	}
	const auto &x = t(s.in[0]).shape, &y = t(s.out[0]).shape;
	const Tensor &w = t(s.in[1]);
	const int out_c = y[3], in_c = x[3];
	s.pad_top = s.padding == TF_SAME ? same_pad(x[1], y[1], s.stride_h, s.filter_h, s.dil_h) : 0;
	s.pad_left = s.padding == TF_SAME ? same_pad(x[2], y[2], s.stride_w, s.filter_w, s.dil_w) : 0;
	const int panels = (out_c + kernels::PANEL - 1) / kernels::PANEL;
	const size_t rounded = (size_t)panels * kernels::PANEL;
	s.bias.assign(rounded, 0.0f);
	if (s.in.size() > 2 && s.in[2] >= 0)
		std::copy(t(s.in[2]).values.begin(), t(s.in[2]).values.end(), s.bias.begin());
	if (s.slope >= 0) {
		s.slopes.assign(rounded, 1.0f);
		std::copy(t(s.slope).values.begin(), t(s.slope).values.end(), s.slopes.begin());
	}
	if (s.code == TF_DEPTHWISE_CONV_2D) {
		s.packed = w.values; // [1][kh][kw][c] is what the kernel takes
		macs += (double)count_of(y) * s.filter_h * s.filter_w;
		return true;
	}
	// [out_c][kh][kw][in_c] -> [panel][kh][kw][in_c][8].
	const size_t taps = (size_t)s.filter_h * s.filter_w;
	s.packed.assign(rounded * taps * in_c, 0.0f);
	for (int o = 0; o < out_c; o++) {
		const int panel = o / kernels::PANEL, lane = o % kernels::PANEL;
		for (size_t tap = 0; tap < taps; tap++)
			for (int c = 0; c < in_c; c++)
				s.packed[(((size_t)panel * taps + tap) * in_c + c) * kernels::PANEL + lane] =
					w.values[((size_t)o * taps + tap) * in_c + c];
	}
	if (zeros.size() < (size_t)in_c)
		zeros.assign((size_t)in_c, 0.0f);
	macs += (double)count_of(y) * (double)taps * in_c;
	return true;
}

// Where each tensor lives during a run: a tensor's place is given to a
// later one once nothing else reads it.
void Net::Impl::plan()
{
	const size_t n = tensors.size();
	std::vector<int> last(n, -1);
	std::vector<bool> keep(n, false);
	for (int i : ins)
		keep[(size_t)i] = true;
	for (int o : outs)
		keep[(size_t)o] = true;
	for (size_t k = 0; k < steps.size(); k++) {
		if (steps[k].skip)
			continue;
		for (int i : steps[k].in)
			if (i >= 0)
				last[(size_t)i] = (int)k;
		if (steps[k].residual >= 0)
			last[(size_t)steps[k].residual] = (int)k;
	}
	std::vector<size_t> capacity;
	std::vector<bool> free_slot;
	auto take = [&](int tensor) {
		Tensor &x = t(tensor);
		if (x.constant || x.slot >= 0)
			return;
		const size_t need = std::max<size_t>(1, x.count());
		int best = -1;
		if (!plain && !keep[(size_t)tensor]) {
			for (size_t s = 0; s < capacity.size(); s++)
				if (free_slot[s] && capacity[s] >= need &&
				    (best < 0 || capacity[s] < capacity[(size_t)best]))
					best = (int)s;
			if (best < 0)
				for (size_t s = 0; s < capacity.size(); s++)
					if (free_slot[s] && (best < 0 || capacity[s] > capacity[(size_t)best]))
						best = (int)s;
		}
		if (best < 0) {
			capacity.push_back(need);
			free_slot.push_back(false);
			best = (int)capacity.size() - 1;
		}
		capacity[(size_t)best] = std::max(capacity[(size_t)best], need);
		free_slot[(size_t)best] = false;
		x.slot = best;
	};
	for (int i : ins)
		take(i);
	for (size_t k = 0; k < steps.size(); k++) {
		const Step &s = steps[k];
		if (s.skip)
			continue;
		take(s.result >= 0 ? s.result : s.out[0]);
		if (plain || keep.empty())
			continue;
		for (int i : s.in)
			if (i >= 0 && last[(size_t)i] == (int)k && !keep[(size_t)i] && t(i).slot >= 0)
				free_slot[(size_t)t(i).slot] = true;
		if (s.residual >= 0 && last[(size_t)s.residual] == (int)k && !keep[(size_t)s.residual] &&
		    t(s.residual).slot >= 0)
			free_slot[(size_t)t(s.residual).slot] = true;
	}
	slots.assign(capacity.size(), {});
	for (size_t s = 0; s < capacity.size(); s++)
		slots[s].assign(capacity[s], 0.0f);
	for (Tensor &x : tensors) {
		if (x.constant)
			x.data = x.type == TF_INT32 ? nullptr : x.values.data();
		else if (x.slot >= 0)
			x.data = slots[(size_t)x.slot].data();
	}
}

namespace {

// a op b over two broadcast shapes, the last dimension done as a run.
template<typename F>
void broadcast(const float *a, const std::vector<int> &sa, const float *b, const std::vector<int> &sb, float *out,
	       const std::vector<int> &so, F op)
{
	const int rank = (int)so.size();
	if (sa == sb) {
		const size_t n = count_of(so);
		for (size_t i = 0; i < n; i++)
			out[i] = op(a[i], b[i]);
		return;
	}
	if (rank == 0) {
		out[0] = op(a[0], b[0]);
		return;
	}
	int da[MOST_DIMS], db[MOST_DIMS], dout[MOST_DIMS];
	align(sa, rank, da);
	align(sb, rank, db);
	align(so, rank, dout);
	size_t stride_a[MOST_DIMS], stride_b[MOST_DIMS];
	size_t step_a = 1, step_b = 1;
	for (int i = rank - 1; i >= 0; i--) {
		stride_a[i] = da[i] == 1 ? 0 : step_a;
		stride_b[i] = db[i] == 1 ? 0 : step_b;
		step_a *= (size_t)da[i];
		step_b *= (size_t)db[i];
	}
	const int inner = dout[rank - 1];
	const size_t ia = stride_a[rank - 1], ib = stride_b[rank - 1];
	const size_t rows = count_of(so) / (size_t)std::max(1, inner);
	int index[MOST_DIMS] = {0};
	for (size_t r = 0; r < rows; r++) {
		size_t oa = 0, ob = 0;
		for (int d = 0; d < rank - 1; d++) {
			oa += (size_t)index[d] * stride_a[d];
			ob += (size_t)index[d] * stride_b[d];
		}
		const float *pa = a + oa, *pb = b + ob;
		float *po = out + r * (size_t)inner;
		for (int i = 0; i < inner; i++)
			po[i] = op(pa[(size_t)i * ia], pb[(size_t)i * ib]);
		for (int d = rank - 2; d >= 0; d--) {
			if (++index[d] < dout[d])
				break;
			index[d] = 0;
		}
	}
}

} // namespace

void Net::Impl::exec(const Step &s)
{
	const Tensor &x = t(s.in[0]);
	Tensor &y = t(s.result >= 0 ? s.result : s.out[0]);
	const float lo = s.lo, hi = s.hi;
	auto clamp = [lo, hi](float v) {
		return std::min(hi, std::max(lo, v));
	};
	switch (s.code) {
	case TF_CONV_2D: {
		kernels::Conv j;
		j.in = x.data;
		j.in_h = x.shape[1];
		j.in_w = x.shape[2];
		j.in_c = x.shape[3];
		const auto &o = t(s.out[0]).shape;
		j.out = y.data;
		j.out_h = o[1];
		j.out_w = o[2];
		j.out_c = o[3];
		j.weights = s.packed.data();
		j.bias = s.bias.data();
		j.kh = s.filter_h;
		j.kw = s.filter_w;
		j.stride_h = s.stride_h;
		j.stride_w = s.stride_w;
		j.dil_h = s.dil_h;
		j.dil_w = s.dil_w;
		j.pad_top = s.pad_top;
		j.pad_left = s.pad_left;
		j.residual = s.residual >= 0 ? t(s.residual).data : nullptr;
		j.slope = s.slope >= 0 ? s.slopes.data() : nullptr;
		j.lo = lo;
		j.hi = hi;
		j.zeros = zeros.data();
		j.row_from = 0;
		j.row_to = j.out_h;
		table->conv(j);
		return;
	}
	case TF_DEPTHWISE_CONV_2D: {
		kernels::Depthwise j;
		j.in = x.data;
		j.in_h = x.shape[1];
		j.in_w = x.shape[2];
		j.c = x.shape[3];
		const auto &o = t(s.out[0]).shape;
		j.out = y.data;
		j.out_h = o[1];
		j.out_w = o[2];
		j.weights = s.packed.data();
		j.bias = s.bias.data();
		j.kh = s.filter_h;
		j.kw = s.filter_w;
		j.stride_h = s.stride_h;
		j.stride_w = s.stride_w;
		j.dil_h = s.dil_h;
		j.dil_w = s.dil_w;
		j.pad_top = s.pad_top;
		j.pad_left = s.pad_left;
		j.slope = s.slope >= 0 ? s.slopes.data() : nullptr;
		j.lo = lo;
		j.hi = hi;
		j.row_from = 0;
		j.row_to = j.out_h;
		table->depthwise(j);
		return;
	}
	case TF_MAX_POOL_2D: {
		const auto &o = y.shape;
		const int ih = x.shape[1], iw = x.shape[2], c = x.shape[3];
		for (int oy = 0; oy < o[1]; oy++)
			for (int ox = 0; ox < o[2]; ox++) {
				float *out = y.data + ((size_t)oy * o[2] + ox) * c;
				for (int k = 0; k < c; k++)
					out[k] = -std::numeric_limits<float>::infinity();
				for (int ky = 0; ky < s.filter_h; ky++) {
					const int iy = oy * s.stride_h - s.pad_top + ky;
					if (iy < 0 || iy >= ih)
						continue;
					for (int kx = 0; kx < s.filter_w; kx++) {
						const int ix = ox * s.stride_w - s.pad_left + kx;
						if (ix < 0 || ix >= iw)
							continue;
						const float *in = x.data + ((size_t)iy * iw + ix) * c;
						for (int k = 0; k < c; k++)
							out[k] = std::max(out[k], in[k]);
					}
				}
				for (int k = 0; k < c; k++)
					out[k] = clamp(out[k]);
			}
		return;
	}
	case TF_ADD:
	case TF_SUB:
	case TF_MUL:
	case TF_DIV:
	case TF_SQUARED_DIFFERENCE:
	case TF_PRELU: {
		const Tensor &b = t(s.in[1]);
		switch (s.code) {
		case TF_ADD:
			broadcast(x.data, x.shape, b.data, b.shape, y.data, y.shape,
				  [&](float p, float q) { return clamp(p + q); });
			break;
		case TF_SUB:
			broadcast(x.data, x.shape, b.data, b.shape, y.data, y.shape,
				  [&](float p, float q) { return clamp(p - q); });
			break;
		case TF_MUL:
			broadcast(x.data, x.shape, b.data, b.shape, y.data, y.shape,
				  [&](float p, float q) { return clamp(p * q); });
			break;
		case TF_DIV:
			broadcast(x.data, x.shape, b.data, b.shape, y.data, y.shape,
				  [&](float p, float q) { return clamp(p / q); });
			break;
		case TF_SQUARED_DIFFERENCE:
			broadcast(x.data, x.shape, b.data, b.shape, y.data, y.shape, [](float p, float q) {
				const float d = p - q;
				return d * d;
			});
			break;
		default:
			broadcast(x.data, x.shape, b.data, b.shape, y.data, y.shape,
				  [](float p, float q) { return p >= 0 ? p : p * q; });
			break;
		}
		return;
	}
	case TF_RELU:
	case TF_RELU6:
	case TF_LOGISTIC:
	case TF_NEG:
	case TF_SQRT:
	case TF_RSQRT: {
		const size_t n = x.count();
		const float *in = x.data;
		float *out = y.data;
		switch (s.code) {
		case TF_RELU:
			for (size_t i = 0; i < n; i++)
				out[i] = std::max(0.0f, in[i]);
			break;
		case TF_RELU6:
			for (size_t i = 0; i < n; i++)
				out[i] = std::min(6.0f, std::max(0.0f, in[i]));
			break;
		case TF_LOGISTIC:
			for (size_t i = 0; i < n; i++)
				out[i] = 1.0f / (1.0f + std::exp(-in[i]));
			break;
		case TF_NEG:
			for (size_t i = 0; i < n; i++)
				out[i] = -in[i];
			break;
		case TF_SQRT:
			for (size_t i = 0; i < n; i++)
				out[i] = std::sqrt(in[i]);
			break;
		default:
			for (size_t i = 0; i < n; i++)
				out[i] = 1.0f / std::sqrt(in[i]);
			break;
		}
		return;
	}
	case TF_RESHAPE:
		std::memcpy(y.data, x.data, x.count() * sizeof(float));
		return;
	case TF_PAD: {
		const auto &p = t(s.in[1]).ints;
		int din[4], dout[4], before[4];
		const int pad = 4 - (int)x.shape.size();
		for (int i = 0; i < 4; i++) {
			const bool real = i >= pad;
			din[i] = real ? x.shape[(size_t)(i - pad)] : 1;
			before[i] = real ? p[(size_t)(2 * (i - pad))] : 0;
			dout[i] = real ? y.shape[(size_t)(i - pad)] : 1;
		}
		std::fill(y.data, y.data + y.count(), 0.0f);
		for (int a = 0; a < din[0]; a++)
			for (int b = 0; b < din[1]; b++)
				for (int c = 0; c < din[2]; c++) {
					const float *from = x.data + (((size_t)a * din[1] + b) * din[2] + c) * din[3];
					float *to = y.data +
						    ((((size_t)(a + before[0]) * dout[1] + (b + before[1])) * dout[2] +
						      (c + before[2])) *
							     dout[3] +
						     before[3]);
					std::memcpy(to, from, (size_t)din[3] * sizeof(float));
				}
		return;
	}
	case TF_CONCATENATION: {
		const auto &o = y.shape;
		size_t outer = 1;
		for (int d = 0; d < s.axis; d++)
			outer *= (size_t)o[(size_t)d];
		float *to = y.data;
		for (size_t r = 0; r < outer; r++)
			for (int i : s.in) {
				const Tensor &part = t(i);
				const size_t run = part.count() / std::max<size_t>(1, outer);
				std::memcpy(to, part.data + r * run, run * sizeof(float));
				to += run;
			}
		if (s.act != TF_ACT_NONE)
			for (size_t i = 0; i < y.count(); i++)
				y.data[i] = clamp(y.data[i]);
		return;
	}
	case TF_MEAN:
	case TF_SUM: {
		const int rank = (int)x.shape.size();
		bool gone[MOST_DIMS] = {false};
		for (int a : t(s.in[1]).ints)
			gone[a < 0 ? a + rank : a] = true;
		// Where each input lands: its index with the summed-up ones dropped.
		size_t to_stride[MOST_DIMS];
		size_t step = 1;
		for (int d = rank - 1; d >= 0; d--) {
			to_stride[d] = gone[d] ? 0 : step;
			if (!gone[d])
				step *= (size_t)x.shape[(size_t)d];
		}
		const size_t n_out = y.count(), n_in = x.count();
		std::vector<double> sum(n_out, 0.0);
		int index[MOST_DIMS] = {0};
		for (size_t i = 0; i < n_in; i++) {
			size_t at = 0;
			for (int d = 0; d < rank; d++)
				at += (size_t)index[d] * to_stride[d];
			sum[at] += x.data[i];
			for (int d = rank - 1; d >= 0; d--) {
				if (++index[d] < x.shape[(size_t)d])
					break;
				index[d] = 0;
			}
		}
		const double div = s.code == TF_MEAN ? (double)n_in / (double)std::max<size_t>(1, n_out) : 1.0;
		for (size_t i = 0; i < n_out; i++)
			y.data[i] = (float)(sum[i] / div);
		return;
	}
	case TF_TRANSPOSE: {
		const auto &perm = t(s.in[1]).ints;
		const int rank = (int)x.shape.size();
		size_t in_stride[MOST_DIMS];
		size_t step = 1;
		for (int d = rank - 1; d >= 0; d--) {
			in_stride[d] = step;
			step *= (size_t)x.shape[(size_t)d];
		}
		int index[MOST_DIMS] = {0};
		const size_t n = y.count();
		for (size_t i = 0; i < n; i++) {
			size_t from = 0;
			for (int d = 0; d < rank; d++)
				from += (size_t)index[d] * in_stride[perm[(size_t)d]];
			y.data[i] = x.data[from];
			for (int d = rank - 1; d >= 0; d--) {
				if (++index[d] < y.shape[(size_t)d])
					break;
				index[d] = 0;
			}
		}
		return;
	}
	case TF_STRIDED_SLICE: {
		const auto &b = t(s.in[1]).ints, &e = t(s.in[2]).ints;
		const int rank = (int)x.shape.size();
		int from[MOST_DIMS], to[MOST_DIMS];
		size_t in_stride[MOST_DIMS];
		size_t step = 1;
		for (int d = rank - 1; d >= 0; d--) {
			in_stride[d] = step;
			step *= (size_t)x.shape[(size_t)d];
		}
		for (int d = 0; d < rank; d++) {
			const int dim = x.shape[(size_t)d];
			from[d] = (s.begin_mask >> d) & 1 ? 0 : b[(size_t)d];
			if (from[d] < 0)
				from[d] += dim;
			from[d] = std::clamp(from[d], 0, dim);
			if ((s.shrink_mask >> d) & 1) {
				to[d] = from[d] + 1;
			} else {
				to[d] = (s.end_mask >> d) & 1 ? dim : e[(size_t)d];
				if (to[d] < 0)
					to[d] += dim;
				to[d] = std::clamp(to[d], 0, dim);
			}
		}
		int index[MOST_DIMS];
		for (int d = 0; d < rank; d++)
			index[d] = from[d];
		const size_t n = y.count();
		for (size_t i = 0; i < n; i++) {
			size_t at = 0;
			for (int d = 0; d < rank; d++)
				at += (size_t)index[d] * in_stride[d];
			y.data[i] = x.data[at];
			for (int d = rank - 1; d >= 0; d--) {
				if (++index[d] < to[d])
					break;
				index[d] = from[d];
			}
		}
		return;
	}
	default:
		return;
	}
}

Net::Net() : impl_(new Impl()) {}

Net::~Net()
{
	delete impl_;
}

Net::Net(Net &&other) noexcept : impl_(other.impl_)
{
	other.impl_ = new Impl();
}

Net &Net::operator=(Net &&other) noexcept
{
	std::swap(impl_, other.impl_);
	return *this;
}

void Net::set_plain(bool plain)
{
	impl_->plain = plain;
}

bool Net::load(const uint8_t *bytes, size_t size, std::string *problem)
{
	const bool plain = impl_->plain;
	delete impl_;
	impl_ = new Impl();
	impl_->plain = plain;
	Impl &m = *impl_;
	bool ok = bytes && m.read(bytes, size);
	for (size_t k = 0; ok && k < m.steps.size(); k++)
		ok = m.check(m.steps[k]);
	// Every input is there before the step that reads it.
	if (ok) {
		std::vector<bool> have(m.tensors.size(), false);
		for (size_t i = 0; i < m.tensors.size(); i++)
			have[i] = m.tensors[i].constant;
		for (int i : m.ins)
			have[(size_t)i] = true;
		for (const Impl::Step &s : m.steps) {
			for (int i : s.in)
				if (i >= 0 && !have[(size_t)i])
					ok = m.fail(std::string(op_name(s.code)) + " reads " +
						    m.tensors[(size_t)i].name + " before it's worked out");
			for (int o : s.out)
				have[(size_t)o] = true;
		}
		for (int o : m.outs)
			if (!have[(size_t)o])
				ok = m.fail("the model never works out its answer");
	}
	if (ok && !plain)
		m.fuse();
	for (size_t k = 0; ok && k < m.steps.size(); k++)
		if (!m.steps[k].skip)
			ok = m.prepare(m.steps[k]);
	if (!ok) {
		if (problem)
			*problem = m.problem.empty() ? "the model couldn't be read" : m.problem;
		const bool keep_plain = m.plain;
		delete impl_;
		impl_ = new Impl();
		impl_->plain = keep_plain;
		return false;
	}
	m.table = plain ? &kernels::plain() : &kernels::best();
	m.plan();
	m.ready = true;
	return true;
}

bool Net::loaded() const
{
	return impl_->ready;
}

size_t Net::inputs() const
{
	return impl_->ins.size();
}

size_t Net::outputs() const
{
	return impl_->outs.size();
}

const std::vector<int> &Net::input_shape(size_t i) const
{
	return impl_->tensors[(size_t)impl_->ins[i]].shape;
}

const std::vector<int> &Net::output_shape(size_t i) const
{
	return impl_->tensors[(size_t)impl_->outs[i]].shape;
}

float *Net::input(size_t i)
{
	return impl_->tensors[(size_t)impl_->ins[i]].data;
}

const float *Net::output(size_t i) const
{
	return impl_->tensors[(size_t)impl_->outs[i]].data;
}

void Net::run()
{
	if (!impl_->ready)
		return;
	Impl &m = *impl_;
	if (!m.timing) {
		for (const Impl::Step &s : m.steps)
			if (!s.skip)
				m.exec(s);
		return;
	}
	for (const Impl::Step &s : m.steps) {
		if (s.skip)
			continue;
		const auto start = std::chrono::steady_clock::now();
		m.exec(s);
		const auto end = std::chrono::steady_clock::now();
		std::string kind = op_name(s.code);
		if (s.code == TF_CONV_2D || s.code == TF_DEPTHWISE_CONV_2D)
			kind += " " + std::to_string(s.filter_h) + "x" + std::to_string(s.filter_w);
		m.times[kind] += std::chrono::duration<double, std::milli>(end - start).count();
	}
}

void Net::set_timing(bool on)
{
	impl_->timing = on;
	impl_->times.clear();
}

std::vector<std::pair<std::string, double>> Net::timing() const
{
	return {impl_->times.begin(), impl_->times.end()};
}

double Net::work() const
{
	return impl_->macs;
}

const float *Net::tensor(const std::string &name, std::vector<int> *shape) const
{
	const Impl &m = *impl_;
	for (size_t i = 0; i < m.tensors.size(); i++) {
		const Impl::Tensor &x = m.tensors[i];
		const bool kept = m.plain || x.constant ||
				  std::find(m.ins.begin(), m.ins.end(), (int)i) != m.ins.end() ||
				  std::find(m.outs.begin(), m.outs.end(), (int)i) != m.outs.end();
		if (x.name == name && x.data && kept) {
			if (shape)
				*shape = x.shape;
			return x.data;
		}
	}
	return nullptr;
}

} // namespace direbot::face::nn
