// Unit tests for the vertical picture's parts that don't need OBS: its
// rules (sizes, servers, which tall scene goes with yours, encoders, file
// types) and its options file, above all that DireBot is never told a key.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/obs/keystore.hpp"
#include "../src/vertical/options.hpp"
#include "../src/vertical/rules.hpp"

#include <cstdio>
#include <string>

using namespace direbot;
using namespace direbot::vertical;

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

static void test_sizes()
{
	CHECK(rules::size_ok(1080, 1920));
	CHECK(rules::size_ok(720, 1280));
	CHECK(rules::size_ok(1080, 1350)); // 4:5
	CHECK(rules::size_ok(1080, 1080)); // square
	CHECK(rules::size_ok(2160, 3840));
	CHECK(!rules::size_ok(1920, 1080)); // wide
	CHECK(!rules::size_ok(1081, 1920)); // odd
	CHECK(!rules::size_ok(1080, 1921));
	CHECK(!rules::size_ok(200, 400));   // too small
	CHECK(!rules::size_ok(4096, 4098)); // too big
	CHECK(!rules::size_ok(0, 0));
}

static void test_servers()
{
	CHECK(rules::host_of("rtmp://a.rtmp.youtube.com/live2") == "a.rtmp.youtube.com");
	CHECK(rules::host_of("rtmps://live-api-s.facebook.com:443/rtmp/") == "live-api-s.facebook.com");
	CHECK(rules::host_of("RTMP://Push.TikTok.com/live/") == "push.tiktok.com");
	// A key written into the server's path or in front never comes out.
	CHECK(rules::host_of("rtmp://example.com/app/live_123_SECRET") == "example.com");
	CHECK(rules::host_of("rtmp://user:SECRET@example.com:1935/app") == "example.com");
	CHECK(rules::host_of("rtmp://example.com?key=SECRET") == "example.com");
	CHECK(rules::host_of("rtmp://[::1]:1935/live") == "[::1]");
	CHECK(rules::host_of("") == "");

	CHECK(rules::server_ok("rtmp://a.rtmp.youtube.com/live2"));
	CHECK(rules::server_ok("rtmps://live-api-s.facebook.com:443/rtmp/"));
	CHECK(rules::server_ok("RTMPS://example.com/app"));
	CHECK(!rules::server_ok("https://example.com/app")); // not a stream server
	CHECK(!rules::server_ok("srt://example.com:9000"));
	CHECK(!rules::server_ok("rtmp://"));
	CHECK(!rules::server_ok("example.com/app"));
	CHECK(!rules::server_ok(""));

	CHECK(rules::usual_server("youtube") == "rtmp://a.rtmp.youtube.com/live2");
	CHECK(rules::usual_server("facebook") == "rtmps://live-api-s.facebook.com:443/rtmp/");
	CHECK(rules::usual_server("twitch") == "rtmp://live.twitch.tv/app");
	CHECK(rules::usual_server("tiktok").empty()); // a new one each time
	CHECK(rules::usual_server("instagram").empty());
	CHECK(rules::server_ok(rules::usual_server("youtube")));

	CHECK(rules::platform_label("tiktok") == "TikTok");
	CHECK(rules::platform_label("youtube") == "YouTube");
	CHECK(rules::platform_label("x") == "X");
	CHECK(rules::platform_label("other") == "Another site");
	CHECK(rules::platform_label("nonsense") == "Another site");

	CHECK(rules::stream_output_name(2) == "direbot_vertical_stream_2");
	CHECK(std::string(rules::RECORD_OUTPUT).rfind(rules::OUTPUT_PREFIX, 0) == 0);
	// DireBot's multistream skips every output starting "direbot_".
	CHECK(std::string(rules::OUTPUT_PREFIX).rfind("direbot_", 0) == 0);
}

static void test_follow()
{
	const std::vector<std::string> tall = {"Vertical", "Gaming", "Vertical - Just Chatting", "BRB (Vertical)"};
	std::map<std::string, std::string> links;
	// Same name, DireBot's "Vertical - " scenes, then "(Vertical)".
	CHECK(rules::follow_pick("Gaming", tall, links) == "Gaming");
	CHECK(rules::follow_pick("Just Chatting", tall, links) == "Vertical - Just Chatting");
	CHECK(rules::follow_pick("BRB", tall, links) == "BRB (Vertical)");
	CHECK(rules::follow_pick("Starting Soon", tall, links) == "");
	CHECK(rules::follow_pick("", tall, links) == "");
	// A pick wins, "" means leave it, and a scene that's gone is nothing.
	links["Gaming"] = "Vertical";
	links["BRB"] = "";
	links["Starting Soon"] = "Deleted";
	CHECK(rules::follow_pick("Gaming", tall, links) == "Vertical");
	CHECK(rules::follow_pick("BRB", tall, links) == "");
	CHECK(rules::follow_pick("Starting Soon", tall, links) == "");
	CHECK(rules::follow_pick("Just Chatting", tall, links) == "Vertical - Just Chatting");
}

static void test_encoders()
{
	const std::set<std::string> nvidia = {"obs_x264", "obs_nvenc_h264_tex", "obs_nvenc_hevc_tex", "jim_nvenc"};
	const std::set<std::string> amd = {"obs_x264", "h264_texture_amf", "h264_fallback_amf"};
	const std::set<std::string> intel = {"obs_x264", "obs_qsv11_v2"};
	const std::set<std::string> plain = {"obs_x264"};

	// The same maker as the wide stream: Simple mode's short names...
	CHECK(rules::pick_encoder("auto", true, "nvenc", nvidia) == "obs_nvenc_h264_tex");
	CHECK(rules::pick_encoder("auto", true, "nvenc_hevc", nvidia) == "obs_nvenc_h264_tex");
	CHECK(rules::pick_encoder("auto", true, "amd", amd) == "h264_texture_amf");
	CHECK(rules::pick_encoder("auto", true, "qsv", intel) == "obs_qsv11_v2");
	CHECK(rules::pick_encoder("auto", true, "x264", nvidia) == "obs_x264");
	// ...and Advanced mode's ids, HEVC and AV1 becoming H.264.
	CHECK(rules::pick_encoder("auto", false, "obs_nvenc_av1_tex", nvidia) == "obs_nvenc_h264_tex");
	CHECK(rules::pick_encoder("auto", false, "h265_texture_amf", amd) == "h264_texture_amf");
	CHECK(rules::pick_encoder("auto", false, "obs_qsv11_hevc", intel) == "obs_qsv11_v2");
	// Only the old NVENC there: that, else x264.
	CHECK(rules::pick_encoder("auto", true, "nvenc", {"obs_x264", "jim_nvenc"}) == "jim_nvenc");
	CHECK(rules::pick_encoder("auto", true, "nvenc", plain) == "obs_x264");
	// One picked by hand, if this OBS has it.
	CHECK(rules::pick_encoder("obs_x264", true, "nvenc", nvidia) == "obs_x264");
	CHECK(rules::pick_encoder("h264_texture_amf", true, "nvenc", nvidia) == "obs_nvenc_h264_tex");
	CHECK(rules::pick_encoder("", false, "obs_x264", plain) == "obs_x264");
}

static void test_files()
{
	CHECK(rules::recording_extension("hybrid_mp4") == "mp4");
	CHECK(rules::recording_extension("fragmented_mp4") == "mp4");
	CHECK(rules::recording_extension("hybrid_mov") == "mov");
	CHECK(rules::recording_extension("mkv") == "mkv");
	CHECK(rules::recording_extension("mpegts") == "ts");
	CHECK(rules::recording_extension("flv") == "flv");
	CHECK(rules::recording_extension("hls") == "mkv"); // not a file: the tall one's an mkv
	CHECK(rules::recording_extension("") == "mkv");
	CHECK(rules::recording_output("hybrid_mp4") == "mp4_output");
	CHECK(rules::recording_output("hybrid_mov") == "mov_output");
	CHECK(rules::recording_output("mkv") == "ffmpeg_muxer");
	CHECK(rules::recording_output("fragmented_mp4") == "ffmpeg_muxer");
	CHECK(rules::recording_muxer_settings("fragmented_mov") == "movflags=frag_keyframe+empty_moov+delay_moov");
	CHECK(rules::recording_muxer_settings("mkv").empty());
}

static void test_sound()
{
	// 0: the track OBS's own stream sends - always 1 in Simple mode.
	CHECK(rules::sound_track(0, true, 4) == 1);
	CHECK(rules::sound_track(0, false, 4) == 4);
	CHECK(rules::sound_track(0, false, 0) == 1);
	CHECK(rules::sound_track(0, false, 9) == 1);
	CHECK(rules::sound_track(3, true, 4) == 3);
	CHECK(rules::sound_track(6, false, 1) == 6);
	CHECK(rules::sound_track(7, false, 2) == 2);
}

static void test_options()
{
	Options plain;
	CHECK(plain.kbps == 6000 && plain.encoder == "auto" && plain.audio_track == 0);
	CHECK(plain.places[0].audio_track == 0);
	CHECK(!plain.with_stream && !plain.record_with_main && !plain.any_ready());

	Options o;
	o.places[0] = {"youtube", "rtmp://a.rtmp.youtube.com/live2", obs::keystore::lock("yt-SECRET-key"), true};
	o.places[1] = {"tiktok", "rtmp://push.tiktok.com/live/stream-SECRET", obs::keystore::lock("tt-SECRET"), false};
	o.places[2] = {"other", "https://wrong.example.com", obs::keystore::lock("x-SECRET"), true};
	o.places[1].audio_track = 5;
	o.kbps = 8000;
	o.with_stream = true;
	CHECK(o.ready(1));
	CHECK(!o.ready(2)); // off
	CHECK(!o.ready(3)); // not a stream server
	CHECK(!o.ready(0) && !o.ready(4));
	CHECK(o.any_ready());
	Options no_key = o;
	no_key.places[0].key_locked.clear();
	CHECK(!no_key.ready(1));

	// The file holds the keys locked, and comes back the same.
	const json file = o.to_file();
	CHECK(file["format"] == 1);
	CHECK(file.dump().find(obs::keystore::lock("yt-SECRET-key")) != std::string::npos);
	const Options back = Options::from_file(file);
	CHECK(back.to_file() == file);
	std::string key;
	CHECK(obs::keystore::unlock(back.places[0].key_locked, key) && key == "yt-SECRET-key");
	obs::keystore::wipe(key);
	CHECK(key.empty());

	// What DireBot is told: never a key, a server only by its host.
	const json told = o.to_public();
	const std::string text = told.dump();
	CHECK(text.find("SECRET") == std::string::npos);
	CHECK(told["places"][0]["key_set"] == true);
	CHECK(told["places"][0]["host"] == "a.rtmp.youtube.com");
	CHECK(told["places"][1]["host"] == "push.tiktok.com");
	CHECK(told["places"][0]["output"] == "direbot_vertical_stream_1");
	CHECK(told["places"][2]["server_ok"] == false);
	CHECK(told["places"][2]["label"] == "Another site");
	CHECK(told["kbps"] == 8000 && told["with_stream"] == true);
	CHECK(told["places"][0]["audio_track"] == 0 && told["places"][1]["audio_track"] == 5);
	CHECK(back.places[1].audio_track == 5);
	CHECK(no_key.to_public()["places"][0]["key_set"] == false);

	// A damaged or odd file: what can be read, the rest as it starts.
	CHECK(Options::from_file(json()).to_file() == plain.to_file());
	CHECK(Options::from_file(json::array()).to_file() == plain.to_file());
	const Options odd = Options::from_file(json::parse(R"({
		"places": [{"platform": "myspace", "enabled": "yes", "server": 5, "audio_track": 9}, 7,
			   {"platform": "kick", "enabled": true, "audio_track": "2"}, {"platform": "x"}],
		"kbps": 99999999, "audio_track": -3, "encoder": "", "with_stream": 1})"));
	CHECK(odd.places[0].platform == "other" && !odd.places[0].enabled && odd.places[0].server.empty());
	CHECK(odd.places[1].platform == plain.places[1].platform); // a 7 is skipped
	CHECK(odd.places[2].platform == "kick" && odd.places[2].enabled);
	CHECK(odd.kbps == rules::MAX_KBPS);
	CHECK(odd.audio_track == 0);
	CHECK(odd.places[0].audio_track == 6 && odd.places[2].audio_track == 0);
	CHECK(odd.encoder == "auto");
	CHECK(!odd.with_stream);
	CHECK(Options::from_file(json::parse(R"({"kbps": 1})")).kbps == rules::MIN_KBPS);
}

int main()
{
	test_sizes();
	test_servers();
	test_follow();
	test_encoders();
	test_files();
	test_sound();
	test_options();
	std::printf("vertical: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
