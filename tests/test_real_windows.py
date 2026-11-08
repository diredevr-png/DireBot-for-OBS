"""
The Windows zip inside a real OBS for Windows - each OBS version the
add-on supports, the newest too. OBS 33 took functions out of obs.dll
that 0.8.1 linked to, and Windows then wouldn't load the add-on at all;
this is the check that catches that.

Runs only on a throwaway CI machine (it uses that Windows user's real OBS
settings folder and C:\\ProgramData\\obs-studio\\plugins):

    set CI=true
    set DIREBOT_OBS_ROOT=C:\\obs-33.0.1          (OBS's Windows zip, unpacked)
    set DIREBOT_OBS_ZIP=release\\direbot-for-obs-0.9.0-windows-x64.zip
    set DIREBOT_OBS_LAYOUT=new                    (new: OBS 33+, legacy: bin\\64bit)
    set DIREBOT_OBS_EXPECT=33.0.1
    python -m pytest tests/test_real_windows.py
"""

import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import zipfile

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_face import PretendPhone, PretendVmcApp, camera_picture, character_picture, read_bmp, unity_turn, write_gif  # noqa: E402
from test_real_obs import VERSION, free_port, vendor, wait_for  # noqa: E402

ROOT = os.environ.get("DIREBOT_OBS_ROOT", "")
ZIP = os.environ.get("DIREBOT_OBS_ZIP", "")
LAYOUT = os.environ.get("DIREBOT_OBS_LAYOUT", "legacy")
EXPECT = os.environ.get("DIREBOT_OBS_EXPECT", "")
MODULE = "direbot-for-obs"

READY = (sys.platform == "win32" and bool(os.environ.get("CI")) and bool(ROOT) and bool(ZIP)
         and os.path.exists(os.path.join(ROOT, "bin", "64bit", "obs64.exe"))
         and os.path.exists(ZIP))
# In CI a skip would look like a pass: there, missing OBS or the zip fails.
if os.environ.get("DIREBOT_OBS_REQUIRE") and not READY:
    raise RuntimeError("DIREBOT_OBS_REQUIRE is set but OBS or the zip isn't there")

pytestmark = pytest.mark.skipif(
    not READY, reason="Windows CI only, with OBS and the zip (see the top of this file)")


def put_in_add_on():
    """The zip into C:\\ProgramData\\obs-studio\\plugins, the way DireBot
    puts it: as it is (bin\\64bit) for older OBS, the DLL beside data\\
    for OBS 33 and newer."""
    plugins = os.path.join(os.environ.get("PROGRAMDATA", r"C:\ProgramData"),
                           "obs-studio", "plugins")
    folder = os.path.join(plugins, MODULE)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(plugins, exist_ok=True)
    with zipfile.ZipFile(ZIP) as z:
        z.extractall(plugins)
    if LAYOUT == "new":
        bin64 = os.path.join(folder, "bin", "64bit")
        for name in os.listdir(bin64):
            shutil.move(os.path.join(bin64, name), os.path.join(folder, name))
        shutil.rmtree(os.path.join(folder, "bin"))
    return folder


class WindowsObs:
    def __init__(self):
        self.home = os.path.join(os.environ["RUNNER_TEMP"] if "RUNNER_TEMP" in os.environ
                                 else os.environ["TEMP"], "direbot-obs-test")
        shutil.rmtree(self.home, ignore_errors=True)
        os.makedirs(self.home)
        self.plugin = put_in_add_on()
        config = os.path.join(os.environ["APPDATA"], "obs-studio")
        shutil.rmtree(config, ignore_errors=True)      # a throwaway machine
        os.makedirs(config)
        self.config = config
        for ini in ("user.ini", "global.ini"):
            with open(os.path.join(config, ini), "w") as f:
                f.write("[General]\nFirstRun=true\n")
        self.ws_port = free_port()
        ws = os.path.join(config, "plugin_config", "obs-websocket")
        os.makedirs(ws)
        with open(os.path.join(ws, "config.json"), "w") as f:
            json.dump({"alerts_enabled": False, "auth_required": False,
                       "first_load": False, "server_enabled": True,
                       "server_password": "", "server_port": self.ws_port}, f)
        self.link_path = os.path.join(config, "plugin_config", MODULE, "link.json")
        self.logs = os.path.join(config, "logs")
        bin64 = os.path.join(ROOT, "bin", "64bit")
        self.out = open(os.path.join(self.home, "obs-out.log"), "w")
        self.proc = subprocess.Popen(
            [os.path.join(bin64, "obs64.exe"), "--disable-updater",
             "--disable-shutdown-check", "--multi"],
            cwd=bin64, stdout=self.out, stderr=subprocess.STDOUT)
        if not wait_for(lambda: os.path.exists(self.link_path), 120, 0.25):
            log = self.log()
            self.kill()
            raise AssertionError("the link never came on - OBS's log:\n" + log)
        self.link = json.load(open(self.link_path))

    def log(self):
        try:
            newest = max(os.listdir(self.logs))
            return open(os.path.join(self.logs, newest), errors="replace").read()
        except (OSError, ValueError):
            return ""

    def client(self):
        return obsws.ReqClient(host="127.0.0.1", port=self.link["port"],
                               password=self.link["password"], timeout=30)

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.out.close()


@pytest.fixture(scope="module")
def obs():
    o = WindowsObs()
    yield o
    o.kill()
    print("\n----- OBS's log -----\n" + o.log())


def test_obs_loads_the_add_on(obs):
    log = obs.log()
    assert "[%s] version %s loaded" % (MODULE, VERSION) in log, log
    assert "%s.dll' not loaded" % MODULE not in log, log
    if EXPECT:
        assert obs.link["obs"] == EXPECT, obs.link
    c = obs.client()
    try:
        info = vendor(c, "GetInfo")
        assert info["plugin"] == VERSION and info["link"] is True
    finally:
        c.disconnect()


def count(rows, colour):
    """How many points are near `colour`."""
    return sum(1 for row in rows for r, g, b in row
               if abs(r - colour[0]) < 40 and abs(g - colour[1]) < 40 and abs(b - colour[2]) < 40)


ORANGE, BLUE, GREEN = (0xe0, 0x80, 0x30), (0x30, 0x60, 0xe0), (0x30, 0xd0, 0x40)


def shot(c, name):
    data = c.send("GetSourceScreenshot", {"sourceName": name, "imageFormat": "bmp",
                                          "imageWidth": 200}, raw=True)["imageData"]
    return read_bmp(base64.b64decode(data.split(",", 1)[1]))


def test_face_avatar_pictures_open(obs):
    """OBS's picture loader (the part OBS 33 changed): a PNG character
    draws, and an animated GIF thrown at it (one of chat's emotes) plays."""
    camera = os.path.join(obs.home, "camera.png")
    still = os.path.join(obs.home, "character.png")
    moving = os.path.join(obs.home, "emote.gif")
    camera_picture(camera)
    character_picture(still)
    # Blue, then green, a fifth of a second each.
    write_gif(moving, 40, 40, [[1] * 1600, [2] * 1600], [(0, 0, 0), BLUE, GREEN], 20)
    c = obs.client()
    try:
        c.send("CreateScene", {"sceneName": "Pictures"}, raw=True)
        c.send("CreateInput", {"sceneName": "Pictures", "inputName": "Camera",
                               "inputKind": "image_source",
                               "inputSettings": {"file": camera}}, raw=True)
        c.send("CreateInput", {"sceneName": "Pictures", "inputName": "Avatar",
                               "inputKind": "direbot_face_avatar",
                               "inputSettings": {"source": "Camera", "character": still}},
               raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Pictures"}, raw=True)
        assert wait_for(lambda: count(shot(c, "Avatar"), ORANGE) > 500, 30, 0.5), obs.log()

        answer = vendor(c, "FaceReact", {"source": "Avatar", "do": "throw", "file": moving,
                                         "stick": 10, "size": 3})
        assert answer.get("ok"), answer
        seen = set()
        until = time.time() + 12
        while time.time() < until and len(seen) < 2:
            rows = shot(c, "Avatar")
            if count(rows, BLUE) > 30:
                seen.add("blue")
            if count(rows, GREEN) > 30:
                seen.add("green")
            time.sleep(0.05)
        assert seen == {"blue", "green"}, (seen, obs.log())
        assert "couldn't open a picture" not in obs.log()
        assert "no picture loader" not in obs.log()
    finally:
        c.disconnect()


def test_your_face_from_a_phone(obs):
    """0.9.0: your face from a phone app, on Windows' own sockets. A pretend
    phone on this machine, asked VTube Studio's way: its turn and a wink
    come through. Asked before the phone is there (Windows then reports
    "nobody's listening" on the socket's next read), it keeps listening and
    finds the phone once it's there. And iFacialMocap's way, on its port
    49983."""
    body = os.path.join(obs.home, "phone-character.png")
    character_picture(body)
    # A port nothing listens on yet: the phone comes there later.
    spare = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    spare.bind(("127.0.0.1", 0))
    later = spare.getsockname()[1]
    spare.close()
    c = obs.client()

    def st():
        return vendor(c, "FaceGetState", {"source": "Phone"})
    phone = ifm = None
    try:
        c.send("CreateScene", {"sceneName": "Phone scene"}, raw=True)
        c.send("CreateInput", {"sceneName": "Phone scene", "inputName": "Phone",
                               "inputKind": "direbot_face_avatar",
                               "inputSettings": {"character": body, "face_from": "phone",
                                                 "phone_app": "vtube_studio", "smoothing": 0,
                                                 "phone_address": "127.0.0.1:%d" % later}},
               raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Phone scene"}, raw=True)
        assert wait_for(lambda: st()["phone"]["listening"], 10, 0.2), st()["phone"]
        time.sleep(3)       # asked a few times, nobody there
        p = st()["phone"]
        assert p["listening"] and p["packets"] == 0 and not st()["found"], p
        phone = PretendPhone(port=later)
        assert wait_for(lambda: st()["found"], 15, 0.2), (st()["phone"], obs.log())
        p = st()["phone"]
        assert p["problem"] == "" and p["shapes"] == 52 and p["per_second"] > 10, p
        phone.rotation = {"x": -30.0, "y": 0.0, "z": 0.0}
        assert wait_for(lambda: abs(st()["turn"]["yaw"] - 30) < 2, 10, 0.1), st()["turn"]
        phone.shapes = {"EyeBlinkLeft": 1.0}
        assert wait_for(lambda: st()["moves"]["eyeBlinkRight"] > 0.9, 10, 0.1), st()["moves"]

        ifm = PretendPhone("ifacialmocap")
        c.send("SetInputSettings", {"inputName": "Phone", "inputSettings": {
            "phone_app": "ifacialmocap", "phone_address": "127.0.0.1:%d" % ifm.port}}, raw=True)
        assert wait_for(lambda: st()["phone"]["way"] == "ifacialmocap" and st()["found"], 15, 0.2), \
            st()["phone"]
        assert st()["phone"]["port"] == 49983
        c.send("RemoveInput", {"inputName": "Phone"}, raw=True)
    finally:
        for p in (phone, ifm):
            if p:
                p.close()
        c.disconnect()


def test_moves_from_a_tracker_app(obs):
    """0.9.0: moves from another tracker app over VMC, on Windows' own
    sockets (one for IPv6 and IPv4 both). A pretend tracker app on this
    machine: its face's moves and its head's turn come through; a second
    avatar on the same port shares it; a port another app has says so."""
    body = os.path.join(obs.home, "vmc-character.png")
    character_picture(body)
    spare = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    spare.bind(("127.0.0.1", 0))
    port = spare.getsockname()[1]
    spare.close()
    c = obs.client()

    def st(name="Tracker app"):
        return vendor(c, "FaceGetState", {"source": name})
    app = taken = None
    try:
        c.send("CreateScene", {"sceneName": "Tracker scene"}, raw=True)
        settings = {"character": body, "face_from": "vmc", "vmc_port": port, "smoothing": 0}
        c.send("CreateInput", {"sceneName": "Tracker scene", "inputName": "Tracker app",
                               "inputKind": "direbot_face_avatar", "inputSettings": settings}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Tracker scene"}, raw=True)
        assert wait_for(lambda: st()["vmc"]["listening"], 10, 0.2), st()["vmc"]
        app = PretendVmcApp(port)
        assert wait_for(lambda: st()["found"], 15, 0.2), (st()["vmc"], obs.log())
        # (How many a second is counted over a second or so.)
        assert wait_for(lambda: st()["vmc"]["per_second"] > 10, 5, 0.1), st()["vmc"]
        v = st()["vmc"]
        assert v["problem"] == "" and v["from"] == "this PC", v
        assert v["sends"]["perfect_sync"] and v["using"]["face"], v
        app.turns["Head"] = unity_turn([0, 1, 0], -30)
        assert wait_for(lambda: abs(st()["turn"]["yaw"] + 30) < 2, 10, 0.1), st()["turn"]
        app.shapes = {"EyeBlinkLeft": 1.0}
        assert wait_for(lambda: st()["moves"]["eyeBlinkRight"] > 0.9, 10, 0.1), st()["moves"]
        # A second avatar on the same port shares it.
        c.send("CreateInput", {"sceneName": "Tracker scene", "inputName": "Tracker two",
                               "inputKind": "direbot_face_avatar", "inputSettings": settings}, raw=True)
        assert wait_for(lambda: st("Tracker two")["found"], 15, 0.2), st("Tracker two")["vmc"]
        assert st()["vmc"]["listening"] and st()["vmc"]["problem"] == ""
        c.send("RemoveInput", {"inputName": "Tracker two"}, raw=True)
        c.send("RemoveInput", {"inputName": "Tracker app"}, raw=True)
        # A port another app has: said in words.
        taken = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        taken.bind(("0.0.0.0", 0))
        busy = taken.getsockname()[1]
        c.send("CreateInput", {"sceneName": "Tracker scene", "inputName": "Tracker busy",
                               "inputKind": "direbot_face_avatar",
                               "inputSettings": dict(settings, vmc_port=busy)}, raw=True)
        assert wait_for(lambda: "Another app" in st("Tracker busy")["vmc"]["problem"], 10, 0.2), \
            st("Tracker busy")["vmc"]
        assert not st("Tracker busy")["vmc"]["listening"]
        c.send("RemoveInput", {"inputName": "Tracker busy"}, raw=True)
    finally:
        if app:
            app.close()
        if taken:
            taken.close()
        c.disconnect()
