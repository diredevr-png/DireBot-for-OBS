// Unit tests for the mouth from your mic (src/face/voice.cpp), on made-up
// sound: vowels made the way a voice makes them (a buzz at its pitch
// through resonances at its formants - men's, women's and children's, as
// Hillenbrand et al. measured them), at OBS's usual rates, in noise and
// through a bass-heavy mic; an "s", quiet, a muted mic, a mic on one
// channel, "say each vowel", and how the vowels move a face's shapes.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/voice.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace direbot::face;

static int failures = 0;
static int passes = 0;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (cond) {                                                      \
			passes++;                                                \
		} else {                                                         \
			failures++;                                              \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

static const double PI = 3.14159265358979323846;

// A made-up vowel: a buzz at `f0` (with a little wobble) softened as a
// throat softens it, through resonances at `fs`, then out of the lips -
// `level_db` loud (RMS, dBFS), over a faint hiss.
static std::vector<float> vowel(double rate, double seconds, double f0, std::vector<double> fs, double level_db,
				unsigned seed = 1)
{
	const double bandwidths[5] = {70, 100, 130, 170, 220};
	while (fs.size() < 5)
		fs.push_back(fs.size() == 2 ? 2600 : fs.size() == 3 ? 3500 : 4500);
	const size_t n = (size_t)(rate * seconds);
	std::vector<double> x(n, 0);
	double phase = 0;
	for (size_t i = 0; i < n; i++) {
		phase += f0 * (1 + 0.01 * std::sin(2 * PI * 5 * i / rate)) / rate;
		if (phase >= 1) {
			phase -= 1;
			x[i] = 1;
		}
	}
	const double a = std::exp(-2 * PI * 150 / rate);
	double y1 = 0, y2 = 0;
	for (size_t i = 0; i < n; i++) {
		y1 = (1 - a) * x[i] + a * y1;
		y2 = (1 - a) * y1 + a * y2;
		x[i] = y2;
	}
	for (size_t k = 0; k < 5; k++) {
		const double T = 1 / rate, C = -std::exp(-2 * PI * bandwidths[k] * T),
			     B = 2 * std::exp(-PI * bandwidths[k] * T) * std::cos(2 * PI * fs[k] * T), A = 1 - B - C;
		double p1 = 0, p2 = 0;
		for (size_t i = 0; i < n; i++) {
			const double y = A * x[i] + B * p1 + C * p2;
			p2 = p1;
			p1 = y;
			x[i] = y;
		}
	}
	double prev = 0;
	for (size_t i = 0; i < n; i++) {
		const double d = x[i] - prev;
		prev = x[i];
		x[i] = d;
	}
	double e = 0;
	for (double v : x)
		e += v * v;
	const double rms = std::sqrt(e / std::max<size_t>(1, n)), want = std::pow(10, level_db / 20);
	std::mt19937 g(seed);
	std::normal_distribution<double> nd(0, 1);
	std::vector<float> out(n);
	for (size_t i = 0; i < n; i++)
		out[i] = (float)(x[i] / rms * want + nd(g) * std::pow(10, -70 / 20.0));
	return out;
}

// An "s": noise, mostly high.
static std::vector<float> hiss(double rate, double seconds, double level_db, unsigned seed = 2)
{
	const size_t n = (size_t)(rate * seconds);
	std::mt19937 g(seed);
	std::normal_distribution<double> nd(0, 1);
	std::vector<double> x(n);
	double prev = 0;
	for (size_t i = 0; i < n; i++) {
		const double v = nd(g);
		x[i] = v - 0.95 * prev;
		prev = v;
	}
	double e = 0;
	for (double v : x)
		e += v * v;
	const double rms = std::sqrt(e / n), want = std::pow(10, level_db / 20);
	std::vector<float> out(n);
	for (size_t i = 0; i < n; i++)
		out[i] = (float)(x[i] / rms * want);
	return out;
}

// Sound fed 10 ms at a time, a step after each, as OBS does it.
struct Run {
	float sum[VOWELS] = {};
	int steps = 0, voiced = 0;
	float open_sum = 0;
	VoiceOut last;
	int best() const
	{
		int b = 0;
		for (int k = 1; k < VOWELS; k++)
			if (sum[k] > sum[b])
				b = k;
		return b;
	}
};

static Run play(Voice &voice, const std::vector<float> &s, double rate, int channels = 2, double skip = 0.3,
		bool muted = false, float gain = 1, int mic_channel = -1)
{
	Run run;
	const size_t chunk = (size_t)(rate / 100);
	std::vector<float> silent(chunk, 0.0f);
	for (size_t i = 0; i + chunk <= s.size(); i += chunk) {
		const float *planes[8];
		for (int c = 0; c < channels; c++)
			planes[c] = mic_channel < 0 || c == mic_channel ? &s[i] : silent.data();
		voice.feed(planes, channels, chunk, (uint32_t)rate, muted, gain);
		run.last = voice.step(0.01f);
		if (i >= rate * skip) {
			for (int k = 0; k < VOWELS; k++)
				run.sum[k] += run.last.vowel[k];
			run.open_sum += run.last.open;
			run.voiced += run.last.voiced;
			run.steps++;
		}
	}
	return run;
}

struct Said {
	const char *who;
	double f0;
	std::vector<double> formants;
	int vowel;
};

// Hillenbrand et al. (1995): men, women, children.
static const std::vector<Said> SAID = {
	{"man", 120, {768, 1333, 2522}, VOWEL_A},    {"woman", 220, {936, 1551, 2815}, VOWEL_A},
	{"man", 120, {342, 2322, 3000}, VOWEL_I},    {"woman", 220, {437, 2761, 3372}, VOWEL_I},
	{"man", 120, {378, 997, 2343}, VOWEL_U},     {"woman", 220, {459, 1105, 2735}, VOWEL_U},
	{"man", 120, {528, 1944, 2600}, VOWEL_E},    {"woman", 220, {634, 2294, 2900}, VOWEL_E},
	{"man", 120, {575, 954, 2400}, VOWEL_O},     {"woman", 220, {668, 1086, 2800}, VOWEL_O},
	{"child", 280, {1002, 1688, 3000}, VOWEL_A}, {"child", 280, {452, 3081, 3600}, VOWEL_I},
};

static void test_vowels()
{
	int right = 0, total = 0;
	for (const Said &v : SAID) {
		for (double rate : {48000.0, 44100.0}) {
			Voice voice;
			const Run run = play(voice, vowel(rate, 1.0, v.f0, v.formants, -24), rate);
			total++;
			right += run.best() == v.vowel;
			if (run.best() != v.vowel)
				std::fprintf(stderr, "  %s's %s at %.0f heard as %s\n", v.who, VOWEL_NAMES[v.vowel], rate,
					     VOWEL_NAMES[run.best()]);
			// Loud enough (-24 dB, past -30): open all the way, a voice.
			CHECK(run.last.open > 0.95f);
			CHECK(run.voiced == run.steps);
			CHECK(run.last.heard && run.last.hearing && !run.last.muted);
			CHECK(std::fabs(run.last.level_db + 24) < 1.5f);
			// Its formants found close to where they are (a child's high
			// voice has few harmonics to find its first one by).
			CHECK(std::fabs(run.last.f1 - v.formants[0]) < v.formants[0] * (v.f0 > 250 ? 0.2 : 0.12));
			CHECK(std::fabs(run.last.f2 - v.formants[1]) < v.formants[1] * 0.1);
			CHECK(run.last.talking > 0.99f);
		}
	}
	CHECK(right == total);
	// The other rates OBS can run at.
	for (double rate : {16000.0, 22050.0, 32000.0, 96000.0}) {
		Voice voice;
		const Run run = play(voice, vowel(rate, 0.8, 120, {768, 1333, 2522}, -24), rate, 1);
		CHECK(run.best() == VOWEL_A);
		Voice voice2;
		const Run run2 = play(voice2, vowel(rate, 0.8, 220, {437, 2761, 3372}, -24), rate, 1);
		CHECK(run2.best() == VOWEL_I);
	}
}

static void test_noise_and_mics()
{
	// In noise (15 dB under the voice), through a bass-heavy mic.
	for (const Said &v : SAID) {
		if (std::string(v.who) != "man")
			continue;
		std::vector<float> s = vowel(48000, 1.0, 130, v.formants, -26);
		std::mt19937 g(9);
		std::normal_distribution<double> nd(0, 1);
		const double a = std::pow(10, (-26 - 15) / 20.0);
		double z = 0;
		for (float &x : s) {
			x += (float)(nd(g) * a);
			z = 0.995 * z + 0.005 * x;
			x = (float)(x + 3 * z);
		}
		Voice voice;
		CHECK(play(voice, s, 48000, 1).best() == v.vowel);
	}
	// A mic on one channel of an audio interface (the other silent).
	{
		Voice voice;
		const Run run = play(voice, vowel(48000, 1.0, 120, {342, 2322, 3000}, -24), 48000, 2, 0.3, false, 1, 1);
		CHECK(run.best() == VOWEL_I);
		CHECK(run.last.open > 0.95f);
	}
	// Its volume slider: as loud as OBS's meter shows it.
	{
		Voice voice;
		const Run run = play(voice, vowel(48000, 0.6, 120, {768, 1333, 2522}, -24), 48000, 1, 0.3, false, 0.1f);
		CHECK(std::fabs(run.last.level_db + 44) < 1.5f);
		CHECK(run.last.open > 0.2f && run.last.open < 0.4f); // -44: 30% of the way from -50 to -30
	}
}

static void test_quiet_hiss_and_mute()
{
	// An "s": a little open, teeth together (I's shape), not a voice.
	{
		Voice voice;
		const Run run = play(voice, hiss(48000, 0.5, -24), 48000, 1, 0.2);
		CHECK(run.voiced == 0);
		CHECK(run.last.heard);
		CHECK(std::fabs(run.last.open - 0.4f) < 0.05f);
		CHECK(run.last.vowel[VOWEL_I] > 0.35f);
		CHECK(run.last.vowel[VOWEL_A] < 0.01f && run.last.vowel[VOWEL_O] < 0.01f);
	}
	// Quieter than the gate: shut.
	{
		Voice voice;
		const Run run = play(voice, hiss(48000, 0.5, -60), 48000, 1, 0.2);
		CHECK(!run.last.heard && run.last.open == 0 && run.last.talking == 0);
		Voice voice2;
		const Run run2 = play(voice2, vowel(48000, 0.5, 120, {768, 1333, 2522}, -58), 48000, 1, 0.2);
		CHECK(!run2.last.heard && run2.last.open == 0);
		for (int k = 0; k < VOWELS; k++)
			CHECK(run2.last.vowel[k] == 0);
	}
	// Muted in OBS: shut, and it says so.
	{
		Voice voice;
		const Run run = play(voice, vowel(48000, 0.5, 120, {768, 1333, 2522}, -24), 48000, 1, 0.2, true);
		CHECK(run.last.muted && run.last.hearing && !run.last.heard && run.last.open == 0);
		CHECK(run.last.level_db <= -99);
	}
	// No sound at all (the mic's gone): not hearing, shut - after a moment.
	{
		Voice voice;
		play(voice, vowel(48000, 0.5, 120, {768, 1333, 2522}, -24), 48000);
		VoiceOut out = voice.step(0.01f);
		CHECK(out.hearing);
		for (int i = 0; i < 60; i++)
			out = voice.step(0.01f);
		CHECK(!out.hearing && out.open < 0.01f && out.level_db <= -99);
		voice.forget();
		CHECK(!voice.step(0.01f).hearing);
	}
	// Bad sound in: never anything but numbers out.
	{
		Voice voice;
		std::vector<float> s = vowel(48000, 0.4, 120, {768, 1333, 2522}, -24);
		for (size_t i = 0; i < s.size(); i += 97)
			s[i] = i % 2 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
		const Run run = play(voice, s, 48000, 1, 0.1);
		bool fine = std::isfinite(run.last.open) && std::isfinite(run.last.level_db);
		for (int k = 0; k < VOWELS; k++)
			fine = fine && std::isfinite(run.last.vowel[k]);
		CHECK(fine);
		voice.feed(nullptr, 2, 480, 48000, false);
		const float *none[2] = {nullptr, nullptr};
		voice.feed(none, 2, 480, 48000, false);
		const float *one[1] = {s.data()};
		voice.feed(one, 1, 480, 1000, false); // not a rate
		voice.feed(one, 0, 480, 48000, false);
		CHECK(std::isfinite(voice.step(std::numeric_limits<float>::quiet_NaN()).open));
		CHECK(std::isfinite(voice.step(-1).open));
		CHECK(std::isfinite(voice.step(1e9f).open));
	}
}

static void test_levels_and_smoothing()
{
	// The gate and full: a voice at -40 is half open with -50 to -30...
	{
		Voice voice;
		const Run run = play(voice, vowel(48000, 0.6, 120, {768, 1333, 2522}, -40), 48000, 1);
		CHECK(std::fabs(run.last.open - 0.5f) < 0.08f);
		// ...all the way open with -45 to -40, and shut with the gate at -35.
		VoiceOptions o;
		o.start_db = -45;
		o.full_db = -40;
		Voice wide;
		wide.set(o);
		CHECK(play(wide, vowel(48000, 0.6, 120, {768, 1333, 2522}, -40), 48000, 1).last.open > 0.9f);
		o.start_db = -35;
		o.full_db = -20;
		Voice gated;
		gated.set(o);
		CHECK(play(gated, vowel(48000, 0.6, 120, {768, 1333, 2522}, -40), 48000, 1).last.open == 0);
	}
	// How wide: 50% opens it half as much.
	{
		VoiceOptions o;
		o.amount = 0.5f;
		Voice voice;
		voice.set(o);
		const Run run = play(voice, vowel(48000, 0.6, 120, {768, 1333, 2522}, -24), 48000, 1);
		CHECK(std::fabs(run.last.vowel[VOWEL_A] - 0.5f * run.last.open * run.sum[VOWEL_A] /
								  std::max(1e-6f, run.sum[VOWEL_A] + run.sum[VOWEL_E] +
											  run.sum[VOWEL_O] + run.sum[VOWEL_I] +
											  run.sum[VOWEL_U])) < 0.1f);
		float total = 0;
		for (int k = 0; k < VOWELS; k++)
			total += run.last.vowel[k];
		CHECK(total < 0.55f && total > 0.4f);
	}
	// Silly options are kept sensible.
	{
		VoiceOptions o;
		o.start_db = std::numeric_limits<float>::quiet_NaN();
		o.full_db = -80;
		o.amount = 50;
		Voice voice;
		voice.set(o);
		const VoiceOptions got = voice.options();
		CHECK(got.start_db == -50 && got.full_db == -47 && got.amount == 2);
	}
	// Set the levels only: a voice's own places stay.
	{
		VoiceOptions o;
		o.profile.f1[VOWEL_A] = 900;
		o.profile.own = true;
		Voice voice;
		voice.set(o);
		VoiceOptions levels;
		levels.start_db = -40;
		voice.set_levels(levels);
		CHECK(voice.options().profile.own && voice.options().profile.f1[VOWEL_A] == 900);
		CHECK(voice.options().start_db == -40);
	}
	// Between words: held a moment (talking stays), then it lets go; the
	// mouth shuts quickly but not in one frame.
	{
		Voice voice;
		play(voice, vowel(48000, 0.5, 120, {768, 1333, 2522}, -24), 48000, 1);
		std::vector<float> quiet(480, 0.0f);
		const float *p[1] = {quiet.data()};
		VoiceOut out;
		float open_after_20ms = 1;
		for (int i = 0; i < 20; i++) {
			voice.feed(p, 1, 480, 48000, false);
			out = voice.step(0.01f);
			if (i == 1)
				open_after_20ms = out.open;
		}
		CHECK(open_after_20ms > 0.2f && open_after_20ms < 0.95f);
		CHECK(out.open < 0.1f);
		CHECK(out.talking > 0.9f); // 200 ms: still talking
		for (int i = 0; i < 150; i++) {
			voice.feed(p, 1, 480, 48000, false);
			out = voice.step(0.01f);
		}
		CHECK(out.talking < 0.01f && out.open == 0);
		CHECK(out.peak_db < -55); // the level bar's peak has fallen back
	}
	// The looks it made, for tests and for the page.
	{
		Voice voice;
		std::vector<float> s = vowel(48000, 0.2, 120, {768, 1333, 2522}, -24);
		for (size_t i = 0; i + 480 <= s.size(); i += 480) {
			const float *p[1] = {&s[i]};
			voice.feed(p, 1, 480, 48000, false);
		}
		const std::vector<Heard> looks = voice.look();
		CHECK(looks.size() >= 15 && looks.size() <= 20);
		CHECK(!looks.empty() && looks.back().voiced && looks.back().matched);
		CHECK(voice.look().empty());
	}
}

static void test_matching_and_profiles()
{
	// Each vowel's own place matches it best, and the shares add up to 1.
	const VoiceProfile p;
	for (int v = 0; v < VOWELS; v++) {
		float m[VOWELS];
		vowel_match(p.f1[v], p.f2[v], p, m);
		float total = 0;
		int best = 0;
		for (int k = 0; k < VOWELS; k++) {
			total += m[k];
			if (m[k] > m[best])
				best = k;
		}
		CHECK(best == v);
		CHECK(std::fabs(total - 1) < 1e-4f);
	}
	// Far from every place: still shares that add up (no NaN).
	{
		float m[VOWELS];
		vowel_match(5000, 100, p, m);
		float total = 0;
		for (float x : m)
			total += std::isfinite(x) ? x : 100;
		CHECK(std::fabs(total - 1) < 1e-4f);
	}
	// As text and back.
	{
		VoiceProfile mine;
		mine.f1[VOWEL_A] = 990;
		mine.f2[VOWEL_I] = 2700;
		mine.own = true;
		const std::string text = profile_text(mine);
		VoiceProfile back;
		CHECK(profile_from(text, back));
		CHECK(back.own && std::fabs(back.f1[VOWEL_A] - 990) < 1 && std::fabs(back.f2[VOWEL_I] - 2700) < 1);
		CHECK(std::fabs(back.f1[VOWEL_O] - p.f1[VOWEL_O]) < 1);
	}
	// Not a profile, or not sensible places: kept as it was.
	for (const char *bad : {"", "nope", "[]", "{\"a\": [5, 10]}", "{\"a\": [800, 700]}", "{\"i\": [400, 9000]}",
				"{\"a\": \"x\"}", "{\"a\": [null, 1200]}"}) {
		VoiceProfile q;
		q.f1[VOWEL_A] = 811;
		const bool ok = profile_from(bad, q);
		CHECK(!ok || q.f1[VOWEL_A] == 811);
		CHECK(q.f1[VOWEL_A] == 811);
	}
	{
		// Four sensible vowels and one not: none taken.
		VoiceProfile all;
		all.f1[VOWEL_I] = 5;
		VoiceProfile q;
		CHECK(!profile_from(profile_text(all), q));
		CHECK(q.f1[VOWEL_I] == p.f1[VOWEL_I]);
		CHECK(sensible_place(800, 1400) && !sensible_place(800, 850) && !sensible_place(100, 900));
	}
}

static void test_calibration()
{
	// A voice whose vowels sit about 15% higher than the usual places
	// (a higher voice): "say each vowel" finds them.
	const double places[VOWELS][3] = {
		{975, 1650, 2900}, {445, 2900, 3450}, {480, 1210, 2700}, {665, 2430, 3100}, {713, 1170, 2750}};
	Voice voice;
	voice.calibrate();
	Calibration c = voice.calibration();
	CHECK(c.running && !c.listening && c.vowel == 0 && std::fabs(c.left - Calibration::READY) < 1e-4f);
	double t = 0;
	int steps = 0;
	bool saw_listening[VOWELS] = {};
	while (voice.calibration().running && steps < 3000) {
		c = voice.calibration();
		if (c.listening)
			saw_listening[c.vowel] = true;
		const std::vector<float> s = c.listening ? vowel(48000, 0.01, 200,
								  {places[c.vowel][0], places[c.vowel][1],
								   places[c.vowel][2]},
								  -25, steps)
							 : std::vector<float>(480, 0.0f);
		const float *p[1] = {s.data()};
		voice.feed(p, 1, 480, 48000, false);
		voice.step(0.01f);
		t += 0.01;
		steps++;
	}
	c = voice.calibration();
	CHECK(c.done && !c.running);
	CHECK(std::fabs(t - VOWELS * (Calibration::READY + Calibration::LISTEN)) < 0.1);
	for (int v = 0; v < VOWELS; v++) {
		CHECK(saw_listening[v]);
		CHECK(c.heard[v]);
		CHECK(std::fabs(c.result.f1[v] - places[v][0]) < places[v][0] * 0.12);
		CHECK(std::fabs(c.result.f2[v] - places[v][1]) < places[v][1] * 0.1);
	}
	CHECK(c.result.own);
	// It's used straight away, and given once to be saved.
	CHECK(voice.options().profile.own);
	VoiceProfile got;
	CHECK(voice.take_calibrated(got) && got.own);
	CHECK(!voice.take_calibrated(got));
	// Saying nothing: nothing heard, the places as they were.
	{
		Voice quiet;
		quiet.calibrate();
		std::vector<float> silence(480, 0.0f);
		const float *p[1] = {silence.data()};
		for (int i = 0; i < 1300 && quiet.calibration().running; i++) {
			quiet.feed(p, 1, 480, 48000, false);
			quiet.step(0.01f);
		}
		const Calibration q = quiet.calibration();
		CHECK(q.done && !q.result.own);
		for (int v = 0; v < VOWELS; v++)
			CHECK(!q.heard[v]);
		VoiceProfile none;
		CHECK(!quiet.take_calibrated(none));
		CHECK(!quiet.options().profile.own);
	}
	// Stopped half way: nothing changes.
	{
		Voice stopped;
		stopped.calibrate();
		for (int i = 0; i < 150; i++)
			stopped.step(0.01f);
		stopped.stop_calibrating();
		CHECK(!stopped.calibration().running && !stopped.calibration().done);
	}
}

static int move_named(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::string(EXPRESSION_NAMES[i]) == name)
			return i;
	return 0;
}

static void test_into_moves()
{
	const int jaw = move_named("jawOpen"), funnel = move_named("mouthFunnel"), pucker = move_named("mouthPucker"),
		  stretch = move_named("mouthStretchLeft"), close = move_named("mouthClose"),
		  smile = move_named("mouthSmileLeft"), blink = move_named("eyeBlinkLeft");
	CHECK(jaw > 0 && funnel > 0 && pucker > 0 && stretch > 0 && close > 0 && smile > 0 && blink > 0);
	float moves[EXPRESSIONS] = {};
	moves[jaw] = 0.3f;
	moves[smile] = 0.6f;
	moves[blink] = 1;
	moves[close] = 0.5f;
	// No share: the camera's moves as they are.
	MicMouth none;
	none.vowel[VOWEL_A] = 1;
	float kept[EXPRESSIONS];
	std::copy(moves, moves + EXPRESSIONS, kept);
	mic_into_moves(none, kept);
	CHECK(std::equal(moves, moves + EXPRESSIONS, kept));
	// All the mic's: an A opens the jaw wide, an O and a U round the lips,
	// an I and an E stretch them; smiles and blinks stay the camera's.
	auto with = [&](int v, float share) {
		MicMouth m;
		m.vowel[v] = 1;
		m.share = share;
		std::vector<float> out(moves, moves + EXPRESSIONS);
		mic_into_moves(m, out.data());
		return out;
	};
	const auto a = with(VOWEL_A, 1), i = with(VOWEL_I, 1), u = with(VOWEL_U, 1), e = with(VOWEL_E, 1),
		   o = with(VOWEL_O, 1);
	CHECK(a[jaw] > 0.7f && a[funnel] < 0.01f && a[stretch] < 0.01f);
	CHECK(o[funnel] > 0.7f && o[jaw] > 0.4f);
	CHECK(u[pucker] > 0.8f && u[funnel] > 0.4f && u[jaw] < 0.3f);
	CHECK(i[stretch] > 0.6f && i[jaw] < 0.25f);
	CHECK(e[stretch] > 0.4f && e[jaw] > i[jaw]);
	for (const auto &x : {a, i, u, e, o}) {
		CHECK(x[smile] == moves[smile] && x[blink] == moves[blink]);
		CHECK(x[close] == 0); // lips apart while it talks
	}
	// Half the mic's: half way between.
	const auto half = with(VOWEL_A, 0.5f);
	CHECK(std::fabs(half[jaw] - (0.3f + (a[jaw] - 0.3f) * 0.5f)) < 1e-4f);
	// Quiet with all of it: the mouth shut.
	const auto shut = with(VOWEL_A, 1);
	MicMouth quiet;
	quiet.share = 1;
	std::vector<float> q(moves, moves + EXPRESSIONS);
	mic_into_moves(quiet, q.data());
	CHECK(q[jaw] == 0 && q[funnel] == 0 && q[stretch] == 0 && shut[jaw] > 0);
}

static void test_speed()
{
	// Ten seconds of stereo sound at 48,000 a second: well under a
	// tenth of a second of work (it runs on OBS's audio thread).
	Voice voice;
	const std::vector<float> s = vowel(48000, 10.0, 130, {700, 1200, 2500}, -24);
	const auto t0 = std::chrono::steady_clock::now();
	play(voice, s, 48000, 2);
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	std::printf("unit_voice: 10 s of stereo sound took %.1f ms\n", ms);
	CHECK(ms < 400);
}

int main()
{
	test_vowels();
	test_noise_and_mics();
	test_quiet_hiss_and_mute();
	test_levels_and_smoothing();
	test_matching_and_profiles();
	test_calibration();
	test_into_moves();
	test_speed();
	std::printf("unit_voice: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
