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

import base64
import json
import math
import os
import shutil
import struct
import threading
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


# Where its eyes and mouth are on the character (shares of it).
SPOTS = {"left_eye": [0.27, 0.36, 0.18, 0.12],
         "right_eye": [0.55, 0.36, 0.18, 0.12],
         "mouth": [0.37, 0.62, 0.26, 0.13]}


def character_picture(path, size=400, eye_holes=False):
    """An orange ball - with see-through eyes, if asked."""
    def hole(u, v):
        for b in (SPOTS["left_eye"], SPOTS["right_eye"]):
            cx, cy = b[0] + b[2] / 2, b[1] + b[3] / 2
            if ((u - cx) / (b[2] * 0.42)) ** 2 + ((v - cy) / (b[3] * 0.42)) ** 2 < 1:
                return True
        return False
    rows = []
    for y in range(size):
        row = bytearray(size * 4)
        for x in range(size):
            if (x - size / 2) ** 2 + (y - size / 2) ** 2 < (size * 0.45) ** 2 and \
                    not (eye_holes and hole(x / size, y / size)):
                row[x * 4:x * 4 + 4] = b"\xe0\x80\x30\xff"
        rows.append(row)
    write_png(path, size, size, rows, alpha=True)


def drawn_picture(path, ovals, size=400):
    """A see-through picture with dark ovals (middle x, y and half sizes as
    shares): a drawn eye or mouth."""
    rows = []
    for y in range(size):
        row = bytearray(size * 4)
        for x in range(size):
            u, v = x / size, y / size
            if any(((u - cx) / rx) ** 2 + ((v - cy) / ry) ** 2 < 1
                   for cx, cy, rx, ry in ovals):
                row[x * 4:x * 4 + 4] = b"\x10\x08\x08\xff"
        rows.append(row)
    write_png(path, size, size, rows, alpha=True)


def ball_points(cx, cy, cz, rx, ry, rz, n=24):
    """A ball's points, their facings and its triangles."""
    pos, nrm, ix = [], [], []
    for i in range(n + 1):
        th = math.pi * i / n
        for j in range(n + 1):
            ph = 2 * math.pi * j / n
            u = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
            pos.append((cx + u[0] * rx, cy + u[1] * ry, cz + u[2] * rz))
            f = (u[0] / rx, u[1] / ry, u[2] / rz)
            k = math.sqrt(sum(c * c for c in f)) or 1
            nrm.append(tuple(c / k for c in f))
    for i in range(n):
        for j in range(n):
            a = i * (n + 1) + j
            b = a + n + 1
            ix += [a, b, a + 1, a + 1, b, b + 1]
    return pos, nrm, ix


def write_glb(path, parts):
    """A plain .glb: one node a part, each (points, facings, triangles,
    colour as linear red/green/blue, roughness)."""
    blob = bytearray()
    views, accessors, meshes, nodes, materials = [], [], [], [], []

    def add(data, target):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob),
                      "byteLength": len(data), "target": target})
        blob.extend(data)
        return len(views) - 1

    for i, (pos, nrm, ix, colour, rough) in enumerate(parts):
        p = add(struct.pack("<%df" % (len(pos) * 3), *[c for v in pos for c in v]), 34962)
        lo = [min(v[k] for v in pos) for k in range(3)]
        hi = [max(v[k] for v in pos) for k in range(3)]
        accessors.append({"bufferView": p, "componentType": 5126, "count": len(pos),
                          "type": "VEC3", "min": lo, "max": hi})
        n = add(struct.pack("<%df" % (len(nrm) * 3), *[c for v in nrm for c in v]), 34962)
        accessors.append({"bufferView": n, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        t = add(struct.pack("<%dI" % len(ix), *ix), 34963)
        accessors.append({"bufferView": t, "componentType": 5125, "count": len(ix), "type": "SCALAR"})
        materials.append({"pbrMetallicRoughness": {"baseColorFactor": list(colour) + [1],
                                                   "metallicFactor": 0, "roughnessFactor": rough}})
        meshes.append({"primitives": [{"attributes": {"POSITION": 3 * i, "NORMAL": 3 * i + 1},
                                       "indices": 3 * i + 2, "material": i}]})
        nodes.append({"name": "part%d" % i, "mesh": i})
    doc = {"asset": {"version": "2.0"}, "scene": 0,
           "scenes": [{"nodes": list(range(len(nodes)))}], "nodes": nodes,
           "meshes": meshes, "materials": materials, "accessors": accessors,
           "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    text = json.dumps(doc).encode()
    text += b" " * (-len(text) % 4)
    while len(blob) % 4:
        blob.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def ball_character(path):
    """A 3D orange ball head 20 cm across, two black eyes on its front."""
    write_glb(path, [
        ball_points(0, 0, 0, 0.1, 0.1, 0.1) + ((0.8, 0.25, 0.04), 0.4),
        ball_points(-0.035, 0.025, 0.088, 0.018, 0.022, 0.012) + ((0.01, 0.01, 0.01), 0.3),
        ball_points(0.035, 0.025, 0.088, 0.018, 0.022, 0.012) + ((0.01, 0.01, 0.01), 0.3),
    ])


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


def read_bmp(data):
    """A screenshot as rows of (r, g, b): 24 or 32 bits a pixel."""
    start, = struct.unpack_from("<I", data, 10)
    w, h, _, bits = struct.unpack_from("<iiHH", data, 18)
    step = bits // 8
    line = (w * step + 3) & ~3
    rows = []
    for y in range(abs(h)):
        at = start + (abs(h) - 1 - y if h > 0 else y) * line
        rows.append([(data[at + x * step + 2], data[at + x * step + 1],
                      data[at + x * step]) for x in range(w)])
    return rows


def test_cutouts_sit_still_while_you_move(run):
    # The face jumps back and forth in the camera's picture, out of step
    # with OBS's frames. Your eye has to stay put in the character's eye:
    # it's cut from the very picture the tracker found it in, not a newer
    # one where it has already moved on.
    face_png = os.path.join(run.home, "camera.png")
    body_png = os.path.join(run.home, "character.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
        character_picture(body_png)
    size = 400
    spot = [0.25, 0.3, 0.2, 0.14]  # its left eye, as shares of the character
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="",
                        timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Moving"})
        send(c, "CreateInput", {
            "sceneName": "Moving", "inputName": "Moving photo",
            "inputKind": "image_source", "inputSettings": {"file": face_png}})
        item = send(c, "GetSceneItemId", {
            "sceneName": "Moving", "sourceName": "Moving photo"})["sceneItemId"]
        send(c, "CreateScene", {"sceneName": "Still"})
        send(c, "CreateInput", {
            "sceneName": "Still", "inputName": "Steady avatar",
            "inputKind": "direbot_face_avatar",
            "inputSettings": {
                "source": "Moving", "character": body_png, "width": size,
                "height": size, "size": 100, "mirror": False,
                "follow_move": 0, "follow_grow": 0, "follow_tilt": 0,
                "turn_pictures": False, "smoothing": 100,
                "spots": json.dumps({"left_eye": spot,
                                     "right_eye": [0.55, 0.3, 0.2, 0.14],
                                     "mouth": [0.38, 0.6, 0.24, 0.14]})}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Still"})

        def steady():
            return vendor(c, "FaceGetState", {"source": "Steady avatar"})
        assert wait_for(lambda: steady()["found"] and steady()["shown"] == 1,
                        30, 0.2), steady()

        def eye_x():
            # Where the dark of the eye is inside the character's eye.
            shot = send(c, "GetSourceScreenshot", {
                "sourceName": "Steady avatar", "imageFormat": "bmp",
                "imageWidth": size})["imageData"]
            rows = read_bmp(base64.b64decode(shot.split(",", 1)[1]))
            x0, y0 = int(spot[0] * size), int(spot[1] * size)
            x1, y1 = int((spot[0] + spot[2]) * size), int((spot[1] + spot[3]) * size)
            dark = [x for y in range(y0, y1) for x in range(x0, x1)
                    if sum(rows[y][x]) < 3 * 90]
            return sum(dark) / len(dark) if len(dark) > 20 else None

        still = [eye_x() for _ in range(6)]
        moving = []
        stop = threading.Event()

        def jiggle():
            # Its own connection: 30 pixels right and back, 23 times a second.
            j = obsws.ReqClient(host="127.0.0.1", port=run.ws_port,
                                password="", timeout=10)
            right = False
            while not stop.is_set():
                right = not right
                j.send("SetSceneItemTransform", {
                    "sceneName": "Moving", "sceneItemId": item,
                    "sceneItemTransform": {"positionX": 30 if right else 0}},
                    raw=True)
                time.sleep(0.043)
            j.disconnect()
        mover = threading.Thread(target=jiggle)
        mover.start()
        try:
            time.sleep(0.5)
            for _ in range(24):
                moving.append(eye_x())
                time.sleep(0.017)
        finally:
            stop.set()
            mover.join()
        assert None not in still and None not in moving, (still, moving)
        spread = max(moving) - min(moving)
        print(f"eye in the cut-out: still {min(still):.1f}..{max(still):.1f},"
              f" moving {min(moving):.1f}..{max(moving):.1f} px")
        assert max(still) - min(still) < 1.5
        # The face jumps 30 px (the eye spot shows it about 1.4 times
        # bigger): a newer picture's eye would wander up to ~40 px.
        assert spread < 6, moving
    finally:
        c.disconnect()


def screenshot(c, name, width=None):
    """What a source shows, as rows of (r, g, b) - kept as a PNG too, in
    the folder DIREBOT_FACE_SHOTS names, for looking at."""
    data = {"sourceName": name, "imageFormat": "bmp"}
    if width:
        data["imageWidth"] = width
    shot = send(c, "GetSourceScreenshot", data)["imageData"]
    rows = read_bmp(base64.b64decode(shot.split(",", 1)[1]))
    keep = os.environ.get("DIREBOT_FACE_SHOTS")
    if keep:
        write_png(os.path.join(keep, name + ".png"), len(rows[0]), len(rows),
                  [bytes(v for px in row for v in px) for row in rows])
    return rows


def middle_of(rows, test, box=None):
    """The middle (x, y) of the pixels `test` picks, and how many."""
    x0, y0, x1, y1 = box or (0, 0, len(rows[0]), len(rows))
    xs, ys = [], []
    for y in range(y0, y1):
        for x in range(x0, x1):
            if test(rows[y][x]):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None, None, 0
    return sum(xs) / len(xs), sum(ys) / len(ys), len(xs)


def turned_rest(rest_text, yaw_degrees):
    """A "Set" that's turned: the same face then reads as turned by
    -yaw_degrees (towards the picture's left)."""
    r = json.loads(rest_text)
    p = r["pose"]
    a = math.radians(yaw_degrees)
    turn = [[math.cos(a), 0, math.sin(a)], [0, 1, 0],
            [-math.sin(a), 0, math.cos(a)]]
    m = [[p[0], p[1], p[2]], [p[4], p[5], p[6]], [p[8], p[9], p[10]]]
    for i in range(3):
        for j in range(3):
            p[i * 4 + j] = round(sum(turn[i][k] * m[k][j] for k in range(3)), 5)
    return json.dumps(r)


ORANGE = (0xe0, 0x80, 0x30)


def near_colour(px, want, by):
    return all(abs(a - b) <= by for a, b in zip(px, want))


def test_it_turns_blends_draws_and_wears(run):
    face_png = os.path.join(run.home, "camera.png")
    body_png = os.path.join(run.home, "character.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
        character_picture(body_png)
    holes_png = os.path.join(run.home, "character-holes.png")
    character_picture(holes_png, eye_holes=True)
    eyes_png = os.path.join(run.home, "eyes-open.png")
    mouth_png = os.path.join(run.home, "mouth-closed.png")
    drawn_picture(eyes_png, [(0.36, 0.42, 0.05, 0.05), (0.64, 0.42, 0.05, 0.05)])
    drawn_picture(mouth_png, [(0.5, 0.685, 0.08, 0.015)])
    size = 400
    # Held still where it is (only "Turned" turns), so where things are
    # can be checked.
    common = {"source": "Looks camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "spots": json.dumps(SPOTS),
              "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "bend": 0}
    avatars = {
        "Plain": {"character": body_png},
        "Turned": {"character": body_png, "bend": 85},
        "Own skin": {"character": body_png, "skin": 0},
        "See-through": {"character": holes_png},
        "Drawn": {"character": body_png, "style": "drawn",
                  "eyes_open": eyes_png, "mouth_closed": mouth_png},
        "Worn": {"character": body_png, "worn": True},
        "Worn bare": {"character": body_png, "worn": True,
                      "show_camera": False},
    }
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="",
                        timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Looks"})
        send(c, "CreateInput", {
            "sceneName": "Looks", "inputName": "Looks camera",
            "inputKind": "image_source", "inputSettings": {"file": face_png}})
        for name, extra in avatars.items():
            send(c, "CreateInput", {
                "sceneName": "Looks", "inputName": name,
                "inputKind": "direbot_face_avatar",
                "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Looks"})

        def st(name):
            return vendor(c, "FaceGetState", {"source": name})
        for name in avatars:
            assert wait_for(lambda: st(name)["found"] and st(name)["shown"] == 1,
                            30, 0.2), st(name)
        s = st("Plain")
        assert s["style"] == "real" and not s["worn"] and s["shaped"]
        assert s["placed"]["shown"] and s["placed"]["picture"] == "main"
        assert not any(s["see_through"].values())
        assert st("See-through")["see_through"] == {
            "left_eye": True, "right_eye": True, "mouth": False}

        # Turned: "Set", then that "Set" turned 25 degrees the other way,
        # so you read as turned 25 degrees to the picture's left. The
        # character turns ("Turn and nod with me": 85%), its mouth sliding
        # over to that side.
        assert vendor(c, "FaceSet", {"source": "Turned"})["ok"]
        assert wait_for(lambda: st("Turned")["set"] and not st("Turned")["setting"],
                        10, 0.1)
        rest = send(c, "GetInputSettings", {"inputName": "Turned"})[
            "inputSettings"]["rest"]
        send(c, "SetInputSettings", {"inputName": "Turned", "inputSettings": {
            "rest": turned_rest(rest, 25)}})
        assert wait_for(lambda: st("Turned")["turn"]["yaw"] < -20, 5, 0.1)
        time.sleep(0.3)
        s = st("Turned")
        assert abs(s["placed"]["yaw"] - 0.85 * s["turn"]["yaw"]) < 0.5, s
        teeth = lambda px: sum(px) > 3 * 205  # noqa: E731
        lower = (0, size // 2, size, size)
        plain_x, _, n1 = middle_of(screenshot(c, "Plain"), teeth, lower)
        turned_x, _, n2 = middle_of(screenshot(c, "Turned"), teeth, lower)
        print(f"teeth: {plain_x:.1f} straight, {turned_x:.1f} turned")
        assert n1 > 100 and n2 > 100
        assert turned_x < plain_x - 10
        # Bending off: it stays flat.
        send(c, "SetInputSettings", {"inputName": "Turned",
                                     "inputSettings": {"bend": 0}})
        assert wait_for(lambda: st("Turned")["placed"]["yaw"] == 0, 5, 0.1)

        # Your skin round your eyes takes the character's colour; with
        # "skin" at 0 it keeps yours: how far from its orange the eye's
        # spot is, on average. (Your eye itself keeps its colours in both.)
        def off_orange(name):
            rows = screenshot(c, name)
            b = SPOTS["left_eye"]
            px = [rows[y][x]
                  for y in range(int(b[1] * size), int((b[1] + b[3]) * size))
                  for x in range(int(b[0] * size), int((b[0] + b[2]) * size))]
            return sum(sum(abs(a - o) for a, o in zip(p, ORANGE)) for p in px) / len(px)
        blended, own = off_orange("Plain"), off_orange("Own skin")
        print(f"eye spot off the character's orange: {blended:.1f} blended,"
              f" {own:.1f} your own skin")
        assert blended < own - 15

        # See-through eyes: yours show from behind it, inside the holes.
        rows = screenshot(c, "See-through")
        b = SPOTS["left_eye"]
        middle = rows[int((b[1] + b[3] / 2) * size)][int((b[0] + b[2] / 2) * size)]
        assert not near_colour(middle, ORANGE, 40) and sum(middle) > 0

        # Its own drawn face: no smile picture, so the closed mouth.
        s = st("Drawn")
        assert s["style"] == "drawn"
        assert set(s["drawn"]) == {"left_eye", "right_eye", "mouth"}
        rows = screenshot(c, "Drawn")
        assert sum(rows[int(0.42 * size)][int(0.36 * size)]) < 3 * 50
        assert sum(rows[int(0.685 * size)][int(0.5 * size)]) < 3 * 50
        _, _, light = middle_of(rows, teeth, lower)
        assert light == 0  # none of your teeth

        # Worn on your head: the camera's size, the character's eyes on
        # yours, the camera round it (or nothing).
        item = send(c, "GetSceneItemId", {
            "sceneName": "Looks", "sourceName": "Worn"})["sceneItemId"]
        t = send(c, "GetSceneItemTransform", {
            "sceneName": "Looks", "sceneItemId": item})["sceneItemTransform"]
        assert (t["sourceWidth"], t["sourceHeight"]) == (W, H)
        s = st("Worn")
        assert s["worn"] and s["placed"]["shown"]
        first, second = s["parts"][0], s["parts"][1]
        ex, ey = (first["x"] + second["x"]) / 2 * W, (first["y"] + second["y"]) / 2 * H
        assert abs(s["placed"]["x"] - ex) < 8 and abs(s["placed"]["y"] - ey) < 8
        rows = screenshot(c, "Worn")
        assert len(rows) == H and len(rows[0]) == W
        assert near_colour(rows[int(ey)][int(ex)], ORANGE, 40)
        assert near_colour(rows[10][10], (96, 96, 96), 4)  # the room
        bare = screenshot(c, "Worn bare")
        assert near_colour(bare[int(ey)][int(ex)], ORANGE, 40)
        assert bare[10][10] == (0, 0, 0)  # nothing (see-through)
        # The face gone: held a moment, then gone.
        empty_png = os.path.join(run.home, "empty-room.png")
        if not os.path.exists(empty_png):
            camera_picture(empty_png, with_face=False)
        send(c, "SetInputSettings", {"inputName": "Looks camera",
                                     "inputSettings": {"file": empty_png}})
        assert wait_for(lambda: not st("Worn")["placed"]["shown"], 5, 0.1)
        rows = screenshot(c, "Worn")
        assert near_colour(rows[int(ey)][int(ex)], (96, 96, 96), 4)
        send(c, "SetInputSettings", {"inputName": "Looks camera",
                                     "inputSettings": {"file": face_png}})
        assert wait_for(lambda: st("Worn")["placed"]["shown"], 10, 0.1)
        for name in avatars:
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


def test_3d_characters(run):
    """A .glb character: loaded on its own, drawn lit with depth (its eyes
    in front of its head), turning with you, worn on your head; a broken
    file says why and draws nothing."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    ball = os.path.join(run.home, "ball.glb")
    ball_character(ball)
    broken = os.path.join(run.home, "broken.glb")
    with open(broken, "wb") as f:
        f.write(b"glTF not really")
    size = 300
    common = {"source": "3D camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 100}
    avatars = {
        "3D": {"model": ball},
        "3D turned": {"model": ball},
        "3D worn": {"model": ball, "worn": True},
        "3D broken": {"model": broken},
    }
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "3D characters"})
        send(c, "CreateInput", {"sceneName": "3D characters", "inputName": "3D camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        for name, extra in avatars.items():
            send(c, "CreateInput", {"sceneName": "3D characters", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "3D characters"})

        def st(name):
            return vendor(c, "FaceGetState", {"source": name})
        for name in ("3D", "3D turned", "3D worn"):
            assert wait_for(lambda: st(name)["found"] and st(name)["model"]["shown"], 30, 0.2), st(name)
        m = st("3D")["model"]
        assert m["loaded"] and not m["loading"] and m["kind"] == "gltf"
        assert m["points"] == 3 * 25 * 25 and m["triangles"] == 3 * 24 * 24 * 2
        assert m["springs"] == 0 and "problem" not in m
        # The broken one: why, in plain words, and nothing drawn.
        assert wait_for(lambda: st("3D broken")["model"].get("problem"), 10, 0.1)
        b = st("3D broken")["model"]
        assert not b["loaded"] and not b["shown"]

        # In its own spot: an orange ball, lit (brighter up and to the
        # left, where the light is), its eyes dark in front of it.
        time.sleep(0.5)
        rows = screenshot(c, "3D")
        mid = rows[size // 2 + 30][size // 2]
        assert mid[0] > 150 and mid[0] > mid[1] > mid[2], mid
        lit, dim = rows[size // 2 - 60][size // 2 - 60], rows[size // 2 + 60][size // 2 + 60]
        assert sum(lit) > sum(dim) + 30, (lit, dim)
        dark = lambda px: sum(px) < 3 * 60 and px != (0, 0, 0)  # noqa: E731
        ex, ey, n = middle_of(rows, dark, (0, 0, size, size // 2 + 20))
        assert n > 50 and abs(ex - size / 2) < 5, (ex, ey, n)
        assert rows[5][5] == (0, 0, 0)   # see-through round it

        # Turned 25 degrees to the picture's left: it turns with you, its
        # eyes going left.
        assert vendor(c, "FaceSet", {"source": "3D turned"})["ok"]
        assert wait_for(lambda: st("3D turned")["set"] and not st("3D turned")["setting"], 10, 0.1)
        rest = send(c, "GetInputSettings", {"inputName": "3D turned"})["inputSettings"]["rest"]
        send(c, "SetInputSettings", {"inputName": "3D turned", "inputSettings": {
            "rest": turned_rest(rest, 25)}})
        assert wait_for(lambda: st("3D turned")["model"]["yaw"] < -20, 5, 0.1)
        time.sleep(0.3)
        tx, _, n = middle_of(screenshot(c, "3D turned"), dark, (0, 0, size, size // 2 + 20))
        print(f"3D eyes: {ex:.1f} straight, {tx:.1f} turned")
        assert n > 30 and tx < ex - 20

        # Worn on your head: the camera's size, the ball over your face,
        # the room round it.
        rows = screenshot(c, "3D worn")
        assert len(rows) == H and len(rows[0]) == W
        s = st("3D worn")
        first, second = s["parts"][0], s["parts"][1]
        fx, fy = (first["x"] + second["x"]) / 2 * W, (first["y"] + second["y"]) / 2 * H
        px = rows[int(fy) + 25][int(fx)]
        assert px[0] > 150 and px[0] > px[1] > px[2], px
        assert near_colour(rows[10][10], (96, 96, 96), 4)
        mx, my = s["model"]["x"], s["model"]["y"]
        assert abs(mx - fx) < 40 and abs(my - fy) < 60, (mx, my, fx, fy)
        for name in avatars:
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


def test_closing_obs_leaves_nothing_behind(run):
    assert run.close() == 0
    log = run.log()
    assert "Number of memory leaks: 0" in log
