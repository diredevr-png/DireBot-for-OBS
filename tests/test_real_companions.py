"""
0.9.0 inside a real OBS: a companion beside the 3D character - a pet of
its own file that trots in, sits at its side (on its floor, sized to it,
mirrored for its other side), is patted by the headpat's 3D hand, and
trots off again, the camera gliding out for it and back; it stays when
the character changes. And a move that stays: the character's own
animation played once, then another as its idle (a ride, a mount kept
hidden in its file until then) until a move back - its companion bolting,
scared, as it starts. Over the made-up files in tests/data/companion.

Skipped unless OBS, Xvfb and the test build are there (see
tests/test_real_face.py).
"""

import json
import os
import shutil
import struct
import time

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_obs import HERE, Obs, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark  # noqa: E402,F401
from test_real_face import STILL, ball_points, camera_picture, middle_of, screenshot, send, write_glb  # noqa: E402

DATA = os.path.join(HERE, "tests", "data", "companion")
WOLF = os.path.join(DATA, "wolf-test.glb")
RIDER = os.path.join(DATA, "dragon-test.vrm")
W, H = 600, 300


@pytest.fixture(scope="module")
def run():
    o = Obs()
    yield o
    if o.proc.poll() is None:
        o.close()
    if not os.environ.get("DIREBOT_OBS_KEEP"):
        shutil.rmtree(o.home, ignore_errors=True)


def drawn(px):
    return px != (0, 0, 0)


def columns(rows, x0=0, x1=W):
    """The columns with anything drawn in them, between x0 and x1."""
    return [x for x in range(x0, x1) if any(drawn(rows[y][x]) for y in range(len(rows)))]


def runs(xs, gap=3):
    """Columns in runs (start, end), split where `gap` or more are empty."""
    out = []
    for x in xs:
        if out and x - out[-1][1] <= gap:
            out[-1][1] = x
        else:
            out.append([x, x])
    return [tuple(r) for r in out]


def width_of(rows):
    xs = columns(rows)
    return (xs[-1] - xs[0]) if xs else 0


def test_a_companion_comes_sits_is_patted_and_goes(run):
    camera = os.path.join(run.home, "camera.png")
    if not os.path.exists(camera):
        camera_picture(camera)
    hand = os.path.join(run.home, "companion-hand.glb")
    write_glb(hand, [ball_points(0.1, 0.015, 0, 0.12, 0.02, 0.045) + ((0.8, 0.8, 0.8), 0.6)],
              materials_more={0: {"name": "skin"}})
    settings = {"source": "Companion camera", "width": W, "height": H, "size": 60, "framing": "whole",
                "model": RIDER, "animation": "idle", "mirror": False, "smoothing": 100, "follow_move": 0,
                "follow_grow": 0, "follow_tilt": 0, "bend": 0, "body_follow": 0, "style": "drawn", **STILL,
                "react_hand_3d": hand, "react_hand_tint": json.dumps({"skin": [1, 0.02, 0.02]})}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st():
        return vendor(c, "FaceGetState", {"source": "Companion"})

    def comp():
        return st()["companion"]

    def cue(**data):
        return vendor(c, "FaceReact", {"source": "Companion", **data})
    red = lambda px: px[0] > 90 and px[0] > 2 * px[1] and px[0] > 2 * px[2]  # noqa: E731
    try:
        send(c, "CreateScene", {"sceneName": "Companions"})
        send(c, "CreateInput", {"sceneName": "Companions", "inputName": "Companion camera",
                                "inputKind": "image_source", "inputSettings": {"file": camera}})
        send(c, "CreateInput", {"sceneName": "Companions", "inputName": "Companion",
                                "inputKind": "direbot_face_avatar", "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Companions"})
        assert wait_for(lambda: st()["model"]["shown"] and st()["react"]["hand_3d"], 30, 0.2), st()
        assert comp()["file"] == "" and comp()["phase"] == "away" and not comp()["shown"], comp()
        time.sleep(0.3)
        alone = screenshot(c, "Companion")
        own = columns(alone)
        print(f"the character alone: columns {own[0]}-{own[-1]}")
        assert own[0] > W * 0.4 and own[-1] < W * 0.6, (own[0], own[-1])
        assert not columns(alone, 0, int(W * 0.4))

        # Not a companion: said.
        assert not cue(do="companion", action="come")["ok"]
        assert not cue(do="companion", action="come", file=camera)["ok"]
        # Patted before it's here: not.
        assert cue(do="companion", action="pet")["pets"] is False

        # It comes: loaded on its own thread, its animations sorted out by
        # their names, its headpat spot its "pat" node - trotting in from
        # the picture's left to sit at the character's side.
        answer = cue(do="companion", action="come", file=WOLF,
                     place={"size": 0.7, "side": -1, "distance": 0.45},
                     view={"zoom": 0.8, "x": 0.05})
        assert answer["ok"] and answer["came"], answer
        assert wait_for(lambda: comp()["loaded"] and comp()["phase"] == "coming", 20, 0.05), comp()
        cs = comp()
        assert cs["clips"] == {"come": "come", "idle": "idle", "pet": "pet", "go": "go", "scared": "scared"}, cs
        assert cs["pat"] == "pat" and cs["busy"] and cs["camera"], cs
        assert [a["name"] for a in cs["animations"]] == ["come", "idle", "pet", "go", "scared"], cs
        # The camera's cue is picked up on the next frame drawn.
        assert wait_for(lambda: st()["camera_view"]["cue"] == "companion", 1, 0.02), st()["camera_view"]
        lefts = []
        until = time.time() + 2.6
        while time.time() < until:
            xs = columns(screenshot(c, "Companion"), 0, own[0] - 4)
            if xs:
                lefts.append(xs[-1])
            time.sleep(0.1)
        print(f"coming in: its right edge at {lefts}")
        assert len(lefts) >= 3 and lefts[-1] > lefts[0] + 10, lefts
        assert pytest.approx(sorted(lefts), abs=1) == lefts  # only ever further in
        assert cue(do="companion", action="pet")["pets"] is False  # still coming
        assert wait_for(lambda: comp()["phase"] == "out", 3, 0.05), comp()
        assert not cue(do="companion", action="come", file=WOLF)["came"]  # here already
        time.sleep(0.8)  # the camera's glide
        here = screenshot(c, "Companion")
        assert not st()["camera_view"]["gliding"]
        xs = columns(here, 0, int(W * 0.45))
        ys = [y for y in range(H) if any(drawn(here[y][x]) for x in xs)]
        print(f"sitting: columns {xs[0]}-{xs[-1]}, rows {ys[0]}-{ys[-1]}; height {comp()['height']}")
        assert xs and xs[0] > 5, xs[:3]
        # On the character's floor: its bottom where the character's is.
        feet = max(y for y in range(H) if any(drawn(px) for px in here[y][int(W * 0.45):]))
        assert abs(ys[-1] - feet) <= 3, (ys[-1], feet)
        assert comp()["drawn"] and 0.5 < comp()["height"] / 0.6 < 1.5, comp()

        # Patted: the 3D hand (red) on its head, not the character's.
        parts = runs(columns(here))
        print(f"sitting beside it: {parts}")
        assert len(parts) == 2, parts  # it and the character, apart
        beside = parts[1][0]
        answer = cue(do="companion", action="pet", count=3)
        assert answer["pets"], answer
        assert wait_for(lambda: comp()["patting"] and comp()["petting"], 2, 0.05), comp()
        seen = 0
        for _ in range(12):
            rows = screenshot(c, "Companion")
            x, y, n = middle_of(rows, red)
            if n > 15:
                seen += 1
                assert x < beside, (x, beside)
                assert y < (ys[0] + ys[-1]) / 2, (y, ys)
            time.sleep(0.1)
        assert seen >= 3, seen
        assert wait_for(lambda: not comp()["patting"], 8, 0.1), comp()

        # The character changes: it stays, beside the new one.
        other = os.path.join(run.home, "rider-copy.vrm")
        shutil.copy(RIDER, other)
        send(c, "SetInputSettings", {"inputName": "Companion", "inputSettings": {"model": other}})
        assert wait_for(lambda: st()["model"]["file"] == other and st()["model"]["shown"], 20, 0.1), st()["model"]
        time.sleep(0.3)
        assert comp()["phase"] == "out" and comp()["drawn"], comp()
        assert columns(screenshot(c, "Companion"), 0, int(W * 0.45))

        # Its other side: mirrored, on the picture's right.
        assert cue(do="companion", action="place", file=WOLF, place={"side": 1, "distance": 0.45})["ok"]
        time.sleep(0.3)
        rows = screenshot(c, "Companion")
        assert not columns(rows, 0, int(W * 0.42))
        assert columns(rows, int(W * 0.58), W)

        # It goes: trotting off, the camera back half way, then gone.
        answer = cue(do="companion", action="go")
        assert answer["went"] and answer["phase"] == "going", answer
        assert not cue(do="companion", action="go")["went"]
        assert wait_for(lambda: st()["camera_view"]["cue"] == "", 3, 0.05), st()["camera_view"]
        assert comp()["phase"] == "going"
        assert wait_for(lambda: comp()["phase"] == "away", 4, 0.05), comp()
        time.sleep(0.8)
        rows = screenshot(c, "Companion")
        assert not comp()["drawn"]
        back = columns(rows)
        assert abs(back[0] - own[0]) <= 2 and abs(back[-1] - own[-1]) <= 2, (back[0], back[-1], own)

        # Straight there and straight off.
        assert cue(do="companion", action="come", file=WOLF, instant=True)["came"]
        assert wait_for(lambda: comp()["phase"] == "out", 2, 0.05), comp()
        assert cue(do="companion", action="go", instant=True)["went"]
        assert wait_for(lambda: comp()["phase"] == "away", 1, 0.02), comp()
        assert cue(do="companion", action="off")["ok"]
        assert wait_for(lambda: comp()["file"] == "" and not comp()["loaded"], 5, 0.1), comp()

        # Its own spot in the box (room made beside it), to a tenth of a
        # percent: moved over by that much of the picture, then back.
        send(c, "SetInputSettings", {"inputName": "Companion", "inputSettings": {"spot_x": 20.5}})
        time.sleep(0.5)
        moved = columns(screenshot(c, "Companion"))
        shift = (moved[0] + moved[-1] - own[0] - own[-1]) / 2
        print(f"spot_x 20.5%: moved {shift} px of {W}")
        assert abs(shift - 0.205 * W) <= 3, shift
        send(c, "SetInputSettings", {"inputName": "Companion", "inputSettings": {"spot_x": 0}})
        time.sleep(0.5)
        back = columns(screenshot(c, "Companion"))
        assert abs(back[0] - own[0]) <= 2 and abs(back[-1] - own[-1]) <= 2, (back[0], back[-1], own)
        send(c, "RemoveInput", {"inputName": "Companion"})
    finally:
        c.disconnect()


def test_a_move_that_stays_and_its_companion_bolting(run):
    camera = os.path.join(run.home, "camera.png")
    if not os.path.exists(camera):
        camera_picture(camera)
    settings = {"source": "Ride camera", "width": W, "height": H, "size": 60, "framing": "whole",
                "model": RIDER, "animation": "idle", "mirror": False, "smoothing": 100, "follow_move": 0,
                "follow_grow": 0, "follow_tilt": 0, "bend": 0, "body_follow": 0, "style": "drawn", **STILL}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st():
        return vendor(c, "FaceGetState", {"source": "Ride"})

    def cue(**data):
        return vendor(c, "FaceReact", {"source": "Ride", **data})
    try:
        send(c, "CreateScene", {"sceneName": "Rides"})
        send(c, "CreateInput", {"sceneName": "Rides", "inputName": "Ride camera",
                                "inputKind": "image_source", "inputSettings": {"file": camera}})
        send(c, "CreateInput", {"sceneName": "Rides", "inputName": "Ride",
                                "inputKind": "direbot_face_avatar", "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Rides"})
        assert wait_for(lambda: st()["model"]["shown"], 30, 0.2), st()
        assert st()["model"]["animation"] == "idle"
        assert st()["stay"] == {"on": False, "swapping": False, "idle": ""}
        framed = st()["model"]["framed"]
        time.sleep(0.3)
        # Its mount is hidden in its file at rest: not drawn.
        standing = width_of(screenshot(c, "Ride"))
        assert standing < W * 0.25, standing

        # Asked wrongly: said.
        assert not cue(do="stay", action="start", idle="nope")["ok"]
        assert not cue(do="stay", action="start", move="nope", idle="dragon_idle")["ok"]
        assert not cue(do="stay", action="start")["ok"]
        assert not cue(do="stay", action="sideways")["ok"]

        # Its companion beside it.
        assert cue(do="companion", action="come", file=WOLF, instant=True)["ok"]
        assert wait_for(lambda: st()["companion"]["phase"] == "out", 20, 0.05), st()["companion"]

        # The ride: its move to it, its companion bolting (scared) as it
        # starts, the camera pulling back for it; its riding idle swapped in
        # as the move ends, over and over.
        answer = cue(do="stay", action="start", move="dragon_come", idle="dragon_idle",
                     view={"zoom": 0.5}, companion="scared")
        assert answer["ok"] and answer["companion"] == "going", answer
        assert wait_for(lambda: st()["stay"]["on"] and st()["camera_view"]["cue"] == "stay", 2, 0.05), st()
        assert st()["stay"]["swapping"] and st()["model"]["animation"] == "idle"
        assert wait_for(lambda: st()["companion"]["phase"] == "away", 4, 0.05), st()["companion"]
        assert wait_for(lambda: not st()["stay"]["swapping"], 6, 0.05), st()["stay"]
        assert st()["stay"] == {"on": True, "swapping": False, "idle": "dragon_idle"}
        assert st()["model"]["animation"] == "dragon_idle"
        assert st()["model"]["framed"] == framed  # framed as it stands, still
        time.sleep(0.6)
        riding = width_of(screenshot(c, "Ride"))
        print(f"standing {standing} px wide, riding {riding} (zoomed out to half)")
        assert riding > standing * 1.6, (standing, riding)
        # A move over it, and back to riding.
        assert cue(do="move", move="dragon_pet")["kind"] == "own"
        assert wait_for(lambda: not st()["react"]["moving"], 5, 0.1), st()["react"]
        assert st()["model"]["animation"] == "dragon_idle"
        # The character changed meanwhile it would start on its own idle;
        # here it stays riding while its setting does.
        send(c, "SetInputSettings", {"inputName": "Ride", "inputSettings": {"floppy": 60}})
        time.sleep(0.3)
        assert st()["model"]["animation"] == "dragon_idle"

        # Back: its move back, its own idle swapped in again as it ends, the
        # camera back as the move starts, its mount hidden again.
        assert cue(do="stay", action="end", move="dragon_go")["ok"]
        assert wait_for(lambda: st()["camera_view"]["cue"] == "", 2, 0.05), st()["camera_view"]
        assert wait_for(lambda: not st()["stay"]["swapping"], 6, 0.05), st()["stay"]
        assert st()["stay"]["on"] is False and st()["model"]["animation"] == "idle"
        time.sleep(1.0)
        again = width_of(screenshot(c, "Ride"))
        assert abs(again - standing) <= 3, (again, standing)

        # With no move: its idle cross-faded in straight away.
        assert cue(do="stay", action="start", idle="dragon_idle")["ok"]
        assert wait_for(lambda: st()["model"]["animation"] == "dragon_idle", 2, 0.05), st()["model"]
        assert cue(do="stay", action="end")["ok"]
        assert wait_for(lambda: st()["model"]["animation"] == "idle", 2, 0.05), st()["model"]

        # A new character starts on its own idle (the ride was the old one's).
        assert cue(do="stay", action="start", idle="dragon_idle")["ok"]
        assert wait_for(lambda: st()["model"]["animation"] == "dragon_idle", 2, 0.05), st()["model"]
        other = os.path.join(run.home, "rider-again.vrm")
        shutil.copy(RIDER, other)
        send(c, "SetInputSettings", {"inputName": "Ride", "inputSettings": {"model": other}})
        assert wait_for(lambda: st()["model"]["file"] == other and st()["model"]["shown"], 20, 0.1), st()["model"]
        assert st()["model"]["animation"] == "idle" and st()["stay"]["on"] is False, st()
        send(c, "RemoveInput", {"inputName": "Ride"})
    finally:
        c.disconnect()


def with_vertex_colour(path, rgb):
    """The .glb at `path` again, its first part's points each coloured
    `rgb` (COLOR_0, floats - linear, as glTF has it)."""
    with open(path, "rb") as f:
        data = f.read()
    n = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + n])
    blob = bytearray(data[20 + n + 8:])
    prim = doc["meshes"][0]["primitives"][0]
    count = doc["accessors"][prim["attributes"]["POSITION"]]["count"]
    while len(blob) % 4:
        blob.append(0)
    doc["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": count * 12, "target": 34962})
    blob.extend(struct.pack("<%df" % (count * 3), *(list(rgb) * count)))
    doc["accessors"].append({"bufferView": len(doc["bufferViews"]) - 1, "componentType": 5126, "count": count,
                             "type": "VEC3"})
    prim["attributes"]["COLOR_0"] = len(doc["accessors"]) - 1
    doc["buffers"][0]["byteLength"] = len(blob)
    text = json.dumps(doc).encode()
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def test_vertex_colours_are_linear_like_the_base_colour(run):
    """glTF's COLOR_0 is linear, as its base colour is (and as three.js
    reads it): a ball coloured 0.1 by its points looks the same as one
    whose material is 0.1 - not 0.1 to the power 2.2, near black. (The
    Northern outfit's fur shells and the beard carry their shading in
    COLOR_0.) Packed into bytes as sRGB for the GPU, then back."""
    camera = os.path.join(run.home, "camera.png")
    if not os.path.exists(camera):
        camera_picture(camera)
    by_material = os.path.join(run.home, "grey-by-material.glb")
    by_points = os.path.join(run.home, "grey-by-points.glb")
    write_glb(by_material, [ball_points(0, 0.1, 0, 0.1, 0.1, 0.1) + ((0.1, 0.1, 0.1), 0.6)])
    write_glb(by_points, [ball_points(0, 0.1, 0, 0.1, 0.1, 0.1) + ((1, 1, 1), 0.6)])
    with_vertex_colour(by_points, (0.1, 0.1, 0.1))
    settings = {"source": "Grey camera", "width": W, "height": H, "size": 60, "framing": "whole",
                "model": by_material, "mirror": False, "smoothing": 100, "follow_move": 0, "follow_grow": 0,
                "follow_tilt": 0, "bend": 0, "body_follow": 0, **STILL}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st():
        return vendor(c, "FaceGetState", {"source": "Grey"})

    def mean():
        rows = screenshot(c, "Grey")
        got = [px for row in rows for px in row if drawn(px)]
        return len(got), [sum(px[k] for px in got) / max(1, len(got)) for k in range(3)]
    try:
        send(c, "CreateScene", {"sceneName": "Greys"})
        send(c, "CreateInput", {"sceneName": "Greys", "inputName": "Grey camera", "inputKind": "image_source",
                                "inputSettings": {"file": camera}})
        send(c, "CreateInput", {"sceneName": "Greys", "inputName": "Grey", "inputKind": "direbot_face_avatar",
                                "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Greys"})
        assert wait_for(lambda: st()["model"]["shown"] and st()["model"]["file"] == by_material, 30, 0.2), st()
        time.sleep(0.4)
        n1, a = mean()
        send(c, "SetInputSettings", {"inputName": "Grey", "inputSettings": {"model": by_points}})
        assert wait_for(lambda: st()["model"]["shown"] and st()["model"]["file"] == by_points, 30, 0.2), st()
        time.sleep(0.4)
        n2, b = mean()
        print(f"a 0.1 grey ball: by its material {a} ({n1} px), by its points {b} ({n2} px)")
        assert n1 > 500 and abs(n1 - n2) < n1 * 0.02, (n1, n2)
        assert all(abs(x - y) <= 3 for x, y in zip(a, b)), (a, b)
        send(c, "RemoveInput", {"inputName": "Grey"})
    finally:
        c.disconnect()


def test_a_ride_puts_the_gaming_desk_away(run):
    """Gaming hands with a move that stays: its hands on its keyboard and
    mouse till the ride starts, then the ride has its arms - its hands let
    go and its keyboard, mouse and pad fade away (framed as before, so
    nothing jumps) - and they're back once it ends."""
    settings = {"width": W, "height": H, "size": 60, "framing": "whole", "model": RIDER,
                "animation": "idle", "mirror": False, "smoothing": 100, "follow_move": 0,
                "follow_grow": 0, "follow_tilt": 0, "bend": 0, "body_follow": 0, "style": "drawn",
                "game_hands": True, "game_device": "keyboard", **STILL}
    c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)

    def st():
        return vendor(c, "FaceGetState", {"source": "Desk ride"})

    def cue(**data):
        return vendor(c, "FaceReact", {"source": "Desk ride", **data})
    try:
        send(c, "CreateScene", {"sceneName": "Desk rides"})
        send(c, "CreateInput", {"sceneName": "Desk rides", "inputName": "Desk ride",
                                "inputKind": "direbot_face_avatar", "inputSettings": settings})
        send(c, "SetCurrentProgramScene", {"sceneName": "Desk rides"})
        assert wait_for(lambda: st()["model"]["shown"], 30, 0.2), st()
        assert wait_for(lambda: st()["gaming"]["shown"] and st()["gaming"]["hands_on"] == 1, 10, 0.1), \
            st()["gaming"]
        assert wait_for(lambda: st()["gaming"]["props_loaded"] == 5, 10, 0.1), st()["gaming"]
        framed = st()["model"]["framed"]
        time.sleep(0.5)
        at_desk = screenshot(c, "Desk ride")
        assert cue(do="stay", action="start", idle="dragon_idle")["ok"]
        assert wait_for(lambda: st()["stay"]["on"] and st()["gaming"]["hands_on"] == 0, 3, 0.05), \
            (st()["stay"], st()["gaming"])
        assert st()["gaming"]["shown"] and st()["model"]["framed"] == framed
        assert cue(do="stay", action="end")["ok"]
        assert wait_for(lambda: not st()["stay"]["on"] and st()["gaming"]["hands_on"] == 1, 3, 0.05), \
            (st()["stay"], st()["gaming"])
        assert st()["model"]["framed"] == framed
        time.sleep(1.0)
        back = screenshot(c, "Desk ride")
        moved = sum(1 for y in range(H) for x in range(W) if drawn(at_desk[y][x]) != drawn(back[y][x]))
        assert moved < W * H * 0.01, moved
        send(c, "RemoveInput", {"inputName": "Desk ride"})
    finally:
        c.disconnect()
