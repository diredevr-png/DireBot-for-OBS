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
import wave
import zlib

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_obs import HERE, Obs, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark  # noqa: E402,F401

SCENE = "Face test"
# 0.5.0's "alive when you're still" off, for pictures compared pixel by
# pixel.
STILL = {"breathing": 0, "sway": 0, "eye_moves": False}
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


def write_gif(path, w, h, frames, palette, delay_cs):
    """An animated GIF, looping: each frame a list of palette numbers (up
    to 4 colours), `delay_cs` hundredths of a second each."""
    def pixels(indices):
        # LZW kept to 3-bit codes: a clear code before every two pixels,
        # so its table never grows a code size.
        codes = []
        for i in range(0, len(indices), 2):
            codes.append(4)
            codes.extend(indices[i:i + 2])
        codes.append(5)
        bits = n = 0
        out = bytearray()
        for code in codes:
            bits |= code << n
            n += 3
            while n >= 8:
                out.append(bits & 0xFF)
                bits >>= 8
                n -= 8
        if n:
            out.append(bits & 0xFF)
        blocks = b"".join(bytes([len(out[i:i + 255])]) + bytes(out[i:i + 255])
                          for i in range(0, len(out), 255))
        return bytes([2]) + blocks + b"\0"
    table = b"".join(bytes(c) for c in (palette + [(0, 0, 0)] * 4)[:4])
    data = (b"GIF89a" + struct.pack("<HHBBB", w, h, 0x81, 0, 0) + table +
            b"\x21\xff\x0bNETSCAPE2.0\x03\x01\0\0\0")
    for frame in frames:
        data += b"\x21\xf9\x04\x00" + struct.pack("<H", delay_cs) + b"\0\0"
        data += b"\x2c" + struct.pack("<HHHHB", 0, 0, w, h, 0) + pixels(frame)
    with open(path, "wb") as f:
        f.write(data + b"\x3b")


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
    """A ball's points, their facings and its triangles (their fronts
    out: corners anticlockwise seen from outside, as glTF has them)."""
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
            ix += [a, a + 1, b, a + 1, b + 1, b]
    return pos, nrm, ix


def write_glb(path, parts, tree=None, animations=None, materials_more=None, pictures=None, top=None):
    """A plain .glb: one node a part, each (points, facings, triangles,
    colour as linear red/green/blue, roughness) - or, given `tree` (glTF
    nodes, the first the top one, "mesh" naming a part), its parts hung on
    those. `animations`: its own, {name: [(node, path, times, values)]}
    (values flat, straight between keys). `materials_more`: more for a
    part's material, {part: {"doubleSided": True, ...}}. `pictures`: a
    part's colour picture, {part: (its UVs, PNG bytes)}. `top`: more at
    its top (a VRM's "extensions")."""
    blob = bytearray()
    views, accessors, meshes, nodes, materials = [], [], [], [], []
    images, textures = [], []

    def add(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        if target:
            views[-1]["target"] = target
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
        attributes = {"POSITION": len(accessors) - 3, "NORMAL": len(accessors) - 2}
        indices = len(accessors) - 1
        pbr = {"baseColorFactor": list(colour) + [1], "metallicFactor": 0, "roughnessFactor": rough}
        if i in (pictures or {}):
            uv, png = pictures[i]
            u = add(struct.pack("<%df" % (len(uv) * 2), *[c for v in uv for c in v]), 34962)
            accessors.append({"bufferView": u, "componentType": 5126, "count": len(uv), "type": "VEC2"})
            attributes["TEXCOORD_0"] = len(accessors) - 1
            images.append({"bufferView": add(png), "mimeType": "image/png"})
            textures.append({"source": len(images) - 1})
            pbr["baseColorTexture"] = {"index": len(textures) - 1}
        more = dict((materials_more or {}).get(i, {}))
        pbr.update(more.pop("pbr", {}))
        materials.append({"pbrMetallicRoughness": pbr, **more})
        meshes.append({"primitives": [{"attributes": attributes, "indices": indices, "material": i}]})
        nodes.append({"name": "part%d" % i, "mesh": i})
    if tree is not None:
        nodes = tree
    anims = []
    for name, channels in (animations or {}).items():
        samplers, chans = [], []
        for node, kind, times, values in channels:
            t = add(struct.pack("<%df" % len(times), *times))
            accessors.append({"bufferView": t, "componentType": 5126, "count": len(times),
                              "type": "SCALAR", "min": [min(times)], "max": [max(times)]})
            v = add(struct.pack("<%df" % len(values), *values))
            accessors.append({"bufferView": v, "componentType": 5126, "count": len(times),
                              "type": "VEC4" if kind == "rotation" else "VEC3"})
            samplers.append({"input": len(accessors) - 2, "output": len(accessors) - 1})
            chans.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": kind}})
        anims.append({"name": name, "samplers": samplers, "channels": chans} if name else
                     {"samplers": samplers, "channels": chans})
    doc = {"asset": {"version": "2.0"}, "scene": 0,
           "scenes": [{"nodes": [0] if tree is not None else list(range(len(nodes)))}],
           "nodes": nodes,
           "meshes": meshes, "materials": materials, "accessors": accessors,
           "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    if anims:
        doc["animations"] = anims
    if images:
        doc["images"], doc["textures"] = images, textures
    doc.update(top or {})
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


def set_up_character(path, animations=None):
    """An orange ball head with a blue body under it (a red badge on its
    front, right) and a green ear sticking out to its right: two links,
    a bead and a tip. Its nodes are named, as a set-up names them."""
    write_glb(path, animations=animations, parts=[
        ball_points(0, 0, 0, 0.1, 0.1, 0.1) + ((0.8, 0.25, 0.04), 0.4),
        ball_points(0, 0, 0, 0.07, 0.06, 0.05) + ((0.05, 0.1, 0.7), 0.5),
        ball_points(0, 0, 0, 0.016, 0.016, 0.016) + ((0.8, 0.01, 0.01), 0.5),
        ball_points(0, 0, 0, 0.012, 0.012, 0.012) + ((0.05, 0.6, 0.05), 0.5),
        ball_points(0, 0, 0, 0.022, 0.022, 0.022) + ((0.05, 0.6, 0.05), 0.5),
    ], tree=[
        {"name": "Character", "children": [1, 2, 4]},
        {"name": "Ball", "mesh": 0},
        {"name": "Body", "translation": [0, -0.16, 0], "mesh": 1, "children": [3]},
        {"name": "Badge", "translation": [0.04, 0, 0.045], "mesh": 2},
        {"name": "Ear0", "translation": [0.09, 0.04, 0], "children": [5]},
        {"name": "Ear1", "translation": [0.06, 0, 0], "mesh": 3, "children": [6]},
        {"name": "Ear2", "translation": [0.06, 0, 0], "mesh": 4},
    ])


# Its eyes and mouth, as DireBot sends them (in the model, its own left
# eye on its left).
SET_UP_SPOTS = {"left_eye": {"at": [0.035, 0.025, 0.1], "w": 0.045, "h": 0.03},
                "right_eye": {"at": [-0.035, 0.025, 0.1], "w": 0.045, "h": 0.03},
                "mouth": {"at": [0, -0.045, 0.09], "w": 0.06, "h": 0.03}}


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
        # A look at each new picture (up to thirty a second, on its own
        # thread: test_it_looks_at_each_new_picture); a picture that stays
        # just the same, like this photo, ten a second. (Counted over 2
        # seconds; fewer when a busy machine makes OBS skip frames.)
        time.sleep(4.5)
        s = state(c)
        fps = send(c, "GetStats")["activeFps"]
        assert max(3.0, 0.25 * min(fps, 30)) <= s["looks_per_second"] <= 16, (s, fps)
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
        fps = send(c, "GetStats")["activeFps"]
        assert max(1.5, 0.12 * min(fps, 30)) <= state(c)["looks_per_second"] <= 8.5, fps

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


def test_it_looks_at_each_new_picture(run):
    """A camera's picture changes every time it sends one: each new one
    is looked at (up to thirty a second), soon after it comes. A small
    picture flickering in a corner of the scene stands in for a camera's
    grain; the photo moving, for you."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    flicker = os.path.join(run.home, "flicker.gif")
    # Three frames 20 ms each: whatever the frame rate, each frame OBS
    # draws shows a different one from the frame before.
    write_gif(flicker, 8, 8, [[i] * 64 for i in range(3)],
              [(10, 10, 10), (60, 60, 60), (120, 120, 120)], 2)
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="",
                        timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Flicker"})
        send(c, "CreateInput", {"sceneName": "Flicker", "inputName": "Flicker photo",
                                "inputKind": "image_source",
                                "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Flicker", "inputName": "Grain",
                                "inputKind": "image_source",
                                "inputSettings": {"file": flicker}})
        photo = send(c, "GetSceneItemId", {"sceneName": "Flicker",
                                           "sourceName": "Flicker photo"})["sceneItemId"]
        send(c, "CreateScene", {"sceneName": "Quick"})
        send(c, "CreateInput", {"sceneName": "Quick", "inputName": "Quick avatar",
                                "inputKind": "direbot_face_avatar",
                                "inputSettings": {"source": "Flicker"}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Quick"})
        quick = lambda: vendor(c, "FaceGetState", {"source": "Quick avatar"})
        assert wait_for(lambda: quick()["found"], 30, 0.2), quick()
        time.sleep(4.5)
        s = quick()
        fps = send(c, "GetStats")["activeFps"]
        assert max(5.0, 0.8 * min(fps, 30)) <= s["looks_per_second"] <= 31, (s, fps)
        # The camera check's count of new pictures a second (0.9.0): one
        # each frame OBS draws.
        cam = s["camera_check"]["camera_fps"]
        assert 0.8 * min(fps, 60) <= cam <= fps + 2, (cam, fps)
        # You move: it sees where you went within a few frames (two to read
        # the picture back and look, one to pick it up; a frame is up to
        # 1/30 s here, longer on a busy machine), not a look or two behind.
        frame = 1 / max(1.0, min(fps, 60))
        took = []
        for step in (40, -40, 40, -40):
            x0 = quick()["parts"][0]["x"]
            at = send(c, "GetSceneItemTransform", {"sceneName": "Flicker",
                                                   "sceneItemId": photo})["sceneItemTransform"]
            t0 = time.perf_counter()
            send(c, "SetSceneItemTransform", {"sceneName": "Flicker", "sceneItemId": photo,
                                              "sceneItemTransform": {"positionX": at["positionX"] + step}})
            assert wait_for(lambda: abs(quick()["parts"][0]["x"] - x0) > 0.01, 2, 0.002)
            took.append(time.perf_counter() - t0)
            time.sleep(0.3)
        print("\nsees a move in %s ms (frames of %.0f ms, looks of %.0f ms)" % (
            [round(1000 * t) for t in took], 1000 * frame, s["ms"]))
        assert sorted(took)[1] < 5 * frame + 2 * s["ms"] / 1000 + 0.05, took
        send(c, "RemoveScene", {"sceneName": "Quick"})
        send(c, "RemoveScene", {"sceneName": "Flicker"})
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
                "turn_pictures": False, "smoothing": 100, **STILL,
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
              "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "bend": 0, **STILL}
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
              "follow_tilt": 0, "bend": 100, **STILL}
    avatars = {
        "3D": {"model": ball},
        "3D turned": {"model": ball},
        "3D worn": {"model": ball, "worn": True},
        # Every way of drawing at once (the hidden head, outlines, the
        # parts, smoothing): the closing test checks each had its values.
        "3D outlined": {"model": ball, "worn": True, "outline": 60},
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
        for name in ("3D", "3D turned", "3D worn", "3D outlined"):
            assert wait_for(lambda: st(name)["found"] and st(name)["model"]["shown"], 30, 0.2), st(name)
        m = st("3D")["model"]
        assert m["loaded"] and not m["loading"] and m["kind"] == "gltf"
        assert m["points"] == 3 * 25 * 25 and m["triangles"] == 3 * 24 * 24 * 2
        assert m["springs"] == 0 and "problem" not in m
        assert not m["set_up"] and m["upright_parts"] == 0
        assert {v["from"] for v in m["spots"].values()} == {"guessed"}
        assert wait_for(lambda: st("3D")["model"]["cards"] == 3, 10, 0.1)
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


def hull_character(path, two_sided=False, mirrored=False):
    """An orange ball in a dark shell a little bigger, the shell turned
    inside out (its triangles' fronts in): a hand-made outline, an
    "inverted hull". One-sided, unless `two_sided`; the whole of it
    mirrored (scaled -1 across), if `mirrored`."""
    ball = ball_points(0, 0, 0, 0.1, 0.1, 0.1)
    pos, nrm, ix = ball_points(0, 0, 0, 0.108, 0.108, 0.108)
    inward = [tuple(-c for c in n) for n in nrm]
    turned = [ix[t + k] for t in range(0, len(ix), 3) for k in (0, 2, 1)]
    write_glb(path, [ball + ((0.8, 0.25, 0.04), 0.4), (pos, inward, turned, (0.01, 0.01, 0.01), 0.9)],
              tree=[{"name": "Character", "children": [1, 2],
                     **({"scale": [-1, 1, 1]} if mirrored else {})},
                    {"name": "Ball", "mesh": 0}, {"name": "Shell", "mesh": 1}],
              materials_more={1: {"doubleSided": True}} if two_sided else None)


def test_one_sided_parts_show_only_their_fronts(run):
    """0.7.0: a part shows only its front unless its material is
    double-sided, as glTF says - so a hand-made outline (a dark shell
    turned inside out round a ball) is a dark rim round the ball, not a
    dark ball; double-sided, it covers the ball; mirrored, it's the same
    as not; and the outline setting still draws round one-sided parts.
    A file made again in its place loads anew."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    files = {}
    for name, extra in (("Hull", {}), ("Hull two-sided", {"two_sided": True}),
                        ("Hull mirrored", {"mirrored": True})):
        files[name] = os.path.join(run.home, name.replace(" ", "-").lower() + ".glb")
        hull_character(files[name], **extra)
    ball = os.path.join(run.home, "ball.glb")
    ball_character(ball)
    size = 300
    common = {"source": "Sides camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 100, **STILL}
    avatars = {name: {"model": path} for name, path in files.items()}
    avatars["Ball plain"] = {"model": ball}
    avatars["Ball outlined"] = {"model": ball, "outline": 60}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Sides"})
        send(c, "CreateInput", {"sceneName": "Sides", "inputName": "Sides camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        for name, extra in avatars.items():
            send(c, "CreateInput", {"sceneName": "Sides", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Sides"})

        def st(name):
            return vendor(c, "FaceGetState", {"source": name})
        for name in avatars:
            assert wait_for(lambda: st(name)["found"] and st(name)["model"]["shown"], 30, 0.2), st(name)
        time.sleep(0.5)

        orange = lambda px: px[0] > 120 and px[0] > px[1] + 40 > px[2] + 40  # noqa: E731
        dark = lambda px: px != (0, 0, 0) and sum(px) < 3 * 50  # noqa: E731

        def across(rows):
            """The middle row: where it starts and ends, and what's there."""
            row = rows[size // 2]
            seen = [x for x, px in enumerate(row) if px != (0, 0, 0)]
            return seen[0], seen[-1], row

        shots = {name: screenshot(c, name) for name in avatars}
        for name in ("Hull", "Hull mirrored"):
            rows = shots[name]
            first, last, row = across(rows)
            mid = rows[size // 2][size // 2]
            # The ball shows through the shell's near side (its back)...
            assert orange(mid), (name, mid)
            # ...in a dark rim (the shell's far side, round it).
            rim = row[first:first + 3] + row[last - 2:last + 1]
            assert all(dark(px) for px in rim), (name, rim)
            assert orange(row[first + 12]) and orange(row[last - 12]), (name, row[first + 12])
        # Both sides: the shell covers it all.
        rows = shots["Hull two-sided"]
        first, last, row = across(rows)
        assert dark(rows[size // 2][size // 2]), rows[size // 2][size // 2]
        assert all(dark(px) for px in row[first + 2:last - 1:7])
        # Mirrored, the same picture (a ball's the same either way round).
        same = sum(1 for a, b in zip(shots["Hull"][size // 2], shots["Hull mirrored"][size // 2])
                   if max(abs(a[k] - b[k]) for k in range(3)) <= 12)
        assert same >= size - 4, same
        # The outline setting: still round the (one-sided) ball, past it.
        p_first, p_last, _ = across(shots["Ball plain"])
        o_first, o_last, o_row = across(shots["Ball outlined"])
        assert o_first < p_first - 1 and o_last > p_last + 1, (p_first, p_last, o_first, o_last)
        assert dark(o_row[o_first]) and dark(o_row[o_last])
        assert orange(shots["Ball plain"][size // 2 + 30][size // 2])
        # Its file made again in its place (as DireBot remakes a starter
        # character): OBS told the same file loads it anew.
        hull_character(files["Hull"], two_sided=True)
        send(c, "SetInputSettings", {"inputName": "Hull", "inputSettings": {"model": files["Hull"]}})
        assert wait_for(lambda: dark(screenshot(c, "Hull")[size // 2][size // 2]), 10, 0.2)
        for name in avatars:
            send(c, "RemoveInput", {"inputName": name})
        send(c, "RemoveInput", {"inputName": "Sides camera"})
    finally:
        c.disconnect()



def edge_character(path, two_sided=False):
    """A light grey square facing the camera (20 cm), its facings leaning
    from straight out at its top to down and away at its bottom - past
    side-on over its lowest third, as a smooth part's facings lean at its
    edge. One-sided, unless `two_sided`."""
    pos = [(-0.1, -0.1, 0), (0.1, -0.1, 0), (0.1, 0.1, 0), (-0.1, 0.1, 0)]
    down = (0, -0.9, -0.44)
    k = math.sqrt(sum(c * c for c in down))
    down = tuple(c / k for c in down)
    nrm = [down, down, (0, 0, 1), (0, 0, 1)]
    write_glb(path, [(pos, nrm, [0, 1, 2, 0, 2, 3], (0.6, 0.6, 0.6), 1.0)],
              materials_more={0: {"doubleSided": True}} if two_sided else None)


def test_one_sided_edges_are_lit_side_on(run):
    """0.8.0: on a one-sided part, a facing leaning away from the camera
    is a smooth facing just past side-on at its edge (its back faces are
    never drawn): it's lit side-on, not turned round - turned round, an
    edge lit from above (under an earlobe, along a raised shoulder) showed
    a bright line. A two-sided part's back faces still turn round."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    one = os.path.join(run.home, "edge.glb")
    two = os.path.join(run.home, "edge-two-sided.glb")
    edge_character(one)
    edge_character(two, two_sided=True)
    size = 300
    common = {"source": "Edges camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 100, "detail_maps": False, "light_preset": "own",
              "light_side": 0, "light_height": 60, "light_brightness": 100,
              "room_brightness": 30, "rim_brightness": 0, **STILL}
    avatars = {"Edge one-sided": one, "Edge two-sided": two}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Edges"})
        send(c, "CreateInput", {"sceneName": "Edges", "inputName": "Edges camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        for name, path in avatars.items():
            send(c, "CreateInput", {"sceneName": "Edges", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, "model": path}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Edges"})

        def st(name):
            return vendor(c, "FaceGetState", {"source": name})
        for name in avatars:
            assert wait_for(lambda: st(name)["found"] and st(name)["model"]["shown"], 30, 0.2), st(name)
        time.sleep(0.5)

        def down_its_side(name):
            """Brightness top to bottom, down a line near its left edge
            (clear of the eyes and mouth drawn on it), its outline left out."""
            rows = screenshot(c, name)
            middle = rows[size // 2]
            seen = [x for x, px in enumerate(middle) if px != (0, 0, 0)]
            assert len(seen) > size // 4, (name, len(seen))
            x = seen[0] + (seen[-1] - seen[0]) * 8 // 100
            ys = [y for y in range(size) if rows[y][x] != (0, 0, 0)]
            return [sum(rows[y][x]) for y in range(ys[0] + 2, ys[-1] - 1)]
        one_sided = down_its_side("Edge one-sided")
        two_sided = down_its_side("Edge two-sided")
        n = len(one_sided)
        assert n > 40, n
        # One-sided: darker the further down (it faces away from the light
        # above), its lowest rows no brighter than the middle.
        middle, bottom = one_sided[n * 6 // 10], max(one_sided[n * 9 // 10:])
        assert bottom <= middle + 12, (middle, bottom, one_sided)
        # Two-sided: its lowest third is a back seen, turned round to face
        # up - lit by the light above, brighter than its middle.
        n2 = len(two_sided)
        assert n2 > 40, n2
        middle2, bottom2 = two_sided[n2 * 6 // 10], max(two_sided[n2 * 9 // 10:])
        assert bottom2 > middle2 + 40, (middle2, bottom2, two_sided)
        for name in avatars:
            send(c, "RemoveInput", {"inputName": name})
        send(c, "RemoveInput", {"inputName": "Edges camera"})
    finally:
        c.disconnect()

def png_bytes(path, w, h, rows):
    """A PNG's bytes (with see-through), by way of a file at `path`."""
    write_png(path, w, h, rows, alpha=True)
    with open(path, "rb") as f:
        data = f.read()
    os.remove(path)
    return data


def test_see_through_parts_keep_their_shine(run):
    """0.7.0: a see-through part (glTF's BLEND) in the glossy look keeps
    the light it reflects - a dark visor 15% solid shines as the same
    visor solid does - while its picture's empty parts stay empty (a
    half-empty card gains no shine where it's empty)."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    dark = ((0.03, 0.03, 0.035), 0.12)
    glass = {"alphaMode": "BLEND", "pbr": {"baseColorFactor": [0.03, 0.03, 0.035, 0.15]}}
    files = {}
    files["Visor"] = os.path.join(run.home, "visor.glb")
    write_glb(files["Visor"], [ball_points(0, 0, 0, 0.1, 0.1, 0.1) + dark], materials_more={0: glass})
    files["Visor solid"] = os.path.join(run.home, "visor-solid.glb")
    write_glb(files["Visor solid"], [ball_points(0, 0, 0, 0.1, 0.1, 0.1) + dark])
    # A card, its picture's left half grey and its right half empty.
    half = [bytes((128, 128, 128, 255)) * 8 + bytes((128, 128, 128, 0)) * 8 for _ in range(16)]
    card = ([(-0.1, -0.1, 0), (0.1, -0.1, 0), (0.1, 0.1, 0), (-0.1, 0.1, 0)], [(0, 0, 1)] * 4,
            [0, 1, 2, 0, 2, 3], (1, 1, 1), 0.12)
    files["Card"] = os.path.join(run.home, "card.glb")
    write_glb(files["Card"], [card], materials_more={0: {"alphaMode": "BLEND"}},
              pictures={0: ([(0, 1), (1, 1), (1, 0), (0, 0)],
                            png_bytes(os.path.join(run.home, "half.png"), 16, 16, half))})
    size = 300
    common = {"source": "Shine camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 100, "look": "glossy", "style": "drawn", **STILL}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Shine"})
        send(c, "CreateInput", {"sceneName": "Shine", "inputName": "Shine camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        for name, path in files.items():
            send(c, "CreateInput", {"sceneName": "Shine", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, "model": path}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Shine"})

        def st(name):
            return vendor(c, "FaceGetState", {"source": name})
        for name in files:
            assert wait_for(lambda: st(name)["found"] and st(name)["model"]["shown"], 30, 0.2), st(name)
        time.sleep(0.5)
        shots = {name: screenshot(c, name) for name in files}

        def brightest(rows):
            return max(max(px) for row in rows for px in row)
        solid, see_through = brightest(shots["Visor solid"]), brightest(shots["Visor"])
        print(f"visor's shine: {solid} solid, {see_through} 15% solid")
        assert solid > 200 and see_through > solid * 0.8, (solid, see_through)
        # ...and the visor itself still mostly see-through: dark, and its
        # middle no more solid than it is.
        mid = shots["Visor"][size // 2 + 40][size // 2]
        assert max(mid) < 60, mid

        # The card fills the picture: its left half grey, its right half
        # (but for its very edge, where the picture wraps round) empty.
        rows = shots["Card"]
        band = range(size // 5, size * 4 // 5)
        grey = [rows[y][x] for y in band for x in range(size // 10, size * 4 // 10)]
        empty = [rows[y][x] for y in band for x in range(size * 6 // 10, size * 9 // 10)]
        assert min(min(px) for px in grey) > 60, min(min(px) for px in grey)
        assert max(max(px) for px in empty) <= 2, max(max(px) for px in empty)
        for name in files:
            send(c, "RemoveInput", {"inputName": name})
        send(c, "RemoveInput", {"inputName": "Shine camera"})
    finally:
        c.disconnect()


def test_set_up_characters(run):
    """A 3D character with its set-up (DireBot's "model_setup", as from an
    imported Lens Studio project): your real eyes and mouth drawn on its
    face where it says, only inside its outline; its ear swinging down,
    pulled as it says; its body staying upright while its head turns with
    you."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    model = os.path.join(run.home, "set-up.glb")
    set_up_character(model)
    # Outfit pictures: a cyan hat in front, a magenta cape behind.
    hat, cape = os.path.join(run.home, "hat.png"), os.path.join(run.home, "cape.png")
    write_png(hat, 40, 20, [b"\x00\xff\xff\xff" * 40] * 20, alpha=True)
    write_png(cape, 40, 40, [b"\xff\x00\xff\xff" * 40] * 40, alpha=True)
    size = 300
    still = json.dumps({"spots": SET_UP_SPOTS, "chains": []})
    swinging = json.dumps({"spots": SET_UP_SPOTS,
                           "chains": [{"bones": ["Ear0", "Ear1", "Ear2"],
                                       "pull": [0, -9.8, 0], "with_head": False}],
                           "upright": [{"bone": "Body", "follow": 6}]})
    # Held still (0.5.0's breathing, sway and eye moves off): two sources
    # breathe in their own time, and these compare them pixel by pixel.
    common = {"source": "Set-up camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 100, "model": model, **STILL}
    avatars = {
        "Real eyes": {"model_setup": still},
        "Drawn": {"model_setup": still, "style": "drawn"},
        "Swinging": {"model_setup": swinging, "style": "drawn"},
        "Broken set-up": {"model_setup": "{not json"},
        "Outfit": {"model_setup": still, "style": "drawn", "outfit": json.dumps([
            {"file": hat, "place": [0.3, 0.0, 0.4, 0.12]},
            {"file": cape, "place": [0, 0, 1, 1], "behind": True}])},
    }
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Set-up characters"})
        send(c, "CreateInput", {"sceneName": "Set-up characters", "inputName": "Set-up camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        for name, extra in avatars.items():
            send(c, "CreateInput", {"sceneName": "Set-up characters", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Set-up characters"})

        def st(name):
            return vendor(c, "FaceGetState", {"source": name})
        for name in avatars:
            assert wait_for(lambda: st(name)["found"] and st(name)["model"]["shown"], 30, 0.2), st(name)
        assert wait_for(lambda: st("Real eyes")["model"]["cards"] == 3, 10, 0.1), st("Real eyes")
        m = st("Real eyes")["model"]
        assert m["set_up"] and m["springs"] == 0 and m["upright_parts"] == 0
        assert {k: v["from"] for k, v in m["spots"].items()} == {
            "left_eye": "set up", "right_eye": "set up", "mouth": "set up"}
        # Its own left eye is on the picture's right, facing you.
        assert m["spots"]["left_eye"]["x"] > m["spots"]["right_eye"]["x"]
        assert st("Drawn")["model"]["cards"] == 0
        s = st("Swinging")["model"]
        assert s["set_up"] and s["springs"] == 1 and s["upright_parts"] == 1, s
        # Set-up text it can't read: as if there were none.
        b = st("Broken set-up")["model"]
        assert not b["set_up"] and "problem" not in b
        assert {v["from"] for v in b["spots"].values()} == {"guessed"}

        # Your eyes and mouth: on its face where its set-up says (two up
        # top, one under them), nowhere else - and never outside it. Each
        # source tracks you itself and settles in its own time (the
        # steadiest smoothing), so they're compared once both have
        # settled on the same turn and place - its ear and body edges
        # otherwise differ by a pixel or two.
        def same_pose():
            a, b = st("Real eyes")["model"], st("Drawn")["model"]
            return all(abs(a[k] - b[k]) <= 0.2 for k in ("yaw", "pitch", "x", "y"))
        time.sleep(1.5)
        assert wait_for(same_pose, 15, 0.2), (st("Real eyes")["model"], st("Drawn")["model"])
        real, drawn = screenshot(c, "Real eyes"), screenshot(c, "Drawn")
        orange = lambda px: px[0] > 120 and px[0] > px[1] + 30 > px[2] + 30  # noqa: E731
        ox, oy, n = middle_of(drawn, orange)
        assert n > 2000, n
        xs = [x for y in range(size) for x in range(size) if orange(drawn[y][x])]
        ys = [y for y in range(size) for x in range(size) if orange(drawn[y][x])]
        top, bottom, left, right = min(ys), max(ys), min(xs), max(xs)
        changed = [(x, y) for y in range(size) for x in range(size)
                   if sum(abs(a - b) for a, b in zip(real[y][x], drawn[y][x])) > 60]
        assert len(changed) > 150, len(changed)
        assert all(drawn[y][x] != (0, 0, 0) for x, y in changed)
        # (Bar a stray pixel or two on an edge: each tracks you itself.)
        outside = [(x, y) for x, y in changed
                   if not (left - 2 <= x <= right + 2 and top - 2 <= y <= bottom + 2)]
        assert len(outside) <= len(changed) // 100, outside
        upper = [(x, y) for x, y in changed if y < oy]
        lower = [(x, y) for x, y in changed if y > oy + (bottom - oy) * 0.15]
        assert sum(1 for x, _ in upper if x < ox - 5) > 30 and sum(1 for x, _ in upper if x > ox + 5) > 30
        assert len(lower) > 30 and abs(sum(x for x, _ in lower) / len(lower) - ox) < 15

        # Outfit pictures ride on it: the hat over its top, the cape
        # behind it (round it, never over it).
        assert wait_for(lambda: st("Outfit")["model"]["outfit_layers"] == 2, 10, 0.1), st("Outfit")
        dressed = screenshot(c, "Outfit")
        cyan = lambda px: px[0] < 60 and px[1] > 180 and px[2] > 180  # noqa: E731
        magenta = lambda px: px[0] > 180 and px[1] < 60 and px[2] > 180  # noqa: E731
        # (Shares of its front picture: the whole of it, ear and all.)
        cape_xs = [x for y in range(size) for x in range(size) if magenta(dressed[y][x])]
        cape_ys = [y for y in range(size) for x in range(size) if magenta(dressed[y][x])]
        assert len(cape_xs) > 500
        cape_mid = (min(cape_xs) + max(cape_xs)) / 2
        hx, hy, n = middle_of(dressed, cyan)
        assert n > 100 and abs(hx - cape_mid) < 8 and hy < min(cape_ys) + 30, (hx, hy, n, cape_mid)
        assert not magenta(dressed[int(oy)][int(ox)]) and orange(dressed[int(oy)][int(ox)])

        # Straight from the front, for DireBot's page: its front box fills
        # the picture, and where its eyes and mouth are on it.
        front = vendor(c, "FaceModelFront", {"source": "Real eyes", "width": 200})
        assert front["ok"] and front["width"] == 200, front.get("error")
        box = front["box"]
        assert abs(front["height"] - round(200 * box["height"] / box["width"])) <= 1
        rgba = base64.b64decode(front["rgba"])
        fw, fh = front["width"], front["height"]
        assert len(rgba) == fw * fh * 4
        px = lambda x, y: tuple(rgba[(y * fw + x) * 4:(y * fw + x) * 4 + 4])  # noqa: E731
        assert px(1, 1)[3] == 0 and px(fw - 2, fh - 2)[3] == 0
        mid = px(int(0.32 * fw), int(0.33 * fh))   # on the ball, off its face
        assert mid[3] == 255 and mid[0] > 120 and mid[0] > mid[1] > mid[2], mid
        fs = front["spots"]
        assert {v["from"] for v in fs.values()} == {"set up"}
        assert fs["left_eye"]["x"] > fs["right_eye"]["x"] and fs["mouth"]["y"] > fs["left_eye"]["y"]
        assert all(0 < v["x"] < 1 and 0 < v["y"] < 1 for v in fs.values())
        assert front["region"] == {"x": 0, "y": 0, "w": 1, "h": 1}
        # Up close: the part round its face (still inside the box), the
        # spots still shares of the whole box.
        close = vendor(c, "FaceModelFront", {"source": "Real eyes", "width": 200, "close": True})
        assert close["ok"] and close["width"] == 200 and close["spots"] == fs and close["box"] == box
        r = close["region"]
        assert 0 <= r["x"] and 0 <= r["y"] and r["x"] + r["w"] <= 1.0001 and r["y"] + r["h"] <= 1.0001
        assert abs(close["height"] - round(200 * r["h"] * box["height"] / (r["w"] * box["width"]))) <= 1
        assert len(base64.b64decode(close["rgba"])) == close["width"] * close["height"] * 4
        no = vendor(c, "FaceModelFront", {"source": "Set-up camera"})
        assert not no["ok"]

        # Its ear: sticking out, still - or hanging down, pulled.
        green = lambda px: px[1] > 90 and px[1] > px[0] + 40 and px[1] > px[2] + 40  # noqa: E731
        time.sleep(3)
        rows = screenshot(c, "Swinging")
        _, still_y, n1 = middle_of(drawn, green)
        _, hang_y, n2 = middle_of(rows, green)
        print(f"ear: {still_y:.1f} still, {hang_y:.1f} hanging")
        assert n1 > 50 and n2 > 50 and hang_y > still_y + 30, (still_y, hang_y)
        red = lambda px: px[0] > 120 and px[1] < 70 and px[2] < 70  # noqa: E731
        drawn_badge, _, n1 = middle_of(drawn, red)
        upright_badge, _, n2 = middle_of(rows, red)
        assert n1 > 20 and n2 > 20 and abs(drawn_badge - upright_badge) < 3

        # Turned 25 degrees: both turn with you, but the upright body
        # stays as it was made - its badge where it was.
        for name in ("Drawn", "Swinging"):
            assert vendor(c, "FaceSet", {"source": name})["ok"]
            assert wait_for(lambda: st(name)["set"] and not st(name)["setting"], 10, 0.1)
            rest = send(c, "GetInputSettings", {"inputName": name})["inputSettings"]["rest"]
            send(c, "SetInputSettings", {"inputName": name, "inputSettings": {
                "rest": turned_rest(rest, 25)}})
        for name in ("Drawn", "Swinging"):
            assert wait_for(lambda: st(name)["model"]["yaw"] < -20, 5, 0.1), st(name)["model"]
        time.sleep(2)
        turned_badge, _, n1 = middle_of(screenshot(c, "Drawn"), red)
        kept_badge, _, n2 = middle_of(screenshot(c, "Swinging"), red)
        print(f"badge: {drawn_badge:.1f} straight, {turned_badge:.1f} turned, {kept_badge:.1f} upright")
        assert n1 > 10 and n2 > 10
        # (Moved a little, maybe, as your head tips a little too.)
        assert abs(turned_badge - drawn_badge) > 12 and abs(kept_badge - upright_badge) < 6
        for name in list(avatars) + ["Set-up camera"]:
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


def test_its_own_animation(run):
    """A character's own animation (glTF's, saved in its file): listed,
    nothing playing until one is chosen, then played over and over - its
    red badge sliding side to side across its body; None (or one it
    hasn't got) stops it, at rest."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    model = os.path.join(run.home, "animated.glb")
    slide = [(3, "translation", [0, 0.5, 1], [0.04, 0, 0.045, -0.04, 0, 0.045, 0.04, 0, 0.045])]
    set_up_character(model, animations={"Slide": slide, "": []})
    size = 300
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Own animation"})
        send(c, "CreateInput", {"sceneName": "Own animation", "inputName": "Animated camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Own animation", "inputName": "Animated",
                                "inputKind": "direbot_face_avatar", "inputSettings": {
                                    "source": "Animated camera", "width": size, "height": size,
                                    "size": 100, "mirror": False, "smoothing": 100,
                                    "follow_move": 0, "follow_grow": 0, "follow_tilt": 0,
                                    "bend": 100, "model": model, "style": "drawn", **STILL,
                                    "model_setup": json.dumps({"spots": SET_UP_SPOTS, "chains": []})}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Own animation"})

        def st():
            return vendor(c, "FaceGetState", {"source": "Animated"})["model"]
        assert wait_for(lambda: st()["shown"], 30, 0.2), st()
        assert st()["animations"] == [{"name": "Slide"}, {"name": "Animation 2"}], st()
        assert st()["animation"] == ""
        red = lambda px: px[0] > 120 and px[1] < 70 and px[2] < 70  # noqa: E731

        def badge_xs(shots, gap):
            xs = []
            for _ in range(shots):
                x, _, n = middle_of(screenshot(c, "Animated"), red)
                assert n > 20, n
                xs.append(x)
                time.sleep(gap)
            return xs
        still = badge_xs(5, 0.15)
        assert max(still) - min(still) < 1.5, still
        send(c, "SetInputSettings", {"inputName": "Animated", "inputSettings": {"animation": "Slide"}})
        assert wait_for(lambda: st()["animation"] == "Slide", 5, 0.1), st()
        moving = badge_xs(12, 0.09)
        print(f"own animation: badge x {min(moving):.1f} to {max(moving):.1f} (still at {still[0]:.1f})")
        assert max(moving) - min(moving) > 15, moving
        for off in ("", "Nope"):
            send(c, "SetInputSettings", {"inputName": "Animated", "inputSettings": {"animation": off}})
            assert wait_for(lambda: st()["animation"] == "", 5, 0.1), st()
            time.sleep(0.2)
            again = badge_xs(4, 0.15)
            assert max(again) - min(again) < 1.5 and abs(again[0] - still[0]) < 1.5, (again, still)
        # OBS's own settings list them, None first.
        props = send(c, "GetInputPropertiesListPropertyItems",
                     {"inputName": "Animated", "propertyName": "animation"})["propertyItems"]
        assert [p["itemValue"] for p in props] == ["", "Slide", "Animation 2"], props
        send(c, "RemoveInput", {"inputName": "Animated"})
    finally:
        c.disconnect()


def test_alive_when_still_tuning_and_camera_check(run):
    """0.5.0: it breathes while you hold still (the picture rises and falls
    a breath at a time; not with breathing off), the tuning's live readings
    and the camera check's are in FaceGetState, and when it can't see you
    it eases into its idle - and is back with you as soon as it sees you."""
    face_png = os.path.join(run.home, "camera.png")
    body_png = os.path.join(run.home, "character.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    if not os.path.exists(body_png):
        character_picture(body_png)
    size = 400
    common = {"source": "Alive camera", "character": body_png, "width": size,
              "height": size, "size": 100, "mirror": False, "smoothing": 100,
              "spots": json.dumps(SPOTS), "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 0, "sway": 0, "eye_moves": False}
    avatars = {"Breathing": {"breathing": 100, "breath_time": 2},
               "Not breathing": {"breathing": 0}}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Alive camera"})
        send(c, "CreateInput", {"sceneName": "Alive camera", "inputName": "Alive photo",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        photo = send(c, "GetSceneItemId", {"sceneName": "Alive camera",
                                           "sourceName": "Alive photo"})["sceneItemId"]
        send(c, "CreateScene", {"sceneName": "Alive"})
        for name, extra in avatars.items():
            send(c, "CreateInput", {"sceneName": "Alive", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Alive"})

        def st(name="Breathing"):
            return vendor(c, "FaceGetState", {"source": name})
        for name in avatars:
            assert wait_for(lambda: st(name)["found"] and st(name)["shown"] == 1, 30, 0.2), st(name)

        # A breath every 2 seconds: the picture rises and falls (up to 0.8%
        # of its height), the other holds still.
        orange = lambda px: px[0] > 150 and 90 < px[1] < 170 and px[2] < 90  # noqa: E731
        ys = {name: [] for name in avatars}
        breaths = []
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 2.6:
            for name in avatars:
                _, y, n = middle_of(screenshot(c, name), orange)
                assert n > 1000, (name, n)
                ys[name].append(y)
            breaths.append(st()["alive"]["breath"])
        print("\nbreathing: middle %.1f..%.1f px, still: %.1f..%.1f px, breath %s" % (
            min(ys["Breathing"]), max(ys["Breathing"]), min(ys["Not breathing"]),
            max(ys["Not breathing"]), breaths))
        assert max(ys["Breathing"]) - min(ys["Breathing"]) > 1.2, ys
        assert max(ys["Not breathing"]) - min(ys["Not breathing"]) < 0.3, ys
        assert min(breaths) < 0.3 and max(breaths) > 0.7, breaths
        assert all(0 <= b <= 1 for b in breaths), breaths
        assert st("Not breathing")["alive"]["breath"] == 0

        # The tuning's readings, live, for each group.
        s = st()
        assert s["alive"]["idle"] == 0, s["alive"]
        groups = {"blink", "mouth", "smile", "round", "brows_up", "brows_down", "look"}
        for part in ("reading", "reading_peak", "shows", "shows_peak"):
            assert set(s["tune"][part]) == groups, s["tune"]
            assert all(0 <= v <= 1 for v in s["tune"][part].values()), s["tune"]
        for g in groups:
            assert s["tune"]["reading_peak"][g] >= s["tune"]["reading"][g] - 0.01, s["tune"]

        # The camera check: the photo's face, where it is, lit by a grey room.
        check = s["camera_check"]
        assert check["seen"] is True, check
        box = check["box"]
        assert 0 < box["x"] < box["x"] + box["w"] <= 1 and 0 < box["y"] < box["y"] + box["h"] <= 1, box
        assert 0.05 < box["w"] < 0.6 and 0.05 < box["h"] < 0.9, box
        assert abs(check["facing"]["yaw"]) < 25 and abs(check["facing"]["roll"]) < 25, check
        light = check["light"]
        assert all(0 <= light[k] <= 1 for k in ("face", "left", "right", "room")), light
        # Its "camera" is a scene as big as OBS's canvas: the grey room is
        # the photo's part of it, the rest black.
        video = send(c, "GetVideoSettings")
        cw, ch = video["baseWidth"], video["baseHeight"]
        assert (check["camera_width"], check["camera_height"]) == (cw, ch), (check, video)
        face_area = box["w"] * cw * box["h"] * ch
        room = 96 / 255 * (W * H - face_area) / (cw * ch - face_area)
        assert 0.15 < light["face"] and abs(light["room"] - room) < 0.04, (light, room)
        # A photo sends no new pictures (0.9.0: the camera's own count).
        assert check["camera_fps"] < 1, check

        # Gone: it holds your look a moment, then eases into its idle.
        send(c, "SetSceneItemEnabled", {"sceneName": "Alive camera", "sceneItemId": photo,
                                        "sceneItemEnabled": False})
        assert wait_for(lambda: not st()["found"], 5, 0.05), st()
        lost = time.perf_counter()
        assert wait_for(lambda: st()["alive"]["idle"] > 0, 5, 0.05), st()["alive"]
        began = time.perf_counter() - lost
        assert wait_for(lambda: st()["alive"]["idle"] == 1, 6, 0.05), st()["alive"]
        print("idle: begins %.1f s after it's lost, all the way at %.1f s" % (
            began, time.perf_counter() - lost))
        assert 0.6 < began < 2.0, began
        gone = st()["camera_check"]
        assert gone["seen"] is False and "box" not in gone, gone
        # Back: with you again at once.
        send(c, "SetSceneItemEnabled", {"sceneName": "Alive camera", "sceneItemId": photo,
                                        "sceneItemEnabled": True})
        assert wait_for(lambda: st()["found"], 5, 0.05), st()
        assert wait_for(lambda: st()["alive"]["idle"] == 0, 2, 0.05), st()["alive"]
        send(c, "RemoveScene", {"sceneName": "Alive"})
        send(c, "RemoveScene", {"sceneName": "Alive camera"})
    finally:
        c.disconnect()


def vowel_wav(path, formants, f0=120, seconds=2, rate=48000, level_db=-24):
    """A made-up vowel, the way tests/unit_voice.cpp makes one: a buzz at
    `f0` softened as a throat softens it, through resonances at its
    formants, out of the lips - `level_db` loud (RMS, dBFS)."""
    n = int(rate * seconds)
    x = [0.0] * n
    phase = 0.0
    for i in range(n):
        phase += f0 * (1 + 0.01 * math.sin(2 * math.pi * 5 * i / rate)) / rate
        if phase >= 1:
            phase -= 1
            x[i] = 1.0
    a = math.exp(-2 * math.pi * 150 / rate)
    y1 = y2 = 0.0
    for i in range(n):
        y1 = (1 - a) * x[i] + a * y1
        y2 = (1 - a) * y1 + a * y2
        x[i] = y2
    fs = list(formants) + [2600, 3500, 4500][len(formants) - 2:]
    for f, bw in zip(fs[:5], (70, 100, 130, 170, 220)):
        t = 1 / rate
        c = -math.exp(-2 * math.pi * bw * t)
        b = 2 * math.exp(-math.pi * bw * t) * math.cos(2 * math.pi * f * t)
        g = 1 - b - c
        p1 = p2 = 0.0
        for i in range(n):
            y = g * x[i] + b * p1 + c * p2
            p2, p1 = p1, y
            x[i] = y
    prev = 0.0
    for i in range(n):
        x[i], prev = x[i] - prev, x[i]
    rms = math.sqrt(sum(v * v for v in x) / n) or 1
    k = 10 ** (level_db / 20) / rms
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(v * k * 32767))))
                               for v in x))


def detail_ball(path, size=64):
    """An orange ball 20 cm across with a skin's material ("Skin"): its
    own picture coordinates, a normal map of bands across it (each 8
    pixels, its bumps tilted up then down) and an occlusion map half
    dark - embedded in the .glb, as a character's are."""
    pos, nrm, ix = ball_points(0, 0, 0, 0.1, 0.1, 0.1)
    n = 24
    uv = [(j / n, i / n) for i in range(n + 1) for j in range(n + 1)]
    up, down = bytes((128, 204, 230, 255)), bytes((128, 51, 230, 255))
    bumps = [(up if (y // 8) % 2 == 0 else down) * size for y in range(size)]
    shade = [bytes((90, 90, 90, 255)) * size for _ in range(size)]
    pictures = []
    for name, rows in (("bumps.png", bumps), ("shade.png", shade)):
        p = path + "." + name
        write_png(p, size, size, rows, alpha=True)
        with open(p, "rb") as f:
            pictures.append(f.read())
        os.remove(p)
    blob = bytearray()
    views = []

    def add(data, target=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        if target:
            views[-1]["target"] = target
        blob.extend(data)
        return len(views) - 1

    accessors = [
        {"bufferView": add(struct.pack("<%df" % (len(pos) * 3), *[c for v in pos for c in v]), 34962),
         "componentType": 5126, "count": len(pos), "type": "VEC3",
         "min": [min(v[k] for v in pos) for k in range(3)],
         "max": [max(v[k] for v in pos) for k in range(3)]},
        {"bufferView": add(struct.pack("<%df" % (len(nrm) * 3), *[c for v in nrm for c in v]), 34962),
         "componentType": 5126, "count": len(nrm), "type": "VEC3"},
        {"bufferView": add(struct.pack("<%df" % (len(uv) * 2), *[c for v in uv for c in v]), 34962),
         "componentType": 5126, "count": len(uv), "type": "VEC2"},
        {"bufferView": add(struct.pack("<%dI" % len(ix), *ix), 34963),
         "componentType": 5125, "count": len(ix), "type": "SCALAR"},
    ]
    images = [{"bufferView": add(data), "mimeType": "image/png"} for data in pictures]
    doc = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
           "nodes": [{"name": "Head", "mesh": 0}],
           "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                       "indices": 3, "material": 0}]}],
           "materials": [{"name": "Skin",
                          "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.25, 0.04, 1],
                                                   "metallicFactor": 0, "roughnessFactor": 0.6},
                          "normalTexture": {"index": 0},
                          "occlusionTexture": {"index": 1}}],
           "textures": [{"source": 0}, {"source": 1}], "images": images,
           "accessors": accessors, "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    text = json.dumps(doc).encode()
    text += b" " * (-len(text) % 4)
    while len(blob) % 4:
        blob.append(0)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def test_lights_detail_maps_soft_skin_views_and_mic(run):
    """0.6.0: the lights you pick (a ready-made set, or your own) light it
    differently; a character's normal and occlusion maps are drawn; soft
    skin on its skin softens the shadow side; a saved camera view frames
    it closer and glides back; and its mouth moves from a mic in OBS (a
    vowel played into it), with "say each vowel" starting, stopping and
    saying in words when there's no mic."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    ball = os.path.join(run.home, "detail.glb")
    detail_ball(ball)
    ah = os.path.join(run.home, "ah.wav")
    vowel_wav(ah, (768, 1333, 2522))      # a man's "a" (Hillenbrand et al.)
    size = 300
    common = {"source": "Lights camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 100, "model": ball, "mouth_from": "camera", **STILL}
    close_up = json.dumps([{"name": "Close-up", "zoom": 2.0},
                           {"name": "Side", "yaw": 35}])
    avatars = {
        "Plain": {"detail_maps": False},
        "Bumped": {},
        "Night": {"detail_maps": False, "light_preset": "night"},
        "Neon": {"detail_maps": False, "light_preset": "neon"},
        "Own lights": {"detail_maps": False, "light_preset": "own", "light_side": 60,
                       "light_height": 0},
        "Soft": {"detail_maps": False, "soft_skin": 100},
        "Viewed": {"detail_maps": False, "views": close_up, "view": "1", "view_glide": 300},
        "Mic mouth": {"detail_maps": False, "mouth_from": "mic", "mic": "Test mic"},
        "No mic": {"detail_maps": False, "mouth_from": "mic", "mic": "Nothing called this"},
    }
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Lights"})
        send(c, "CreateInput", {"sceneName": "Lights", "inputName": "Lights camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Lights", "inputName": "Test mic",
                                "inputKind": "ffmpeg_source",
                                "inputSettings": {"local_file": ah, "is_local_file": True,
                                                  "looping": True}})
        for name, extra in avatars.items():
            send(c, "CreateInput", {"sceneName": "Lights", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Lights"})

        def st(name):
            return vendor(c, "FaceGetState", {"source": name})
        for name in avatars:
            assert wait_for(lambda: st(name)["found"] and st(name)["model"]["shown"], 30, 0.2), st(name)
        m = st("Bumped")["model"]
        # (0.7.0: its file has no tangents, so they're worked out.)
        assert m["materials"] == [{"name": "Skin", "bumps": True, "shadows": True, "skin": True,
                                   "own_tangents": False}], m
        assert m["bump_parts"] == 1 and m["shadow_parts"] == 1 and m["skin_parts"] == 1
        assert m["guessed_tangent_parts"] == 1
        assert st("Plain")["lights"]["preset"] == "studio" and not st("Plain")["lights"]["detail"]
        night = st("Night")["lights"]
        assert night["preset"] == "night" and night["colour"] == "#7fa0ff" and night["rim"] == 70
        own = st("Own lights")["lights"]
        assert own["preset"] == "own" and own["side"] == 60 and own["height"] == 0
        assert st("Soft")["lights"]["soft_skin"] == 100
        time.sleep(1.0)
        orange = lambda px: px[0] > 60 and px[0] > px[2] + 20  # noqa: E731

        def ball_pixels(rows):
            return [(x, y) for y in range(size) for x in range(size) if rows[y][x] != (0, 0, 0)]

        def mean(rows, spots):
            return [sum(rows[y][x][k] for x, y in spots) / max(1, len(spots)) for k in range(3)]
        plain = screenshot(c, "Plain")
        spots = ball_pixels(plain)
        assert len(spots) > 10000, len(spots)
        pm = mean(plain, spots)
        assert pm[0] > pm[1] > pm[2], pm                  # orange, as ever

        # Its normal map: bands of light and shade across it, top to
        # bottom (each band's bumps face up, then down); its occlusion
        # map: the room's light darker.
        # (Down its left side, clear of the eyes and mouth drawn on it.)
        bumped = screenshot(c, "Bumped")
        bx = size // 2 - 80
        column = [sum(bumped[y][bx]) - sum(plain[y][bx]) for y in range(size)
                  if plain[y][bx] != (0, 0, 0)]
        middle = sorted(column)[len(column) // 2]
        bands = [d > middle for d in column]
        turns = sum(1 for a, b in zip(bands, bands[1:]) if a != b)
        print("bumped minus plain, down the ball:", column[::6], turns)
        assert turns >= 5 and max(column) - min(column) > 100, column
        assert sum(mean(bumped, spots)) < sum(pm) - 5

        # Night: darker and bluer; neon: pinker and bluer; your own (the
        # light level, on the picture's right): the right side lit.
        nm = mean(screenshot(c, "Night"), spots)
        assert sum(nm) < sum(pm) - 60 and nm[2] / max(1, nm[0]) > pm[2] / pm[0] + 0.3, (nm, pm)
        neon = mean(screenshot(c, "Neon"), spots)
        assert neon[1] / max(1, neon[0]) < pm[1] / pm[0] - 0.1, (neon, pm)
        assert neon[2] / max(1, neon[0]) > pm[2] / pm[0] + 0.15, (neon, pm)
        own_rows = screenshot(c, "Own lights")
        left = [(x, y) for x, y in spots if x < size // 2 - 40]
        right = [(x, y) for x, y in spots if x > size // 2 + 40]
        assert sum(mean(own_rows, right)) > sum(mean(own_rows, left)) + 60
        assert sum(mean(plain, left)) > sum(mean(plain, right))      # the studio's: from the left

        # Soft skin: light reaching round into the shadow side, warmer.
        soft = screenshot(c, "Soft")
        shadow = [(x, y) for x, y in spots if x > size // 2 + 30 and y > size // 2 + 30]
        sm, ps = mean(soft, shadow), mean(plain, shadow)
        assert sm[0] > ps[0] + 3, (sm, ps)

        # A saved view: closer (the ball bigger), then back to the main
        # view, gliding.
        cv = st("Viewed")["camera_view"]
        assert cv["view"] == "1" and [v["name"] for v in cv["views"]] == ["Close-up", "Side"]
        assert wait_for(lambda: not st("Viewed")["camera_view"]["gliding"], 5, 0.1)
        near = len(ball_pixels(screenshot(c, "Viewed")))
        assert near > size * size * 0.97, (near, len(spots))    # twice as close: it fills it
        send(c, "SetInputSettings", {"inputName": "Viewed", "inputSettings": {"view": "main"}})
        assert wait_for(lambda: st("Viewed")["camera_view"]["gliding"], 2, 0.02)
        assert wait_for(lambda: not st("Viewed")["camera_view"]["gliding"], 5, 0.1)
        assert st("Viewed")["camera_view"]["showing"] == "main"
        back = len(ball_pixels(screenshot(c, "Viewed")))
        assert abs(back - len(spots)) < len(spots) * 0.05, (back, len(spots))

        # The mouth from the mic: it hears the "a" and opens.
        def mic(name):
            return st(name)["mic"]
        assert wait_for(lambda: mic("Mic mouth")["hearing"] and mic("Mic mouth")["voiced"], 10, 0.1), \
            mic("Mic mouth")
        heard = mic("Mic mouth")
        print("the mic's mouth:", heard)
        assert heard["found"] and heard["listening_to"] == "Test mic" and heard["from"] == "mic"
        assert -40 < heard["level"] < -10, heard
        assert wait_for(lambda: mic("Mic mouth")["share"] > 0.9, 5, 0.1), mic("Mic mouth")
        assert max(heard["vowels"], key=heard["vowels"].get) == "a", heard["vowels"]
        assert heard["open"] > 0.3
        assert st("Plain")["mic"]["share"] == 0          # its mouth from the camera
        assert vendor(c, "FaceMicCalibrate", {"source": "Mic mouth", "do": "start"})["ok"]
        assert wait_for(lambda: mic("Mic mouth")["calibration"]["running"], 3, 0.05)
        assert vendor(c, "FaceMicCalibrate", {"source": "Mic mouth", "do": "stop"})["ok"]
        assert wait_for(lambda: not mic("Mic mouth")["calibration"]["running"], 3, 0.05)
        none = vendor(c, "FaceMicCalibrate", {"source": "No mic", "do": "start"})
        assert not none["ok"] and "mic" in none["error"], none
        assert not mic("No mic")["found"]
        for name in list(avatars) + ["Test mic", "Lights camera"]:
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


def disc_picture(path, size, colour):
    """A see-through picture with one round blob of `colour` in it."""
    rows = []
    for y in range(size):
        row = bytearray(size * 4)
        for x in range(size):
            if (x - size / 2 + 0.5) ** 2 + (y - size / 2 + 0.5) ** 2 < (size * 0.45) ** 2:
                row[x * 4:x * 4 + 4] = bytes(colour) + b"\xff"
        rows.append(row)
    write_png(path, size, size, rows, alpha=True)


def hand_picture(path, w=48, h=64, colour=(240, 190, 150)):
    """A made-up patting hand: a skin-coloured block, see-through round it."""
    rows = []
    for y in range(h):
        row = bytearray(w * 4)
        for x in range(w):
            if 4 <= x < w - 4 and 4 <= y < h - 2:
                row[x * 4:x * 4 + 4] = bytes(colour) + b"\xff"
        rows.append(row)
    write_png(path, w, h, rows, alpha=True)


def vrma_file(path, seconds=2.0):
    """A VRM animation file (.vrma): its hips turning a little and back."""
    s, co = math.sin(math.radians(10)), math.cos(math.radians(10))
    blob = struct.pack("<3f", 0.0, seconds / 2, seconds) + \
        struct.pack("<12f", 0, 0, 0, 1, 0, s, 0, co, 0, 0, 0, 1)
    doc = {"asset": {"version": "2.0"}, "extensionsUsed": ["VRMC_vrm_animation"],
           "extensions": {"VRMC_vrm_animation": {
               "specVersion": "1.0", "humanoid": {"humanBones": {"hips": {"node": 0}}}}},
           "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": "Hips"}],
           "buffers": [{"byteLength": len(blob)}],
           "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 12},
                           {"buffer": 0, "byteOffset": 12, "byteLength": 48}],
           "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3,
                          "type": "SCALAR", "min": [0], "max": [seconds]},
                         {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC4"}],
           "animations": [{"name": "Dance", "samplers": [{"input": 0, "output": 1}],
                           "channels": [{"sampler": 0, "target": {"node": 0, "path": "rotation"}}]}]}
    text = json.dumps(doc).encode()
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + blob)


def quick_shots(c, name, seconds):
    """Screenshots of a source, one after another for `seconds`: (seconds
    since the first, rows) each - read after, so they come quickly."""
    raw, t0 = [], time.time()
    while time.time() - t0 < seconds:
        data = send(c, "GetSourceScreenshot", {"sourceName": name, "imageFormat": "bmp"})["imageData"]
        raw.append((time.time() - t0, data))
    return [(t, read_bmp(base64.b64decode(d.split(",", 1)[1]))) for t, d in raw]


def keep_shot(rows, name):
    keep = os.environ.get("DIREBOT_FACE_SHOTS")
    if keep:
        write_png(os.path.join(keep, name + ".png"), len(rows[0]), len(rows),
                  [bytes(v for px in row for v in px) for row in rows])


TOMATO = (230, 20, 20)
FLASH = (255, 240, 40)


def test_reactions_on_cue(run):
    """0.7.0, DireBot's FaceReact: something thrown flies in from the side
    it's thrown from and hits its face (counted, a flash where it hits, its
    head knocked back and settling, then it falls away); a volley lands one
    after another, too many at once is said in words, and stop ends it all;
    a headpat's hand comes down on top of its head; a bounce moves it; a
    face shows only on a 3D character that has it; a VRM animation file's
    move only on a VRM; its own animation plays by name, any case."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    body_png = os.path.join(run.home, "react-character.png")
    character_picture(body_png)
    tomato = os.path.join(run.home, "react-tomato.png")
    disc_picture(tomato, 64, TOMATO)
    flash = os.path.join(run.home, "react-flash.png")
    disc_picture(flash, 64, FLASH)
    hand = os.path.join(run.home, "react-hand.png")
    hand_picture(hand)
    dance = os.path.join(run.home, "dance.vrma")
    vrma_file(dance, 2.0)
    model = os.path.join(run.home, "react-animated.glb")
    slide = [(3, "translation", [0, 0.5, 1], [0.04, 0, 0.045, -0.04, 0, 0.045, 0.04, 0, 0.045])]
    set_up_character(model, animations={"Slide": slide})
    size = 300
    react = {"react_items": json.dumps([{"file": tomato},
                                        {"file": os.path.join(run.home, "not-there.png")}]),
             "react_hand": hand, "react_hit": flash,
             "react_moves": json.dumps([{"name": "Dance", "file": dance}])}
    common = {"source": "React camera", "width": size, "height": size, "size": 100,
              "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
              "follow_tilt": 0, "bend": 0, **STILL, **react}
    avatars = {
        "React picture": {"character": body_png, "style": "drawn", "spots": json.dumps(SPOTS)},
        "React 3D": {"model": model, "style": "drawn", "bend": 100,
                     "model_setup": json.dumps({"spots": SET_UP_SPOTS, "chains": []})},
    }
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st(name):
        return vendor(c, "FaceGetState", {"source": name})

    def cue(name, **data):
        return vendor(c, "FaceReact", {"source": name, **data})
    red = lambda px: px[0] > 170 and px[1] < 60 and px[2] < 60  # noqa: E731
    yellow = lambda px: px[0] > 200 and px[1] > 200 and px[2] < 110  # noqa: E731
    skin = lambda px: px[0] > 200 and 160 < px[1] < 220 and 110 < px[2] < 190  # noqa: E731
    orange = lambda px: px[0] > 170 and 90 < px[1] < 160 and px[2] < 90  # noqa: E731
    try:
        send(c, "CreateScene", {"sceneName": "Reactions"})
        send(c, "CreateInput", {"sceneName": "Reactions", "inputName": "React camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        for name, extra in avatars.items():
            send(c, "CreateInput", {"sceneName": "Reactions", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Reactions"})
        assert wait_for(lambda: st("React picture")["found"] and st("React picture")["shown"] == 1,
                        30, 0.2), st("React picture")
        assert wait_for(lambda: st("React 3D")["found"] and st("React 3D")["model"]["shown"], 30, 0.2), \
            st("React 3D")
        # What it has to do it with.
        r = st("React picture")["react"]
        assert r["items"] == [{"loaded": True}, {"loaded": False}], r
        assert r["hand"] and r["hit"] and r["hits"] == 0 and r["flying"] == 0 and not r["busy"], r
        assert r["faces"] == [] and not r["arms"] and not r["vrm_moves"], r
        assert r["moves"] == [{"name": "Dance", "length": 2.0}], r
        r3 = st("React 3D")["react"]
        assert not r3["arms"] and not r3["vrm_moves"] and r3["faces"] == [], r3

        # Asked for what it can't do: said in words.
        bad = cue("React picture", do="throw", item=1)
        assert not bad["ok"] and "no picture" in bad["error"], bad
        assert not cue("React picture", do="throw", item=7)["ok"]
        assert "doesn't know" in cue("React picture", do="juggle")["error"]
        nope = cue("React 3D", do="move", move="Moonwalk")
        assert not nope["ok"] and "Moonwalk" in nope["error"], nope
        gone = vendor(c, "FaceReact", {"source": "Nobody", "do": "stop"})
        assert not gone["ok"] and "isn't in OBS" in gone["error"], gone

        # Faces: on a 3D character that has it, only.
        f = cue("React picture", do="face", face="happy", seconds=1)
        assert f["ok"] and f["shows"] is False and "3D" in f["why"], f
        f = cue("React 3D", do="face", face="Happy", seconds=1)
        assert f["ok"] and f["shows"] is False and "no face called that" in f["why"], f
        # Moves: a nod for any character, a wave only with a VRM's arms, a
        # VRM animation file's only on a VRM, its own by name, any case.
        m = cue("React 3D", do="move", move="nod")
        assert m == {"ok": True, "kind": "head", "shows": True}, m
        m = cue("React 3D", do="move", move="Wave")
        assert m == {"ok": True, "kind": "arms", "shows": False}, m
        m = cue("React 3D", do="move", move="dance")
        assert m == {"ok": True, "kind": "file", "shows": False}, m
        assert cue("React 3D", do="stop")["ok"]
        time.sleep(0.6)
        badge = lambda px: px[0] > 120 and px[1] < 70 and px[2] < 70  # noqa: E731
        still = [middle_of(screenshot(c, "React 3D"), badge)[0] for _ in range(3)]
        assert max(still) - min(still) < 1.5, still
        m = cue("React 3D", do="move", move="slide")
        assert m == {"ok": True, "kind": "own", "shows": True}, m
        xs = [middle_of(rows, badge)[0] for _, rows in quick_shots(c, "React 3D", 1.0)]
        print(f"its own animation on cue: badge x {min(xs):.1f} to {max(xs):.1f} (still {still[0]:.1f})")
        assert max(xs) - min(xs) > 15, xs
        assert wait_for(lambda: not st("React 3D")["react"]["busy"], 3, 0.1), st("React 3D")["react"]

        # A throw from the picture's left: in the air, then a hit - a flash
        # by its face, its head knocked back and settling - then gone.
        time.sleep(0.3)
        rest = screenshot(c, "React picture")
        rest_x, rest_y, rest_n = middle_of(rest, orange)
        assert rest_n > 10000 and middle_of(rest, red)[2] == 0 and middle_of(rest, yellow)[2] == 0
        t = cue("React picture", do="throw", item=0, side=-1, strength=1, size=1,
                face="", face_seconds=1)
        assert t["ok"] and abs(t["hits_in"] - 0.5) < 0.11, t
        shots = quick_shots(c, "React picture", 1.6)
        seen = [(at, middle_of(rows, red), middle_of(rows, yellow)[2], middle_of(rows, orange))
                for at, rows in shots]
        flying = [(at, x) for at, (x, _, n), _, _ in seen if n > 40]
        flashed = [at for at, _, n, _ in seen if n > 30]
        moved = max(math.hypot(o[0] - rest_x, o[1] - rest_y) for _, _, _, o in seen if o[2])
        print(f"thrown: in the air {len(flying)} of {len(shots)} looks, x "
              f"{[round(x) for _, x in flying]}, the flash at {[round(a, 2) for a in flashed]} s, "
              f"knocked {moved:.1f} px")
        assert flying and flying[0][1] < rest_x - 30, (flying, rest_x)     # from its left
        assert flashed, seen
        assert moved > 3, moved
        busiest = max(shots, key=lambda s: middle_of(s[1], red)[2])[1]
        keep_shot(busiest, "React throw")
        assert wait_for(lambda: st("React picture")["react"]["hits"] == 1, 3, 0.05), \
            st("React picture")["react"]
        assert wait_for(lambda: st("React picture")["react"]["flying"] == 0, 3, 0.05)
        time.sleep(1.0)
        after = screenshot(c, "React picture")
        assert middle_of(after, red)[2] == 0 and middle_of(after, yellow)[2] == 0
        ax, ay, _ = middle_of(after, orange)
        assert math.hypot(ax - rest_x, ay - rest_y) < 1.5, (ax, ay, rest_x, rest_y)   # settled

        # A volley lands one after another; too many at once is said; stop
        # ends it (nothing more hits).
        gaps = [cue("React picture", do="throw", item=0)["hits_in"] for _ in range(3)]
        assert gaps[1] > gaps[0] + 0.05 and gaps[2] > gaps[1] + 0.05, gaps
        assert wait_for(lambda: st("React picture")["react"]["hits"] == 4, 4, 0.05), \
            st("React picture")["react"]
        answers = [cue("React picture", do="throw", item=0) for _ in range(30)]
        refused = [a for a in answers if not a["ok"]]
        assert refused and "Too much" in refused[0]["error"], answers[-1]
        assert 16 <= len(answers) - len(refused) <= 24, len(refused)
        assert cue("React picture", do="stop")["ok"]
        time.sleep(1.2)
        hits = st("React picture")["react"]["hits"]
        time.sleep(1.0)
        r = st("React picture")["react"]
        assert r["hits"] == hits and r["flying"] == 0 and hits < 4 + 24, r

        # A headpat: the hand comes down on top of its head, pats, and goes.
        assert middle_of(screenshot(c, "React picture"), skin)[2] == 0
        assert cue("React picture", do="pat", count=3, size=1)["ok"]
        shots = quick_shots(c, "React picture", 1.4)
        pats = [(at, middle_of(rows, skin)) for at, rows in shots]
        shown = [(at, x, y) for at, (x, y, n) in pats if n > 100]
        print(f"headpat: the hand in {len(shown)} of {len(shots)} looks, at "
              f"{[(round(x), round(y)) for _, x, y in shown[-3:]]}, its face at "
              f"({rest_x:.0f}, {rest_y:.0f})")
        assert shown, pats
        _, hx, hy = shown[-1]
        assert abs(hx - rest_x) < 40 and hy < rest_y - 30, (hx, hy, rest_x, rest_y)
        keep_shot(max(shots, key=lambda s: middle_of(s[1], skin)[2])[1], "React headpat")
        assert wait_for(lambda: middle_of(screenshot(c, "React picture"), skin)[2] == 0, 4, 0.1)
        assert wait_for(lambda: not st("React picture")["react"]["busy"], 4, 0.1), \
            st("React picture")["react"]

        # A bounce: up and down.
        assert cue("React picture", do="move", move="bounce")["kind"] == "head"
        ys = [middle_of(rows, orange)[1] for _, rows in quick_shots(c, "React picture", 1.0)]
        print(f"bounce: its middle from y {min(ys):.1f} to {max(ys):.1f} (rest {rest_y:.1f})")
        assert min(ys) < rest_y - 5, (ys, rest_y)

        # Thrown at a 3D character: in the air over it, a hit.
        before3 = st("React 3D")["react"]["hits"]
        assert cue("React 3D", do="throw", item=0, side=1)["ok"]
        shots = quick_shots(c, "React 3D", 0.9)
        assert any(middle_of(rows, red)[2] > 40 for _, rows in shots)
        keep_shot(max(shots, key=lambda s: middle_of(s[1], red)[2])[1], "React 3D throw")
        assert wait_for(lambda: st("React 3D")["react"]["hits"] == before3 + 1, 3, 0.05)

        # Its pictures changed: a missing one is said not loaded.
        send(c, "SetInputSettings", {"inputName": "React picture", "inputSettings": {
            "react_items": json.dumps([{"file": tomato}]), "react_hand": ""}})
        assert wait_for(lambda: st("React picture")["react"]["items"] == [{"loaded": True}]
                        and not st("React picture")["react"]["hand"], 5, 0.1)
        for name in list(avatars) + ["React camera"]:
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


def gif_picture(path, size, colour):
    """A GIF (as chat's emotes often are): a square of `colour` with a
    see-through ring round it. Written plainly - every two pixels a fresh
    start, so its codes stay 3 bits long."""
    pixels = [1 if size // 8 <= x < size - size // 8 and size // 8 <= y < size - size // 8 else 0
              for y in range(size) for x in range(size)]
    codes = []
    for i in range(0, len(pixels), 2):
        codes += [4] + pixels[i:i + 2]
    codes.append(5)
    bits, data = 0, bytearray()
    acc = 0
    for code in codes:
        acc |= code << bits
        bits += 3
        while bits >= 8:
            data.append(acc & 0xFF)
            acc >>= 8
            bits -= 8
    if bits:
        data.append(acc & 0xFF)
    blocks = b"".join(bytes([len(data[i:i + 255])]) + bytes(data[i:i + 255]) for i in range(0, len(data), 255))
    with open(path, "wb") as f:
        f.write(b"GIF89a" + struct.pack("<HHBBB", size, size, 0x81, 0, 0))
        f.write(b"\0\0\0" + bytes(colour) + b"\0" * 6)              # 4 colours, 0 see-through
        f.write(b"\x21\xf9\x04\x01\x0a\x00\x00\x00")              # a graphic control: index 0 clear
        f.write(b"\x2c" + struct.pack("<HHHHB", 0, 0, size, size, 0))
        f.write(b"\x02" + blocks + b"\x00\x3b")


GREEN = (0.01, 0.6, 0.01)       # linear, as a model has it
BLUE = (30, 60, 230)


def test_pats_from_a_side_3d_things_and_emotes(run):
    """0.8.1: a headpat's hand comes in from the side asked - a picture
    (turned at its wrist), or a 3D hand, over a 2D character too, its skin
    the colour asked - and a 3D hand that can't be read is said, the
    picture patting instead; a 3D thing to throw loads on its own and is
    thrown like a picture; one of chat's emotes is thrown by its picture's
    file (a GIF too), kept for the next time."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    body_png = os.path.join(run.home, "react-character.png")
    character_picture(body_png)
    hand = os.path.join(run.home, "react-hand.png")
    hand_picture(hand)
    hand3d = os.path.join(run.home, "react-hand.glb")
    # A long flat bar for a hand: the palm at the middle of its one end, the
    # rest going back towards its wrist (+x: its fingers point -x).
    write_glb(hand3d, [ball_points(0.1, 0.015, 0, 0.12, 0.02, 0.045) + ((0.8, 0.8, 0.8), 0.6)],
              materials_more={0: {"name": "skin"}})
    broken = os.path.join(run.home, "broken-hand.glb")
    with open(broken, "wb") as f:
        f.write(b"glTF not really")
    ball = os.path.join(run.home, "green-ball.glb")
    write_glb(ball, [ball_points(0, 0, 0, 0.1, 0.1, 0.1) + (GREEN, 0.5)])
    emote = os.path.join(run.home, "emote.gif")
    gif_picture(emote, 48, BLUE)
    size = 300
    settings = {"source": "Pat camera", "width": size, "height": size, "size": 85, "mirror": False,
                "smoothing": 100, "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "bend": 0,
                **STILL, "character": body_png, "style": "drawn", "spots": json.dumps(SPOTS),
                "react_hand": hand, "react_items": json.dumps([{"file": ball}])}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st():
        return vendor(c, "FaceGetState", {"source": "Pat"})["react"]

    def cue(**data):
        return vendor(c, "FaceReact", {"source": "Pat", **data})
    skin = lambda px: px[0] > 200 and 160 < px[1] < 220 and 110 < px[2] < 190  # noqa: E731
    reddish = lambda px: px[0] > 90 and px[0] > 2 * px[1] and px[0] > 2 * px[2]  # noqa: E731
    green = lambda px: px[1] > 100 and px[1] > 2 * px[0] and px[1] > 2 * px[2]  # noqa: E731
    blue = lambda px: px[2] > 150 and px[2] > 2 * px[0] and px[2] > 2 * px[1]  # noqa: E731

    def first_in(test, seconds):
        """Where the first look with `test`'s pixels has them (x), and the
        most there were."""
        shots = quick_shots(c, "Pat", seconds)
        seen = [middle_of(rows, test) for _, rows in shots]
        xs = [x for x, _, n in seen if n > 40]
        return (xs[0] if xs else None), max(n for _, _, n in seen), shots
    try:
        send(c, "CreateScene", {"sceneName": "Pats"})
        send(c, "CreateInput", {"sceneName": "Pats", "inputName": "Pat camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Pats", "inputName": "Pat",
                                "inputKind": "direbot_face_avatar", "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Pats"})
        assert wait_for(lambda: vendor(c, "FaceGetState", {"source": "Pat"})["shown"] == 1, 30, 0.2)
        # The 3D thing loads on its own thread.
        assert wait_for(lambda: st()["items"] == [{"loaded": True, "three_d": True}], 20, 0.1), st()
        rest = screenshot(c, "Pat")
        orange = lambda px: px[0] > 170 and 90 < px[1] < 160 and px[2] < 90  # noqa: E731
        rest_x, rest_y, _ = middle_of(rest, orange)

        # The picture hand, from the left, then from the right.
        for side in (-1, 1):
            assert cue(do="pat", count=2, side=side)["ok"]
            x, most, shots = first_in(skin, 1.2)
            print(f"picture hand from {side}: first at x {x}, face at {rest_x:.0f}")
            assert x is not None and (x - rest_x) * side > 10, (x, rest_x)
            keep_shot(max(shots, key=lambda s: middle_of(s[1], skin)[2])[1], f"Pat picture {side}")
            assert wait_for(lambda: not st()["patting"], 5, 0.1), st()

        # A 3D hand that can't be read: said, and the picture pats.
        send(c, "SetInputSettings", {"inputName": "Pat", "inputSettings": {"react_hand_3d": broken}})
        assert wait_for(lambda: st().get("hand_3d_problem"), 10, 0.1), st()
        assert not st()["hand_3d"]
        assert cue(do="pat", count=1, side=1)["ok"]
        assert first_in(skin, 1.0)[1] > 40
        assert wait_for(lambda: not st()["patting"], 5, 0.1), st()

        # Our 3D hand, over the 2D character, its skin coloured red.
        send(c, "SetInputSettings", {"inputName": "Pat", "inputSettings": {
            "react_hand_3d": hand3d, "react_hand_tint": json.dumps({"skin": [1, 0.02, 0.02]})}})
        assert wait_for(lambda: st()["hand_3d"] and not st().get("hand_3d_problem"), 10, 0.1), st()
        # From the left, its arm back towards the left the whole time.
        assert cue(do="pat", count=2, side=-1)["ok"]
        x, most, shots = first_in(reddish, 1.4)
        xs = [middle_of(rows, reddish) for _, rows in shots]
        xs = [x for x, _, n in xs if n > 150]
        print(f"3D hand from the left over a 2D character: in {len(xs)} looks, x "
              f"{[round(x) for x in xs]}, most {most} pixels")
        assert xs and max(xs) < rest_x - 5, (xs, rest_x)
        assert all(middle_of(rows, skin)[2] < 20 for _, rows in shots)      # not the picture
        keep_shot(max(shots, key=lambda s: middle_of(s[1], reddish)[2])[1], "Pat 3D hand on 2D")
        assert wait_for(lambda: not st()["patting"], 5, 0.1), st()

        # A 3D thing thrown: in the air, a hit.
        hits = st()["hits"]
        t = cue(do="throw", item=0, side=1, size=1, strength=1)
        assert t["ok"], t
        x, most, shots = first_in(green, 1.0)
        assert most > 60, most
        keep_shot(max(shots, key=lambda s: middle_of(s[1], green)[2])[1], "Thrown 3D thing")
        assert wait_for(lambda: st()["hits"] == hits + 1, 3, 0.05), st()

        # One of chat's emotes, by its file: thrown, kept for next time.
        t = cue(do="throw", file=emote, size=1, side=-1)
        assert t["ok"], t
        x, most, shots = first_in(blue, 1.0)
        assert most > 60, most
        keep_shot(max(shots, key=lambda s: middle_of(s[1], blue)[2])[1], "Thrown emote")
        assert cue(do="throw", file=emote)["ok"] and st()["emotes"] == 1
        bad = cue(do="throw", file=os.path.join(run.home, "not-there.gif"))
        assert not bad["ok"] and "couldn't be opened" in bad["error"], bad
        assert wait_for(lambda: st()["hits"] == hits + 3, 4, 0.05), st()
        assert wait_for(lambda: not st()["busy"], 5, 0.1), st()

        # A pat squishes its head (springing back), and its eyes stay shut
        # a moment after the hand has gone.
        assert cue(do="pat", count=2, side=1)["ok"]
        seen = []
        end = time.time() + 3
        while time.time() < end:
            now = st()
            seen.append((now["patting"], now["squish"], now["squint"]))
            time.sleep(0.03)
        print(f"pat: squish {max(q for _, q, _ in seen)} to {min(q for _, q, _ in seen)}")
        assert max(q for _, q, _ in seen) > 0.6 and min(q for _, q, _ in seen) < -0.02
        gone = next(i for i in range(1, len(seen)) if seen[i - 1][0] and not seen[i][0])
        assert seen[gone][2] > 0.6 and seen[-1][2] == 0, seen[gone:]

        # Stuck: an emote where it hit a while, then off; a stop drops one
        # at once.
        hits = st()["hits"]
        assert cue(do="throw", file=emote, stick=2, side=1)["ok"]
        assert wait_for(lambda: st()["stuck"] == 1, 1.5, 0.05), st()
        time.sleep(1.0)
        shot = screenshot(c, "Pat")
        assert middle_of(shot, blue)[2] > 40
        keep_shot(shot, "Stuck emote")
        assert wait_for(lambda: st()["stuck"] == 0, 3, 0.05), st()
        assert cue(do="throw", file=emote, stick=10)["ok"]
        assert wait_for(lambda: st()["stuck"] == 1, 1.5, 0.05), st()
        assert cue(do="stop")["ok"]
        assert wait_for(lambda: st()["stuck"] == 0, 0.5, 0.02), st()
        assert wait_for(lambda: not st()["busy"], 5, 0.1), st()

        # A bonk: the 3D ball over its head first, then down on it - its
        # head squished.
        hits = st()["hits"]
        assert cue(do="throw", item=0, size=1, **{"from": "above"})["ok"]
        shots = quick_shots(c, "Pat", 0.4)
        ys = [middle_of(rows, green) for _, rows in shots]
        ys = [y for _, y, n in ys if n > 40]
        print(f"bonk: the ball at y {[round(y) for y in ys]}, the face at {rest_y:.0f}")
        assert ys and ys[0] < rest_y - 40, (ys, rest_y)
        keep_shot(min(shots, key=lambda s: middle_of(s[1], green)[1] if middle_of(s[1], green)[2] > 40
                       else 1e9)[1], "Bonk wind-up")
        assert wait_for(lambda: st()["hits"] == hits + 1, 2, 0.05), st()
        assert wait_for(lambda: not st()["busy"], 5, 0.1), st()
        # (Again, watched closely as it lands: the squish is quick.)
        assert cue(do="throw", item=0, size=1, **{"from": "above"})["ok"]
        squished = 0
        end = time.time() + 1.2
        while time.time() < end:
            squished = max(squished, st()["squish"])
            time.sleep(0.01)
        assert st()["hits"] == hits + 2 and squished > 0.3, (st(), squished)
        assert wait_for(lambda: not st()["busy"], 5, 0.1), st()

        # A barrage: five, one after another.
        hits = st()["hits"]
        t = cue(do="throw", file=emote, count=5, side=-1)
        assert t["ok"] and t["thrown"] == 5 and abs(t["every"] - 0.1) < 1e-6, t
        assert wait_for(lambda: st()["hits"] == hits + 5, 3, 0.05), st()
        for name in ("Pat", "Pat camera"):
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


# ---- 0.8.0: your hands and arms ------------------------------------------------

BODY = os.path.join(HERE, "tests", "data", "body")
ARM_GREEN = (0.05, 0.6, 0.08)
HAND_RED = (0.8, 0.02, 0.02)
FOLLOWER_CYAN = (0.02, 0.55, 0.55)


def body_camera(path):
    """The body tracker's test photo (tests/data/body/astronaut.ppm: head,
    shoulders and both arms, hands together in front) as a PNG."""
    w, h, px = read_ppm(os.path.join(BODY, "astronaut.ppm"))
    write_png(path, w, h, [px[y * w * 3:(y + 1) * w * 3] for y in range(h)])


def arms_character(path, vrm=True):
    """A figure standing in a T, facing the camera: an orange ball head, a
    blue body, green arms and red hands, each part hung on its own bone (so
    it moves with it); a VRM 1.0 with those bones, unless `vrm` is False.
    Beside its body, under its left arm, a cyan ball on a bone that copies
    its left upper arm's turn (VRM's node constraints, from 0.8.0)."""
    parts = [ball_points(0, 0.1, 0, 0.1, 0.11, 0.1) + ((0.8, 0.25, 0.04), 0.4),
             ball_points(0, 0.1, 0, 0.14, 0.2, 0.08) + ((0.05, 0.1, 0.7), 0.5)]
    tree = [{"name": "hips", "translation": [0, 1, 0], "children": [1]},
            {"name": "spine", "translation": [0, 0.1, 0], "children": [2]},
            {"name": "chest", "translation": [0, 0.15, 0], "mesh": 1, "children": [3, 5, 9]},
            {"name": "neck", "translation": [0, 0.25, 0], "children": [4]},
            {"name": "head", "translation": [0, 0.08, 0], "mesh": 0}]
    bones = {"hips": 0, "spine": 1, "chest": 2, "neck": 3, "head": 4}
    for side, sx in (("left", 1), ("right", -1)):
        first = len(tree)
        parts += [ball_points(sx * 0.12, 0, 0, 0.12, 0.035, 0.035) + (ARM_GREEN, 0.5),
                  ball_points(sx * 0.12, 0, 0, 0.12, 0.03, 0.03) + (ARM_GREEN, 0.5),
                  ball_points(sx * 0.05, 0, 0, 0.05, 0.02, 0.04) + (HAND_RED, 0.5)]
        mesh = len(parts) - 3
        tree += [{"name": side + "Shoulder", "translation": [sx * 0.04, 0.2, 0], "children": [first + 1]},
                 {"name": side + "UpperArm", "translation": [sx * 0.1, 0, 0], "mesh": mesh,
                  "children": [first + 2]},
                 {"name": side + "LowerArm", "translation": [sx * 0.24, 0, 0], "mesh": mesh + 1,
                  "children": [first + 3]},
                 {"name": side + "Hand", "translation": [sx * 0.24, 0, 0], "mesh": mesh + 2}]
        for k, bone in enumerate(("Shoulder", "UpperArm", "LowerArm", "Hand")):
            bones[side + bone] = first + k
    parts.append(ball_points(0, 0.2, 0, 0.04, 0.04, 0.04) + (FOLLOWER_CYAN, 0.5))
    tree[2]["children"].append(len(tree))
    tree.append({"name": "follower", "translation": [0.35, 0, 0], "mesh": len(parts) - 1,
                 "extensions": {"VRMC_node_constraint": {
                     "specVersion": "1.0",
                     "constraint": {"rotation": {"source": bones["leftUpperArm"], "weight": 1}}}}})
    top = {"extensionsUsed": ["VRMC_node_constraint"]}
    if vrm:
        top = {"extensionsUsed": ["VRMC_vrm", "VRMC_node_constraint"],
                "extensions": {"VRMC_vrm": {
                    "specVersion": "1.0",
                    "meta": {"name": "Arms", "authors": ["DireBot"], "avatarPermission": "everyone"},
                    "humanoid": {"humanBones": {k: {"node": v} for k, v in bones.items()}}}}}
    write_glb(path, parts, tree=tree, top=top)


def test_hands_and_arms_follow_you(run):
    """0.8.0: a 3D VRM character in its own spot follows your arms from the
    same camera once it's turned on (the body models loading then): its
    arms go where yours are - from hanging by its sides to bent, its hands
    together in front - and back when it's turned off. FaceGetState says
    what it follows and what of you the camera sees (for DireBot's camera
    check), and your body's points; calibrating with no hand up gives up
    and says so; forgetting it clears the source's own; a character that
    can't follow says why; light mode looks fewer times. A bone copying its
    arm's turn (VRM's node constraints) goes round with it. From 0.9.0: how
    often it looks at most, the body model's share, the palm finder and
    how often each hand was found."""
    camera = os.path.join(run.home, "astronaut.png")
    body_camera(camera)
    figure = os.path.join(run.home, "arms.vrm")
    arms_character(figure)
    plain = os.path.join(run.home, "arms-plain.glb")
    arms_character(plain, vrm=False)
    size = 300
    common = {"source": "Arms camera", "width": size, "height": size, "size": 100,
              "framing": "whole", "style": "drawn", "smoothing": 100, "follow_move": 0,
              "follow_grow": 0, "follow_tilt": 0, "body_follow": 0, **STILL}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st(name="Arms", points=False):
        return vendor(c, "FaceGetState", {"source": name, "points": points})

    def arms(name="Arms", points=False):
        return st(name, points)["hands_and_arms"]

    def change(name, settings):
        send(c, "SetInputSettings", {"inputName": name, "inputSettings": settings})
    green = lambda px: px[1] > 90 and px[0] < 70 and px[2] < 70  # noqa: E731
    red = lambda px: px[0] > 120 and px[1] < 50 and px[2] < 50  # noqa: E731
    cyan = lambda px: px[1] > 100 and px[2] > 100 and px[0] < 100 and abs(px[1] - px[2]) < 70  # noqa: E731
    try:
        send(c, "CreateScene", {"sceneName": "Hands and arms"})
        send(c, "CreateInput", {"sceneName": "Hands and arms", "inputName": "Arms camera",
                                "inputKind": "image_source", "inputSettings": {"file": camera}})
        for name, extra in (("Arms", {"model": figure}), ("Arms plain", {"model": plain}),
                            ("Arms worn", {"model": figure, "worn": True})):
            send(c, "CreateInput", {"sceneName": "Hands and arms", "inputName": name,
                                    "inputKind": "direbot_face_avatar",
                                    "inputSettings": {**common, **extra}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Hands and arms"})
        assert wait_for(lambda: st()["model"]["shown"], 30, 0.2), st()
        assert st()["model"]["kind"] == "vrm1"
        # Off until it's turned on: nothing loaded, nothing looked at.
        a = arms()
        assert a["on"] is False and a["can"] is False and a["why_not"] == "off", a
        assert a["hands"] is True and a["light"] is False
        assert not a["loading"] and not a["ready"] and a["framing"] == {"found": False}, a
        rest = screenshot(c, "Arms")
        keep_shot(rest, "arms-rest")
        gx0, gy0, gn0 = middle_of(rest, green)
        _, ry0, rn0 = middle_of(rest, red)
        assert gn0 > 200 and rn0 > 20, (gn0, rn0)
        spread0 = sum(abs(x - size / 2) for row in rest for x, px in enumerate(row) if red(px)) / rn0
        # Its bone copying its left upper arm's turn (hanging by its side).
        model = st()["model"]
        assert model["following_bones"] == 1 and model["following_bones_left_out"] == 0, model
        assert st("Arms plain")["model"]["following_bones"] == 1
        cx0, cy0, cn0 = middle_of(rest, cyan)
        assert cn0 > 30, cn0

        # On: the body models load, it finds you and follows both arms.
        for name in ("Arms", "Arms plain", "Arms worn"):
            change(name, {"follow_arms": True})
        assert wait_for(lambda: arms()["ready"], 60, 0.2), arms()
        assert wait_for(lambda: arms()["found"] and arms()["following"]["left_arm"] and
                        arms()["following"]["right_arm"], 30, 0.2), arms()
        # (How many looks a second is counted over a second.)
        assert wait_for(lambda: arms()["looks_per_second"] > 2, 5, 0.2), arms()
        a = arms(points=True)
        assert a["on"] and a["can"] and a["why_not"] == "" and "problem" not in a, a
        assert 2 < a["looks_per_second"] <= 16 and a["ms"] > 0, a
        # 0.9.0: hands up to 30 looks a second, the body model half that;
        # the palm finder loaded. (A photo isn't a new picture each frame,
        # so it's looked at less.)
        assert a["most_per_second"] == 30 and a["palm_finder"] is True, a
        assert 0 < a["body_per_second"] <= a["looks_per_second"] + 0.5, a
        assert a["hands_seen"] == {"left": {"found": 0}, "right": {"found": 0}}, a
        # No hands in this photo (MediaPipe finds none either).
        assert not a["following"]["left_hand"] and not a["following"]["right_hand"], a
        f = a["framing"]
        assert f["found"] and f["head"] and f["shoulders"] == 2 and f["elbows"] == 2, f
        assert f["wrists"] == 2 and f["hands"] == 0, f
        # Her shoulders: 297 and 113 of 512 across, at 236 down.
        assert abs(f["shoulder_width"] - 0.36) < 0.03 and abs(f["shoulder_height"] - 0.46) < 0.03, f
        # Her nose and shoulders where MediaPipe puts them (following her
        # from look to look, as MediaPipe's own VIDEO mode does, moves them
        # a few pixels on a still photo); her elbows and wrists in sight.
        expected = json.load(open(os.path.join(BODY, "expected.json")))["pictures"]["astronaut"]
        assert len(a["points"]) == 25
        for i in (0, 11, 12):
            want = expected["points"][i]
            got = a["points"][i]
            assert abs(got["x"] * 512 - want[0]) < 15 and abs(got["y"] * 512 - want[1]) < 20, (i, got, want)
        for i in (0, 11, 12, 13, 14, 15, 16):
            got = a["points"][i]
            assert got["seen"] > 0.5 and 0 < got["x"] < 1 and 0 < got["y"] < 1, (i, got)
        assert "left_hand_points" not in a and "right_hand_points" not in a
        # Its arms hang relaxed by its sides until then; now its elbows bend
        # and its hands come up and in, together in front of it. (The body
        # models settle on the photo over their first looks - a few a
        # second on a slow PC - so it's looked at until they have.)
        shots = []

        def risen():
            shot = screenshot(c, "Arms")
            _, _, gn = middle_of(shot, green)
            _, ry, rn = middle_of(shot, red)
            spread = sum(abs(x - size / 2) for row in shot for x, px in enumerate(row) if red(px)) / max(rn, 1)
            shots.append((shot, gn, ry, rn, spread))
            return gn > 40 and rn > 10 and ry < ry0 - 0.04 * size and spread < 0.6 * spread0
        time.sleep(1.5)
        wait_for(risen, 8, 0.5)
        follow, gn1, ry1, rn1, spread1 = shots[-1]
        keep_shot(follow, "arms-following")
        assert gn1 > 40 and rn1 > 10, (gn1, rn1)
        assert ry1 < ry0 - 0.04 * size, (ry0, ry1)
        assert spread1 < 0.6 * spread0, (spread0, spread1)
        # The ball copying its upper arm's turn has gone round with it.
        cx1, cy1, cn1 = middle_of(follow, cyan)
        assert cn1 > 30 and math.hypot(cx1 - cx0, cy1 - cy0) > 0.02 * size, (cx0, cy0, cx1, cy1, cn1)

        # Ones that can't follow say why; the camera isn't looked at for them.
        p = arms("Arms plain")
        assert p["on"] and not p["can"] and p["why_not"] == "not_vrm" and not p["found"], p
        w = arms("Arms worn")
        assert w["on"] and not w["can"] and w["why_not"] == "worn", w
        bad = vendor(c, "FaceArms", {"source": "Arms worn", "do": "calibrate"})
        assert not bad["ok"] and "own spot" in bad["error"], bad

        # Calibrating with no hand by your ear: it gives up and says so.
        r = vendor(c, "FaceArms", {"source": "Arms", "do": "calibrate"})
        assert r == {"ok": True}, r
        assert arms()["calibrating"]
        assert wait_for(lambda: not arms()["calibrating"], 12, 0.2), arms()
        a = arms()
        assert a["calibration"] == "no hand" and not a["calibrated"], a
        assert "doesn't know" in vendor(c, "FaceArms", {"source": "Arms", "do": "juggle"})["error"]
        # A calibration put in the settings (DireBot's Undo) is read; forgetting it clears it.
        kept = json.dumps({"v": 1, "by_ear": [0.1, -0.05, 0], "open": [0] * 15})
        change("Arms", {"arms_calibration": kept})
        assert wait_for(lambda: arms()["calibrated"], 5, 0.1), arms()
        assert vendor(c, "FaceArms", {"source": "Arms", "do": "forget"}) == {"ok": True}
        assert not arms()["calibrated"]
        got = send(c, "GetInputSettings", {"inputName": "Arms"})["inputSettings"]
        assert got.get("arms_calibration", "") == "", got
        # Without your hands, calibrating asks for them. (OBS hands a video
        # source its new settings on its next frame.)
        change("Arms", {"arms_hands": False})
        assert wait_for(lambda: arms()["hands"] is False, 5, 0.05), arms()
        nope = vendor(c, "FaceArms", {"source": "Arms", "do": "calibrate"})
        assert not nope["ok"] and "hands" in nope["error"], nope
        # Light mode: hands at most 15 looks a second (a photo, fewer).
        change("Arms", {"arms_light": True, "arms_hands": True})
        assert wait_for(lambda: arms()["light"], 5, 0.1)
        time.sleep(3)
        assert arms()["looks_per_second"] <= 11, arms()
        assert arms()["most_per_second"] == 15, arms()
        # Arms only: the body model on each look, at most 15 a second.
        change("Arms", {"arms_light": False, "arms_hands": False})
        assert wait_for(lambda: arms()["most_per_second"] == 15 and not arms()["light"], 5, 0.1), arms()

        # Off: its arms hang by its sides again.
        change("Arms", {"follow_arms": False})
        assert wait_for(lambda: arms()["why_not"] == "off", 5, 0.1), arms()
        time.sleep(1)
        back = screenshot(c, "Arms")
        keep_shot(back, "arms-back")
        gx2, gy2, gn2 = middle_of(back, green)
        assert abs(gy2 - gy0) < 0.02 * size and abs(gx2 - gx0) < 0.02 * size, (gx0, gy0, gx2, gy2)
        cx2, cy2, _ = middle_of(back, cyan)
        assert abs(cy2 - cy0) < 0.02 * size and abs(cx2 - cx0) < 0.02 * size, (cx0, cy0, cx2, cy2)
        for name in ("Arms", "Arms plain", "Arms worn"):
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


def test_hidden_it_still_loads_and_all_of_it_fills_its_spot(run):
    """0.8.1: a 3D character in a scene that isn't shown (or switched off
    in the one that is) is loaded all the same - OBS only draws what's
    shown, and it used to wait to be drawn, saying it was loading for
    ever. "All of it" frames it as it stands, its arms down - not as
    wide as its arms reach in the T it was made in - so it fills its
    spot, top to bottom. FaceGetState says the shape of what's framed
    (for DireBot's "Fit the OBS box to it"), and Size goes past 100%."""
    camera = os.path.join(run.home, "astronaut.png")
    if not os.path.exists(camera):
        body_camera(camera)
    figure = os.path.join(run.home, "arms.vrm")
    if not os.path.exists(figure):
        arms_character(figure)
    size = 300
    common = {"source": "Fill camera", "width": size, "height": size, "size": 100,
              "framing": "whole", "model": figure, "mirror": False, "smoothing": 100,
              "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "body_follow": 0,
              "bend": 0, **STILL}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st(name):
        return vendor(c, "FaceGetState", {"source": name})["model"]
    try:
        send(c, "CreateScene", {"sceneName": "Fill shown"})
        send(c, "CreateScene", {"sceneName": "Fill not shown"})
        send(c, "CreateInput", {"sceneName": "Fill shown", "inputName": "Fill camera",
                                "inputKind": "image_source", "inputSettings": {"file": camera}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Fill shown"})
        # One in a scene that isn't shown, one switched off where it is.
        send(c, "CreateInput", {"sceneName": "Fill not shown", "inputName": "Fill away",
                                "inputKind": "direbot_face_avatar", "inputSettings": common})
        item = send(c, "CreateInput", {"sceneName": "Fill shown", "inputName": "Fill off",
                                       "inputKind": "direbot_face_avatar",
                                       "inputSettings": common})["sceneItemId"]
        send(c, "SetSceneItemEnabled", {"sceneName": "Fill shown", "sceneItemId": item,
                                        "sceneItemEnabled": False})
        for name in ("Fill away", "Fill off"):
            assert wait_for(lambda: st(name)["loaded"], 20, 0.2), st(name)
            m = st(name)
            assert not m["shown"] and not m["loading"] and m["kind"] == "vrm1", m
            assert vendor(c, "FaceGetState", {"source": name})["showing"] is False
        # The shape of what's framed: as it stands, its arms hanging (its
        # hands below its body; the cyan ball that copies its arm swung
        # down and out), with a little room round it - where its file's
        # box, the T, is 1.44 wide and 0.64 tall.
        f = st("Fill away")["framed"]
        print(f"framed as it stands: {f}")
        assert 0.85 < f["w"] < 1.05 and 0.9 < f["h"] < 1.0, f
        # Head and shoulders: half a metre round its head.
        send(c, "SetInputSettings", {"inputName": "Fill away", "inputSettings": {"framing": "head"}})
        assert wait_for(lambda: st("Fill away")["framed"] == {"w": 0.5, "h": 0.5}, 5, 0.1), st("Fill away")
        send(c, "SetInputSettings", {"inputName": "Fill away", "inputSettings": {"framing": "whole"}})
        assert wait_for(lambda: st("Fill away")["framed"] == f, 5, 0.1), st("Fill away")

        # Shown now: it fills its spot top to bottom (its arms down by its
        # sides, its hands in the picture), see-through either side.
        send(c, "SetSceneItemEnabled", {"sceneName": "Fill shown", "sceneItemId": item,
                                        "sceneItemEnabled": True})
        assert wait_for(lambda: st("Fill off")["shown"], 10, 0.1), st("Fill off")
        time.sleep(0.5)
        rows = screenshot(c, "Fill off")
        drawn = lambda px: px != (0, 0, 0)  # noqa: E731
        ys = [y for y in range(size) if any(drawn(px) for px in rows[y])]
        xs = [x for x in range(size) if any(drawn(rows[y][x]) for y in range(size))]
        print(f"all of it at 100%: rows {ys[0]}-{ys[-1]}, columns {xs[0]}-{xs[-1]}")
        # (Framed by the T, it was 0.64 / 1.44 of that, and its hands
        # hung out of the bottom.)
        assert ys[-1] - ys[0] > size * 0.85, (ys[0], ys[-1])
        assert ys[0] > 0 and ys[-1] < size - 1   # all of it, nothing cut off
        assert xs[0] > 0 and xs[-1] < size - 1
        _, _, hands = middle_of(rows, lambda px: px[0] > 120 and px[1] < 60 and px[2] < 60)
        assert hands > 10, hands
        whole = sum(1 for row in rows for px in row if drawn(px))
        # Size past 100%: twice as big, its top kept in its spot (just
        # under the edge) and cut off at the bottom - not at its middle.
        send(c, "SetInputSettings", {"inputName": "Fill off", "inputSettings": {"size": 200}})
        time.sleep(0.5)
        rows = screenshot(c, "Fill off")
        bigger = sum(1 for row in rows for px in row if drawn(px))
        top = next(y for y in range(size) if any(drawn(px) for px in rows[y]))
        print(f"drawn at 100%: {whole} pixels, at 200%: {bigger}, its top at row {top}")
        assert bigger > whole * 1.8, (whole, bigger)
        assert 0 < top < size * 0.08, top
        assert any(drawn(px) for px in rows[-1])
        for name in ("Fill away", "Fill off", "Fill camera"):
            send(c, "RemoveInput", {"inputName": name})
    finally:
        c.disconnect()


def test_closing_obs_leaves_nothing_behind(run):
    assert run.close() == 0
    log = run.log()
    assert "Number of memory leaks: 0" in log
    # Every value a 3D technique reads was set just before it (Windows
    # won't draw one with any missing).
    assert "Windows won't draw it" not in log
