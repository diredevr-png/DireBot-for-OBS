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

#include "voice.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

#include <nlohmann/json.hpp>

namespace direbot::face {

const char *const VOWEL_NAMES[VOWELS] = {"a", "i", "u", "e", "o"};
const char *const VOWEL_VRM[VOWELS] = {"aa", "ih", "ou", "ee", "oh"};

namespace {

constexpr double PI = 3.14159265358979323846;
// The sound is brought down to about this many samples a second (a
// voice's first formants are well under 4,000 Hz).
constexpr double TARGET_RATE = 12000;
// Each look: the last 30 ms, every 10 ms.
constexpr double WINDOW_S = 0.030, HOP_S = 0.010;
// Linear prediction's order: 6 resonances in 6,000 Hz.
constexpr int ORDER = 12;
// How far a vowel's formants can be from its place and still match it
// (natural logs: 0.22 is about 25%).
constexpr float SIGMA1 = 0.22f, SIGMA2 = 0.20f;
// A voice: repeats itself at least this much (0 to 1) at its pitch, and
// at least this share of it under 4,500 Hz.
constexpr float VOICED_REPEAT = 0.5f, VOICED_LOW = 0.35f;
// Smoothing (seconds): the vowels' shares; the mouth opening, closing;
// talking starting, ending (after a hold between words).
constexpr float SHARE_TIME = 0.06f, OPEN_TIME = 0.025f, CLOSE_TIME = 0.07f;
constexpr float TALK_ON = 0.05f, TALK_OFF = 0.25f, TALK_HOLD = 0.3f;
// A sound that's loud but not a voice ("s", "t", "f") opens it this much,
// teeth together.
constexpr float HISS_OPEN = 0.4f;
// No sound for this long (seconds): no mic, quiet.
constexpr float GONE = 0.25f;
// The level bar's peak falls back this many dB a second.
constexpr float PEAK_FALL = 20; // as OBS's own meter's peak
constexpr size_t MOST_HEARD = 64;

float clamp01(float v)
{
	return std::min(1.0f, std::max(0.0f, v));
}
float mix(float a, float b, float t)
{
	return a + (b - a) * t;
}
// Eased towards `to` with a time constant `tau` (seconds).
float ease(float from, float to, float dt, float tau)
{
	if (tau <= 0)
		return to;
	return from + (to - from) * (1 - std::exp(-dt / tau));
}
float db_of(double mean_square)
{
	return (float)(10 * std::log10(std::max(mean_square, 1e-10)));
}

// An expression's number by its name (0, "_neutral", if none).
int expression_id(const char *name)
{
	for (int i = 1; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	return 0;
}

float median(std::vector<float> v)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	const size_t n = v.size();
	return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// A 6th-order Butterworth low-pass at `fc`, as three biquads (each b0 b1
// b2 a1 a2, a0 being 1).
void design_low_pass(double rate, double fc, float out[3][5])
{
	static const double Q[3] = {0.51763809, 0.70710678, 1.93185165};
	const double w0 = 2 * PI * fc / rate, c = std::cos(w0), s = std::sin(w0);
	for (int k = 0; k < 3; k++) {
		const double alpha = s / (2 * Q[k]), a0 = 1 + alpha;
		out[k][0] = (float)((1 - c) / 2 / a0);
		out[k][1] = (float)((1 - c) / a0);
		out[k][2] = (float)((1 - c) / 2 / a0);
		out[k][3] = (float)(-2 * c / a0);
		out[k][4] = (float)((1 - alpha) / a0);
	}
}

// Linear prediction (autocorrelation, Levinson-Durbin) of `x`: a[0] = 1,
// then a[1..order]. False if it's silent.
bool predict(const std::vector<double> &x, int order, double rate, double *a)
{
	const size_t n = x.size();
	std::vector<double> r(order + 1, 0.0);
	for (int k = 0; k <= order; k++) {
		double s = 0;
		for (size_t i = (size_t)k; i < n; i++)
			s += x[i] * x[i - k];
		// A little bandwidth on every resonance (a 60 Hz lag window) keeps
		// the prediction steady.
		const double g = 2 * PI * 60 * k / rate;
		r[k] = s * std::exp(-0.5 * g * g);
	}
	if (r[0] <= 1e-12)
		return false;
	r[0] *= 1.0001;
	std::vector<double> prev(order + 1, 0.0);
	for (int i = 0; i <= order; i++)
		a[i] = 0;
	a[0] = 1;
	double err = r[0];
	for (int i = 1; i <= order; i++) {
		double acc = r[i];
		for (int j = 1; j < i; j++)
			acc += a[j] * r[i - j];
		const double k = -acc / err;
		std::copy(a, a + order + 1, prev.begin());
		for (int j = 1; j < i; j++)
			a[j] = prev[j] + k * prev[i - j];
		a[i] = k;
		err *= 1 - k * k;
		if (err <= 1e-15)
			return false;
	}
	return true;
}

// The roots of z^n + a[1] z^(n-1) + ... + a[n] (Durand-Kerner).
std::vector<std::complex<double>> roots_of(const double *a, int n)
{
	using C = std::complex<double>;
	std::vector<C> z(n);
	const C seed(0.4, 0.9);
	C p(1, 0);
	for (int i = 0; i < n; i++) {
		z[i] = p;
		p *= seed;
	}
	auto value = [&](C x) {
		C v(1, 0);
		for (int i = 1; i <= n; i++)
			v = v * x + a[i];
		return v;
	};
	for (int it = 0; it < 120; it++) {
		double moved = 0;
		for (int i = 0; i < n; i++) {
			C d(1, 0);
			for (int j = 0; j < n; j++)
				if (j != i)
					d *= z[i] - z[j];
			if (std::abs(d) < 1e-300)
				d = C(1e-12, 0);
			const C step = value(z[i]) / d;
			z[i] -= step;
			moved = std::max(moved, std::abs(step));
		}
		if (moved < 1e-10)
			break;
	}
	return z;
}

// A voice's first two formants in `x` (pre-emphasised and windowed
// here), or false.
bool formants(const std::vector<double> &x, double rate, float &f1, float &f2)
{
	const size_t n = x.size();
	std::vector<double> y(n);
	for (size_t i = 0; i < n; i++) {
		const double pre = x[i] - (i ? 0.97 * x[i - 1] : 0.0);
		y[i] = pre * (0.54 - 0.46 * std::cos(2 * PI * i / (n - 1)));
	}
	double a[ORDER + 1];
	if (!predict(y, ORDER, rate, a))
		return false;
	const auto z = roots_of(a, ORDER);
	std::vector<float> found;
	for (const auto &r : z) {
		if (!std::isfinite(r.real()) || !std::isfinite(r.imag()) || r.imag() <= 0)
			continue;
		const double f = std::arg(r) * rate / (2 * PI);
		const double mag = std::abs(r);
		if (mag <= 0 || mag >= 1)
			continue;
		const double bw = -std::log(mag) * rate / PI;
		if (f < 150 || f > rate / 2 - 150 || bw > 600)
			continue;
		found.push_back((float)f);
	}
	std::sort(found.begin(), found.end());
	f1 = f2 = 0;
	size_t i = 0;
	while (i < found.size() && found[i] < 200)
		i++;
	if (i >= found.size() || found[i] > 1100)
		return false;
	f1 = found[i];
	for (size_t j = i + 1; j < found.size(); j++)
		if (found[j] >= f1 + 150) {
			if (found[j] <= 3200)
				f2 = found[j];
			break;
		}
	if (f2 <= 0) {
		f1 = 0;
		return false;
	}
	return true;
}

// How strongly `x` repeats itself at a voice's pitch (70 to 400 Hz): the
// best normalised match of it against itself moved on, 0 to 1. On every
// other sample (pitch needs no more).
float repeats(const std::vector<double> &x, double rate)
{
	std::vector<double> h;
	h.reserve(x.size() / 2 + 1);
	for (size_t i = 0; i < x.size(); i += 2)
		h.push_back(x[i]);
	const double hr = rate / 2;
	const int lo = std::max(2, (int)std::floor(hr / 400)), hi = (int)std::ceil(hr / 70);
	const int m = (int)h.size();
	float best = 0;
	for (int lag = lo; lag <= hi && lag < m - 8; lag++) {
		double xy = 0, xx = 0, yy = 0;
		for (int i = 0; i + lag < m; i++) {
			xy += h[i] * h[i + lag];
			xx += h[i] * h[i];
			yy += h[i + lag] * h[i + lag];
		}
		if (xx <= 1e-12 || yy <= 1e-12)
			continue;
		best = std::max(best, (float)(xy / std::sqrt(xx * yy)));
	}
	return best;
}

} // namespace

void vowel_match(float f1, float f2, const VoiceProfile &p, float out[VOWELS])
{
	double w[VOWELS], sum = 0;
	const double l1 = std::log(std::max(1.0f, f1)), l2 = std::log(std::max(1.0f, f2));
	for (int v = 0; v < VOWELS; v++) {
		const double d1 = (l1 - std::log(std::max(1.0f, p.f1[v]))) / SIGMA1;
		const double d2 = (l2 - std::log(std::max(1.0f, p.f2[v]))) / SIGMA2;
		w[v] = -0.5 * (d1 * d1 + d2 * d2);
	}
	// Shares of the best (so a sound far from every vowel still picks the
	// nearest), adding up to 1.
	const double top = *std::max_element(w, w + VOWELS);
	for (int v = 0; v < VOWELS; v++) {
		w[v] = std::exp(w[v] - top);
		sum += w[v];
	}
	for (int v = 0; v < VOWELS; v++)
		out[v] = (float)(w[v] / sum);
}

std::string profile_text(const VoiceProfile &p)
{
	nlohmann::json j = nlohmann::json::object();
	for (int v = 0; v < VOWELS; v++)
		j[VOWEL_NAMES[v]] = {std::lround(p.f1[v]), std::lround(p.f2[v])};
	return j.dump();
}

// A voice's first formant is somewhere 150 to 1,200 Hz and its second
// above it, under 3,500.
bool sensible_place(float f1, float f2)
{
	return std::isfinite(f1) && std::isfinite(f2) && f1 >= 150 && f1 <= 1200 && f2 >= f1 + 100 && f2 <= 3500;
}

bool profile_from(const std::string &text, VoiceProfile &p)
{
	const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
	if (!j.is_object())
		return false;
	VoiceProfile got;
	for (int v = 0; v < VOWELS; v++) {
		auto it = j.find(VOWEL_NAMES[v]);
		if (it == j.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number() ||
		    !(*it)[1].is_number())
			return false;
		const float f1 = (*it)[0].get<float>(), f2 = (*it)[1].get<float>();
		if (!sensible_place(f1, f2))
			return false;
		got.f1[v] = f1;
		got.f2[v] = f2;
	}
	got.own = true;
	p = got;
	return true;
}

Voice::Voice() = default;

namespace {

VoiceOptions sensible(const VoiceOptions &o)
{
	VoiceOptions c = o;
	c.start_db = std::min(-6.0f, std::max(-90.0f, std::isfinite(c.start_db) ? c.start_db : -50.0f));
	c.full_db = std::min(0.0f, std::max(c.start_db + 3, std::isfinite(c.full_db) ? c.full_db : -30.0f));
	c.amount = std::min(2.0f, std::max(0.0f, std::isfinite(c.amount) ? c.amount : 1.0f));
	return c;
}

} // namespace

void Voice::set(const VoiceOptions &o)
{
	std::lock_guard<std::mutex> l(state_lock_);
	options_ = sensible(o);
}

void Voice::set_levels(const VoiceOptions &o)
{
	std::lock_guard<std::mutex> l(state_lock_);
	const VoiceProfile keep = options_.profile;
	options_ = sensible(o);
	options_.profile = keep;
}

VoiceOptions Voice::options() const
{
	std::lock_guard<std::mutex> l(state_lock_);
	return options_;
}

void Voice::feed(const float *const *planes, int channels, size_t frames, uint32_t rate, bool muted, float gain)
{
	if (!planes || channels <= 0 || !frames || rate < 8000 || rate > 384000)
		return;
	gain = std::isfinite(gain) ? std::min(20.0f, std::max(0.0f, gain)) : 1.0f;
	std::lock_guard<std::mutex> l(lock_);
	fed_ = true;
	muted_ = muted;
	if (rate != rate_) {
		rate_ = rate;
		down_ = std::max(1, (int)std::lround(rate / TARGET_RATE));
		const double kept = (double)rate / down_;
		design_low_pass(rate, kept * 0.38, low_);
		std::memset(low_z_, 0, sizeof(low_z_));
		phase_ = 0;
		ring_.assign((size_t)std::lround(kept * WINDOW_S), 0.0f);
		ring_at_ = since_hop_ = have_ = 0;
		energy_now_ = low_now_ = 0;
		raw_now_ = 0;
		std::fill(hop_energy_, hop_energy_ + 3, 0.0f);
		std::fill(hop_low_, hop_low_ + 3, 0.0f);
	}
	// The loudest channel: an audio interface often has the mic on one.
	const float *in = nullptr;
	double most = -1;
	for (int c = 0; c < channels; c++) {
		if (!planes[c])
			continue;
		double e = 0;
		for (size_t i = 0; i < frames; i++)
			e += (double)planes[c][i] * planes[c][i];
		if (e > most) {
			most = e;
			in = planes[c];
		}
	}
	if (!in)
		return;
	const double kept = (double)rate_ / down_;
	const size_t hop = std::max<size_t>(1, (size_t)std::lround(kept * HOP_S));
	for (size_t i = 0; i < frames; i++) {
		float x = muted ? 0.0f : in[i] * gain;
		if (!std::isfinite(x))
			x = 0;
		energy_now_ += (double)x * x;
		raw_now_++;
		float y = x;
		for (int k = 0; k < 3; k++) {
			const float *b = low_[k];
			float *z = low_z_[k];
			const float out = b[0] * y + z[0];
			z[0] = b[1] * y - b[3] * out + z[1];
			z[1] = b[2] * y - b[4] * out;
			y = out;
		}
		if (++phase_ < down_)
			continue;
		phase_ = 0;
		ring_[ring_at_] = y;
		ring_at_ = (ring_at_ + 1) % ring_.size();
		have_ = std::min(have_ + 1, ring_.size());
		low_now_ += (double)y * y;
		if (++since_hop_ < hop)
			continue;
		hop_energy_[hop_slot_] = (float)(energy_now_ / std::max<size_t>(1, raw_now_));
		hop_low_[hop_slot_] = (float)(low_now_ / since_hop_);
		hop_slot_ = (hop_slot_ + 1) % 3;
		energy_now_ = low_now_ = 0;
		raw_now_ = 0;
		since_hop_ = 0;
		if (have_ >= ring_.size())
			analyse();
	}
}

// One look at the last 30 ms (the audio thread, `lock_` held).
void Voice::analyse()
{
	Heard h;
	double whole = 0, low = 0;
	for (int k = 0; k < 3; k++) {
		whole += hop_energy_[k];
		low += hop_low_[k];
	}
	whole /= 3;
	low /= 3;
	h.level_db = db_of(whole);
	const size_t n = ring_.size();
	std::vector<double> x(n);
	double mean = 0;
	for (size_t i = 0; i < n; i++) {
		x[i] = ring_[(ring_at_ + i) % n];
		mean += x[i];
	}
	mean /= (double)n;
	for (double &v : x)
		v -= mean;
	const double kept = (double)rate_ / down_;
	const float low_share = whole > 1e-10 ? (float)(low / whole) : 0.0f;
	if (h.level_db > -80 && low_share >= VOICED_LOW && repeats(x, kept) >= VOICED_REPEAT) {
		h.voiced = true;
		formants(x, kept, h.f1, h.f2);
	}
	heard_.push_back(h);
	if (heard_.size() > MOST_HEARD)
		heard_.erase(heard_.begin(), heard_.begin() + (heard_.size() - MOST_HEARD));
}

// The looks since the last time, matched to the vowels (`state_lock_` held).
std::vector<Heard> Voice::take_heard()
{
	std::vector<Heard> out;
	{
		std::lock_guard<std::mutex> l(lock_);
		out.swap(heard_);
	}
	for (Heard &h : out)
		if (h.f1 > 0 && h.f2 > 0) {
			vowel_match(h.f1, h.f2, options_.profile, h.match);
			h.matched = true;
		}
	return out;
}

std::vector<Heard> Voice::look()
{
	std::lock_guard<std::mutex> l(state_lock_);
	return take_heard();
}

void Voice::forget()
{
	std::lock_guard<std::mutex> l(lock_);
	heard_.clear();
	fed_ = false;
	muted_ = false;
	rate_ = 0;
}

VoiceOut Voice::step(float dt)
{
	std::lock_guard<std::mutex> state(state_lock_);
	dt = std::max(0.0f, std::min(std::isfinite(dt) ? dt : 0.0f, 0.5f));
	bool fed, muted;
	{
		std::lock_guard<std::mutex> l(lock_);
		fed = fed_;
		fed_ = false;
		muted = muted_;
	}
	std::vector<Heard> got = take_heard();
	since_sound_ = fed ? 0 : since_sound_ + dt;
	const bool hearing = since_sound_ < GONE;
	const VoiceOptions &o = options_;
	const float range = std::max(1.0f, o.full_db - o.start_db);
	bool heard = false;
	float open_to = got.empty() ? open_target_ : 0;
	float share_to[VOWELS];
	bool new_share = false;
	for (const Heard &h : got) {
		out_.level_db = h.level_db;
		const bool loud = h.level_db >= o.start_db;
		heard = heard || loud;
		const float opening = clamp01((h.level_db - o.start_db) / range);
		if (loud && h.voiced && h.matched) {
			std::copy(h.match, h.match + VOWELS, share_to);
			new_share = true;
			open_to = std::max(open_to, opening);
			out_.f1 = h.f1;
			out_.f2 = h.f2;
		} else if (loud && !h.voiced) {
			// "s", "t", "f": a little open, teeth together.
			std::fill(share_to, share_to + VOWELS, 0.0f);
			share_to[VOWEL_I] = 1;
			new_share = true;
			open_to = std::max(open_to, opening * HISS_OPEN);
		} else if (loud) {
			// A voice whose vowel wasn't found this time: open, as it was.
			open_to = std::max(open_to, opening);
		}
		if (cal_.running && cal_.listening && h.voiced && h.matched && h.level_db >= o.start_db - 6) {
			cal_f1_.push_back(h.f1);
			cal_f2_.push_back(h.f2);
		}
		out_.voiced = h.voiced;
	}
	if (!hearing || muted) {
		out_.level_db = -100;
		heard = false;
		open_to = 0;
		out_.voiced = false;
	}
	open_target_ = open_to;
	if (new_share)
		for (int v = 0; v < VOWELS; v++)
			share_[v] = ease(share_[v], share_to[v], dt, SHARE_TIME);
	out_.open = ease(out_.open, open_target_, dt, open_target_ > out_.open ? OPEN_TIME : CLOSE_TIME);
	if (out_.open < 1e-4f)
		out_.open = 0;
	quiet_for_ = heard ? 0 : quiet_for_ + dt;
	const float talk_to = quiet_for_ < TALK_HOLD ? 1.0f : 0.0f;
	out_.talking = ease(out_.talking, talk_to, dt, talk_to > out_.talking ? TALK_ON : TALK_OFF);
	if (out_.talking < 1e-3f)
		out_.talking = 0;
	out_.heard = heard;
	out_.peak_db = std::max(out_.level_db, out_.peak_db - PEAK_FALL * dt);
	float total = 0;
	for (int v = 0; v < VOWELS; v++)
		total += share_[v];
	for (int v = 0; v < VOWELS; v++)
		out_.vowel[v] = std::min(1.0f, (total > 1e-6f ? share_[v] / total : 0.0f) * out_.open * o.amount);
	out_.hearing = hearing;
	out_.muted = hearing && muted;

	// "Say each vowel".
	if (cal_.running) {
		cal_.left -= dt;
		if (!cal_.listening && cal_.left <= 0) {
			cal_.listening = true;
			cal_.left = Calibration::LISTEN;
			cal_f1_.clear();
			cal_f2_.clear();
		} else if (cal_.listening && cal_.left <= 0) {
			const int v = cal_.vowel;
			const float f1 = median(cal_f1_), f2 = median(cal_f2_);
			// Heard often enough, and where a voice's vowel can be (so the
			// profile saved reads back).
			if (cal_f1_.size() >= 8 && sensible_place(f1, f2)) {
				cal_.result.f1[v] = f1;
				cal_.result.f2[v] = f2;
				cal_.heard[v] = true;
			}
			if (++cal_.vowel >= VOWELS) {
				int heard_count = 0;
				for (bool b : cal_.heard)
					heard_count += b;
				cal_.running = false;
				cal_.listening = false;
				cal_.vowel = VOWELS - 1;
				cal_.left = 0;
				cal_.done = true;
				cal_.result.own = heard_count >= 3;
				if (cal_.result.own) {
					options_.profile = cal_.result;
					cal_ready_ = true;
				}
			} else {
				cal_.listening = false;
				cal_.left = Calibration::READY;
			}
		}
	}
	return out_;
}

void Voice::calibrate()
{
	std::lock_guard<std::mutex> l(state_lock_);
	cal_ = Calibration();
	cal_.running = true;
	cal_.left = Calibration::READY;
	cal_.result = options_.profile;
	cal_f1_.clear();
	cal_f2_.clear();
	cal_ready_ = false;
}

void Voice::stop_calibrating()
{
	std::lock_guard<std::mutex> l(state_lock_);
	cal_ = Calibration();
	cal_ready_ = false;
}

Calibration Voice::calibration() const
{
	std::lock_guard<std::mutex> l(state_lock_);
	return cal_;
}

bool Voice::take_calibrated(VoiceProfile &out)
{
	std::lock_guard<std::mutex> l(state_lock_);
	if (!cal_ready_)
		return false;
	cal_ready_ = false;
	out = cal_.result;
	return true;
}

void mic_into_moves(const MicMouth &m, float moves[EXPRESSIONS])
{
	const float share = clamp01(m.share);
	if (share <= 0)
		return;
	static const int jaw = expression_id("jawOpen"), funnel = expression_id("mouthFunnel"),
			 pucker = expression_id("mouthPucker"), close = expression_id("mouthClose"),
			 stretch[2] = {expression_id("mouthStretchLeft"), expression_id("mouthStretchRight")},
			 lower[2] = {expression_id("mouthLowerDownLeft"), expression_id("mouthLowerDownRight")},
			 upper[2] = {expression_id("mouthUpperUpLeft"), expression_id("mouthUpperUpRight")};
	const float a = clamp01(m.vowel[VOWEL_A]), i = clamp01(m.vowel[VOWEL_I]), u = clamp01(m.vowel[VOWEL_U]),
		    e = clamp01(m.vowel[VOWEL_E]), o = clamp01(m.vowel[VOWEL_O]);
	// Each vowel's mouth, as Apple's face shapes make it: A open wide, I
	// and E wide lips (E more open), U and O round lips (O more open).
	auto set = [&](int id, float to) {
		if (id > 0)
			moves[id] = mix(moves[id], clamp01(to), share);
	};
	set(jaw, 0.75f * a + 0.2f * i + 0.25f * u + 0.45f * e + 0.5f * o);
	set(funnel, 0.8f * o + 0.45f * u);
	set(pucker, 0.85f * u + 0.25f * o);
	set(close, 0);
	for (int s = 0; s < 2; s++) {
		set(stretch[s], 0.7f * i + 0.45f * e);
		set(lower[s], 0.35f * e + 0.3f * i + 0.2f * a);
		set(upper[s], 0.2f * e + 0.2f * i);
	}
}

} // namespace direbot::face
