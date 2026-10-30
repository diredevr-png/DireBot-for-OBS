// The face tracker on a video, for tools/face-video-check.py (a test
// tool, not part of DireBot for OBS): RGB frames, one after another, on
// stdin; for each one, a record of floats to OUT.
//
//   face-look TASK OUT W H FPS [SMOOTHING]
//
// A record: found, presence, ms, searched, then the 478 points (x, y, z
// in pixels), the 52 expressions and the 16 numbers of the head's pose.

#include "../src/face/tracker.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace direbot::face;

int main(int argc, char **argv)
{
	if (argc < 6) {
		std::fprintf(stderr, "face-look TASK OUT W H FPS [SMOOTHING]\n");
		return 2;
	}
	Tracker t;
	std::string problem;
	if (!t.load_file(argv[1], &problem)) {
		std::fprintf(stderr, "%s\n", problem.c_str());
		return 1;
	}
	const int w = std::atoi(argv[3]), h = std::atoi(argv[4]);
	const double fps = std::atof(argv[5]);
	t.set_smoothing(argc > 6 ? (float)std::atof(argv[6]) : 0.5f);
	FILE *out = std::fopen(argv[2], "wb");
	if (!out || w <= 0 || h <= 0 || fps <= 0)
		return 2;
	std::vector<uint8_t> rgb((size_t)w * h * 3), px((size_t)w * h * 4, 255);
	std::vector<float> record;
	for (long frame = 0; std::fread(rgb.data(), 1, rgb.size(), stdin) == rgb.size(); frame++) {
		for (size_t i = 0; i < (size_t)w * h; i++) {
			px[i * 4] = rgb[i * 3];
			px[i * 4 + 1] = rgb[i * 3 + 1];
			px[i * 4 + 2] = rgb[i * 3 + 2];
		}
		// The time as MediaPipe's video mode gets it: whole milliseconds.
		const Face &f = t.look(Frame{px.data(), w, h, w * 4, false}, std::round(frame * 1000.0 / fps) / 1000.0);
		record = {(float)f.found, f.presence, f.ms, (float)f.searched};
		for (const Point &p : f.points) {
			record.push_back(p.x);
			record.push_back(p.y);
			record.push_back(p.z);
		}
		record.insert(record.end(), f.expressions, f.expressions + EXPRESSIONS);
		record.insert(record.end(), f.pose, f.pose + 16);
		std::fwrite(record.data(), sizeof(float), record.size(), out);
	}
	std::fclose(out);
	return 0;
}
