"""
The vertical picture inside a real OBS (31.1 or newer, for its canvases),
driven the way DireBot drives it: the "Vertical..." vendor requests over
DireBot Link, and OBS's own websocket requests for what's in its scenes.
Streams go to ffmpeg listening on this PC, never the internet.

Skipped unless OBS 31.1+, Xvfb, ffmpeg and the test build are there (see
test_real_obs.py; an OBS built from source: OBS_PREFIX in
tools/build-linux-test.sh, and its bin first on PATH). An OBS built
against its own MbedTLS 3 on a PC whose libraries bring MbedTLS 2 too
crashes as soon as any stream connects, its own included; preload the
MbedTLS 3 it was built with:
    DIREBOT_OBS_PRELOAD="$MBEDTLS/lib/libmbedcrypto.so.16 \
        $MBEDTLS/lib/libmbedx509.so.7 $MBEDTLS/lib/libmbedtls.so.21"
"""

import glob
import json
import os
import re
import shutil
import subprocess
import time

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_obs import Obs, free_port, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark as needs_obs  # noqa: E402

pytestmark = [needs_obs, pytest.mark.skipif(
    not shutil.which("ffmpeg"), reason="needs ffmpeg to listen for streams")]

KEY = "vkey-SECRET-123"
MAIN_KEY = "main-SECRET-456"


def new_enough(version):
    parts = [int(p) for p in version.split(".")[:2] if p.isdigit()]
    return parts >= [31, 1]


@pytest.fixture(scope="module")
def run():
    """Two OBS runs in one home: the second checks what the first left."""
    state = {"obs": Obs()}
    if not new_enough(state["obs"].link["obs"]):
        state["obs"].close()
        pytest.skip("the vertical picture needs OBS 31.1 or newer")
    state["records"] = os.path.join(state["obs"].home, "Recordings")
    os.makedirs(state["records"])
    yield state
    o = state["obs"]
    if o.proc.poll() is None:
        o.close()
    # DIREBOT_OBS_KEEP=1 keeps OBS's settings and logs, to look at.
    if not os.environ.get("DIREBOT_OBS_KEEP"):
        shutil.rmtree(o.home, ignore_errors=True)


def v(run, request, data=None):
    c = run["obs"].client()
    try:
        return vendor(c, request, data)
    finally:
        c.disconnect()


def send(run, request, data=None):
    c = run["obs"].client()
    try:
        return c.send(request, data, raw=True)
    finally:
        c.disconnect()


def probe(path, what="v:0", entries="stream=width,height"):
    if not shutil.which("ffprobe"):
        return None
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", what, "-show_entries",
         entries, "-of", "csv=p=0", path],
        capture_output=True, text=True).stdout.strip()
    return out


class Listener:
    """ffmpeg waiting for one RTMP stream, saving it."""

    count = 0

    def __init__(self, folder, key, port=None):
        self.port = port or free_port()
        # A file of its own: ffmpeg won't write over one, and would end
        # the stream as soon as it came.
        Listener.count += 1
        self.file = os.path.join(folder, "got-%d-%d.flv"
                                 % (self.port, Listener.count))
        self.url = "rtmp://127.0.0.1:%d/live" % self.port
        self.proc = subprocess.Popen(
            ["ffmpeg", "-nostdin", "-loglevel", "error", "-listen", "1",
             "-i", "%s/%s" % (self.url, key), "-c", "copy", "-f", "flv",
             self.file], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)

    def done(self, seconds=15):
        try:
            self.proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        return self.file


def profile_dir(home):
    found = glob.glob(os.path.join(home, ".config", "obs-studio", "basic",
                                   "profiles", "*", "basic.ini"))
    assert found, "no OBS profile"
    return os.path.dirname(found[0])


def test_the_plugin_says_it_has_the_vertical_picture(run):
    info = v(run, "GetInfo")
    assert info["features"]["vertical"] is True
    state = v(run, "VerticalGetState")
    assert state["ok"] and state["exists"] is False
    assert state["width"] == 1080 and state["height"] == 1920


def test_setting_up_makes_the_tall_picture_with_its_first_scene(run):
    state = v(run, "VerticalSetUp", {"width": 1080, "height": 1920})
    assert state["ok"] and state["exists"] and state["video"]
    assert [s["name"] for s in state["scenes"]] == ["Vertical"]
    assert state["current"] == "Vertical"
    assert state["fps"] > 0
    # Not drawn until something uses it.
    assert state["awake"] is False
    # Setting up again changes nothing.
    again = v(run, "VerticalSetUp")
    assert again["uuid"] == state["uuid"] and len(again["scenes"]) == 1
    # Its scenes aren't among your usual ones.
    names = [s["sceneName"] for s in send(run, "GetSceneList")["scenes"]]
    assert "Vertical" not in names


def test_sizes_it_cant_be_are_turned_down(run):
    for w, h in ((1921, 1920), (1920, 1080), (100, 200), (8192, 8192)):
        answer = v(run, "VerticalSetSize", {"width": w, "height": h})
        assert answer["ok"] is False and answer["error"]
    state = v(run, "VerticalSetSize", {"width": 720, "height": 1280})
    assert state["ok"] and (state["width"], state["height"]) == (720, 1280)
    state = v(run, "VerticalSetSize", {"width": 1080, "height": 1920})
    assert (state["width"], state["height"]) == (1080, 1920)


def test_tall_scenes_take_obs_websockets_own_requests(run):
    made = v(run, "VerticalCreateScene", {"name": "Gaming"})
    assert made["ok"] and made["scene"]["name"] == "Gaming"
    uuid = made["scene"]["uuid"]
    # A taken name gets a number.
    again = v(run, "VerticalCreateScene", {"name": "Gaming"})
    assert again["scene"]["name"] == "Gaming 2"
    assert v(run, "VerticalRemoveScene", {"scene": "Gaming 2"})["ok"]

    # What DireBot's scene builder does: sources into a tall scene by its
    # id, and a picture of it for the preview.
    send(run, "CreateInput", {
        "sceneUuid": uuid, "inputName": "Tall colour",
        "inputKind": "color_source_v3",
        "inputSettings": {"color": 4278190335, "width": 1080,
                          "height": 1920},
        "sceneItemEnabled": True})
    items = send(run, "GetSceneItemList", {"sceneUuid": uuid})["sceneItems"]
    assert [i["sourceName"] for i in items] == ["Tall colour"]
    shot = send(run, "GetSourceScreenshot", {
        "sourceUuid": uuid, "imageFormat": "png", "imageWidth": 108})
    assert shot["imageData"].startswith("data:image/png;base64,")

    assert v(run, "VerticalRenameScene",
             {"scene": "Gaming", "to": "Gameplay"})["ok"]
    state = v(run, "VerticalRenameScene",
              {"scene": "Gameplay", "to": "Gaming"})
    assert sorted(s["name"] for s in state["scenes"]) == ["Gaming",
                                                          "Vertical"]
    # The last scene can't go.
    v(run, "VerticalRemoveScene", {"scene": "Vertical"})
    answer = v(run, "VerticalRemoveScene", {"scene": "Gaming"})
    assert answer["ok"] is False
    v(run, "VerticalCreateScene", {"name": "Vertical"})


def test_direbot_hears_what_changed(run):
    got = []

    def on_vendor_event(data):
        if data.vendor_name == "direbot":
            got.append((data.event_type, data.event_data))

    o = run["obs"]
    events = obsws.EventClient(host="127.0.0.1", port=o.link["port"],
                               password=o.link["password"],
                               subs=obsws.Subs.VENDORS, timeout=5)
    events.callback.register(on_vendor_event)
    try:
        time.sleep(0.3)
        v(run, "VerticalSetScene", {"scene": "Gaming"})
        assert wait_for(lambda: any(
            t == "VerticalChanged" and d.get("current") == "Gaming"
            for t, d in got))
        latest = [d for t, d in got if t == "VerticalChanged"][-1]
        # Lists come through obs_data whole: they're lists of objects.
        assert {s["name"] for s in latest["scenes"]} >= {"Gaming",
                                                          "Vertical"}
        assert len(latest["options"]["places"]) == 3
    finally:
        events.disconnect()


def test_the_tall_scene_follows_yours(run):
    send(run, "CreateScene", {"sceneName": "Just Chatting"})
    v(run, "VerticalCreateScene", {"name": "Just Chatting"})
    v(run, "VerticalSetScene", {"scene": "Vertical"})
    state = v(run, "VerticalSetFollow", {
        "on": True, "links": [{"main": "Scene", "tall": "Gaming"}]})
    assert state["follow"]["on"] is True
    send(run, "SetCurrentProgramScene", {"sceneName": "Just Chatting"})
    assert wait_for(lambda: v(run, "VerticalGetState")["current"] ==
                    "Just Chatting")
    send(run, "SetCurrentProgramScene", {"sceneName": "Scene"})
    assert wait_for(lambda: v(run, "VerticalGetState")["current"] ==
                    "Gaming")
    # A scene with nothing to go with leaves the tall one alone.
    send(run, "SetCurrentProgramScene", {"sceneName": "Warm up"})
    time.sleep(0.5)
    assert v(run, "VerticalGetState")["current"] == "Gaming"


def test_direbot_looking_wakes_it_and_it_sleeps_again(run):
    answer = v(run, "VerticalLook", {"seconds": 2})
    assert answer["awake"] is True
    assert wait_for(lambda: v(run, "VerticalGetState")["awake"] is False, 6)


def test_recording_a_tall_file(run):
    send(run, "SetRecordDirectory", {"recordDirectory": run["records"]})
    state = v(run, "VerticalStartRecord")
    assert state["ok"] and state["recording"] and state["awake"]
    # Its sound: the track OBS's own stream sends (1, in Simple mode).
    assert state["outputs"]["recording"]["track"] == 1
    time.sleep(2.5)
    state = v(run, "VerticalPauseRecord", {"pause": True})
    assert state["outputs"]["recording"]["paused"] is True
    seconds = state["outputs"]["recording"]["seconds"]
    assert seconds >= 2
    time.sleep(1.2)
    state = v(run, "VerticalPauseRecord", {"pause": False})
    assert state["outputs"]["recording"]["seconds"] == seconds
    name = state["outputs"]["recording"]["file"]
    assert name.endswith((" Vertical.mp4", " Vertical.mkv", " Vertical.mov",
                          " Vertical.flv", " Vertical.ts"))
    time.sleep(1)
    v(run, "VerticalStopRecord")
    assert wait_for(lambda: not v(run, "VerticalGetState")["recording"])
    path = os.path.join(run["records"], name)
    assert wait_for(lambda: os.path.getsize(path) > 10000)
    size = probe(path)
    assert size in (None, "1080,1920")
    # Nothing uses it now: asleep, and its encoder let go.
    assert wait_for(lambda: v(run, "VerticalGetState")["awake"] is False, 4)


def test_recording_with_obs_makes_two_files(run):
    v(run, "VerticalSetOptions", {"record_with_main": True})
    before = set(os.listdir(run["records"]))
    send(run, "StartRecord")
    assert wait_for(lambda: v(run, "VerticalGetState")["recording"], 10)
    assert v(run, "VerticalGetState")["with_main"]["record"] is True
    time.sleep(2)
    send(run, "StopRecord")
    assert wait_for(lambda: not v(run, "VerticalGetState")["recording"], 10)
    new = sorted(set(os.listdir(run["records"])) - before)
    assert len(new) == 2, new
    wide, tall = sorted(new, key=len)
    assert tall == os.path.splitext(wide)[0] + " Vertical" + \
        os.path.splitext(wide)[1]
    v(run, "VerticalSetOptions", {"record_with_main": False})


def test_direbot_picks_a_site_but_never_a_server_or_key(run):
    state = v(run, "VerticalSetOptions", {"places": [
        {"slot": 1, "platform": "youtube", "enabled": True}]})
    place = state["options"]["places"][0]
    assert place["platform"] == "youtube" and place["enabled"] is True
    assert place["host"] == "a.rtmp.youtube.com" and place["server_ok"]
    assert place["key_set"] is False
    # A server or key from DireBot is ignored.
    state = v(run, "VerticalSetOptions", {"places": [
        {"slot": 1, "server": "rtmp://elsewhere.example/app",
         "key": "nope"}]})
    place = state["options"]["places"][0]
    assert place["host"] == "a.rtmp.youtube.com" and not place["key_set"]
    answer = v(run, "VerticalStartStream", {"slot": 1})
    assert answer["ok"] is False and "stream key" in answer["error"]
    assert answer["streaming"] is False
    # Out of range or nonsense: no harm.
    state = v(run, "VerticalSetOptions", {"kbps": 999999, "audio_track": 9,
                                          "places": [{"slot": 7}]})
    assert state["options"]["kbps"] == 20000
    assert state["options"]["audio_track"] == 6
    v(run, "VerticalSetOptions", {"kbps": 6000, "audio_track": 0})


def test_the_key_window_opens_in_obs(run):
    answer = v(run, "VerticalOpenSettings")
    assert answer["ok"]
    assert wait_for(lambda: v(run, "VerticalGetState")["settings_open"], 10)
    if shutil.which("xdotool"):
        env = dict(os.environ, DISPLAY=run["obs"].display)

        def xdo(*args):
            return subprocess.run(["xdotool", *args], env=env,
                                  capture_output=True, text=True,
                                  timeout=10).stdout.split()
        # Focus, not activate: the pretend screen has no window manager.
        assert wait_for(lambda: xdo("search", "--name", "DireBot Vertical"),
                        10)
        win = xdo("search", "--name", "DireBot Vertical")[0]
        xdo("windowfocus", "--sync", win)
        xdo("key", "Escape")
        assert wait_for(
            lambda: not v(run, "VerticalGetState")["settings_open"], 10)


def test_obs_keeps_it_all_when_it_closes_and_opens_again(run):
    o = run["obs"]
    before = v(run, "VerticalGetState")
    assert o.close() == 0
    log = o.log()
    assert "Number of memory leaks: 0" in log

    # Where it goes, as OBS's window would have saved it: a key, locked
    # (a test build on Linux locks it as plain text).
    listener_folder = os.path.join(o.home, "Streams")
    os.makedirs(listener_folder, exist_ok=True)
    path = os.path.join(profile_dir(o.home), "direbot-vertical.json")
    saved = json.load(open(path))
    assert saved["places"][0]["platform"] == "youtube"
    run["vertical_port"] = free_port()
    saved["places"][0] = {
        "platform": "other", "enabled": True, "key": "plain:" + KEY,
        "server": "rtmp://127.0.0.1:%d/live" % run["vertical_port"]}
    saved["with_stream"] = True
    with open(path, "w") as f:
        json.dump(saved, f)

    run["obs"] = Obs(home=o.home)
    state = v(run, "VerticalGetState")
    assert state["exists"] and state["uuid"] == before["uuid"]
    assert (state["width"], state["height"]) == (1080, 1920)
    assert sorted(s["name"] for s in state["scenes"]) == \
        sorted(s["name"] for s in before["scenes"])
    assert state["current"] == before["current"]
    assert state["follow"] == before["follow"]
    gaming = next(s for s in state["scenes"] if s["name"] == "Gaming")
    items = send(run, "GetSceneItemList",
                 {"sceneUuid": gaming["uuid"]})["sceneItems"]
    assert [i["sourceName"] for i in items] == ["Tall colour"]
    place = state["options"]["places"][0]
    assert place["key_set"] is True and place["host"] == "127.0.0.1"


def test_streaming_with_its_key_and_never_telling_it(run):
    folder = os.path.join(run["obs"].home, "Streams")
    got = []

    def on_vendor_event(data):
        got.append(json.dumps(data.event_data))

    o = run["obs"]
    events = obsws.EventClient(host="127.0.0.1", port=o.link["port"],
                               password=o.link["password"],
                               subs=obsws.Subs.VENDORS, timeout=5)
    events.callback.register(on_vendor_event)
    # Listening where the saved server says.
    listener = Listener(folder, KEY, run["vertical_port"])
    try:
        # Its own sound: OBS's audio track 3 (DireBot's sound grid picks).
        state = v(run, "VerticalSetOptions", {"places": [
            {"slot": 1, "audio_track": 3}]})
        assert state["options"]["places"][0]["audio_track"] == 3
        state = v(run, "VerticalStartStream", {"slot": 1})
        assert state["ok"], state
        assert wait_for(lambda: v(run, "VerticalGetState")["outputs"]
                        ["streams"][0]["live"], 15)
        time.sleep(3)
        state = v(run, "VerticalGetState")
        assert state["outputs"]["streams"][0]["kbps"] > 0
        assert state["outputs"]["streams"][0]["track"] == 3
        assert state["awake"] is True
        v(run, "VerticalStopStream", {"slot": 1})
        assert wait_for(lambda: not v(run, "VerticalGetState")["streaming"],
                        10)
        file = listener.done()
        assert os.path.getsize(file) > 10000
        assert probe(file) in (None, "1080,1920")
        assert probe(file, "a:0", "stream=codec_name") in (None, "aac")
        time.sleep(0.5)
    finally:
        if listener.proc.poll() is None:
            listener.proc.kill()
        events.disconnect()
        v(run, "VerticalSetOptions", {"places": [
            {"slot": 1, "audio_track": 0}]})
    # The key went to OBS's stream and nowhere else.
    assert KEY not in json.dumps(v(run, "VerticalGetState"))
    assert got and not any(KEY in e for e in got)
    assert KEY not in run["obs"].log()
    assert MAIN_KEY not in run["obs"].log()


def test_going_live_and_stopping_with_obs(run):
    folder = os.path.join(run["obs"].home, "Streams")
    main = Listener(folder, MAIN_KEY)
    tall = Listener(folder, KEY, run["vertical_port"])
    try:
        send(run, "SetStreamServiceSettings", {
            "streamServiceType": "rtmp_custom",
            "streamServiceSettings": {"server": main.url, "key": MAIN_KEY}})
        send(run, "StartStream")
        assert wait_for(lambda: v(run, "VerticalGetState")["streaming"], 20)
        assert v(run, "VerticalGetState")["with_main"]["streams"] is True
        time.sleep(2)
        send(run, "StopStream")
        assert wait_for(lambda: not v(run, "VerticalGetState")["streaming"],
                        15)
        assert os.path.getsize(tall.done()) > 10000
        main.done()
    finally:
        for p in (main.proc, tall.proc):
            if p.poll() is None:
                p.kill()
    # OBS's own stream settings are its own: the tall stream's are apart.
    # (The link shows OBS's key as stars, so it's read from OBS's file.)
    settings = send(run, "GetStreamServiceSettings")
    assert settings["streamServiceSettings"]["server"] == main.url
    assert settings["streamServiceSettings"]["key"] == "********"
    files = glob.glob(os.path.join(run["obs"].home, ".config", "obs-studio",
                                   "basic", "profiles", "*", "service.json"))
    assert files
    with open(files[0], encoding="utf-8") as f:
        assert json.load(f)["settings"]["key"] == MAIN_KEY


def test_a_site_dropping_then_ending_your_stream(run):
    """The tall site goes away while you're live, so its stream waits to
    reconnect; ending your stream then stops it, and OBS carries on (it
    used to wait on itself for good here)."""
    folder = os.path.join(run["obs"].home, "Streams")
    main = Listener(folder, MAIN_KEY)
    tall = Listener(folder, KEY, run["vertical_port"])
    try:
        send(run, "SetStreamServiceSettings", {
            "streamServiceType": "rtmp_custom",
            "streamServiceSettings": {"server": main.url, "key": MAIN_KEY}})
        send(run, "StartStream")
        assert wait_for(lambda: v(run, "VerticalGetState")["outputs"]
                        ["streams"][0]["live"], 20)
        tall.proc.kill()
        assert wait_for(lambda: v(run, "VerticalGetState")["outputs"]
                        ["streams"][0]["reconnecting"], 15)
        send(run, "StopStream")
        assert wait_for(lambda: not send(run, "GetStreamStatus")
                        ["outputActive"], 15)
        assert wait_for(lambda: not v(run, "VerticalGetState")["streaming"],
                        10)
        main.done()
    finally:
        for p in (main.proc, tall.proc):
            if p.poll() is None:
                p.kill()


def test_another_profile_stops_its_streams(run):
    """Where it goes and the keys are the profile's, so switching profile
    mid-stream stops it - on OBS 31 and up, making a new profile switches
    to it without saying it's changing first. Back on yours, it's all
    there."""
    folder = os.path.join(run["obs"].home, "Streams")
    tall = Listener(folder, KEY, run["vertical_port"])
    mine = send(run, "GetProfileList")["currentProfileName"]
    try:
        assert v(run, "VerticalStartStream", {"slot": 1})["ok"]
        assert wait_for(lambda: v(run, "VerticalGetState")["outputs"]
                        ["streams"][0]["live"], 15)
        time.sleep(2)
        send(run, "CreateProfile", {"profileName": "Another"})
        assert wait_for(lambda: send(run, "GetProfileList")
                        ["currentProfileName"] == "Another", 10)
        assert wait_for(lambda: not v(run, "VerticalGetState")["streaming"],
                        10)
        assert v(run, "VerticalGetState")["exists"]
        assert not v(run, "VerticalGetState")["options"]["places"][0][
            "key_set"]
        assert os.path.getsize(tall.done()) > 10000
        send(run, "SetCurrentProfile", {"profileName": mine})
        assert wait_for(lambda: v(run, "VerticalGetState")["options"][
            "places"][0]["key_set"], 10)
    finally:
        if tall.proc.poll() is None:
            tall.proc.kill()
    assert "Another profile: the vertical streams and recording stop" in \
        run["obs"].log()


def test_closing_obs_lets_everything_go(run):
    o = run["obs"]
    v(run, "VerticalLook", {"seconds": 30})
    started = time.time()
    assert o.close() == 0
    assert time.time() - started < 20
    log = o.log()
    # OBS's own RTMP output loses the packet it was sending when a server
    # drops it (rtmp-stream.c's send_packet and send_headers return without
    # letting it go, OBS 32 and its master too): at most one per drop, and
    # the only drop here is test_a_site_dropping_then_ending_your_stream's.
    leaks = int(re.search(r"Number of memory leaks: (\d+)", log).group(1))
    assert leaks <= log.count("Disconnected from rtmp://"), log[-2000:]
    assert log.count("Tried to call obs_frontend_remove") <= 2
    assert KEY not in log
