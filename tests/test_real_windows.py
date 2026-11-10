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

from test_real_face import (PretendPhone, PretendVmcApp, arms_character, camera_picture,  # noqa: E402
                            character_picture, middle_of, read_bmp, unity_turn, write_gif)
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
        # (How many a second is counted over a second or so.)
        assert wait_for(lambda: st()["phone"]["per_second"] > 10, 5, 0.1), st()["phone"]
        p = st()["phone"]
        assert p["problem"] == "" and p["shapes"] == 52, p
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
            {k: st("Tracker busy")["vmc"][k] for k in ("problem", "listening", "port", "packets")}
        assert not st("Tracker busy")["vmc"]["listening"]
        c.send("RemoveInput", {"inputName": "Tracker busy"}, raw=True)
    finally:
        if app:
            app.close()
        if taken:
            taken.close()
        c.disconnect()



def test_posed_on_the_graphics_card(obs):
    """Hair and fur work: on Windows (Direct3D) a 3D character is posed on
    the graphics card (the -skin effect), and looks the same as posed by
    the CPU (the card_posing setting off) - its arms brought down from its
    T, its parts where they were."""
    figure = os.path.join(obs.home, "posed.vrm")
    arms_character(figure)
    c = obs.client()

    def st():
        return vendor(c, "FaceGetState", {"source": "Posed"})["model"]
    try:
        c.send("CreateScene", {"sceneName": "Posed scene"}, raw=True)
        c.send("CreateInput", {"sceneName": "Posed scene", "inputName": "Posed",
                               "inputKind": "direbot_face_avatar",
                               "inputSettings": {"model": figure, "width": 200, "height": 200,
                                                 "size": 100, "framing": "whole", "style": "drawn",
                                                 "mirror": False, "smoothing": 0, "breathing": 0,
                                                 "sway": 0, "eye_moves": False}}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Posed scene"}, raw=True)
        assert wait_for(lambda: st()["shown"], 60, 0.2), obs.log()
        assert wait_for(lambda: st()["on_card"], 10, 0.1), (st(), obs.log())
        log = obs.log()
        assert "3D effect" not in log and "Windows won't draw" not in log, log
        time.sleep(0.8)
        card = shot(c, "Posed")
        c.send("SetInputSettings", {"inputName": "Posed", "inputSettings": {"card_posing": False}}, raw=True)
        assert wait_for(lambda: not st()["on_card"], 10, 0.1), st()
        time.sleep(0.8)
        cpu = shot(c, "Posed")
        assert len(card) == len(cpu) and len(card[0]) == len(cpu[0])
        drawn = sum(1 for row in cpu for px in row if max(px) > 40)
        assert drawn > 200, drawn
        off = sum(1 for a_row, b_row in zip(card, cpu) for a, b in zip(a_row, b_row)
                  if max(abs(int(x) - int(y)) for x, y in zip(a, b)) > 24)
        print("\nPosed on the card vs by the CPU: %d of %d pixels differ" % (off, len(cpu) * len(cpu[0])))
        assert off <= max(20, drawn // 100), off
        # Back on the card.
        c.send("SetInputSettings", {"inputName": "Posed", "inputSettings": {"card_posing": True}}, raw=True)
        assert wait_for(lambda: st()["on_card"], 10, 0.1), st()
    finally:
        try:
            c.send("RemoveInput", {"inputName": "Posed"}, raw=True)
        except Exception:
            pass
        c.disconnect()


class Typist:
    """Windows' own SendInput, as a person at this PC: presses of keys no
    keyboard most people have (F13 to F16, so nothing in OBS answers
    them) and the mouse moved. Each returns how many Windows took."""

    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        word, dword, ulong_ptr = wintypes.WORD, wintypes.DWORD, ctypes.c_size_t

        class Mouse(ctypes.Structure):
            _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", dword),
                        ("dwFlags", dword), ("time", dword), ("dwExtraInfo", ulong_ptr)]

        class Key(ctypes.Structure):
            _fields_ = [("wVk", word), ("wScan", word), ("dwFlags", dword), ("time", dword),
                        ("dwExtraInfo", ulong_ptr)]

        class Hardware(ctypes.Structure):
            _fields_ = [("uMsg", dword), ("wParamL", word), ("wParamH", word)]

        class Either(ctypes.Union):
            _fields_ = [("mi", Mouse), ("ki", Key), ("hi", Hardware)]

        class Input(ctypes.Structure):
            _fields_ = [("type", dword), ("u", Either)]
        self.Input = Input
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(Input), ctypes.c_int]
        self.user32.SendInput.restype = wintypes.UINT
        self.user32.MapVirtualKeyW.argtypes = [wintypes.UINT, wintypes.UINT]
        self.user32.MapVirtualKeyW.restype = wintypes.UINT

    def _send(self, inputs):
        array = (self.Input * len(inputs))(*inputs)
        took = self.user32.SendInput(len(inputs), array, self.ctypes.sizeof(self.Input))
        assert took == len(inputs), "SendInput took %d of %d (error %d)" % (
            took, len(inputs), self.ctypes.get_last_error())
        return took

    def press(self, n):
        inputs = []
        for i in range(n):
            vk = 0x7C + i % 4      # F13 to F16
            scan = self.user32.MapVirtualKeyW(vk, 0)
            for up in (0, 2):        # (KEYEVENTF_KEYUP)
                one = self.Input(type=1)
                one.u.ki.wVk, one.u.ki.wScan, one.u.ki.dwFlags = vk, scan, up
                inputs.append(one)
        return self._send(inputs)

    def move(self, dx, dy):
        one = self.Input(type=0)
        one.u.mi.dx, one.u.mi.dy, one.u.mi.dwFlags = dx, dy, 1   # (MOUSEEVENTF_MOVE)
        return self._send([one])


def test_gaming_hands_read_this_pc(obs):
    """0.9.0: gaming hands on Windows - your keyboard and mouse read with
    Raw Input on a hidden window of its own (while another window has the
    focus too), pads with XInput, a VRM character's hands on its own
    keyboard and mouse (the props from the zip). Keys pressed are counted
    (never which), typing seen, the mouse's moves followed; turned off, the
    keyboard and mouse are let go of and nothing more is counted."""
    figure = os.path.join(obs.home, "gaming.vrm")
    arms_character(figure)
    size = 200
    c = obs.client()
    typist = Typist()

    def gaming():
        return vendor(c, "FaceGetState", {"source": "Gaming"})["gaming"]

    def change(settings):
        c.send("SetInputSettings", {"inputName": "Gaming", "inputSettings": settings}, raw=True)
    red = lambda px: px[0] > 120 and px[1] < 60 and px[2] < 60  # noqa: E731
    try:
        c.send("CreateScene", {"sceneName": "Gaming scene"}, raw=True)
        c.send("CreateInput", {"sceneName": "Gaming scene", "inputName": "Gaming",
                               "inputKind": "direbot_face_avatar",
                               "inputSettings": {"model": figure, "width": size, "height": size,
                                                 "size": 100, "framing": "whole", "style": "drawn",
                                                 "mirror": False, "smoothing": 0, "breathing": 0,
                                                 "sway": 0, "eye_moves": False}}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Gaming scene"}, raw=True)
        assert wait_for(lambda: vendor(c, "FaceGetState", {"source": "Gaming"})["model"]["shown"], 60, 0.2), \
            obs.log()
        g = gaming()
        assert not g["on"] and not g["reading"]["keyboard"] and not g["reading"]["mouse"], g
        time.sleep(0.5)
        _, rest_y, rest_n = middle_of(shot(c, "Gaming"), red)
        assert rest_n > 5, rest_n

        change({"game_hands": True})
        assert wait_for(lambda: gaming()["shown"] and gaming()["hands_on"] == 1, 15, 0.1), gaming()
        assert wait_for(lambda: gaming()["reading"]["keyboard"] or gaming()["reading"]["keyboard_problem"],
                        10, 0.1), gaming()
        r = gaming()["reading"]
        print("\nGaming hands read:", r)
        assert r["keyboard"] and r["keyboard_problem"] == "", r
        assert r["mouse"] and r["mouse_problem"] == "", r
        assert r["pads"] or r["pads_problem"], r
        # Its keyboard, mouse, mat, pad and sticks, from the zip, drawn.
        assert wait_for(lambda: gaming()["props_loaded"] == gaming()["props_wanted"] == 5, 15, 0.1), gaming()
        assert gaming()["props_problem"] == ""
        time.sleep(0.8)
        _, desk_y, desk_n = middle_of(shot(c, "Gaming"), red)
        assert desk_n > 5 and rest_y - desk_y > 0.08 * size, (rest_y, desk_y)

        # Keys pressed (in another window: OBS's own has the focus, or none):
        # counted, and typing once there are a few with the mouse still.
        assert not gaming()["used"]["keys"]
        typist.press(2)
        assert wait_for(lambda: gaming()["used"]["keys"], 3, 0.05), gaming()
        assert gaming()["using"] == "keyboard_mouse"
        typed = []

        def typing():
            typed.append(typist.press(1))
            return gaming()["typing"]
        assert wait_for(typing, 6, 0.15), (len(typed), gaming())
        # (How many and that they were used - nothing about which.)
        g = gaming()
        assert set(g["used"]) == {"keys", "mouse", "pad"}, g
        assert "F13" not in json.dumps(g) and "124" not in json.dumps(g["used"]), g
        # The mouse moved: followed, and typing stops.
        for _ in range(10):
            typist.move(15, 0)
            time.sleep(0.03)
        assert wait_for(lambda: gaming()["used"]["mouse"] and not gaming()["typing"], 3, 0.05), gaming()

        # Off: the keyboard and mouse let go of, and keys aren't counted.
        change({"game_hands": False})
        assert wait_for(lambda: not gaming()["reading"]["keyboard"] and not gaming()["reading"]["mouse"],
                        5, 0.1), gaming()
        assert wait_for(lambda: not gaming()["shown"], 5, 0.1), gaming()
        time.sleep(1.2)
        typist.press(4)
        time.sleep(0.3)
        g = gaming()
        assert not g["used"]["keys"] and not g["typing"], g
        time.sleep(0.5)
        _, off_y, _ = middle_of(shot(c, "Gaming"), red)
        assert abs(off_y - rest_y) < 0.04 * size, (rest_y, off_y)
    finally:
        try:
            c.send("RemoveInput", {"inputName": "Gaming"}, raw=True)
        except Exception:
            pass
        c.disconnect()
