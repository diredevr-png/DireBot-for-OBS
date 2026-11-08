"""
The Windows zip inside a real OBS for Windows - each OBS version the
add-on supports, the newest too. OBS 33 took functions out of obs.dll
that 0.8.1 linked to, and Windows then wouldn't load the add-on at all;
this is the check that catches that.

Runs only on a throwaway CI machine (it uses that Windows user's real OBS
settings folder and C:\\ProgramData\\obs-studio\\plugins):

    set CI=true
    set DIREBOT_OBS_ROOT=C:\\obs-33.0.1          (OBS's Windows zip, unpacked)
    set DIREBOT_OBS_ZIP=release\\direbot-for-obs-0.8.2-windows-x64.zip
    set DIREBOT_OBS_LAYOUT=new                    (new: OBS 33+, legacy: bin\\64bit)
    set DIREBOT_OBS_EXPECT=33.0.1
    python -m pytest tests/test_real_windows.py
"""

import base64
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_face import camera_picture, character_picture, read_bmp, write_gif  # noqa: E402
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
