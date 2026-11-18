"""DireBot for OBS 0.9.0 in a real OBS: what things do as they hit the face
avatar (a splat left on it - only on the character, never the air round
it - with juice thrown off and drips running down; feathers floating down;
pieces and a puff bursting off, some snow left a moment), and "DireBot
Throws", the filter on a camera that throws land on your real face in.

Run as tests/test_real_obs.py says (DIREBOT_OBS_PLUGIN=... python3 -m pytest
tests/test_real_throws.py); DIREBOT_FACE_SHOTS=<folder> keeps the pictures.
"""

import base64
import json
import math
import os
import shutil
import struct
import time
import zlib

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_obs import Obs, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark  # noqa: E402,F401
from test_real_face import (STILL, SPOTS, camera_picture, character_picture, disc_picture,  # noqa: E402
                            keep_shot, middle_of, quick_shots, screenshot, send, write_png)
from test_real_hair import add_numbers, open_glb, save_glb  # noqa: E402

TOMATO = (230, 20, 20)
MARK = (230, 0, 230)      # the splat left on it
DRIP = (0, 220, 230)
DROP = (20, 200, 20)
FEATHER = (30, 60, 240)
CHUNK = (120, 0, 200)
SNOW = (140, 90, 40)      # the snow left on it
PUFF = (250, 250, 250)


def near(colour, tol=45):
    return lambda px: all(abs(a - b) <= tol for a, b in zip(px, colour))


def block_picture(path, w, h, colour):
    """A picture all of `colour` (a drip: as tall as it's drawn)."""
    write_png(path, w, h, [bytes(colour) * w for _ in range(h)])


@pytest.fixture(scope="module")
def run():
    o = Obs()
    yield o
    if o.proc.poll() is None:
        o.close()
    if not os.environ.get("DIREBOT_OBS_KEEP"):
        shutil.rmtree(o.home, ignore_errors=True)


def pictures(home):
    files = {}
    for name, colour, size in (("tomato", TOMATO, 64), ("mark", MARK, 96), ("drop", DROP, 32),
                               ("feather", FEATHER, 32), ("chunk", CHUNK, 32), ("snow", SNOW, 96),
                               ("puff", PUFF, 64), ("pillow", (240, 240, 120), 64),
                               ("snowball", (200, 220, 255), 64)):
        files[name] = os.path.join(home, f"fx-{name}.png")
        disc_picture(files[name], size, colour)
    files["drip"] = os.path.join(home, "fx-drip.png")
    block_picture(files["drip"], 8, 32, DRIP)
    return files


def items(f):
    return json.dumps([
        {"file": f["tomato"], "hit": {"effect": "splat", "mark": f["mark"], "drip": f["drip"],
                                      "bits": [f["drop"]], "seconds": 2}},
        {"file": f["pillow"], "hit": {"effect": "feathers", "bits": [f["feather"]]}},
        {"file": f["snowball"], "hit": {"effect": "pieces", "mark": f["snow"], "bits": [f["chunk"]],
                                        "puff": f["puff"], "seconds": 1}},
        {"file": f["tomato"]},
        {"file": f["tomato"], "hit": {"effect": "splat", "mark": os.path.join(f["mark"] + "-gone.png")}},
    ])


def test_what_things_do_as_they_hit_the_face_avatar(run):
    """A tomato's splat stays on the character only (never past its edge),
    juice flies off and falls, drips run down, and it slides off and fades
    in its time; a pillow's feathers float down slowly; a snowball bursts
    into chunks and a puff, its snow left a moment; a thing with none of
    these does as before; a picture that isn't there is said."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    body_png = os.path.join(run.home, "fx-character.png")
    character_picture(body_png)
    f = pictures(run.home)
    size = 300
    settings = {"source": "Fx camera", "width": size, "height": size, "size": 85, "mirror": False,
                "smoothing": 100, "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "bend": 0,
                **STILL, "character": body_png, "style": "drawn", "spots": json.dumps(SPOTS),
                "react_items": items(f)}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st():
        return vendor(c, "FaceGetState", {"source": "Fx"})["react"]

    def cue(**data):
        return vendor(c, "FaceReact", {"source": "Fx", **data})
    orange = lambda px: px[0] > 170 and 90 < px[1] < 160 and px[2] < 90  # noqa: E731
    try:
        send(c, "CreateScene", {"sceneName": "Effects"})
        send(c, "CreateInput", {"sceneName": "Effects", "inputName": "Fx camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Effects", "inputName": "Fx",
                                "inputKind": "direbot_face_avatar", "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Effects"})
        assert wait_for(lambda: vendor(c, "FaceGetState", {"source": "Fx"})["shown"] == 1, 30, 0.2)
        r = st()
        assert r["items"][0] == {"loaded": True, "hit": {"effect": "splat", "loaded": True}}, r["items"]
        assert r["items"][1] == {"loaded": True, "hit": {"effect": "feathers", "loaded": True}}
        assert r["items"][2] == {"loaded": True, "hit": {"effect": "pieces", "loaded": True}}
        assert r["items"][3] == {"loaded": True}
        assert r["items"][4] == {"loaded": True, "hit": {"effect": "splat", "loaded": False}}
        assert r["marks"] == 0 and r["bits"] == 0
        rest = screenshot(c, "Fx")
        # The character: an orange ball - its middle and how far it goes.
        ox, oy, on = middle_of(rest, orange)
        radius = math.sqrt(on / math.pi)
        print(f"the character: middle ({ox:.0f}, {oy:.0f}), {radius:.0f} across")
        assert on > 10000

        # The tomato: a splat left on it, juice flying off, drips running.
        t = cue(do="throw", item=0, side=1, size=1.6, strength=1)
        assert t["ok"], t
        shots = quick_shots(c, "Fx", 1.6)
        mark = [(at, middle_of(rows, near(MARK))) for at, rows in shots]
        juice = [(at, middle_of(rows, near(DROP))) for at, rows in shots]
        drips = [(at, rows) for at, rows in shots if middle_of(rows, near(DRIP))[2] > 4]
        marked = [at for at, (_, _, n) in mark if n > 200]
        flew = [(at, y) for at, (_, y, n) in juice if n > 10]
        print(f"splat: on in {len(marked)} of {len(shots)} looks (from {marked[:1]} s), juice in "
              f"{len(flew)}, drips in {len(drips)}")
        assert marked and marked[0] < 0.75 and len(marked) > len(shots) * 0.4, marked
        assert flew and len(flew) < len(shots) * 0.7, flew
        assert drips, "no drips"
        # The drips run down: their lowest point lower later.
        def lowest(rows):
            ys = [y for y, row in enumerate(rows) for px in row if near(DRIP)(px)]
            return max(ys) if ys else None
        first_low, last_low = lowest(drips[0][1]), lowest(drips[-1][1])
        print(f"drip foot from y {first_low} to {last_low}")
        assert last_low > first_low + 3, (first_low, last_low)
        # Never in the air round it: every splat pixel is on the orange ball.
        best = max(shots, key=lambda s: middle_of(s[1], near(MARK))[2])[1]
        keep_shot(best, "Splat on the face avatar")
        outside = [(x, y) for y, row in enumerate(best) for x, px in enumerate(row)
                   if near(MARK)(px) and math.hypot(x - ox, y - oy) > radius + 2]
        assert not outside, outside[:5]
        # Its stay (2 s here), then sliding off and fading: gone.
        assert wait_for(lambda: st()["marks"] == 0, 4, 0.1), st()
        assert middle_of(screenshot(c, "Fx"), near(MARK))[2] == 0
        assert wait_for(lambda: not st()["busy"], 3, 0.1), st()

        # The pillow: feathers float down slowly, swaying, for a few seconds.
        assert cue(do="throw", item=1, side=-1, **{"from": "above"})["ok"]
        time.sleep(0.75)
        ys = []
        for _ in range(4):
            _, y, n = middle_of(screenshot(c, "Fx"), near(FEATHER, 60))
            if n > 20:
                ys.append(y)
            time.sleep(0.35)
        print(f"feathers' middle going down: {[round(y) for y in ys]}")
        assert len(ys) >= 3 and ys[-1] > ys[0] + 2, ys
        # Slowly: not more than about half a head a second.
        assert (ys[-1] - ys[0]) / (0.35 * (len(ys) - 1) + 0.2) < radius, ys
        assert wait_for(lambda: st()["bits"] == 0, 5, 0.1), st()

        # The snowball: crushed, bursting into chunks and a puff, its snow
        # left a moment (1 s here).
        assert cue(do="throw", item=2, side=1)["ok"]
        shots = quick_shots(c, "Fx", 1.4)
        chunks = [at for at, rows in shots if middle_of(rows, near(CHUNK))[2] > 15]
        snow = [at for at, rows in shots if middle_of(rows, near(SNOW))[2] > 100]
        puffs = [at for at, rows in shots if middle_of(rows, near(PUFF, 10))[2] > 60]
        print(f"snowball: chunks in {len(chunks)}, snow left in {len(snow)}, puff in {len(puffs)} of {len(shots)}")
        assert chunks and snow and puffs
        keep_shot(max(shots, key=lambda s: middle_of(s[1], near(CHUNK))[2])[1], "Snowball pieces")
        assert wait_for(lambda: st()["marks"] == 0 and st()["bits"] == 0, 4, 0.1), st()

        # A thing with nothing for its hit: as before (no marks, no bits).
        assert cue(do="throw", item=3, side=1)["ok"]
        seen_any = False
        end = time.time() + 1.5
        while time.time() < end:
            now = st()
            seen_any = seen_any or now["marks"] or now["bits"]
            time.sleep(0.05)
        assert not seen_any
        # Its splat's picture isn't there: thrown all the same, nothing left.
        assert cue(do="throw", item=4, side=1)["ok"]
        assert wait_for(lambda: st()["hits"] >= 5, 3, 0.05)
        assert middle_of(screenshot(c, "Fx"), near(MARK))[2] == 0

        # Stop: what's left on it goes at once.
        assert cue(do="throw", item=0, count=4)["ok"]
        assert wait_for(lambda: st()["marks"] > 0, 3, 0.05), st()
        assert cue(do="stop")["ok"]
        assert wait_for(lambda: st()["marks"] == 0 and st()["bits"] == 0, 1.5, 0.05), st()
    finally:
        c.disconnect()


def test_throws_filter_on_a_camera(run):
    """"DireBot Throws" on a camera: asleep it leaves the camera exactly as
    it is; thrown at, it finds your face and the tomato splats on it, the
    camera jolting without ever showing an edge; with the jolt off the
    room stays put; it asks for no character and does a face avatar's
    things only as a camera can; then it goes back to sleep."""
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    f = pictures(run.home)
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    who = {"source": "Throw cam", "filter": "DireBot Throws"}

    def st():
        return vendor(c, "FaceGetState", who)

    def cue(**data):
        return vendor(c, "FaceReact", {**who, **data})
    grey = lambda px: all(abs(v - 96) <= 6 for v in px)  # noqa: E731 - the room
    try:
        send(c, "CreateScene", {"sceneName": "Throws"})
        send(c, "CreateInput", {"sceneName": "Throws", "inputName": "Throw cam",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Throws"})
        time.sleep(0.5)
        plain = screenshot(c, "Throw cam")
        send(c, "CreateSourceFilter", {"sourceName": "Throw cam", "filterName": "DireBot Throws",
                                       "filterKind": "direbot_throws_filter",
                                       "filterSettings": {"react_items": items(f),
                                                          # Never a character, a phone or a mic:
                                                          "character": face_png, "face_from": "phone",
                                                          "worn": True, "mirror": True}})
        assert wait_for(lambda: st()["react"]["items"][0].get("hit", {}).get("loaded"), 10, 0.1), st()
        s = st()
        assert s["camera"]["awake"] is False and s["camera"]["shake"] is True, s["camera"]
        assert s["face_from"] == "camera", s["face_from"]
        # Asleep: the camera exactly as it was.
        time.sleep(0.3)
        assert screenshot(c, "Throw cam") == plain
        # Not one that's there: said.
        bad = vendor(c, "FaceReact", {"source": "Throw cam", "filter": "Nope", "do": "stop"})
        assert not bad["ok"] and "isn't in OBS" in bad["error"], bad
        no = cue(do="companion", action="come")
        assert not no["ok"] and "camera" in no["error"], no

        # Thrown at: it wakes, finds the face, the tomato splats on it.
        t = cue(do="throw", item=0, side=1, size=1, strength=1)
        assert t["ok"], t
        shots = quick_shots(c, "Throw cam", 1.4)
        cam = st()["camera"]
        print(f"the face in the camera: {cam}")
        assert cam["awake"] and cam["seen"] and cam["face"], cam
        # (The photo's face is in the camera's left half, near the top.)
        assert 300 < cam["x"] < 600 and 100 < cam["y"] < 400 and 120 < cam["head"] < 500, cam
        marked = [(at, middle_of(rows, near(MARK))) for at, rows in shots]
        marked = [(at, x, y) for at, (x, y, n) in marked if n > 200]
        assert marked, "no splat"
        at, x, y = marked[len(marked) // 2]
        print(f"splat at ({x:.0f}, {y:.0f}), {at:.2f} s after the throw")
        assert math.hypot(x - cam["x"], y - cam["y"]) < cam["head"] * 0.5, (x, y, cam)
        keep_shot(max(shots, key=lambda s: middle_of(s[1], near(MARK))[2])[1], "Splat on the camera")
        # The jolt: the room moves, but no edge ever shows (each corner is
        # still the room).
        moved = False
        for _, rows in shots:
            h, w = len(rows), len(rows[0])
            for px in (rows[0][0], rows[0][w - 1], rows[h - 1][0], rows[h - 1][w - 1]):
                assert grey(px), px
            moved = moved or rows != plain
        assert moved
        # Back to sleep once it's all over: the camera exactly as it was.
        assert wait_for(lambda: not st()["camera"]["awake"], 9, 0.2), st()["camera"]
        time.sleep(0.3)
        assert screenshot(c, "Throw cam") == plain

        # The jolt off: the room stays exactly where it is (only the thing
        # thrown and what it leaves change it).
        send(c, "SetSourceFilterSettings", {"sourceName": "Throw cam", "filterName": "DireBot Throws",
                                            "filterSettings": {"camera_shake": False}})
        assert wait_for(lambda: st()["camera"]["shake"] is False, 3, 0.1)
        assert cue(do="throw", item=3, side=-1, strength=2)["ok"]
        shots = quick_shots(c, "Throw cam", 1.0)
        far = [(2, 2), (w - 3, h - 3), (w - 3, 2), (2, h - 3)]
        for _, rows in shots:
            for x0, y0 in far:
                assert rows[y0][x0] == plain[y0][x0]
        assert wait_for(lambda: not st()["camera"]["awake"], 9, 0.2)
        assert screenshot(c, "Throw cam") == plain
    finally:
        c.disconnect()


def test_a_face_avatar_on_that_camera_sees_it_plain_and_3d_things_land(run):
    """A face avatar worn over the camera that has the throws filter gets
    the camera as it is - never the tomato or the jolt (they're the
    camera's own: a tomato over your face would hide it from the face
    avatar's tracking) - while the camera itself shows them; and a 3D
    thing thrown at the camera is drawn there too."""
    from test_real_face import ball_points, write_glb
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    body_png = os.path.join(run.home, "fx-character.png")
    character_picture(body_png)
    f = pictures(run.home)
    ball = os.path.join(run.home, "fx-ball.glb")
    write_glb(ball, [ball_points(0, 0, 0, 0.1, 0.1, 0.1) + ((0.02, 0.75, 0.02), 0.5)])
    things = json.loads(items(f)) + [{"file": ball}]
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    who = {"source": "Shared cam", "filter": "DireBot Throws"}

    def cam_st():
        return vendor(c, "FaceGetState", who)

    def cue(**data):
        return vendor(c, "FaceReact", {**who, **data})
    green = lambda px: px[1] > 120 and px[0] < 90 and px[2] < 90  # noqa: E731
    try:
        send(c, "CreateScene", {"sceneName": "Shared"})
        send(c, "CreateInput", {"sceneName": "Shared", "inputName": "Shared cam",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Shared", "inputName": "Worn fx",
                                "inputKind": "direbot_face_avatar",
                                "inputSettings": {"source": "Shared cam", "width": 480, "height": 270,
                                                  "size": 100, "mirror": False, "smoothing": 100,
                                                  "follow_move": 0, "follow_grow": 0, "follow_tilt": 0,
                                                  "bend": 0, **STILL, "character": body_png,
                                                  "spots": json.dumps(SPOTS), "worn": True}})
        send(c, "SetCurrentProgramScene", {"sceneName": "Shared"})
        assert wait_for(lambda: vendor(c, "FaceGetState", {"source": "Worn fx"})["found"], 30, 0.2)
        time.sleep(1.0)
        plain_cam = screenshot(c, "Shared cam")
        send(c, "CreateSourceFilter", {"sourceName": "Shared cam", "filterName": "DireBot Throws",
                                       "filterKind": "direbot_throws_filter",
                                       "filterSettings": {"react_items": json.dumps(things),
                                                          "camera_shake": True}})
        assert wait_for(lambda: cam_st()["react"]["items"][0].get("hit", {}).get("loaded"), 10, 0.1)
        # (Its 3D thing on the graphics card and drawn once out of sight:
        # the 3D effect is built for that, a long frame - not while the
        # throws are watched.)
        assert wait_for(lambda: cam_st()["react"]["things"]["loaded"] == 1
                        and cam_st()["react"]["things"]["to_warm"] == 0, 10, 0.1), cam_st()["react"]
        rest = screenshot(c, "Worn fx")
        # A barrage of tomatoes at the camera: the camera shows them, the
        # worn face avatar over it never does.
        assert cue(do="throw", item=0, side=1, count=4, strength=2)["ok"]
        cam_marked = avatar_marked = 0
        avatar_moved = 0
        # (On a slow machine one look takes over a second, and the first is
        # before anything has hit: it looks on until the splat has shown.)
        end = time.time() + 1.6
        while time.time() < end or (cam_marked <= 200 and time.time() < end + 6):
            cam_marked = max(cam_marked, middle_of(screenshot(c, "Shared cam"), near(MARK))[2])
            shot = screenshot(c, "Worn fx")
            avatar_marked = max(avatar_marked, middle_of(shot, near(MARK))[2])
            avatar_marked = max(avatar_marked, middle_of(shot, near(TOMATO, 30))[2])
            avatar_moved = max(avatar_moved, sum(
                1 for y in range(0, len(shot), 6) for x in range(0, len(shot[0]), 6)
                if any(abs(a - b) > 24 for a, b in zip(shot[y][x], rest[y][x]))))
        print(f"the camera's splat {cam_marked} px; the worn avatar's {avatar_marked} px, "
              f"{avatar_moved} spots changed")
        assert cam_marked > 200
        assert avatar_marked == 0
        # (Its picture is the camera as it is: no jolt in it either - the
        # odd pixel aside.)
        assert avatar_moved < 20
        assert vendor(c, "FaceGetState", {"source": "Worn fx"})["found"]
        assert screenshot(c, "Shared cam") != plain_cam or cam_st()["camera"]["awake"]
        assert wait_for(lambda: not cam_st()["camera"]["awake"], 12, 0.2)

        # A 3D thing at the camera: drawn there.
        r = cam_st()["react"]["items"][5]
        assert wait_for(lambda: cam_st()["react"]["items"][5].get("loaded"), 10, 0.1), r
        assert cue(do="throw", item=5, side=-1, size=2)["ok"]
        shots = quick_shots(c, "Shared cam", 1.0)
        seen = max(middle_of(rows, green)[2] for _, rows in shots)
        print(f"the 3D ball at the camera: {seen} px at most")
        assert seen > 60
        keep_shot(max(shots, key=lambda s: middle_of(s[1], green)[2])[1], "3D thing at the camera")
        assert cue(do="stop")["ok"]
    finally:
        c.disconnect()


def read_png(data):
    """A PNG (8 bits, RGB or RGBA) as rows of (r, g, b, a)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    at, idat, w = 8, bytearray(), 0
    while at < len(data):
        n, kind = struct.unpack_from(">I4s", data, at)
        body = data[at + 8:at + 8 + n]
        if kind == b"IHDR":
            w, h, depth, colour = struct.unpack_from(">IIBB", body)
            assert depth == 8 and colour in (2, 6), (depth, colour)
            step = 4 if colour == 6 else 3
        elif kind == b"IDAT":
            idat += body
        at += 12 + n
    raw, line = zlib.decompress(bytes(idat)), w * step
    rows, prev = [], bytearray(line)
    for y in range(h):
        kind, cur = raw[y * (line + 1)], bytearray(raw[y * (line + 1) + 1:(y + 1) * (line + 1)])
        for i in range(line):
            a = cur[i - step] if i >= step else 0
            b, c = prev[i], prev[i - step] if i >= step else 0
            if kind == 1:
                cur[i] = (cur[i] + a) & 255
            elif kind == 2:
                cur[i] = (cur[i] + b) & 255
            elif kind == 3:
                cur[i] = (cur[i] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(cur[x * step:x * step + step]) + ((255,) if step == 3 else ()) for x in range(w)])
        prev = cur
    return rows


def test_a_3d_thing_fading_shows_only_its_front(run):
    """0.12.0: a solid 3D thing fading is evenly see-through - its front
    alone, never its own insides or far side through it (a pinecone's
    inner scales, a snowball's back). A red ball with a green ball inside
    it, as the puffs a hit sends rising (each fading in, then slowly out:
    over a second or more see-through), big enough to rise past the
    character's edge into the clear air round it - where the picture's
    alpha is the thing's own: the red is caught part see-through there,
    the green inside it never shows."""
    from test_real_face import ball_points, write_glb
    face_png = os.path.join(run.home, "camera.png")
    if not os.path.exists(face_png):
        camera_picture(face_png)
    body_png = os.path.join(run.home, "fx-character.png")
    character_picture(body_png)
    f = pictures(run.home)
    ball = os.path.join(run.home, "fx-ball-in-ball.glb")
    # (The inside one first in the file: drawn first, it would show through
    # a red one blended over it.)
    write_glb(ball, [ball_points(0, 0, 0, 0.075, 0.075, 0.075) + ((0.02, 0.75, 0.02), 0.5),
                     ball_points(0, 0, 0, 0.1, 0.1, 0.1) + ((0.8, 0.02, 0.02), 0.5)])
    things = [{"file": f["tomato"], "hit": {"effect": "none",
                                            "pieces": [{"file": ball, "way": "rise", "count": 4, "size": 0.6}]}}]
    settings = {"source": "Fade camera", "width": 240, "height": 240, "size": 60, "mirror": False,
                "smoothing": 100, "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "bend": 0,
                **STILL, "character": body_png, "style": "drawn", "spots": json.dumps(SPOTS),
                "react_items": json.dumps(things)}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st():
        return vendor(c, "FaceGetState", {"source": "Fade fx"})["react"]
    try:
        send(c, "CreateScene", {"sceneName": "Fade"})
        send(c, "CreateInput", {"sceneName": "Fade", "inputName": "Fade camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Fade", "inputName": "Fade fx",
                                "inputKind": "direbot_face_avatar", "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Fade"})
        assert wait_for(lambda: vendor(c, "FaceGetState", {"source": "Fade fx"})["shown"] == 1, 30, 0.2)
        assert wait_for(lambda: st()["items"][0].get("hit", {}).get("loaded"), 10, 0.1), st()

        def shot():
            data = send(c, "GetSourceScreenshot", {"sourceName": "Fade fx", "imageFormat": "png"})["imageData"]
            return base64.b64decode(data.split(",", 1)[1])
        rest = read_png(shot())
        # The clear air: well away from the character (a hit knocks its head
        # a little way), in blocks of 4 pixels.
        body = {(x // 4 + i, y // 4 + j) for y, row in enumerate(rest) for x, px in enumerate(row) if px[3]
                for i in range(-3, 4) for j in range(-3, 4)}
        clear = [[(x // 4, y // 4) not in body for x in range(len(row))] for y, row in enumerate(rest)]
        air = sum(map(sum, clear))
        assert air > 2000, air
        assert vendor(c, "FaceReact", {"source": "Fade fx", "do": "throw", "item": 0, "side": 1})["ok"]
        raw, t0 = [], time.time()
        while time.time() - t0 < 3.5:
            raw.append(shot())
        thin = solid = green = 0
        best = None
        for data in raw:
            rows = read_png(data)
            n_thin = 0
            for y, row in enumerate(rows):
                for x, (r, g, b, a) in enumerate(row):
                    if not clear[y][x] or a < 8:
                        continue
                    # (In the clear air: what's there is the thing alone. Red
                    # lit has as much green as blue in it - white light on it
                    # adds both alike; green from inside it shows as more.)
                    if g > b + 25:
                        green += 1
                    elif r > g + 60 and r > b + 60:
                        if a < 215:
                            n_thin += 1
                        else:
                            solid += 1
            thin += n_thin
            if best is None or n_thin > best[0]:
                best = (n_thin, rows)
        print(f"{len(raw)} shots: red in the air part see-through {thin} spots, whole {solid}; "
              f"green (its inside) {green}")
        if best and best[0]:
            keep_shot([[px[:3] for px in row] for row in best[1]], "A 3D thing fading")
        assert thin > 150, (thin, solid)
        assert green == 0
        assert wait_for(lambda: st()["pieces"] == 0, 4, 0.1), st()
    finally:
        c.disconnect()


def with_shapes(path, shapes):
    """Shapes (glTF's morph targets) on a .glb's first part: [(name, each
    point's move (x, y, z))]."""
    doc, blob = open_glb(path)
    part = doc["meshes"][0]["primitives"][0]
    part["targets"] = []
    for _, moves in shapes:
        raw = struct.pack("<%df" % (len(moves) * 3), *[c for m in moves for c in m])
        bounds = {"min": [min(m[k] for m in moves) for k in range(3)],
                  "max": [max(m[k] for m in moves) for k in range(3)]}
        part["targets"].append({"POSITION": add_numbers(doc, blob, raw, "VEC3", len(moves), 5126, more=bounds)})
    doc["meshes"][0]["weights"] = [0.0] * len(shapes)
    doc["meshes"][0]["extras"] = {"targetNames": [name for name, _ in shapes]}
    save_glb(path, doc, blob)


def card_things(home):
    """3D things for things_on_the_card: a red ball to throw, and three
    green pieces a hit throws off it (a piece's "open" shape shows all
    its life, unless it's a crown) - a ball as made; the same with an
    "open" shape that makes it three times as big; and that one again
    with its "open" shape second (the first, were it shown, would shrink
    it to nothing). Their files, and how many points they have in all."""
    from test_real_face import ball_points, write_glb
    files = {name: os.path.join(home, f"card-{name}.glb") for name in ("ball", "made", "opens", "opens-second")}
    ball = ball_points(0, 0, 0, 0.1, 0.1, 0.1)
    write_glb(files["ball"], [ball + ((0.8, 0.02, 0.02), 0.5)])
    piece = ball_points(0, 0, 0, 0.05, 0.05, 0.05)
    grow = [(2 * x, 2 * y, 2 * z) for x, y, z in piece[0]]
    gone = [(-x, -y, -z) for x, y, z in piece[0]]
    for name in ("made", "opens", "opens-second"):
        write_glb(files[name], [piece + ((0.02, 0.75, 0.02), 0.5)])
    with_shapes(files["opens"], [("open", grow)])
    with_shapes(files["opens-second"], [("spare", gone), ("open", grow)])
    return files, len(ball[0]) + 3 * len(piece[0])


def card_items(files):
    return [{"file": files["ball"], "hit": {"effect": "none", "pieces": [
        {"file": files[piece], "way": "chunk", "count": 2, "size": 0.2}]}}
        for piece in ("made", "opens", "opens-second")]


def things_on_the_card(c, source, files, points, shots, log=lambda: "", wait_free=True):
    """0.12.0's speed fix, for a face avatar `source` that has card_items:
    3D things are sent to the graphics card once, as they're loaded, and
    drawn once out of sight then; thrown, they're put where they are by
    the card - its shapes and all - with nothing more sent; and the
    pictures they're drawn into are let go of when none has been thrown
    for a while. `shots(seconds)`: pictures of the source for that long,
    rows of (r, g, b). (Shared with tests/test_real_windows.py: the same
    on OpenGL and Direct3D.)"""
    def things():
        return vendor(c, "FaceGetState", {"source": source})["react"]["things"]

    def react():
        return vendor(c, "FaceGetState", {"source": source})["react"]

    def green(rows):
        return sum(1 for row in rows for px in row if px[1] > px[0] + 50 and px[1] > px[2] + 50)

    def red(rows):
        return sum(1 for row in rows for px in row if px[0] > px[1] + 90 and px[0] > px[2] + 90)

    assert wait_for(lambda: all(i.get("loaded") and i.get("hit", {}).get("loaded")
                                for i in react()["items"]) and len(react()["items"]) == 3, 30, 0.1), (react(), log())
    # On the card: all four files' points, each drawn once out of sight.
    assert wait_for(lambda: things()["loaded"] == 4 and things()["to_warm"] == 0, 30, 0.1), (things(), log())
    was = things()
    print("things:", was)
    assert was["points"] == points and was["triangles"] > 0 and was["bytes"] > points * 40, was
    assert was["warm"] is True and was["warmed"] == 4, was
    sent = was["points_sent"]
    assert sent >= points, was
    seen = {}
    for item, piece in enumerate(("made", "opens", "opens-second")):
        # (Still: nothing in the air or coming off it.)
        assert wait_for(lambda: react()["flying"] == 0 and react()["pieces"] == 0, 10, 0.1), react()
        assert vendor(c, "FaceReact", {"source": source, "do": "throw", "item": item, "side": 1})["ok"]
        rows = shots(2.2)
        seen[piece] = (max(red(r) for r in rows), max(green(r) for r in rows))
    print("the most seen of the ball and its piece (red, green):", seen)
    # The ball flew in, each time; a piece came off it.
    for piece, (ball, bit) in seen.items():
        assert ball > 150, (piece, seen, log())
        assert bit > 40, (piece, seen, log())
    # Its "open" shape shows: three times as big across, so about nine
    # times as much of it seen (each piece flies its own way, some nearer:
    # give or take) - whichever of its shapes "open" is, and the one
    # before it not shown (it would be gone).
    made, opens, second = seen["made"][1], seen["opens"][1], seen["opens-second"][1]
    assert 4 * made < opens < 16 * made, (seen, log())
    assert 4 * made < second < 16 * made, (seen, log())
    # Nothing more was sent to the card for any of that.
    now = things()
    assert now["points_sent"] == sent, (was, now)
    assert now["drawn_into_bytes"] > 0, now
    assert "aren't drawn" not in log()
    if not wait_free:
        return
    # A while after the last of them, the pictures they were drawn into
    # are let go of - and made again when the next one's thrown.
    assert wait_for(lambda: things()["drawn_into_bytes"] == 0, 45, 0.5), things()
    assert vendor(c, "FaceReact", {"source": source, "do": "throw", "item": 1, "side": -1})["ok"]
    rows = shots(2.2)
    again = (max(red(r) for r in rows), max(green(r) for r in rows))
    print("thrown again (red, green):", again)
    assert again[0] > 150 and 4 * made < again[1] < 16 * made, (again, seen, log())
    assert things()["drawn_into_bytes"] > 0 and things()["points_sent"] == sent, things()


def test_3d_things_stay_on_the_graphics_card(run):
    """0.12.0's speed fix: see things_on_the_card."""
    face_png = os.path.join(run.home, "camera.png")
    body_png = os.path.join(run.home, "character.png")
    camera_picture(face_png)
    character_picture(body_png)
    files, points = card_things(run.home)
    settings = {"source": "Card camera", "width": 300, "height": 300, "size": 60, "mirror": False,
                "smoothing": 100, "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "bend": 0,
                **STILL, "character": body_png, "style": "drawn", "spots": json.dumps(SPOTS),
                "react_items": json.dumps(card_items(files))}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
    try:
        send(c, "CreateScene", {"sceneName": "Card"})
        send(c, "CreateInput", {"sceneName": "Card", "inputName": "Card camera",
                                "inputKind": "image_source", "inputSettings": {"file": face_png}})
        send(c, "CreateInput", {"sceneName": "Card", "inputName": "Card fx",
                                "inputKind": "direbot_face_avatar", "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Card"})
        assert wait_for(lambda: vendor(c, "FaceGetState", {"source": "Card fx"})["shown"] == 1, 30, 0.2)

        def shots(seconds):
            got = [rows for _, rows in quick_shots(c, "Card fx", seconds)]
            keep_shot(max(got, key=lambda rows: sum(1 for row in rows for px in row
                                                    if px[1] > px[0] + 50 and px[1] > px[2] + 50)),
                      "A piece off a 3D thing")
            return got
        things_on_the_card(c, "Card fx", files, points, shots)
    finally:
        c.disconnect()
