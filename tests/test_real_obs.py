"""
DireBot for OBS inside a real OBS, with the very client DireBot uses
(obsws-python). OBS runs on a pretend screen (Xvfb) with its own empty
settings, so nothing of anyone's is touched, and nothing streams.

Skipped unless OBS, Xvfb and the test build are there (Linux):

    apt install obs-studio libobs-dev xvfb xdotool
    BUILD_DIR=/tmp/direbot-obs tools/build-linux-test.sh
    DIREBOT_OBS_PLUGIN=/tmp/direbot-obs/direbot-for-obs.so \\
        python3 -m pytest tests/test_real_obs.py
"""

import json
import math
import os
import random
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
         and shutil.which("Xvfb")),
    reason="needs OBS, Xvfb and the test build (see the top of this file)")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(test, seconds=10.0, step=0.05):
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


class Obs:
    """One OBS, from start to a proper close."""

    def __init__(self):
        self.home = tempfile.mkdtemp(prefix="direbot-obs-home-")
        config = os.path.join(self.home, ".config", "obs-studio")
        plugin = os.path.join(config, "plugins", "direbot-for-obs")
        os.makedirs(os.path.join(plugin, "bin", "64bit"))
        shutil.copy(PLUGIN, os.path.join(plugin, "bin", "64bit"))
        shutil.copytree(os.path.join(HERE, "data"),
                        os.path.join(plugin, "data"))
        # OBS's normal websocket too, to check the plugin's requests work
        # over it as well.
        self.ws_port = free_port()
        ws = os.path.join(config, "plugin_config", "obs-websocket")
        os.makedirs(ws)
        with open(os.path.join(ws, "config.json"), "w") as f:
            json.dump({"alerts_enabled": False, "auth_required": False,
                       "first_load": False, "server_enabled": True,
                       "server_password": "", "server_port": self.ws_port},
                      f)
        self.link_path = os.path.join(config, "plugin_config",
                                      "direbot-for-obs", "link.json")
        self.logs = os.path.join(config, "logs")
        self.sound = os.path.join(self.home, "tone.wav")
        sine_wav(self.sound)

        self.display = ":%d" % random.randint(150, 400)
        self.xvfb = subprocess.Popen(
            ["Xvfb", self.display, "-screen", "0", "1600x900x24",
             "-nolisten", "tcp"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        # No internet for this OBS: its services list update then fails at
        # once instead of holding OBS's window while it waits.
        nowhere = "http://127.0.0.1:9"
        env = dict(os.environ, HOME=self.home,
                   XDG_CONFIG_HOME=os.path.join(self.home, ".config"),
                   DISPLAY=self.display, LIBGL_ALWAYS_SOFTWARE="1",
                   http_proxy=nowhere, https_proxy=nowhere,
                   HTTP_PROXY=nowhere, HTTPS_PROXY=nowhere, no_proxy="")
        # A sanitizer build of the plugin needs its runtime loaded first:
        # DIREBOT_OBS_PRELOAD=$(gcc -print-file-name=libasan.so)
        if os.environ.get("DIREBOT_OBS_PRELOAD"):
            env["LD_PRELOAD"] = os.environ["DIREBOT_OBS_PRELOAD"]
        self.out = open(os.path.join(self.home, "obs-out.log"), "w")
        self.proc = subprocess.Popen(
            ["obs", "--disable-updater", "--disable-shutdown-check",
             "--multi"], cwd=self.home, env=env, stdout=self.out,
            stderr=subprocess.STDOUT)
        if not wait_for(lambda: os.path.exists(self.link_path), 90, 0.2):
            self.close()
            raise AssertionError("the link never came on:\n" + self.log())
        self.link = json.load(open(self.link_path))
        # OBS's window is busy for a few seconds after it opens (it waits
        # for its services list), and adding a scene waits for the window.
        warm = obsws.ReqClient(host="127.0.0.1", port=self.link["port"],
                               password=self.link["password"], timeout=60)
        warm.create_scene("Warm up")
        warm.disconnect()

    def log(self):
        try:
            newest = max(os.listdir(self.logs))
            return open(os.path.join(self.logs, newest),
                        errors="replace").read()
        except (OSError, ValueError):
            return ""

    def client(self, password=None):
        return obsws.ReqClient(
            host="127.0.0.1", port=self.link["port"], timeout=5,
            password=self.link["password"] if password is None else password)

    def close(self, seconds=30):
        """Close OBS the way its window's X does (SIGINT on Linux)."""
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
    shutil.rmtree(o.home, ignore_errors=True)


def test_the_link_file_says_where_and_only_this_user_can_open_it(obs):
    link = obs.link
    assert link["format"] == 1 and link["plugin"] == "0.1.0"
    assert link["pid"] == obs.proc.pid
    assert len(link["password"]) >= 40
    assert link["obs"].startswith(("30.", "31.", "32."))
    assert "DireBot Link is on (port %d" % link["port"] in obs.log()
    # The password is never in OBS's log.
    assert link["password"] not in obs.log()


def test_requests_answer_through_real_obs_websocket(obs):
    c = obs.client()
    try:
        assert c.get_version().obs_version == obs.link["obs"]
        c.create_scene("DireBot test")
        names = [s["sceneName"] for s in c.get_scene_list().scenes]
        assert "DireBot test" in names
        with pytest.raises(obsws.error.OBSSDKRequestError) as e:
            c.create_scene("DireBot test")
        assert e.value.code == 601          # already there: OBS's own answer
        # Lists of plain values come back whole (obs_data can't hold them).
        kinds = c.send("GetInputKindList", {"unversioned": True}, raw=True)
        assert "ffmpeg_source" in kinds["inputKinds"]
    finally:
        c.disconnect()


def test_a_wrong_password_gets_nothing(obs):
    with pytest.raises(Exception):
        obs.client(password="not-it").get_version()
    c = obs.client()
    assert c.get_version().obs_version
    c.disconnect()


def test_scene_events_reach_direbot(obs):
    got = []

    def on_current_program_scene_changed(data):
        got.append(data.scene_name)

    events = obsws.EventClient(host="127.0.0.1", port=obs.link["port"],
                               password=obs.link["password"],
                               subs=obsws.Subs.SCENES, timeout=5)
    events.callback.register(on_current_program_scene_changed)
    c = obs.client()
    try:
        time.sleep(0.3)
        c.create_scene("DireBot events")
        c.set_current_program_scene("DireBot events")
        assert wait_for(lambda: "DireBot events" in got)
        c.set_current_program_scene("Scene")
        assert wait_for(lambda: got[-1:] == ["Scene"])
    finally:
        events.disconnect()
        c.disconnect()


def test_audio_levels_like_obs_websockets_own(obs):
    c = obs.client()
    c.set_current_program_scene("Scene")
    c.create_input("Scene", "Tone", "ffmpeg_source",
                   {"local_file": obs.sound, "looping": True,
                    "is_local_file": True}, True)
    levels = {}

    def on_input_volume_meters(data):
        for row in data.inputs:
            levels[row["inputName"]] = row["inputLevelsMul"]

    events = obsws.EventClient(host="127.0.0.1", port=obs.link["port"],
                               password=obs.link["password"],
                               subs=obsws.Subs.INPUTVOLUMEMETERS, timeout=5)
    events.callback.register(on_input_volume_meters)
    try:
        assert wait_for(lambda: levels.get("Tone") and
                        levels["Tone"][0][0] > 0.1, 15), levels
        channels = levels["Tone"]
        assert len(channels) == 2
        level, peak, input_peak = channels[0]
        # A sine at half volume: level ~0.35, peak ~0.5.
        assert 0.25 < level < 0.45 and 0.4 < peak < 0.6, channels
        c.set_input_mute("Tone", True)
        assert wait_for(lambda: levels["Tone"][0][0] == 0 and
                        levels["Tone"][0][2] > 0.4, 5), levels["Tone"]
        c.set_input_mute("Tone", False)
        c.set_input_volume("Tone", vol_mul=0.5)
        assert wait_for(lambda: 0.2 < levels["Tone"][0][1] < 0.3 and
                        levels["Tone"][0][2] > 0.4, 5), levels["Tone"]
    finally:
        events.disconnect()
        c.disconnect()


def vendor(client, request, data=None):
    return client.send("CallVendorRequest", {
        "vendorName": "direbot", "requestType": request,
        "requestData": data or {}}, raw=True)["responseData"]


def test_the_plugins_own_requests_over_the_link_and_obs_websocket(obs):
    c = obs.client()
    try:
        info = vendor(c, "GetInfo")
        assert info["plugin"] == "0.1.0" and info["api"] == 1
        assert info["link"] is True and info["link_port"] == obs.link["port"]
        assert info["features"] == {"link": True, "meters": True,
                                    "usage": True}
        usage = vendor(c, "GetUsage")
        assert usage["fps"] > 0 and usage["frame_budget_ms"] > 0
        assert usage["memory_mb"] > 10
        names = [s["name"] for s in usage["sources"]]
        assert "Tone" in names
        tone = next(s for s in usage["sources"] if s["name"] == "Tone")
        assert tone["kind"] == "ffmpeg_source" and tone["type"] == "input"
        assert tone["hints"]["local_file"] is True
        # No file paths or web addresses in a report.
        assert obs.sound not in json.dumps(usage)
        with pytest.raises(obsws.error.OBSSDKRequestError) as e:
            vendor(c, "NoSuchThing")
        assert e.value.code == 600
    finally:
        c.disconnect()
    # OBS's normal websocket (no password set for this test).
    plain = obsws.ReqClient(host="127.0.0.1", port=obs.ws_port, password="",
                            timeout=5)
    try:
        info = vendor(plain, "GetInfo")
        assert info["plugin"] == "0.1.0"
        assert info["features"]["link"] is True
    finally:
        plain.disconnect()


def test_a_browser_overlays_address_is_never_reported(obs):
    c = obs.client()
    try:
        c.create_input("Scene", "Overlay", "browser_source",
                       {"url": "http://127.0.0.1:8123/overlay?key=SECRET1",
                        "width": 1920, "height": 1080}, True)
        c.create_input("Scene", "Widget", "browser_source",
                       {"url": "https://example.com/widget/SECRET2"}, False)
        report = json.dumps(vendor(c, "GetUsage"))
        assert "SECRET" not in report and "example.com" not in report
        usage = json.loads(report)
        overlay = next(s for s in usage["sources"] if s["name"] == "Overlay")
        assert overlay["hints"]["this_pc"] is True
        assert overlay["hints"]["port"] == 8123
        widget = next(s for s in usage["sources"] if s["name"] == "Widget")
        assert widget["hints"]["this_pc"] is False
        assert "port" not in widget["hints"]
    except obsws.error.OBSSDKRequestError as e:
        if e.code == 605:     # this OBS was built without its browser
            pytest.skip("no browser source in this OBS")
        raise
    finally:
        c.disconnect()


def test_many_events_and_requests_at_once(obs):
    got = []

    def on_input_name_changed(data):
        got.append(data.input_name)

    events = obsws.EventClient(host="127.0.0.1", port=obs.link["port"],
                               password=obs.link["password"],
                               subs=obsws.Subs.INPUTS, timeout=5)
    events.callback.register(on_input_name_changed)
    clients = [obs.client() for _ in range(4)]
    try:
        time.sleep(0.3)
        name = "Tone"
        for i in range(40):
            new = f"Tone {i}"
            clients[i % 4].set_input_name(name, new)
            name = new
        assert wait_for(lambda: len(got) == 40), len(got)
        assert got == [f"Tone {i}" for i in range(40)]
        clients[0].set_input_name(name, "Tone")
    finally:
        events.disconnect()
        for c in clients:
            c.disconnect()


def test_closing_obs_while_direbot_is_connected(obs):
    # Connected, listening to the meters, and asking for things right up
    # to the moment OBS closes: OBS still closes at once, cleanly.
    c = obs.client()
    events = obsws.EventClient(host="127.0.0.1", port=obs.link["port"],
                               password=obs.link["password"],
                               subs=obsws.Subs.INPUTVOLUMEMETERS |
                               obsws.Subs.SCENES, timeout=5)
    vendor(c, "GetUsage")
    started = time.time()
    code = obs.close()
    assert time.time() - started < 15
    assert code == 0
    log = obs.log()
    assert "[direbot-for-obs] unloaded" in log
    # Its own lines only: nothing it did made OBS complain (obs-websocket
    # itself makes one of these on the way out).
    assert log.count("Tried to call obs_frontend_remove_event_callback") <= 1
    assert "Number of memory leaks: 0" in log
    # The link file goes with OBS.
    assert not os.path.exists(obs.link_path)
    for x in (events, c):
        try:
            x.disconnect()
        except Exception:
            pass
