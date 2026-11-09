"""DireBot for OBS 0.9.0 in a real OBS: what things do as they hit the face
avatar (a splat left on it - only on the character, never the air round
it - with juice thrown off and drips running down; feathers floating down;
pieces and a puff bursting off, some snow left a moment), and "DireBot
Throws", the filter on a camera that throws land on your real face in.

Run as tests/test_real_obs.py says (DIREBOT_OBS_PLUGIN=... python3 -m pytest
tests/test_real_throws.py); DIREBOT_FACE_SHOTS=<folder> keeps the pictures.
"""

import json
import math
import os
import shutil
import time

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_obs import Obs, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark  # noqa: E402,F401
from test_real_face import (STILL, SPOTS, camera_picture, character_picture, disc_picture,  # noqa: E402
                            keep_shot, middle_of, quick_shots, screenshot, send, write_png)

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
        rest = screenshot(c, "Worn fx")
        # A barrage of tomatoes at the camera: the camera shows them, the
        # worn face avatar over it never does.
        assert cue(do="throw", item=0, side=1, count=4, strength=2)["ok"]
        cam_marked = avatar_marked = 0
        avatar_moved = 0
        end = time.time() + 1.6
        while time.time() < end:
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
