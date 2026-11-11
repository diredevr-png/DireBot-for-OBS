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
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import zipfile

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_face import (PretendPhone, PretendVmcApp, arms_character, camera_picture,  # noqa: E402
                            character_picture, middle_of, read_bmp, unity_turn, write_gif)
from test_real_hair import (LIFT, MAGENTA, WIDE_HAIR, Strands, add_strands, hair_character,  # noqa: E402
                            hair_over_body, magenta, open_glb, per_metre, save_glb, settles, top_of, white,
                            world_of, yellow)
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


def take_out_add_on():
    shutil.rmtree(os.path.join(os.environ.get("PROGRAMDATA", r"C:\ProgramData"),
                               "obs-studio", "plugins", MODULE), ignore_errors=True)


class WindowsObs:
    def __init__(self, add_on=True):
        """`add_on` False: OBS on its own, the add-on taken out (for what
        OBS itself leaves behind when it closes)."""
        self.ended = None       # its log, once it has closed
        self.home = os.path.join(os.environ["RUNNER_TEMP"] if "RUNNER_TEMP" in os.environ
                                 else os.environ["TEMP"], "direbot-obs-test")
        shutil.rmtree(self.home, ignore_errors=True)
        os.makedirs(self.home)
        self.plugin = put_in_add_on() if add_on else take_out_add_on()
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
        if not add_on:
            # No link without the add-on: OBS's own WebSocket server and
            # its window say it's up.
            self.link = {"port": self.ws_port, "password": ""}
            if not wait_for(lambda: self.answers() and self.mains(), 120, 0.25):
                log = self.log()
                self.kill()
                raise AssertionError("OBS on its own never came up - its log:\n" + log)
            return
        if not wait_for(lambda: os.path.exists(self.link_path), 120, 0.25):
            log = self.log()
            self.kill()
            raise AssertionError("the link never came on - OBS's log:\n" + log)
        self.link = json.load(open(self.link_path))

    def answers(self):
        """OBS's own WebSocket server takes a connection."""
        try:
            socket.create_connection(("127.0.0.1", self.ws_port), 0.5).close()
            return True
        except OSError:
            return False

    def mains(self):
        """OBS's own window (its title starts "OBS "), once it's there."""
        return [handle for handle, title in self.windows() if title.startswith("OBS ")]

    def log(self):
        if self.ended is not None:
            return self.ended
        try:
            newest = max(os.listdir(self.logs))
            return open(os.path.join(self.logs, newest), errors="replace").read()
        except (OSError, ValueError):
            return ""

    def client(self):
        return obsws.ReqClient(host="127.0.0.1", port=self.link["port"],
                               password=self.link["password"], timeout=30)

    def windows(self):
        """Its windows on the screen: (handle, title) of each."""
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        each_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.EnumWindows.argtypes = [each_type, wintypes.LPARAM]
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        user32.IsWindowVisible.argtypes = [wintypes.HWND]
        user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        found = []

        def each(handle, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(handle, ctypes.byref(pid))
            if pid.value == self.proc.pid and user32.IsWindowVisible(handle):
                title = ctypes.create_unicode_buffer(300)
                user32.GetWindowTextW(handle, title, 300)
                found.append((handle, title.value))
            return True
        user32.EnumWindows(each_type(each), 0)
        return found

    def close(self, wait=180):
        """Closed the way the X on its window closes it, so OBS takes
        everything down in order and its log ends with how much was never
        given back. False: it's still there after `wait` seconds."""
        import ctypes
        from ctypes import wintypes
        if self.proc.poll() is None:
            user32 = ctypes.WinDLL("user32", use_last_error=True)
            user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
            wait_for(self.mains, 30, 0.25)
            mains = self.mains()
            assert mains, ("OBS's own window isn't there", self.windows())
            for handle in mains:
                user32.PostMessageW(handle, 0x0010, 0, 0)       # (WM_CLOSE)
            try:
                self.proc.wait(timeout=wait)
            except subprocess.TimeoutExpired:
                return False
        self.out.close()
        self.ended = self.log()
        return True

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



def skinned_sleeves(path):
    """arms_character (a VRM), with a sleeve over each arm that its bones
    bend (a skin: each point hung on the upper and lower arm, shared round
    the elbow) - as real characters are made."""
    arms_character(path)
    with open(path, "rb") as f:
        data = f.read()
    jlen, = struct.unpack_from("<I", data, 12)
    doc = json.loads(data[20:20 + jlen])
    blob = bytearray(data[20 + jlen + 8:])
    nodes = doc["nodes"]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}

    def world(i):
        t = [0.0, 0.0, 0.0]
        while i is not None:
            for k in range(3):
                t[k] += nodes[i].get("translation", [0, 0, 0])[k]
            i = parent.get(i)
        return t

    def add(raw, target=None):
        while len(blob) % 4:
            blob.append(0)
        view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(raw)}
        if target:
            view["target"] = target
        doc["bufferViews"].append(view)
        blob.extend(raw)
        return len(doc["bufferViews"]) - 1

    def accessor(view, kind, count, ctype, more=None):
        doc["accessors"].append(dict({"bufferView": view, "componentType": ctype, "count": count,
                                      "type": kind}, **(more or {})))
        return len(doc["accessors"]) - 1

    names = {n.get("name"): i for i, n in enumerate(nodes)}
    joints = [names["leftUpperArm"], names["leftLowerArm"], names["rightUpperArm"], names["rightLowerArm"]]
    pos, nrm, jnt, wgt, ix = [], [], [], [], []
    for side, sx in ((0, 1), (1, -1)):
        start = world(joints[side * 2])
        rings, around = 25, 12
        for r in range(rings):
            d = 0.48 * r / (rings - 1)
            upper = min(1.0, max(0.0, (0.30 - d) / 0.12))
            for a in range(around):
                ang = 2 * math.pi * a / around
                cy, cz = math.cos(ang), math.sin(ang)
                pos.append((start[0] + sx * d, start[1] + 0.045 * cy, start[2] + 0.045 * cz))
                nrm.append((0.0, cy, cz))
                jnt.append((side * 2, side * 2 + 1, 0, 0))
                wgt.append((upper, 1.0 - upper, 0.0, 0.0))
        base = side * rings * around
        for r in range(rings - 1):
            for a in range(around):
                p0 = base + r * around + a
                p1 = base + r * around + (a + 1) % around
                q0, q1 = p0 + around, p1 + around
                ix += [p0, q0, p1, p1, q0, q1] if sx > 0 else [p0, p1, q0, p1, q1, q0]
    lo = [min(v[k] for v in pos) for k in range(3)]
    hi = [max(v[k] for v in pos) for k in range(3)]
    a_pos = accessor(add(struct.pack("<%df" % (len(pos) * 3), *[c for v in pos for c in v]), 34962),
                     "VEC3", len(pos), 5126, {"min": lo, "max": hi})
    a_nrm = accessor(add(struct.pack("<%df" % (len(nrm) * 3), *[c for v in nrm for c in v]), 34962),
                     "VEC3", len(nrm), 5126)
    a_jnt = accessor(add(struct.pack("<%dH" % (len(jnt) * 4), *[c for v in jnt for c in v]), 34962),
                     "VEC4", len(jnt), 5123)
    a_wgt = accessor(add(struct.pack("<%df" % (len(wgt) * 4), *[c for v in wgt for c in v]), 34962),
                     "VEC4", len(wgt), 5126)
    a_ix = accessor(add(struct.pack("<%dI" % len(ix), *ix), 34963), "SCALAR", len(ix), 5125)
    ibm = []
    for jn in joints:
        w = world(jn)
        ibm += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1]
    a_ibm = accessor(add(struct.pack("<%df" % len(ibm), *ibm)), "MAT4", len(joints), 5126)
    doc["materials"].append({"pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.1, 0.7, 1],
                                                      "metallicFactor": 0, "roughnessFactor": 0.6}})
    doc["meshes"].append({"primitives": [{"attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "JOINTS_0": a_jnt,
                                                         "WEIGHTS_0": a_wgt},
                                          "indices": a_ix, "material": len(doc["materials"]) - 1}]})
    doc["skins"] = [{"joints": joints, "inverseBindMatrices": a_ibm}]
    nodes.append({"name": "sleeves", "mesh": len(doc["meshes"]) - 1, "skin": 0})
    nodes[0].setdefault("children", []).append(len(nodes) - 1)
    doc["buffers"][0]["byteLength"] = len(blob)
    text = json.dumps(doc).encode()
    text += b" " * (-len(text) % 4)
    while len(blob) % 4:
        blob.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def test_posed_on_the_graphics_card(obs):
    """Hair and fur work: on Windows (Direct3D) a 3D character is posed on
    the graphics card (the -skin effect), and looks the same as posed by
    the CPU (the card_posing setting off) - its arms brought down from its
    T, its parts where they were, the sleeves its arm bones bend (a skin)
    bent the same."""
    figure = os.path.join(obs.home, "posed.vrm")
    skinned_sleeves(figure)
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
        purple = sum(1 for row in cpu for px in row
                     if px[0] > 110 and px[2] > 130 and px[1] < px[0] - 40 and px[1] < px[2] - 60)
        assert purple > 30, purple
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


def test_hair_strands_on_the_graphics_card(obs):
    """Hair and fur work: a character with hair strands on Windows
    (Direct3D). Its strands are posed on the graphics card (the -skin
    effect: by its bones, and by its expression's spots) and look the same
    as posed by the CPU - at rest, with its expression showing (the hair
    on its head lifted), with its neck tipped over (its own animation,
    holding still) and with an animation of its own holding the
    expression's shape on (its hair lifted as far as its expression's). FaceGetState says how many are kept and drawn; the
    hair budget, far too small, brings the share drawn down to its least
    by the graphics card's own clock, and none lets them all back; the
    hair detail setting keeps its strands for Light and wears its stand-in
    hair for Today."""
    figure = os.path.join(obs.home, "hair.vrm")
    strands = hair_character(figure, standin=True, played=True)
    c = obs.client()

    def st():
        return vendor(c, "FaceGetState", {"source": "Hair"})["model"]

    def hair():
        return st()["hair"]

    def change(**settings):
        c.send("SetInputSettings", {"inputName": "Hair", "inputSettings": settings}, raw=True)

    def reloaded():
        """After a change that loads its file again: wait for it."""
        time.sleep(0.5)
        assert wait_for(lambda: not st()["loading"] and st()["shown"], 60, 0.1), (st(), obs.log())
        time.sleep(0.8)

    def cue(**data):
        return vendor(c, "FaceReact", {"source": "Hair", **data})

    def poses():
        """Its picture at rest, with its expression showing, with its
        neck tipped over, and with its own animation holding the
        expression's shape on (a "weights" track its file asks to be
        played)."""
        out = {}
        time.sleep(0.8)
        out["rest"] = shot(c, "Hair")
        answer = cue(do="face", face="happy", seconds=60)
        assert answer["ok"] and answer["shows"], answer
        assert wait_for(lambda: st()["expressions"].get("happy", 0) >= 0.99, 10, 0.05), st()["expressions"]
        time.sleep(0.5)
        out["lifted"] = shot(c, "Hair")
        assert cue(do="stop")["ok"]
        assert wait_for(lambda: st()["expressions"].get("happy", 0) == 0, 10, 0.05), st()["expressions"]
        change(animation="Tilt")
        assert wait_for(lambda: st()["animation"] == "Tilt", 10, 0.1), st()
        time.sleep(0.8)
        out["tilted"] = shot(c, "Hair")
        change(animation="Lift")
        assert wait_for(lambda: st()["animation"] == "Lift", 10, 0.1), st()
        time.sleep(1.0)
        assert st()["expressions"].get("happy", 0) == 0, st()["expressions"]
        out["played"] = shot(c, "Hair")
        change(animation="")
        assert wait_for(lambda: st()["animation"] == "", 10, 0.1), st()
        return out

    def pixels(rows, test):
        return sum(1 for row in rows for px in row if test(px))

    def unlike(a, b):
        assert len(a) == len(b) and len(a[0]) == len(b[0])
        return sum(1 for a_row, b_row in zip(a, b) for x, y in zip(a_row, b_row)
                   if max(abs(int(p) - int(q)) for p, q in zip(x, y)) > 24)
    try:
        c.send("CreateScene", {"sceneName": "Hair scene"}, raw=True)
        # (Smaller than its spot, so there's room over its head for its
        # hair to lift into; the hair budget off, so the same picture comes
        # every time.)
        c.send("CreateInput", {"sceneName": "Hair scene", "inputName": "Hair",
                               "inputKind": "direbot_face_avatar",
                               "inputSettings": {"model": figure, "width": 200, "height": 200,
                                                 "size": 60, "framing": "whole", "style": "drawn",
                                                 "mirror": False, "smoothing": 0, "breathing": 0,
                                                 "sway": 0, "eye_moves": False,
                                                 "hair_budget_ms": 0}}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Hair scene"}, raw=True)
        assert wait_for(lambda: st()["shown"], 60, 0.2), obs.log()
        assert wait_for(lambda: st()["on_card"], 10, 0.1), (st(), obs.log())
        assert wait_for(lambda: hair()["drawn"] > 0, 10, 0.1), (hair(), obs.log())
        time.sleep(0.5)
        h = hair()
        print("\nHair on the card: %s" % h)
        assert h["detail"] == "full" and h["standin"] and h["strands"] == strands, h
        assert h["loaded"] == strands and h["shown_strands"] == strands and h["shown_loaded"] == strands, h
        assert h["drawn"] == strands and h["pieces"] > strands and h["draws"] > 0, h
        assert h["share"] == 1 and h["budget_ms"] == 0, h
        log = obs.log()
        assert "3D effect" not in log and "Windows won't draw" not in log, log

        # Posed on the card: at rest, its expression, its neck tipped.
        card = poses()
        metre = per_metre(card["rest"])
        up = top_of(card["rest"], magenta) - top_of(card["lifted"], magenta)
        print("Its expression on the card: the top of its hair up %d pixels (%.3f m; the file says %s)"
              % (up, up / metre, LIFT))
        assert abs(up / metre - LIFT) < 0.03, (up, metre)
        assert unlike(card["tilted"], card["rest"]) > 300, unlike(card["tilted"], card["rest"])
        # (Its animation's shape: its hair as far over its body as its
        # expression put it - fitted to its spot as it stands in its
        # animation, so measured by its body.)
        over = [hair_over_body(card[name]) for name in ("rest", "lifted", "played")]
        print("Its animation's shape on the card: its hair %.3f m over its body at rest, %.3f with its "
              "expression, %.3f with its animation (the file lifts it %s)" % (*over, LIFT))
        assert abs(over[1] - over[0] - LIFT) < 0.03 and abs(over[2] - over[0] - LIFT) < 0.03, over
        # (Standing in its "Lift" animation it's drawn a little smaller.)
        for name, rows in card.items():
            assert pixels(rows, magenta) > (400 if name == "played" else 600) and pixels(rows, yellow) == 0, \
                (name, pixels(rows, magenta))

        # The hair budget, by the graphics card's own clock: far too
        # small, the share drawn falls to its least and no further (still
        # over, it stays there), and the count with it; none, all of them
        # again.
        assert wait_for(lambda: hair()["ms"] >= 0, 10, 0.1), \
            ("the graphics card's clock never answered", hair(), obs.log())
        change(hair_budget_ms=0.001)
        assert wait_for(lambda: hair()["share"] < 0.5, 20, 0.05), (hair(), obs.log())
        least = settles(lambda: hair()["share"])
        h = hair()
        print("A hair budget far too small: its least share %s: %s" % (least, h))
        assert least is not None and 0 < least < 0.25, (h, obs.log())
        assert h["share"] == least and h["ms"] > h["budget_ms"], h
        # (Full's rule: the one that keeps the share it was over at.)
        assert h["over"] == least, h
        assert h["loaded"] == strands and 16 <= h["drawn"] < 0.25 * strands, h
        assert abs(h["drawn"] - least * strands) <= 0.02 * strands, (h, least)
        change(hair_budget_ms=0)
        assert wait_for(lambda: hair()["share"] == 1 and hair()["drawn"] == strands, 10, 0.1), hair()

        # The hair detail setting: Light keeps its strands (all of so few),
        # Today wears its stand-in hair instead, Full brings them back.
        change(hair_detail="light")
        reloaded()
        h = hair()
        assert h["detail"] == "light" and h["loaded"] == strands and h["drawn"] == strands, h
        assert st()["on_card"] and pixels(shot(c, "Hair"), magenta) > 600
        change(hair_detail="today")
        reloaded()
        h = hair()
        assert h["detail"] == "today" and h["strands"] == strands, h
        assert h["loaded"] == 0 and h["drawn"] == 0 and h["pieces"] == 0 and h["shown_strands"] == 0, h
        rows = shot(c, "Hair")
        assert pixels(rows, magenta) == 0 and pixels(rows, yellow) > 150, (pixels(rows, magenta),
                                                                          pixels(rows, yellow))
        change(hair_detail="full")
        reloaded()
        assert wait_for(lambda: hair()["drawn"] == strands, 10, 0.1), hair()
        assert hair()["loaded"] == strands and st()["on_card"], (hair(), st())
        time.sleep(0.5)
        again = unlike(shot(c, "Hair"), card["rest"])
        assert again <= 20, again

        # Posed by the CPU: the same pictures.
        change(card_posing=False)
        assert wait_for(lambda: not st()["on_card"], 10, 0.1), st()
        reloaded()
        assert wait_for(lambda: hair()["drawn"] == strands, 10, 0.1), hair()
        cpu = poses()
        for name in ("rest", "lifted", "tilted", "played"):
            drawn = sum(1 for row in cpu[name] for px in row if max(px) > 40)
            off = unlike(card[name], cpu[name])
            print("Hair posed on the card vs by the CPU, %s: %d of %d pixels differ (%d drawn, %d of hair)"
                  % (name, off, len(cpu[name]) * len(cpu[name][0]), drawn, pixels(cpu[name], magenta)))
            assert drawn > 1500 and pixels(cpu[name], magenta) > (400 if name == "played" else 600), (name, drawn)
            assert off <= max(20, drawn // 100), (name, off)
        log = obs.log()
        assert "3D effect" not in log and "Windows won't draw" not in log, log
        # Back on the card.
        change(card_posing=True)
        assert wait_for(lambda: st()["on_card"], 10, 0.1), st()
    finally:
        try:
            c.send("RemoveInput", {"inputName": "Hair"}, raw=True)
        except Exception:
            pass
        c.disconnect()


def test_hidden_hair_is_left_out_on_the_card(obs):
    """Hair that can't show - behind a solid part - is left out on the
    graphics card before it's cut into pixels (the -skin effect's
    strand_shown; FaceGetState's "skips_hidden"), and the picture is the
    same as with every hair sent ("hair_skip_hidden" 0, a number the
    settings window doesn't show). The test's own view (2: only the hair
    that's left out, drawn over everything) shows which it is: the white
    hairs hanging behind its body, never seen from the front - and none of
    the hair in plain sight, the tuft on its hand (but for a hair's first
    piece, where its root is inside the hand). Sent as lines ("hair_lines"
    2) the same hairs are left out - and of a coat of thin hairs with a
    few wide ones among them, whose thin hairs alone go as lines. Posed by
    the CPU nothing is left out."""
    figure = os.path.join(obs.home, "hidden.vrm")
    strands = hair_character(figure, crown=0, locks=0, tuft=200, behind=400)
    assert strands == 600
    # (The same with its tuft's hairs thin: a tenth of a pixel wide.)
    thin = os.path.join(obs.home, "hidden-thin.vrm")
    assert hair_character(thin, crown=0, locks=0, tuft=200, behind=400, width=0.0006) == strands
    # (And a crown of thin hairs, every fifth one wide - with a few behind
    # its body.)
    among = os.path.join(obs.home, "hidden-among.vrm")
    among_strands = hair_character(among, crown=600, locks=0, tuft=200, behind=100, width=0.0006, wide_every=5)
    assert among_strands == 900
    c = obs.client()

    def st(name="Hidden"):
        return vendor(c, "FaceGetState", {"source": name})["model"]

    def hair(name="Hidden"):
        return st(name)["hair"]

    def change(name="Hidden", **settings):
        c.send("SetInputSettings", {"inputName": name, "inputSettings": settings}, raw=True)

    def pixels(rows, test):
        return sum(1 for row in rows for px in row if test(px))

    def unlike(a, b):
        assert len(a) == len(b) and len(a[0]) == len(b[0])
        return sum(1 for a_row, b_row in zip(a, b) for x, y in zip(a_row, b_row)
                   if max(abs(int(p) - int(q)) for p, q in zip(x, y)) > 24)

    def faint(px):
        return px[0] > 6 and px[2] > 6 and px[1] * 2 < min(px[0], px[2])
    try:
        c.send("CreateScene", {"sceneName": "Hidden scene"}, raw=True)
        for name, model, lines in (("Hidden", figure, -1), ("Hidden thin", thin, 2), ("Hidden among", among, -1)):
            c.send("CreateInput", {"sceneName": "Hidden scene", "inputName": name,
                                   "inputKind": "direbot_face_avatar",
                                   "inputSettings": {"model": model, "width": 300, "height": 300,
                                                     "size": 100, "framing": "whole", "style": "drawn",
                                                     "mirror": False, "smoothing": 0, "breathing": 0,
                                                     "sway": 0, "eye_moves": False,
                                                     "hair_budget_ms": 0, "hair_lines": lines}}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Hidden scene"}, raw=True)
        for name in ("Hidden", "Hidden thin", "Hidden among"):
            assert wait_for(lambda: st(name)["shown"], 60, 0.2), obs.log()
            assert wait_for(lambda: st(name)["on_card"], 10, 0.1), (st(name), obs.log())
            assert wait_for(lambda: hair(name)["drawn"] == (among_strands if name == "Hidden among" else strands),
                            10, 0.1), (hair(name), obs.log())
        time.sleep(0.8)

        # Left out on the card: the picture has none of the hair behind its
        # body, and all of the tuft on its hand.
        h = hair()
        print("\nHidden hair left out on the card: %s" % h)
        assert h["skips_hidden"] and h["lines"] == 0 and h["pieces"] >= strands, h
        skipped = shot(c, "Hidden")
        tuft = pixels(skipped, magenta)
        assert tuft > 300 and pixels(skipped, white) == 0, (tuft, pixels(skipped, white))
        # Every hair sent: the same picture.
        change(hair_skip_hidden=0)
        assert wait_for(lambda: not hair()["skips_hidden"], 10, 0.1), hair()
        time.sleep(0.8)
        sent = shot(c, "Hidden")
        off = unlike(skipped, sent)
        print("Hidden hair left out vs every hair sent: %d of %d pixels differ (%d of hair)"
              % (off, len(sent) * len(sent[0]), pixels(sent, magenta)))
        assert off <= 8 and pixels(sent, white) == 0, (off, pixels(sent, white))
        assert hair()["pieces"] == h["pieces"] and hair()["drawn"] == strands, hair()
        # Only what's left out, drawn over everything: the hair behind its
        # body, and none of the tuft.
        change(hair_skip_hidden=2)
        assert wait_for(lambda: hair()["skips_hidden"], 10, 0.1), hair()
        time.sleep(0.8)
        only = shot(c, "Hidden")
        print("Only the hair that's left out: %d white pixels (behind its body), %d magenta (its tuft: %d in plain sight)"
              % (pixels(only, white), pixels(only, magenta), tuft))
        assert pixels(only, white) > 300, pixels(only, white)
        assert pixels(only, magenta) <= 0.6 * tuft, (pixels(only, magenta), tuft)
        change(hair_skip_hidden=-1)
        assert wait_for(lambda: hair()["skips_hidden"], 10, 0.1), hair()
        time.sleep(0.8)
        assert unlike(shot(c, "Hidden"), skipped) <= 8

        # Sent as lines on the card: the same ones left out - the same
        # picture as with every hair sent; only what's left out, the hair
        # behind its body (lines, a pixel wide); and its thin tuft has as
        # much hair in the picture as ribbons.
        h = hair("Hidden thin")
        print("Sent as lines on the card: %s" % h)
        assert h["skips_hidden"] and h["lines"] == h["pieces"] >= strands, h
        lines = shot(c, "Hidden thin")
        assert pixels(lines, faint) > 40 and pixels(lines, white) == 0, (pixels(lines, faint), pixels(lines, white))
        change("Hidden thin", hair_skip_hidden=0)
        assert wait_for(lambda: not hair("Hidden thin")["skips_hidden"], 10, 0.1), hair("Hidden thin")
        time.sleep(0.8)
        assert unlike(shot(c, "Hidden thin"), lines) <= 8
        change("Hidden thin", hair_skip_hidden=2)
        assert wait_for(lambda: hair("Hidden thin")["skips_hidden"], 10, 0.1), hair("Hidden thin")
        time.sleep(0.8)
        only = shot(c, "Hidden thin")
        print("As lines, only what's left out: %d white pixels (behind its body), %d of its tuft (%d in plain sight)"
              % (pixels(only, white), pixels(only, faint), pixels(lines, faint)))
        assert pixels(only, white) > 100, pixels(only, white)
        assert pixels(only, faint) <= 0.6 * pixels(lines, faint), (pixels(only, faint), pixels(lines, faint))
        change("Hidden thin", hair_skip_hidden=-1, hair_lines=0)
        assert wait_for(lambda: hair("Hidden thin")["lines"] == 0 and hair("Hidden thin")["skips_hidden"], 10, 0.1), \
            hair("Hidden thin")
        time.sleep(0.8)
        ribbons = shot(c, "Hidden thin")
        ink_lines = sum(px[0] for row in lines for px in row if faint(px))
        ink_ribbons = sum(px[0] for row in ribbons for px in row if faint(px))
        print("Thin hair on the card: %d of red as lines, %d as ribbons" % (ink_lines, ink_ribbons))
        assert ink_ribbons > 1000 and 0.85 * ink_ribbons < ink_lines < 1.15 * ink_ribbons, (ink_lines, ink_ribbons)

        # Thin hairs with a few wide ones among them: the thin ones' pieces
        # lines and the rest ribbons, each layer drawn in two goes - with
        # the hidden ones left out of both, and as much hair in the picture
        # as with none of it lines, its wide hairs as wide.
        h = hair("Hidden among")
        print("Thin hairs among wide ones on the card: %s" % h)
        assert h["skips_hidden"] and h["pieces"] > among_strands, h
        assert 0.6 * h["pieces"] < h["lines"] < 0.85 * h["pieces"], h
        mixed = shot(c, "Hidden among")
        assert pixels(mixed, white) == 0 and pixels(mixed, magenta) > 300, (pixels(mixed, white),
                                                                            pixels(mixed, magenta))
        change("Hidden among", hair_skip_hidden=0)
        assert wait_for(lambda: not hair("Hidden among")["skips_hidden"], 10, 0.1), hair("Hidden among")
        time.sleep(0.8)
        assert unlike(shot(c, "Hidden among"), mixed) <= 8
        change("Hidden among", hair_skip_hidden=-1, hair_lines=0)
        assert wait_for(lambda: hair("Hidden among")["lines"] == 0 and hair("Hidden among")["skips_hidden"], 10,
                        0.1), hair("Hidden among")
        time.sleep(0.8)
        ribbons = shot(c, "Hidden among")
        ink_mixed = sum(px[0] for row in mixed for px in row if faint(px))
        ink_ribbons = sum(px[0] for row in ribbons for px in row if faint(px))
        print("Thin hairs among wide ones: %d of red with the thin as lines (%d solid pixels), %d all ribbons (%d)"
              % (ink_mixed, pixels(mixed, magenta), ink_ribbons, pixels(ribbons, magenta)))
        assert ink_ribbons > 1000 and 0.9 * ink_ribbons < ink_mixed < 1.1 * ink_ribbons, (ink_mixed, ink_ribbons)
        assert 0.95 * pixels(ribbons, magenta) < pixels(mixed, magenta) < 1.05 * pixels(ribbons, magenta), \
            (pixels(mixed, magenta), pixels(ribbons, magenta))
        assert hair("Hidden among")["pieces"] == h["pieces"], hair("Hidden among")

        # Posed by the CPU: nothing's left out, the same picture.
        change(card_posing=False)
        assert wait_for(lambda: not st()["on_card"], 10, 0.1), st()
        time.sleep(0.5)
        assert wait_for(lambda: not st()["loading"] and st()["shown"], 60, 0.1), (st(), obs.log())
        assert wait_for(lambda: hair()["drawn"] == strands, 10, 0.1), hair()
        time.sleep(0.8)
        assert not hair()["skips_hidden"], hair()
        cpu = shot(c, "Hidden")
        off = unlike(cpu, skipped)
        print("Hidden hair left out on the card vs posed by the CPU: %d pixels differ" % off)
        assert off <= max(20, tuft // 20) and pixels(cpu, white) == 0, (off, pixels(cpu, white))
        change(card_posing=True)
        assert wait_for(lambda: st()["on_card"], 10, 0.1), st()
        log = obs.log()
        assert "3D effect" not in log and "Windows won't draw" not in log, log
    finally:
        for name in ("Hidden", "Hidden thin", "Hidden among"):
            try:
                c.send("RemoveInput", {"inputName": name}, raw=True)
            except Exception:
                pass
        c.disconnect()


def test_hair_laid_out_as_drawn_on_the_card(obs):
    """Hair posed on the graphics card can lie in its memory in the order
    it's drawn in ("hair_layout" 1, a number the settings window doesn't
    show; FaceGetState's "laid": the pieces drawn from memory laid out that
    way, "laid_mb": how much of it there is, and "laid_ms": how long making
    it took). Its hull - the inner layers of some of its strands, which
    hide the hair well behind them - is laid out too, a draw a group
    ("hull_pieces", "hull_draws"). It's the same hair: the
    picture is the same to the pixel as with a point's numbers once, where
    its strand is ("hair_layout" 0) - as ribbons, as lines, with its thin
    hairs alone as lines, with only some of its strands drawn (small in the
    picture), and with the hidden hair left out (and only the left-out hair
    drawn). Posed by the CPU nothing is laid out."""
    figure = os.path.join(obs.home, "laid.vrm")
    strands = hair_character(figure, crown=600, locks=20, tuft=200, behind=200)
    thin = os.path.join(obs.home, "laid-thin.vrm")
    assert hair_character(thin, crown=600, locks=20, tuft=200, behind=200, width=0.0006) == strands
    among = os.path.join(obs.home, "laid-among.vrm")
    assert hair_character(among, crown=600, locks=20, tuft=200, behind=200, width=0.0006, wide_every=5) == strands
    names = ("Laid", "Laid thin", "Laid among")
    c = obs.client()

    def st(name):
        return vendor(c, "FaceGetState", {"source": name})["model"]

    def hair(name):
        return st(name)["hair"]

    def change(name, **settings):
        c.send("SetInputSettings", {"inputName": name, "inputSettings": settings}, raw=True)

    def differ(a, b):
        assert len(a) == len(b) and len(a[0]) == len(b[0])
        return sum(1 for a_row, b_row in zip(a, b) for x, y in zip(a_row, b_row) if tuple(x) != tuple(y))

    def drawn_px(rows):
        return sum(1 for row in rows for px in row if any(px))

    def settled(name, layout):
        """Its hair drawn from the layout asked for, the same count twice."""
        def right():
            h = hair(name)
            return h["pieces"] > 0 and h["laid"] == (h["pieces"] if layout else 0)
        assert wait_for(right, 15, 0.1), (name, layout, hair(name), obs.log())
        time.sleep(0.8)
        assert right(), (name, layout, hair(name))
        return hair(name)
    try:
        c.send("CreateScene", {"sceneName": "Laid scene"}, raw=True)
        for name, model, lines in zip(names, (figure, thin, among), (-1, 2, -1)):
            c.send("CreateInput", {"sceneName": "Laid scene", "inputName": name,
                                   "inputKind": "direbot_face_avatar",
                                   "inputSettings": {"model": model, "width": 300, "height": 300,
                                                     "size": 100, "framing": "whole", "style": "drawn",
                                                     "mirror": False, "smoothing": 0, "breathing": 0,
                                                     "sway": 0, "eye_moves": False,
                                                     "hair_budget_ms": 0, "hair_lines": lines,
                                                     "hair_layout": 0}}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Laid scene"}, raw=True)
        for name in names:
            assert wait_for(lambda: st(name)["shown"], 60, 0.2), obs.log()
            assert wait_for(lambda: st(name)["on_card"], 10, 0.1), (st(name), obs.log())
            assert wait_for(lambda: hair(name)["drawn"] == strands, 10, 0.1), (hair(name), obs.log())

        def both(name, what, least=2000, **settings):
            """The picture by point and laid out as drawn, with `settings`:
            the same to the pixel (at least `least` of them drawn); what was
            drawn each way."""
            change(name, hair_layout=0, **settings)
            before = settled(name, 0)
            by_point = shot(c, name)
            change(name, hair_layout=1)
            after = settled(name, 1)
            laid = shot(c, name)
            off = differ(by_point, laid)
            print("%s, %s: %d of %d drawn pixels differ laid out as drawn; by point %s; laid out %s"
                  % (name, what, off, drawn_px(by_point), before, after))
            assert drawn_px(by_point) > least and off == 0, (name, what, off, drawn_px(by_point))
            for key in ("drawn", "pieces", "lines", "skips_hidden", "hull_pieces"):
                assert before[key] == after[key], (name, what, key, before, after)
            # (Its hull laid out: a draw a group, never more than by point.)
            assert after["hull_draws"] <= before["hull_draws"], (name, what, before, after)
            assert (after["hull_draws"] > 0) == (after["hull_pieces"] > 0), (name, what, after)
            assert before["laid_mb"] >= 0 and after["laid_mb"] > 0, (before, after)
            return before, after

        print()
        # Ribbons: every hair, with the hidden ones left out.
        before, after = both("Laid", "ribbons")
        assert after["lines"] == 0 and after["drawn"] == strands and after["skips_hidden"], after
        # (One draw a run, as by point.)
        assert after["draws"] == before["draws"], (before, after)
        # (Its hull: some of its inner pieces; laid out, fewer draws.)
        assert 0 < after["hull_pieces"] < after["pieces"], after
        assert 0 < after["hull_draws"] < before["hull_draws"], (before, after)
        # (What hair took is two clocks added up: one of them told apart.)
        assert (after["ms"] < 0) == (after["depth_ms"] < 0) and after["depth_ms"] <= after["ms"] + 1e-6, after
        # Every hair sent, and only the hair that would be left out.
        both("Laid", "every hair sent", hair_skip_hidden=0)
        both("Laid", "only what's left out", hair_skip_hidden=2)
        change("Laid", hair_skip_hidden=-1)
        # Lines.
        before, after = both("Laid thin", "lines")
        assert after["lines"] == after["pieces"], after
        # Small in the picture (and every hair drawn left solid): only some
        # of its strands, the first of them.
        for size in (30, 12, 6):
            before, after = both("Laid thin", "small (size %d), some of its strands" % size, least=40, size=size,
                                 hair_lines=-1, hair_solid=1.0)
            if after["drawn"] < strands:
                break
        assert 0 < after["drawn"] < strands, after
        before, after = both("Laid thin", "small, as ribbons", least=40, hair_lines=0)
        assert after["lines"] == 0 and 0 < after["drawn"] < strands, after
        # Thin hairs among wide ones: the thin ones lines, the rest ribbons
        # - two sorts, each laid out apart.
        before, after = both("Laid among", "thin hairs as lines among wide ones")
        assert 0.6 * after["pieces"] < after["lines"] < 0.85 * after["pieces"], after
        both("Laid among", "all ribbons", hair_lines=0)
        both("Laid among", "all lines", hair_lines=2)
        # (By then all four of its layouts are made: more of the card's
        # memory than one.)
        assert hair("Laid among")["laid_mb"] > after["laid_mb"], (hair("Laid among"), after)

        # The hair budget's share held ("hair_share", for measuring): only
        # that share of its strands is drawn, each as much wider - the start
        # of every run, and of its hull's - by point and laid out the same
        # to the pixel, as ribbons and with its thin hairs as lines. And a
        # budget's lines kept with fewer hairs ("hair_line_loss": how much
        # of what they cover they may lose, up to 0.4 - Full's own -
        # instead of the first way's twentieth): never fewer lines, never
        # fewer hairs, than the first way.
        for share in (0.6, 0.3):
            before, after = both("Laid", "its share held at %s, ribbons" % share, least=1000, hair_share=share)
            assert abs(after["share"] - share) < 1e-6 and after["lines"] == 0, after
            assert abs(after["drawn"] - share * strands) <= 0.03 * strands, (share, after)
            assert 0 < after["hull_pieces"] < after["pieces"], after
        change("Laid", hair_share=-1)
        kept = {}
        for loss in (0.05, 0.4):
            for share in (0.7, 0.4):
                before, after = both("Laid among", "its share held at %s, a budget's lines losing %s" % (share, loss),
                                     least=1000, hair_lines=-1, hair_share=share, hair_line_loss=loss)
                assert abs(after["share"] - share) < 1e-6 and 16 <= after["drawn"] <= share * strands * 1.03, \
                    (share, after)
                kept[(loss, share)] = after
        for share in (0.7, 0.4):
            assert kept[(0.4, share)]["lines"] >= kept[(0.05, share)]["lines"], (share, kept)
            assert kept[(0.4, share)]["drawn"] >= kept[(0.05, share)]["drawn"], (share, kept)
        change("Laid among", hair_share=-1, hair_line_loss=-1)
        assert wait_for(lambda: hair("Laid among")["share"] == 1 and hair("Laid among")["drawn"] == strands, 10, 0.1), \
            hair("Laid among")

        # For measuring only (numbers the settings window doesn't show), by
        # point and laid out: the hidden-hair test made with nothing left
        # out ("hair_skip_hidden" 4) is the picture with every hair sent, to
        # the pixel; and hair into one pixel of the picture's corner
        # ("hair_measure" 1) sends as many pieces and leaves the picture
        # with no hair in it (every piece left out, "hair_skip_hidden" 3),
        # but for that pixel at most; so do "hair_measure" 2 and 3, which
        # read less of each corner and leave every piece out.
        for layout in (0, 1):
            change("Laid", hair_layout=layout, hair_skip_hidden=0, hair_measure=-1)
            every = settled("Laid", layout)
            sent = shot(c, "Laid")
            change("Laid", hair_skip_hidden=4)
            tested = settled("Laid", layout)
            off = differ(sent, shot(c, "Laid"))
            print("Laid, layout %d: the test made with nothing left out, %d pixels differ from every hair sent; %s"
                  % (layout, off, tested))
            assert off == 0 and tested["skips_hidden"] and not every["skips_hidden"], (off, every, tested)
            assert tested["pieces"] == every["pieces"], (every, tested)
            change("Laid", hair_skip_hidden=3)
            settled("Laid", layout)
            bare = shot(c, "Laid")
            assert differ(sent, bare) > 200, differ(sent, bare)
            change("Laid", hair_skip_hidden=0, hair_measure=1)
            measured = settled("Laid", layout)
            off = differ(bare, shot(c, "Laid"))
            print("Laid, layout %d: hair into one pixel, %d pixels differ from no hair at all; %s"
                  % (layout, off, measured))
            assert off <= 1 and measured["pieces"] == every["pieces"], (off, every, measured)
            assert measured["draws"] == every["draws"], (every, measured)
            # (And with less of each corner read - "hair_measure" 2: its
            # place and bones, 3: its place alone - every piece left out.)
            for measure in (2, 3):
                change("Laid", hair_measure=measure)
                lean = settled("Laid", layout)
                off = differ(bare, shot(c, "Laid"))
                print("Laid, layout %d: measure %d, %d pixels differ from no hair at all; %s"
                      % (layout, measure, off, lean))
                assert off == 0 and lean["pieces"] == every["pieces"] and lean["draws"] == every["draws"], (
                    measure, off, every, lean)
            # (In two draws a run - "hair_measure" 5: the picture with every
            # hair sent, from more draws. With no hull - 4: none of its
            # pieces are drawn, the hairs as ever.)
            change("Laid", hair_measure=5)
            two = settled("Laid", layout)
            off = differ(sent, shot(c, "Laid"))
            print("Laid, layout %d: in two draws a run, %d pixels differ from every hair sent; %s" % (layout, off, two))
            assert off == 0 and two["pieces"] == every["pieces"], (off, every, two)
            assert every["draws"] < two["draws"] <= 2 * every["draws"], (every, two)
            assert two["hull_pieces"] == every["hull_pieces"] > 0, (every, two)
            change("Laid", hair_measure=4)
            none = settled("Laid", layout)
            print("Laid, layout %d: no hull; %s" % (layout, none))
            assert none["hull_pieces"] == 0 and none["hull_draws"] == 0, none
            assert none["pieces"] == every["pieces"] and none["draws"] == every["draws"], (every, none)
        # (Making the layouts took some time, and it's told.)
        assert hair("Laid")["laid_ms"] > 0, hair("Laid")
        change("Laid", hair_skip_hidden=-1, hair_measure=-1)

        # Posed by the CPU: nothing's laid out, and the picture's as it was.
        change("Laid", hair_layout=1)
        settled("Laid", 1)
        on_card = shot(c, "Laid")
        change("Laid", card_posing=False)
        assert wait_for(lambda: not st("Laid")["on_card"], 10, 0.1), st("Laid")
        time.sleep(0.5)
        assert wait_for(lambda: not st("Laid")["loading"] and st("Laid")["shown"], 60, 0.1), (st("Laid"), obs.log())
        assert wait_for(lambda: hair("Laid")["drawn"] == strands, 10, 0.1), hair("Laid")
        time.sleep(0.8)
        h = hair("Laid")
        assert h["laid"] == 0 and h["laid_mb"] == 0 and h["pieces"] > 0, h
        cpu = shot(c, "Laid")
        off = sum(1 for a_row, b_row in zip(cpu, on_card) for x, y in zip(a_row, b_row)
                  if max(abs(int(p) - int(q)) for p, q in zip(x, y)) > 24)
        print("Laid out on the card vs posed by the CPU: %d pixels differ" % off)
        assert off <= 60, off
        change("Laid", card_posing=True)
        assert wait_for(lambda: st("Laid")["on_card"], 10, 0.1), st("Laid")
        settled("Laid", 1)
        log = obs.log()
        assert "3D effect" not in log and "Windows won't draw" not in log, log
    finally:
        for name in names:
            try:
                c.send("RemoveInput", {"inputName": name}, raw=True)
            except Exception:
                pass
        c.disconnect()


# The bones tufts_character's tufts stand on: across the picture from its
# right hand to its left.
TUFT_BONES = ("head", "chest", "leftUpperArm", "leftLowerArm", "leftHand", "rightUpperArm", "rightLowerArm",
              "rightHand")


def tufts_character(path, per=160, width=0.006, wide_every=0, vrm=True):
    """arms_character with a tuft of `per` magenta strands (6 points each)
    standing on each of TUFT_BONES, hung on it: eight groups of strands
    side by side, whose order from far to near turns round as the camera
    goes from one side of it to the other. Every hair `width` wide - but
    with `wide_every`, every so-manyth WIDE_HAIR wide (two sorts). Returns
    how many strands the file has."""
    arms_character(path, vrm=vrm)
    doc, blob = open_glb(path)
    names = {n.get("name"): i for i, n in enumerate(doc["nodes"])}
    bones = [names[b] for b in TUFT_BONES]
    hair = Strands()
    for k, bone in enumerate(bones):
        at = world_of(doc, bone)
        # (Its head's and its chest's stand in front of them, to be seen.)
        out = 0.13 if k < 2 else 0.0
        for i in range(per):
            a, b = (i * 0.61803) % 1, (i * 0.75488) % 1
            root = (at[0] - 0.04 + 0.08 * a, at[1] + 0.03, at[2] + out - 0.03 + 0.06 * b)
            points = [(root[0] + (a - 0.5) * 0.05 * t, root[1] + 0.16 * t, root[2] + 0.02 * t * t)
                      for t in (0, 0.2, 0.4, 0.6, 0.8, 1)]
            hair.add(points, WIDE_HAIR if wide_every and i % wide_every == 0 else width, MAGENTA,
                     [[(k, 1.0)]] * 6)
    add_strands(doc, blob, hair, bones)
    save_glb(path, doc, blob)
    return hair.count


def test_a_layer_of_hair_in_one_draw_on_the_card(obs):
    """Laid out as drawn ("hair_layout" 1), a layer of a part's hair goes to
    the card a group of strands at a time - those one bone holds, far groups
    first: a draw a group. With "hair_merge" 1 (a number the settings window
    doesn't show) a layer's groups go together - their corners copied in the
    order they're drawn in - in one draw while they keep that order, and in
    a few once some have changed places, until it's made again
    (FaceGetState's "merged": the pieces sent so; "merges": how many times
    such a stretch has been made; and fewer "draws"). The same corners in
    the same order, so the same picture to the pixel: as ribbons, with thin
    hairs as lines among wide ribbons, seen from either side (its groups'
    order turned round), and with only a share of its strands drawn. Two
    sorts drawn in one go (thin hairs among wide ones, all as ribbons) and
    hair posed by the CPU stay a draw a group."""
    figure = os.path.join(obs.home, "tufts.vrm")
    strands = tufts_character(figure)
    among = os.path.join(obs.home, "tufts-among.vrm")
    assert tufts_character(among, width=0.0006, wide_every=5) == strands
    views = json.dumps([{"name": "Left", "yaw": -100}, {"name": "Right", "yaw": 100}])
    names = ("Tufts", "Tufts among")
    groups, layers = len(TUFT_BONES), 5
    c = obs.client()

    def st(name):
        return vendor(c, "FaceGetState", {"source": name})["model"]

    def hair(name):
        return st(name)["hair"]

    def change(name, **settings):
        c.send("SetInputSettings", {"inputName": name, "inputSettings": settings}, raw=True)

    def differ(a, b):
        assert len(a) == len(b) and len(a[0]) == len(b[0])
        return sum(1 for a_row, b_row in zip(a, b) for x, y in zip(a_row, b_row) if tuple(x) != tuple(y))

    def drawn_px(rows):
        return sum(1 for row in rows for px in row if magenta(px))

    def settled(name, merged, laid=True):
        """Its hair drawn laid out - all of it a layer's groups together,
        or none of it - the same twice."""
        def right():
            h = hair(name)
            return (h["pieces"] > 0 and h["laid"] == (h["pieces"] if laid else 0)
                    and h["merged"] == (h["pieces"] if merged else 0))
        assert wait_for(right, 20, 0.1), (name, merged, hair(name), obs.log())
        time.sleep(0.8)
        assert right(), (name, merged, hair(name))
        return hair(name)
    try:
        c.send("CreateScene", {"sceneName": "Tufts scene"}, raw=True)
        for name, model in zip(names, (figure, among)):
            c.send("CreateInput", {"sceneName": "Tufts scene", "inputName": name,
                                   "inputKind": "direbot_face_avatar",
                                   "inputSettings": {"model": model, "width": 300, "height": 300,
                                                     "size": 100, "framing": "whole", "style": "drawn",
                                                     "mirror": False, "smoothing": 0, "breathing": 0,
                                                     "sway": 0, "eye_moves": False, "hair_budget_ms": 0,
                                                     "hair_layout": 1, "hair_merge": 0, "views": views,
                                                     "view": "main", "view_glide": 0}}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Tufts scene"}, raw=True)
        for name in names:
            assert wait_for(lambda: st(name)["shown"], 60, 0.2), obs.log()
            assert wait_for(lambda: st(name)["on_card"], 10, 0.1), (st(name), obs.log())
            assert wait_for(lambda: hair(name)["drawn"] == strands, 10, 0.1), (hair(name), obs.log())
        print()
        for name in names:
            # A draw a group, from the front and from each side: the
            # pictures to match.
            plain, was = {}, {}
            for view in ("main", "1", "2"):
                change(name, view=view)
                time.sleep(0.6)
                was[view] = settled(name, False)
                plain[view] = shot(c, name)
                assert drawn_px(plain[view]) > 150, (name, view, drawn_px(plain[view]))
            assert differ(plain["1"], plain["2"]) > 100 and differ(plain["main"], plain["1"]) > 100, name
            sorts = 2 if name == "Tufts among" else 1
            least = layers * sorts
            # (Every group has pieces in every layer: a draw each.)
            assert was["main"]["draws"] == groups * least, (name, was)
            if sorts == 2:
                assert 0 < was["main"]["lines"] < was["main"]["pieces"], was

            # Together, from the front: one draw a layer (and sort), the
            # same picture.
            change(name, view="main", hair_merge=1)
            time.sleep(0.6)
            h = settled(name, True)
            assert wait_for(lambda: hair(name)["draws"] == least, 10, 0.1), (name, hair(name))
            h = hair(name)
            off = differ(shot(c, name), plain["main"])
            print("%s, its layers' groups together from the front: %d pixels differ; a draw a group %s; together %s"
                  % (name, off, was["main"], h))
            assert off == 0, (name, off)
            for key in ("drawn", "pieces", "lines", "hull_pieces", "hull_draws", "skips_hidden"):
                assert h[key] == was["main"][key], (name, key, was["main"], h)
            assert h["merges"] >= least and h["laid_mb"] > was["main"]["laid_mb"], (name, was["main"], h)

            # Seen from one side, then the other: its groups' order turns
            # round. At once the rows that still hold are a draw each - the
            # same picture, never more draws than a draw a group - and once
            # it's made again, one draw a layer.
            for view in ("1", "2"):
                made = hair(name)["merges"]
                change(name, view=view)
                time.sleep(0.25)
                soon = hair(name)
                off = differ(shot(c, name), plain[view])
                print("%s, turned to view %s: %d pixels differ at once (%d draws, %d stretches made so far)"
                      % (name, view, off, soon["draws"], soon["merges"]))
                assert off == 0, (name, view, off)
                assert least <= soon["draws"] <= was[view]["draws"], (name, view, soon, was[view])
                # (Never left with more than a few rows a layer.)
                assert wait_for(lambda: hair(name)["draws"] <= 4 * least, 10, 0.1), (name, view, hair(name))
                h = settled(name, True)
                assert differ(shot(c, name), plain[view]) == 0, (name, view)
            # (From one side to the other every arm's groups have turned
            # round: made again, each layer in one draw.)
            assert wait_for(lambda: hair(name)["draws"] == least, 10, 0.1), (name, hair(name))
            assert hair(name)["merges"] >= made + least, (name, made, hair(name))

            # Never made again for a new order ("hair_merge" 2, for this
            # test): turned back, the rows that hold stay a draw each - more
            # than one a layer, fewer than a draw a group - and the picture
            # is still the same.
            change(name, hair_merge=2)
            time.sleep(0.5)
            made = settled(name, True)["merges"]
            change(name, view="1")
            time.sleep(1.5)
            h = settled(name, True)
            off = differ(shot(c, name), plain["1"])
            print("%s, turned back and not made again: %d pixels differ; %s" % (name, off, h))
            assert off == 0 and h["merges"] == made, (name, off, made, h)
            assert least < h["draws"] < was["1"]["draws"], (name, h, was["1"])

            # A share of its strands held: made for those, the same picture
            # as a draw a group.
            change(name, view="main", hair_merge=0, hair_share=0.5)
            time.sleep(0.6)
            some = settled(name, False)
            assert 0 < some["drawn"] < strands and some["pieces"] < was["main"]["pieces"], some
            held = shot(c, name)
            made = some["merges"]
            change(name, hair_merge=1)
            h = settled(name, True)
            off = differ(shot(c, name), held)
            print("%s, half its strands: %d pixels differ; a draw a group %s; together %s" % (name, off, some, h))
            assert off == 0 and h["pieces"] == some["pieces"] and h["drawn"] == some["drawn"], (name, off, some, h)
            assert h["merges"] > made and h["draws"] < some["draws"], (name, made, some, h)
            change(name, hair_share=-1)
            time.sleep(0.6)
            h = settled(name, True)
            assert h["drawn"] == strands and differ(shot(c, name), plain["main"]) == 0, (name, h)

        # Two sorts drawn in one go - thin hairs among wide ones, all as
        # ribbons: a group's wide pieces, then its thin ones, a group at a
        # time. Nothing one draw can do: a draw a group, the picture as it
        # is without.
        change("Tufts among", hair_lines=0, hair_merge=0)
        time.sleep(0.6)
        before = settled("Tufts among", False)
        ribbons = shot(c, "Tufts among")
        change("Tufts among", hair_merge=1)
        time.sleep(1.5)
        after = settled("Tufts among", False)
        assert after["lines"] == 0 and after["draws"] == before["draws"], (before, after)
        assert differ(shot(c, "Tufts among"), ribbons) == 0
        change("Tufts among", hair_lines=-1)

        # A measure's two draws a run ("hair_measure" 5): as it was, not
        # together.
        change("Tufts", hair_measure=5)
        time.sleep(0.6)
        two = settled("Tufts", False)
        assert two["draws"] == 2 * groups * layers, two
        change("Tufts", hair_measure=-1)
        settled("Tufts", True)

        # Posed by the CPU: nothing laid out, nothing together.
        change("Tufts", card_posing=False)
        assert wait_for(lambda: not st("Tufts")["on_card"], 10, 0.1), st("Tufts")
        time.sleep(0.5)
        assert wait_for(lambda: not st("Tufts")["loading"] and st("Tufts")["shown"], 60, 0.1), (st("Tufts"), obs.log())
        assert wait_for(lambda: hair("Tufts")["drawn"] == strands, 10, 0.1), hair("Tufts")
        time.sleep(0.8)
        h = hair("Tufts")
        assert h["laid"] == 0 and h["merged"] == 0 and h["pieces"] > 0, h
        log = obs.log()
        assert "3D effect" not in log and "Windows won't draw" not in log, log
    finally:
        for name in names:
            try:
                c.send("RemoveInput", {"inputName": name}, raw=True)
            except Exception:
                pass
        c.disconnect()


def test_a_companions_hair_is_drawn_as_its_owners(obs):
    """A companion beside a character is drawn in its picture, by its
    settings: its hair too is laid out as drawn ("hair_layout" 1) and its
    layers' groups sent together ("hair_merge" 1) when its owner's is - and
    is the same picture to the pixel as by point, a draw a group."""
    owner = os.path.join(obs.home, "owner.vrm")
    arms_character(owner)
    friend = os.path.join(obs.home, "friend.glb")
    strands = tufts_character(friend, vrm=False)
    c = obs.client()

    def st():
        return vendor(c, "FaceGetState", {"source": "Owner"})

    def hair():
        return st()["model"]["hair"]

    def change(**settings):
        c.send("SetInputSettings", {"inputName": "Owner", "inputSettings": settings}, raw=True)

    def settled(laid, merged):
        def right():
            h = hair()
            return (h["drawn"] == strands and h["pieces"] > 0 and h["laid"] == (h["pieces"] if laid else 0)
                    and h["merged"] == (h["pieces"] if merged else 0))
        assert wait_for(right, 20, 0.1), (laid, merged, hair(), obs.log())
        time.sleep(0.8)
        assert right(), (laid, merged, hair())
        return hair()
    try:
        c.send("CreateScene", {"sceneName": "Owner scene"}, raw=True)
        c.send("CreateInput", {"sceneName": "Owner scene", "inputName": "Owner",
                               "inputKind": "direbot_face_avatar",
                               "inputSettings": {"model": owner, "width": 300, "height": 300,
                                                 "size": 100, "framing": "whole", "style": "drawn",
                                                 "mirror": False, "smoothing": 0, "breathing": 0,
                                                 "sway": 0, "eye_moves": False, "hair_budget_ms": 0,
                                                 "hair_layout": 0, "hair_merge": 0}}, raw=True)
        c.send("SetCurrentProgramScene", {"sceneName": "Owner scene"}, raw=True)
        assert wait_for(lambda: st()["model"]["shown"], 60, 0.2), obs.log()
        assert wait_for(lambda: st()["model"]["on_card"], 10, 0.1), (st()["model"], obs.log())
        # (Its owner has no hair of its own.)
        time.sleep(0.5)
        assert hair()["drawn"] == 0 and hair()["pieces"] == 0, hair()
        answer = vendor(c, "FaceReact", {"source": "Owner", "do": "companion", "action": "come", "file": friend,
                                         "instant": True})
        assert answer["ok"], answer
        assert wait_for(lambda: st()["companion"]["drawn"], 60, 0.2), (st()["companion"], obs.log())
        assert wait_for(lambda: st()["companion"]["phase"] == "out", 20, 0.1), st()["companion"]
        by_point = settled(False, False)
        time.sleep(1.0)
        before = shot(c, "Owner")
        assert sum(1 for row in before for px in row if magenta(px)) > 60, by_point
        change(hair_layout=1)
        laid = settled(True, False)
        assert laid["draws"] == by_point["draws"] and laid["pieces"] == by_point["pieces"], (by_point, laid)
        assert sum(1 for a, b in zip(shot(c, "Owner"), before) for x, y in zip(a, b) if tuple(x) != tuple(y)) == 0
        change(hair_merge=1)
        together = settled(True, True)
        assert wait_for(lambda: hair()["draws"] == 5, 10, 0.1), hair()
        off = sum(1 for a, b in zip(shot(c, "Owner"), before) for x, y in zip(a, b) if tuple(x) != tuple(y))
        print("\nA companion's hair: %d pixels differ laid out with its layers' groups together; by point %s; "
              "together %s" % (off, by_point, hair()))
        assert off == 0 and together["pieces"] == by_point["pieces"], (off, by_point, together)
        log = obs.log()
        assert "3D effect" not in log and "Windows won't draw" not in log, log
    finally:
        try:
            c.send("RemoveInput", {"inputName": "Owner"}, raw=True)
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


def leaks(log):
    """How many things OBS's log says were never given back (None: it
    doesn't say - OBS didn't close in order)."""
    found = re.search(r"Number of memory leaks: (\d+)", log)
    return int(found.group(1)) if found else None


def hair_scene(o, c, name="Kept", **settings):
    """A character with hair strands in a scene of its own, shown, posed on
    the graphics card, the card's clock answering. Returns (its state, a
    way to change its settings and wait for it)."""
    figure = os.path.join(o.home, "hair-%s.vrm" % name.lower())
    hair_character(figure, standin=True)

    def st():
        return vendor(c, "FaceGetState", {"source": name})["model"]

    def change(**new):
        c.send("SetInputSettings", {"inputName": name, "inputSettings": new}, raw=True)
        time.sleep(0.5)
        assert wait_for(lambda: not st()["loading"] and st()["shown"], 60, 0.1), (st(), o.log())
        time.sleep(0.8)
    scenes = [x["sceneName"] for x in c.send("GetSceneList", raw=True)["scenes"]]
    if "Kept scene" not in scenes:
        c.send("CreateScene", {"sceneName": "Kept scene"}, raw=True)
    c.send("CreateInput", {"sceneName": "Kept scene", "inputName": name,
                           "inputKind": "direbot_face_avatar",
                           "inputSettings": dict({"model": figure, "width": 200, "height": 200,
                                                  "size": 60, "framing": "whole", "style": "drawn",
                                                  "mirror": False, "smoothing": 0, "breathing": 0,
                                                  "sway": 0, "eye_moves": False}, **settings)}, raw=True)
    c.send("SetCurrentProgramScene", {"sceneName": "Kept scene"}, raw=True)
    assert wait_for(lambda: st()["shown"], 60, 0.2), o.log()
    assert wait_for(lambda: st()["on_card"], 10, 0.1), (st(), o.log())
    assert wait_for(lambda: st()["hair"]["drawn"] > 0, 10, 0.1), (st()["hair"], o.log())
    if settings.get("hair_budget_ms", -1) != 0:
        assert wait_for(lambda: st()["hair"]["ms"] >= 0, 20, 0.1), (st()["hair"], o.log())
    return st, change


def idle(o, c):
    """The add-on loaded, nothing made."""
    time.sleep(2)


def hair_kept(o, c):
    """One character with hair strands, the hair budget its own, its hair
    detail changed to Light, to Today and back, the budget far too small
    and its own again - and still in its scene when OBS closes, as a
    streamer closes OBS."""
    st, change = hair_scene(o, c)
    assert st()["hair"]["budget_ms"] > 0, st()["hair"]
    for detail in ("light", "today", "full"):
        change(hair_detail=detail)
        assert st()["hair"]["detail"] == detail, st()["hair"]
    change(hair_budget_ms=0.001)
    assert wait_for(lambda: st()["hair"]["share"] < 0.5, 20, 0.05), (st()["hair"], o.log())
    change(hair_budget_ms=-1)
    time.sleep(1.5)


def hair_recorded(o, c):
    """As a test of what hair costs goes: three characters with hair
    strands at once (Full, Light, and Full with no budget), each shown in
    turn while OBS records and a picture of it is saved - then all of it
    taken away again before OBS closes."""
    made = {"Full": {}, "Light": {"hair_detail": "light"}, "Unbudgeted": {"hair_budget_ms": 0}}
    for name, settings in made.items():
        hair_scene(o, c, name, **settings)
    items = {name: c.send("GetSceneItemId", {"sceneName": "Kept scene", "sourceName": name},
                          raw=True)["sceneItemId"] for name in made}
    try:
        c.send("SetRecordDirectory", {"recordDirectory": o.home}, raw=True)
    except Exception as e:      # (an OBS whose WebSocket has no such request)
        print("no SetRecordDirectory here: %s" % e)
    recorded = 0
    for shown in made:
        for name, item in items.items():
            c.send("SetSceneItemEnabled", {"sceneName": "Kept scene", "sceneItemId": item,
                                           "sceneItemEnabled": name == shown}, raw=True)
        c.send("StartRecord", raw=True)
        assert wait_for(lambda: c.send("GetRecordStatus", raw=True)["outputActive"], 20, 0.2), o.log()
        time.sleep(2)
        c.send("SaveSourceScreenshot", {"sourceName": shown, "imageFormat": "png",
                                        "imageFilePath": os.path.join(o.home, "kept-%s.png" % shown)},
               raw=True)
        c.send("StopRecord", raw=True)
        assert wait_for(lambda: not c.send("GetRecordStatus", raw=True)["outputActive"], 30, 0.2), o.log()
        recorded += 1
    time.sleep(2)       # (the last file written out)
    assert recorded == 3
    for name in made:
        c.send("RemoveInput", {"inputName": name}, raw=True)
    others = [x["sceneName"] for x in c.send("GetSceneList", raw=True)["scenes"]
              if x["sceneName"] != "Kept scene"]
    assert others, "OBS's own first scene isn't there"
    c.send("SetCurrentProgramScene", {"sceneName": others[0]}, raw=True)
    c.send("RemoveScene", {"sceneName": "Kept scene"}, raw=True)
    time.sleep(2)


def test_closing_obs_leaves_nothing_behind(obs):
    """(After every test above: it closes their OBS.) Closed the way the X
    on its window closes it, OBS's log says how much was never given back.
    With the add-on that's no more than OBS leaves on its own: after all
    the tests above (the pictures, the phone, the tracker app, characters
    posed on the graphics card, hair strands on it, gaming hands), with
    the add-on doing nothing, with a character with hair strands still in
    its scene, and after recording three of them and taking them away."""
    left, said = {}, {}
    closed = obs.close()
    if not closed:
        seen = obs.windows()
        obs.kill()      # (so the next OBS has the machine to itself)
        said["after all the tests"] = "OBS didn't close: %s" % (seen,)
    left["after all the tests"] = leaks(obs.log())

    def session(name, add_on=True, doing=None):
        o = None
        try:
            o = WindowsObs(add_on=add_on)
            if doing:
                c = o.client()
                try:
                    doing(o, c)
                finally:
                    c.disconnect()
            if not o.close():
                said[name] = "OBS didn't close: %s" % (o.windows(),)
            left[name] = leaks(o.log())
            print("\n----- OBS's log, %s -----\n%s" % (name, o.log()))
        except Exception as e:
            said[name] = "%s: %s" % (type(e).__name__, e)
            left.setdefault(name, None)
            if o:
                print("\n----- OBS's log, %s (failed) -----\n%s" % (name, o.log()))
        finally:
            if o:
                o.kill()

    session("OBS on its own", add_on=False)
    session("the add-on, nothing made", doing=idle)
    session("hair kept till it closes", doing=hair_kept)
    session("hair recorded, then taken away", doing=hair_recorded)
    print("\nLeft behind when OBS closes (how many things were never given back):")
    for name, n in left.items():
        print("  %-34s %s%s" % (name, n, "   " + said[name][:300] if name in said else ""))
    assert not said, said
    own = left["OBS on its own"]
    assert own is not None, left
    assert all(n == own for n in left.values()), left
