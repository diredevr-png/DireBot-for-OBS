"""
"DireBot Face Avatar" inside a real OBS: a made-up camera (the test photo
in tests/data/face, on a grey room) shown through a face avatar, which
has to find the face where it is in the picture, "Set" it, go easy on the
PC when asked, and let go when the face is gone. OBS runs on a pretend
screen with its own empty settings, like test_real_obs.py.

Skipped unless OBS, Xvfb and the test build are there:

    BUILD_DIR=/tmp/b tools/build-linux-test.sh
    DIREBOT_OBS_PLUGIN=/tmp/b/direbot-for-obs.so \\
        python3 -m pytest tests/test_real_face.py
"""

import json
import os
import shutil
import struct
import time
import zlib

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_obs import HERE, Obs, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark  # noqa: E402,F401

SCENE = "Face test"
FACE = os.path.join(HERE, "tests", "data", "face")
# The camera's picture: the photo twice as big, this far in.
W, H, AT_X, AT_Y = 960, 540, 220, 20


def write_png(path, w, h, rows, alpha=False):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))
    raw = b"".join(b"\0" + bytes(row) for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8,
                                           6 if alpha else 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def read_ppm(path):
    data = open(path, "rb").read()
    magic, w, h, top, rest = data.split(maxsplit=4)
    assert magic == b"P6" and top == b"255"
    return int(w), int(h), rest[:int(w) * int(h) * 3]


def camera_picture(path, with_face=True):
    w, h, px = read_ppm(os.path.join(FACE, "portrait.ppm"))
    rows = []
    for y in range(H):
        row = bytearray([96]) * (W * 3)
        sy = (y - AT_Y) // 2
        if with_face and 0 <= sy < h:
            line = px[sy * w * 3:(sy + 1) * w * 3]
            for x in range(w):
                at = (AT_X + 2 * x) * 3
                row[at:at + 6] = line[x * 3:x * 3 + 3] * 2
        rows.append(row)
    write_png(path, W, H, rows)


def character_picture(path, size=400):
    rows = []
    for y in range(size):
        row = bytearray(size * 4)
        for x in range(size):
            if (x - size / 2) ** 2 + (y - size / 2) ** 2 < (size * 0.45) ** 2:
                row[x * 4:x * 4 + 4] = b"\xe0\x80\x30\xff"
        rows.append(row)
    write_png(path, size, size, rows, alpha=True)


def send(c, request, data=None):
    return c.send(request, data or {}, raw=True)


def state(c, points=False):
    return vendor(c, "FaceGetState", {"source": "Avatar", "points": points})


@pytest.fixture(scope="module")
def run():
    o = Obs()
    yield o
    if o.proc.poll() is None:
        o.close()
    if not os.environ.get("DIREBOT_OBS_KEEP"):
        shutil.rmtree(o.home, ignore_errors=True)


def test_it_finds_the_face_and_sets_it(run):
    face_png = os.path.join(run.home, "camera.png")
    empty_png = os.path.join(run.home, "empty-room.png")
    body_png = os.path.join(run.home, "character.png")
    camera_picture(face_png)
    camera_picture(empty_png, with_face=False)
    character_picture(body_png)
    # OBS's own websocket: the same requests DireBot sends.
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="",
                        timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": SCENE})
        send(c, "CreateInput", {
            "sceneName": SCENE, "inputName": "Camera",
            "inputKind": "image_source",
            "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {
            "sceneName": SCENE, "inputName": "Avatar",
            "inputKind": "direbot_face_avatar",
            "inputSettings": {"source": "Camera", "character": body_png}})
        send(c, "SetCurrentProgramScene", {"sceneName": SCENE})

        # Found, where the face is in the camera's picture.
        assert wait_for(lambda: state(c)["found"], 30, 0.2), state(c)
        s = state(c, points=True)
        assert s["ok"] and s["ready"] and s["showing"]
        assert s["camera"] == "Camera" and "problem" not in s
        assert s["presence"] > 0.9 and not s["set"]
        assert len(s["parts"]) == 3
        first, second, mouth = s["parts"]
        assert first["x"] < second["x"]      # the first eye: picture's left
        assert mouth["y"] > first["y"] and mouth["y"] > second["y"]
        assert all(0 < p["w"] < 0.2 and 0 < p["h"] < 0.2 for p in s["parts"])
        # The points where the face is (the photo's own, from MediaPipe).
        assert len(s["points"]) == 478
        want = json.load(open(os.path.join(FACE, "expected.json")))["plain"]
        for i in (1, 33, 263, 13):          # nose, eyes' corners, lips
            x, y = want["points"][i][:2]
            assert abs(s["points"][i]["x"] - (AT_X + 2 * x + 1) / W) < 0.006
            assert abs(s["points"][i]["y"] - (AT_Y + 2 * y + 1) / H) < 0.01
        # Thirty looks a second (OBS here runs at 30 frames a second), on
        # its own thread. (Counted over 2 seconds.)
        time.sleep(4.5)
        s = state(c)
        assert 24 <= s["looks_per_second"] <= 31, s
        assert 0 < s["ms"] < 40

        # "Set": kept in its settings, and the same face reads as at rest.
        assert vendor(c, "FaceSet", {"source": "Avatar"})["ok"]
        assert wait_for(lambda: state(c)["set"] and not state(c)["setting"],
                        10, 0.1)
        rest = send(c, "GetInputSettings", {"inputName": "Avatar"})[
            "inputSettings"]["rest"]
        assert json.loads(rest)["v"] == 1 and len(json.loads(rest)["pose"]) == 12
        time.sleep(0.5)
        s = state(c)
        assert all(abs(s["turn"][k]) < 2 for k in ("yaw", "pitch", "roll"))
        assert max(s["moves"].values()) < 0.1
        # A face avatar it doesn't know, and a "Set" while not shown.
        assert not vendor(c, "FaceSet", {"source": "Camera"})["ok"]
        assert not vendor(c, "FaceGetState", {"source": "Nope"})["ok"]

        # Easy on the PC: half as many looks.
        send(c, "SetInputSettings", {"inputName": "Avatar",
                                     "inputSettings": {"easy": True}})
        time.sleep(4.5)
        assert 12 <= state(c)["looks_per_second"] <= 16

        # The face gone: held for a moment, then the character's own face.
        send(c, "SetInputSettings", {"inputName": "Camera",
                                     "inputSettings": {"file": empty_png}})
        assert wait_for(lambda: not state(c)["found"], 5, 0.05)
        assert wait_for(lambda: state(c)["shown"] == 0, 3, 0.05)
        s = state(c)
        assert not s["holding"] and s["set"]
        # Back.
        send(c, "SetInputSettings", {"inputName": "Camera",
                                     "inputSettings": {"file": face_png}})
        assert wait_for(lambda: state(c)["found"] and state(c)["shown"] == 1,
                        5, 0.05)

        # Hidden: it stops looking, and "Set" says to show it first.
        item = send(c, "GetSceneItemId", {
            "sceneName": SCENE, "sourceName": "Avatar"})["sceneItemId"]
        send(c, "SetSceneItemEnabled", {"sceneName": SCENE,
                                        "sceneItemId": item,
                                        "sceneItemEnabled": False})
        assert wait_for(lambda: not state(c)["showing"], 5, 0.1)
        answer = vendor(c, "FaceSet", {"source": "Avatar"})
        assert not answer["ok"] and "Show" in answer["error"]
        send(c, "SetSceneItemEnabled", {"sceneName": SCENE,
                                        "sceneItemId": item,
                                        "sceneItemEnabled": True})
        assert wait_for(lambda: state(c)["found"], 10, 0.1)

        # Cleared: gone from its settings too.
        assert vendor(c, "FaceSet", {"source": "Avatar", "clear": True})["ok"]
        assert wait_for(lambda: not state(c)["set"], 5, 0.1)
        assert send(c, "GetInputSettings", {"inputName": "Avatar"})[
            "inputSettings"]["rest"] == ""
        # What it draws comes out (the character, the size it's set to).
        shot = send(c, "GetSourceScreenshot", {
            "sourceName": "Avatar", "imageFormat": "png", "imageWidth": 200})
        assert shot["imageData"].startswith("data:image/png;base64,")
    finally:
        c.disconnect()


def test_closing_obs_leaves_nothing_behind(run):
    assert run.close() == 0
    log = run.log()
    assert "Number of memory leaks: 0" in log
