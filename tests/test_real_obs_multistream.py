"""
DireBot Multistream inside a real OBS: places stream to test servers on
this PC (ffmpeg listening for RTMP), with the very client DireBot uses
(obsws-python) over OBS's normal websocket - the same requests go through
DireBot Link too, which needs a newer OBS than some Linux PCs have. OBS
runs on a pretend screen (Xvfb) with its own empty settings, so nothing of
anyone's is touched, and nothing goes out to the internet.

Skipped unless OBS, Xvfb, ffmpeg and the test build are there (Linux):

    add-apt-repository ppa:obsproject/obs-studio   (OBS 31.1 or newer)
    apt install obs-studio xvfb xdotool ffmpeg
    BUILD_DIR=/tmp/direbot-obs tools/build-linux-test.sh
    DIREBOT_OBS_PLUGIN=/tmp/direbot-obs/direbot-for-obs.so \\
        python3 -m pytest tests/test_real_obs_multistream.py
"""

import json
import math
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import wave

import pytest

obsws = pytest.importorskip("obsws_python")

PLUGIN = os.environ.get("DIREBOT_OBS_PLUGIN", "")
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

pytestmark = pytest.mark.skipif(
    not (PLUGIN and os.path.exists(PLUGIN) and shutil.which("obs")
         and shutil.which("Xvfb") and shutil.which("ffmpeg")),
    reason="needs OBS, Xvfb, ffmpeg and the test build (see the top)")

# Stream keys in this test: none may ever show up in a report, a log line
# or anywhere but the profile's own file.
KEYS = ("SECRETKEYONE", "SECRETKEYTWO", "IMPORTEDKEY9", "MAINSTREAMKEY")
PROFILE = "DireBotA"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(test, seconds=10.0, step=0.1):
    until = time.time() + seconds
    while time.time() < until:
        try:
            if test():
                return True
        except Exception:
            pass
        time.sleep(step)
    return bool(test())


def sine_wav(path, seconds=2, rate=48000, hz=440, level=0.5):
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(seconds * rate):
            v = int(level * 32767 * math.sin(2 * math.pi * hz * i / rate))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))


class Listener:
    """A pretend streaming site: ffmpeg taking one RTMP stream into a
    file."""

    def __init__(self, folder, name, port=None):
        self.port = port or free_port()
        self.server = "rtmp://127.0.0.1:%d/live" % self.port
        self.file = os.path.join(folder, name + ".flv")
        self.log = open(os.path.join(folder, name + ".log"), "w")
        self.proc = subprocess.Popen(
            ["ffmpeg", "-nostdin", "-loglevel", "error", "-listen", "1",
             "-i", self.server + "/x", "-c", "copy", "-y", self.file],
            stdout=self.log, stderr=subprocess.STDOUT)

    def listening(self):
        """ffmpeg is waiting for the stream (without connecting: it takes
        one connection only)."""
        try:
            rows = open("/proc/net/tcp").read().splitlines()[1:]
        except OSError:
            return True
        port = ":%04X" % self.port
        return any(r.split()[1].endswith(port) and r.split()[3] == "0A"
                   for r in rows)

    def finish(self, seconds=15):
        try:
            self.proc.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            self.proc.terminate()
            self.proc.wait()
        self.log.close()

    def streams(self):
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries",
             "stream=codec_type,codec_name,height", "-of", "json",
             self.file], capture_output=True, text=True).stdout
        return json.loads(out or "{}").get("streams", [])

    def loudest(self):
        """The loudest the sound got, in dB (about -91: silence)."""
        out = subprocess.run(
            ["ffmpeg", "-nostdin", "-i", self.file, "-vn", "-af",
             "volumedetect", "-f", "null", "-"],
            capture_output=True, text=True).stderr
        m = re.search(r"max_volume: (-?[\d.]+) dB", out)
        return float(m.group(1)) if m else None


class Obs:
    def __init__(self):
        self.home = tempfile.mkdtemp(prefix="direbot-obs-ms-")
        self.config = os.path.join(self.home, ".config", "obs-studio")
        plugin = os.path.join(self.config, "plugins", "direbot-for-obs")
        os.makedirs(os.path.join(plugin, "bin", "64bit"))
        shutil.copy(PLUGIN, os.path.join(plugin, "bin", "64bit"))
        shutil.copytree(os.path.join(HERE, "data"),
                        os.path.join(plugin, "data"))
        self.ws_port = free_port()
        ws = os.path.join(self.config, "plugin_config", "obs-websocket")
        os.makedirs(ws)
        with open(os.path.join(ws, "config.json"), "w") as f:
            json.dump({"alerts_enabled": False, "auth_required": False,
                       "first_load": False, "server_enabled": True,
                       "server_password": "", "server_port": self.ws_port},
                      f)
        # obs-websocket before 5.5 (OBS 30.0 and 30.1) keeps it here. And
        # no first-run setup wizard ("FirstRun" means it has run): while
        # it's open, OBS won't stream.
        with open(os.path.join(self.config, "global.ini"), "w") as f:
            f.write("[General]\nFirstRun=true\n"
                    "[OBSWebSocket]\nFirstLoad=false\nServerEnabled=true\n"
                    "ServerPort=%d\nAlertsEnabled=false\nAuthRequired=false\n"
                    "ServerPassword=\n" % self.ws_port)
        # OBS 31 and newer keep FirstRun here.
        with open(os.path.join(self.config, "user.ini"), "w") as f:
            f.write("[General]\nFirstRun=true\n")
        self.logs = os.path.join(self.config, "logs")
        self.sound = os.path.join(self.home, "tone.wav")
        sine_wav(self.sound)

        self.display = ":%d" % random.randint(401, 650)
        self.xvfb = subprocess.Popen(
            ["Xvfb", self.display, "-screen", "0", "1600x900x24",
             "-nolisten", "tcp"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        nowhere = "http://127.0.0.1:9"
        self.env = dict(os.environ, HOME=self.home,
                        XDG_CONFIG_HOME=os.path.join(self.home, ".config"),
                        DISPLAY=self.display, LIBGL_ALWAYS_SOFTWARE="1",
                        http_proxy=nowhere, https_proxy=nowhere,
                        HTTP_PROXY=nowhere, HTTPS_PROXY=nowhere,
                        no_proxy="")
        # An OBS built elsewhere may need its own libraries loaded first:
        # DIREBOT_OBS_PRELOAD (see test_real_obs.py).
        if os.environ.get("DIREBOT_OBS_PRELOAD"):
            self.env["LD_PRELOAD"] = os.environ["DIREBOT_OBS_PRELOAD"]
        self.out = open(os.path.join(self.home, "obs-out.log"), "w")
        self.proc = subprocess.Popen(
            ["obs", "--disable-updater", "--disable-shutdown-check",
             "--multi"], cwd=self.home, env=self.env, stdout=self.out,
            stderr=subprocess.STDOUT)
        self.c = None
        try:
            up = wait_for(self._connect, 90, 0.5)
        except Exception:
            up = False
        if not up:
            self.close()
            raise AssertionError("OBS's websocket never came on:\n" +
                                 self.log())
        # OBS's window is busy for a few seconds after it opens, and a
        # newer OBS answers "not ready" until it has loaded.
        wait_for(self._warm_up, 60, 0.5)

    def _warm_up(self):
        try:
            self.c.create_scene("Warm up")
        except obsws.error.OBSSDKRequestError as e:
            if e.code == 207:           # OBS isn't ready yet
                return False
        return True

    def _connect(self):
        self.c = obsws.ReqClient(host="127.0.0.1", port=self.ws_port,
                                 password="", timeout=30)
        return True

    def vendor(self, request, data=None):
        return self.c.send("CallVendorRequest", {
            "vendorName": "direbot", "requestType": request,
            "requestData": data or {}}, raw=True)["responseData"]

    def profile(self, name, create=False):
        """Switches OBS to profile `name` (making it first), and waits:
        OBS 31 and newer switch in their own time."""
        self.c.send("CreateProfile" if create else "SetCurrentProfile",
                    {"profileName": name})
        assert wait_for(lambda: self.c.get_profile_list()
                        .current_profile_name == name, 10, 0.1)
        assert wait_for(lambda: self.vendor("GetMultistream")
                        .get("profile") == name, 10, 0.1)

    def profile_dir(self, name):
        return os.path.join(self.config, "basic", "profiles", name)

    def log(self):
        try:
            newest = max(os.listdir(self.logs))
            return open(os.path.join(self.logs, newest),
                        errors="replace").read()
        except (OSError, ValueError):
            return ""

    def xdo(self, *args):
        return subprocess.run(["xdotool", *args], env=self.env,
                              capture_output=True, text=True,
                              timeout=10).stdout.split()

    def close(self, seconds=30):
        if self.c:
            try:
                self.c.disconnect()
            except Exception:
                pass
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=seconds)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
                raise AssertionError("OBS didn't close")
        self.xvfb.terminate()
        self.xvfb.wait()
        self.out.close()
        return self.proc.returncode


@pytest.fixture(scope="module")
def obs():
    o = Obs()
    yield o
    if o.proc.poll() is None:
        o.close()
    # DIREBOT_OBS_KEEP=1 keeps OBS's settings and logs to look at.
    if not os.environ.get("DIREBOT_OBS_KEEP"):
        shutil.rmtree(o.home, ignore_errors=True)


def place(report, name):
    return next(p for p in report["places"] if p["name"] == name)


def no_keys_in(text):
    for key in KEYS:
        assert key not in text, key


def test_multistream_is_there_with_nothing_set_up(obs):
    info = obs.vendor("GetInfo")
    assert info["features"]["multistream"] is True
    report = obs.vendor("GetMultistream")
    assert report["ok"] is True and report["api"] == 1
    assert report["places"] == []
    assert report["main"]["live"] is False
    assert [t["track"] for t in report["tracks"]] == [1, 2, 3, 4, 5, 6]
    assert {"youtube", "kick", "tiktok", "other"} <= {
        s["key"] for s in report["sites"]}
    assert any(e["id"] == "obs_x264" for e in report["encoders"])


def test_a_profiles_places_load_with_it(obs, tmp_path):
    obs.profile(PROFILE, create=True)
    folder = obs.profile_dir(PROFILE)
    assert os.path.isdir(folder)
    obs.one = Listener(str(tmp_path), "one")
    obs.two = Listener(str(tmp_path), "two")
    obs.main_site = Listener(str(tmp_path), "main")
    # Written while OBS is on another profile (OBS saves a profile's
    # settings as it leaves it).
    obs.profile("Untitled")
    assert obs.vendor("GetMultistream")["places"] == []
    # Where OBS streams in this profile (obs-websocket 5.3's
    # SetStreamServiceSettings leaks the service it makes).
    with open(os.path.join(folder, "service.json"), "w") as f:
        json.dump({"type": "rtmp_custom", "settings": {
            "server": obs.main_site.server, "key": "MAINSTREAMKEY"}}, f)
    # As DireBot Multistream's window in OBS saves them (on Windows the
    # keys are locked to the Windows account; here they're "plain:").
    with open(os.path.join(folder, "direbot-multistream.json"), "w") as f:
        json.dump({"format": 1, "lighter": {"kbps": 1200, "height": 480},
                   "places": [
                       {"id": "lighter1", "name": "Lighter one",
                        "site": "other", "server": obs.one.server,
                        "key": "plain:SECRETKEYONE", "video": "lighter",
                        "track": 2, "with_stream": False,
                        "stop_with_stream": False},
                       {"id": "same0001", "name": "Same one",
                        "site": "youtube", "server": obs.two.server,
                        "key": "plain:SECRETKEYTWO", "video": "same",
                        "track": 0},
                       {"id": "nokey001", "name": "No key", "site": "kick",
                        "server": "rtmp://127.0.0.1:9/app"},
                       {"id": "lost0001", "name": "Locked elsewhere",
                        "site": "other", "server": "rtmp://127.0.0.1:9/x",
                        "key": "dpapi:AAAA"}]}, f)
    # Places load when OBS switches to their profile.
    obs.profile(PROFILE)
    report = obs.vendor("GetMultistream")
    assert report["profile"] == PROFILE
    assert [p["name"] for p in report["places"]] == [
        "Lighter one", "Same one", "No key", "Locked elsewhere"]
    one = place(report, "Lighter one")
    assert one["key_set"] is True and one["ready"] == ""
    assert one["host"] == "127.0.0.1" and one["protocol"] == "RTMP"
    assert one["output"] == "direbot_multistream_lighter1"
    assert place(report, "No key")["ready"].endswith("has no stream key yet.")
    lost = place(report, "Locked elsewhere")
    assert lost["key_lost"] is True and lost["key_set"] is False
    assert report["lighter"] == {"kbps": 1200, "height": 480, "encoder": ""}
    # Never a key, and never a whole server address (a key can be in it).
    text = json.dumps(report)
    no_keys_in(text)
    assert obs.one.server not in text
    assert "Multistream: 4 place(s) in this profile." in obs.log()
    assert "1 stream key(s) were typed in on another PC" in obs.log()


def test_what_cant_start_says_why(obs):
    r = obs.vendor("MultistreamStart", {"id": "nokey001"})
    assert r["ok"] is False and "no stream key" in r["error"]
    r = obs.vendor("MultistreamStart", {"id": "lost0001"})
    assert r["ok"] is False and "typed in again" in r["error"]
    # Your stream's own picture: only once you're live.
    r = obs.vendor("MultistreamStart", {"id": "same0001"})
    assert r["ok"] is False and r["error"].startswith("Go live first")
    r = obs.vendor("MultistreamStart", {"id": "gone"})
    assert r["ok"] is False and "isn't in DireBot Multistream" in r["error"]
    # A server or key is never set from outside OBS.
    r = obs.vendor("MultistreamSet", {"id": "lighter1",
                                      "changes": {"key": "x"}})
    assert r["ok"] is False and "only changed in OBS" in r["error"]
    r = obs.vendor("MultistreamSet", {"id": "lighter1",
                                      "changes": {"server": "rtmp://x"}})
    assert r["ok"] is False


def test_music_on_the_main_stream_only(obs):
    # A tone on audio track 1 only (your stream's), like music that may
    # only go to Twitch.
    obs.c.set_current_program_scene("Scene")
    obs.c.create_input("Scene", "Music", "ffmpeg_source",
                       {"local_file": obs.sound, "looping": True,
                        "is_local_file": True}, True)
    obs.c.send("SetInputAudioTracks", {
        "inputName": "Music", "inputAudioTracks": {
            "1": True, "2": False, "3": False, "4": False, "5": False,
            "6": False}})
    # A place on track 2 and a Lighter picture starts on its own, before
    # you're live.
    r = obs.vendor("MultistreamStart", {"id": "lighter1"})
    assert r["ok"] is True, r
    assert wait_for(lambda: place(obs.vendor("GetMultistream"),
                                  "Lighter one")["state"] == "sending", 20)
    time.sleep(3)
    one = place(obs.vendor("GetMultistream"), "Lighter one")
    assert one["bytes"] > 10000 and one["sending_video"] == "lighter"
    assert one["sending_kbps"] == 1200 and one["seconds"] > 1

    # Then your stream, to the main pretend site: "Same one" goes with it.
    # A place sharing your stream's picture joins at its next keyframe:
    # every second here (Twitch has one every 2 seconds), not x264's 8.
    obs.c.send("SetProfileParameter", {
        "parameterCategory": "SimpleOutput",
        "parameterName": "x264Settings", "parameterValue": "keyint=30"})
    obs.c.start_stream()
    assert wait_for(lambda: obs.c.get_stream_status().output_active, 20)
    assert wait_for(lambda: place(obs.vendor("GetMultistream"),
                                  "Same one")["state"] == "sending", 20)
    report = obs.vendor("GetMultistream")
    assert report["main"]["live"] is True
    assert report["main"]["audio_track"] == 1
    same = place(report, "Same one")
    assert same["sending_video"] == "same"
    assert same["sending_kbps"] == report["main"]["video_kbps"] > 0
    assert wait_for(lambda: place(obs.vendor("GetMultistream"),
                                  "Same one")["frames"] > 60, 20)

    # Ending your stream ends "Same one" but not "Lighter one" (set not
    # to).
    obs.c.stop_stream()
    assert wait_for(lambda: not obs.c.get_stream_status().output_active, 20)
    assert wait_for(lambda: place(obs.vendor("GetMultistream"),
                                  "Same one")["state"] == "off", 20)
    assert place(obs.vendor("GetMultistream"),
                 "Lighter one")["state"] == "sending"
    r = obs.vendor("MultistreamStopAll")
    assert r["ok"] is True and r["stopped"] == 1
    assert wait_for(lambda: place(obs.vendor("GetMultistream"),
                                  "Lighter one")["state"] == "off", 20)
    for site in (obs.one, obs.two, obs.main_site):
        site.finish()

    # What each site got: the lighter picture at its size, and the music
    # everywhere but on the place set to audio track 2.
    def kinds(site):
        return {s["codec_type"]: s for s in site.streams()}
    one, two, main = kinds(obs.one), kinds(obs.two), kinds(obs.main_site)
    assert one["video"]["height"] == 480
    assert two["video"]["height"] == main["video"]["height"]
    assert "audio" in one and "audio" in two and "audio" in main
    assert obs.main_site.loudest() > -20      # the music
    assert obs.two.loudest() > -20            # the same sound as your stream
    assert obs.one.loudest() < -60            # no music: track 2


def test_changes_from_direbot_and_new_places(obs):
    r = obs.vendor("MultistreamSet", {"id": "same0001", "changes": {
        "video": "own", "kbps": 3000, "height": 720, "track": 3,
        "with_stream": False}})
    assert r["ok"] is True
    p = r["place"]
    assert (p["video"], p["kbps"], p["height"], p["track"],
            p["with_stream"]) == ("own", 3000, 720, 3, False)
    r = obs.vendor("MultistreamSet", {"id": "same0001",
                                      "changes": {"kbps": "lots"}})
    assert r["ok"] is False
    r = obs.vendor("MultistreamSet", {"id": "lighter",
                                      "changes": {"kbps": 1500}})
    assert r["ok"] is True
    assert obs.vendor("GetMultistream")["lighter"]["kbps"] == 1500

    r = obs.vendor("MultistreamAdd", {"site": "youtube"})
    assert r["ok"] is True and r["name"] == "YouTube"
    new = place(obs.vendor("GetMultistream"), "YouTube")
    assert new["key_set"] is False and new["server_set"] is True
    assert new["host"] == "a.rtmps.youtube.com"
    assert new["protocol"] == "RTMPS"
    r = obs.vendor("MultistreamAdd", {"site": "youtube"})
    assert r["name"] == "YouTube 2"
    assert obs.vendor("MultistreamAdd", {"site": "myspace"})["ok"] is False

    # Saved in the profile within a second or so, keys locked.
    path = os.path.join(obs.profile_dir(PROFILE), "direbot-multistream.json")

    def saved():
        return json.load(open(path))
    assert wait_for(lambda: len(saved()["places"]) == 6, 5)
    f = saved()
    assert f["lighter"]["kbps"] == 1500
    keys = {p["name"]: p["key"] for p in f["places"]}
    assert keys["Lighter one"] == "plain:SECRETKEYONE"
    assert keys["Locked elsewhere"] == "dpapi:AAAA"   # never lost
    assert keys["YouTube"] == ""


def test_bringing_places_over_from_multiple_rtmp_outputs(obs):
    folder = obs.profile_dir(PROFILE)
    r = obs.vendor("MultistreamImport", {"from": "multi_rtmp"})
    assert r["ok"] is False and "nothing saved" in r["error"]
    with open(os.path.join(folder, "obs-multi-rtmp.json"), "w") as f:
        json.dump({"targets": [
            {"id": "t1", "name": "Kick", "sync-start": True,
             "service-param": {"server": "rtmp://127.0.0.1:9/app",
                               "key": "IMPORTEDKEY9"}}]}, f)
    r = obs.vendor("MultistreamImport", {"from": "multi_rtmp"})
    assert r["ok"] is True and [p["name"] for p in r["added"]] == ["Kick"]
    kick = place(obs.vendor("GetMultistream"), "Kick")
    # Switched off until the old add-on's copy is.
    assert kick["on"] is False and kick["key_set"] is True
    assert kick["came_from"] == "multi_rtmp" and kick["site"] == "kick"
    # Twice: nothing new.
    r = obs.vendor("MultistreamImport", {"from": "multi_rtmp"})
    assert r["ok"] is True and r.get("added", []) == []
    assert obs.vendor("MultistreamImport",
                      {"from": "elsewhere"})["ok"] is False
    no_keys_in(json.dumps(obs.vendor("GetMultistream")))


def test_the_window_in_obs(obs):
    if not shutil.which("xdotool"):
        pytest.skip("needs xdotool")

    def window():
        found = obs.xdo("search", "--name", "DireBot Multistream")
        return found[0] if found else None

    def press(win, key):
        obs.xdo("windowfocus", "--sync", win)
        obs.xdo("key", key)

    def is_open():
        return obs.vendor("GetMultistream")["window_open"]

    r = obs.vendor("MultistreamOpen", {"id": "lighter1"})
    assert r["ok"] is True
    assert wait_for(window, 10) and wait_for(is_open, 5)
    # The window isn't a source anyone sees.
    names = [i["inputName"] for i in obs.c.get_input_list().inputs]
    assert "DireBot Multistream" not in names
    # Cancel (Escape): nothing changes, the key stays.
    press(window(), "Escape")
    assert wait_for(lambda: not is_open(), 10)
    report = obs.vendor("GetMultistream")
    assert place(report, "Lighter one")["key_set"] is True
    assert len(report["places"]) == 7

    # OK with a change in it puts the window's settings on OBS's Undo list;
    # Undo and Redo with the window shut change nothing.
    obs.vendor("MultistreamOpen", {})
    assert wait_for(window, 10) and wait_for(is_open, 5)
    obs.vendor("MultistreamSet", {"id": "lighter1",
                                  "changes": {"name": "Renamed"}})
    time.sleep(0.5)
    press(window(), "Return")
    assert wait_for(lambda: not is_open(), 10)
    assert place(obs.vendor("GetMultistream"), "Renamed")["key_set"]
    main = obs.xdo("search", "--name", "^OBS ")[0]
    for key in ("ctrl+z", "ctrl+y"):
        press(main, key)
        time.sleep(1)
        report = obs.vendor("GetMultistream")
        assert place(report, "Renamed")["key_set"] is True
        assert len(report["places"]) == 7
    assert "OBS's Undo / Redo doesn't change places" in obs.log()


def test_another_profile_with_a_damaged_file(obs):
    obs.profile("DireBotB", create=True)
    folder = obs.profile_dir("DireBotB")
    with open(os.path.join(folder, "direbot-multistream.json"), "w") as f:
        f.write("{not json")
    obs.profile(PROFILE)
    obs.profile("DireBotB")
    report = obs.vendor("GetMultistream")
    assert report["places"] == [] and report["file_problem"] == "damaged"
    # Kept beside, never written over.
    assert open(os.path.join(folder, "direbot-multistream.json.damaged")
                ).read() == "{not json"
    obs.profile(PROFILE)
    assert len(obs.vendor("GetMultistream")["places"]) == 7


def test_a_new_profile_saves_and_stops_this_ones_places(obs, tmp_path):
    # From OBS 31 on, OBS makes a new profile without first saying it's
    # leaving this one.
    site = Listener(str(tmp_path), "again", obs.one.port)
    assert wait_for(site.listening, 10)
    assert obs.vendor("MultistreamStart", {"id": "lighter1"})["ok"] is True
    assert wait_for(lambda: place(obs.vendor("GetMultistream"),
                                  "Renamed")["state"] == "sending", 20)
    # Changed a moment before.
    obs.vendor("MultistreamSet", {"id": "same0001",
                                  "changes": {"name": "Same again"}})
    obs.profile("DireBotC", create=True)
    assert obs.vendor("GetMultistream")["places"] == []

    def ours_sending():
        return [o["outputName"] for o in obs.c.get_output_list().outputs
                if o["outputName"].startswith("direbot_multistream_")
                and o["outputActive"]]
    assert wait_for(lambda: not ours_sending(), 10), ours_sending()
    site.finish()
    saved = json.load(open(os.path.join(obs.profile_dir(PROFILE),
                                        "direbot-multistream.json")))
    assert "Same again" in [p["name"] for p in saved["places"]]
    obs.profile(PROFILE)
    assert len(obs.vendor("GetMultistream")["places"]) == 7


def test_closing_obs_leaves_nothing_behind(obs):
    code = obs.close()
    assert code == 0
    log = obs.log()
    assert "[direbot-for-obs] unloaded" in log
    assert "Number of memory leaks: 0" in log
    # No stream key in OBS's log, ever (OBS's own included: it never
    # writes them).
    no_keys_in(log)
    no_keys_in(open(os.path.join(obs.home, "obs-out.log"),
                    errors="replace").read())
    assert "Multistream: starting Lighter one (127.0.0.1, lighter picture," \
        " audio track 2)." in log
    # Saved as it was left.
    saved = json.load(open(os.path.join(obs.profile_dir(PROFILE),
                                        "direbot-multistream.json")))
    keys = {p["name"]: p["key"] for p in saved["places"]}
    assert keys["Renamed"] == "plain:SECRETKEYONE"
    assert keys["Kick"] == "plain:IMPORTEDKEY9"
