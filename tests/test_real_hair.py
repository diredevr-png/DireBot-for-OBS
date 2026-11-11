"""
Hair strands inside a real OBS: a made-up character with real strands of
hair (a file's lines, as a hair groom is exported) shown by "DireBot Face
Avatar" - its hair drawn, hidden behind its solid parts, smooth where it's
thin, fewer of it drawn the smaller it is on screen, moved by its
expressions and its bones; the hair detail setting and the hair budget;
and a see-through part's empty pixels hiding nothing.

Skipped unless OBS, Xvfb and the test build are there (see
tests/test_real_face.py).
"""

import json
import math
import os
import shutil
import struct
import time

import pytest

obsws = pytest.importorskip("obsws_python")

from test_real_obs import HERE, Obs, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark  # noqa: E402,F401
from test_real_face import (STILL, arms_character, ball_points, keep_shot, middle_of, png_bytes,  # noqa: E402
                            quick_shots, screenshot, send, write_glb)

# The hair's colours (linear, as glTF has them): its strands, the strands
# put behind its body, its stand-in hair, a companion's strands.
MAGENTA = (1.0, 0.0, 1.0)
WHITE = (1.0, 1.0, 1.0)
YELLOW = (0.9, 0.8, 0.02)
LIME = (0.3, 1.0, 0.0)

WOLF = os.path.join(HERE, "tests", "data", "companion", "wolf-test.glb")


def open_glb(path):
    """A .glb's description and its numbers, to add to."""
    with open(path, "rb") as f:
        data = f.read()
    n, = struct.unpack_from("<I", data, 12)
    return json.loads(data[20:20 + n]), bytearray(data[20 + n + 8:])


def save_glb(path, doc, blob):
    while len(blob) % 4:
        blob.append(0)
    doc["buffers"][0]["byteLength"] = len(blob)
    text = json.dumps(doc).encode()
    text += b" " * (-len(text) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + bytes(blob))


def add_numbers(doc, blob, raw, kind, count, ctype, target=None, more=None):
    """One more list of numbers in the file: its accessor's number."""
    while len(blob) % 4:
        blob.append(0)
    view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(raw)}
    if target:
        view["target"] = target
    doc["bufferViews"].append(view)
    blob.extend(raw)
    doc["accessors"].append(dict({"bufferView": len(doc["bufferViews"]) - 1, "componentType": ctype,
                                  "count": count, "type": kind}, **(more or {})))
    return len(doc["accessors"]) - 1


def floats(doc, blob, rows, kind, target=34962, bounds=False):
    width = len(rows[0]) if isinstance(rows[0], (tuple, list)) else 1
    flat = [c for v in rows for c in v] if width > 1 else list(rows)
    more = None
    if bounds:
        more = {"min": [min(v[k] for v in rows) for k in range(width)],
                "max": [max(v[k] for v in rows) for k in range(width)]}
    return add_numbers(doc, blob, struct.pack("<%df" % len(flat), *flat), kind, len(rows), 5126, target, more)


def world_of(doc, node):
    """Where a node is at rest (the test files' nodes are only moved, never
    turned or sized)."""
    nodes = doc["nodes"]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    at = [0.0, 0.0, 0.0]
    while node is not None:
        assert "rotation" not in nodes[node] and "scale" not in nodes[node]
        for k in range(3):
            at[k] += nodes[node].get("translation", [0, 0, 0])[k]
        node = parent.get(node)
    return at


class Strands:
    """Hair strands being made: each a run of points root to tip, as wide
    and coloured as said, every point hung on bones - and how far the
    file's one expression moves each point."""

    def __init__(self):
        self.pos, self.width, self.colour, self.joints, self.weights = [], [], [], [], []
        self.moves, self.pairs = [], []
        self.count = 0

    def add(self, points, width, colour, bones, move=(0.0, 0.0, 0.0)):
        """`bones`: for each point, up to 4 (which of the skin's bones, its
        share)."""
        first = len(self.pos)
        for p, held in zip(points, bones):
            held = (list(held) + [(0, 0.0)] * 4)[:4]
            self.pos.append(tuple(p))
            self.width.append(width)
            self.colour.append(tuple(colour))
            self.joints.append(tuple(j for j, _ in held))
            self.weights.append(tuple(w for _, w in held))
            self.moves.append(tuple(move))
        self.pairs += [k for i in range(first, len(self.pos) - 1) for k in (i, i + 1)]
        self.count += 1


def add_strands(doc, blob, hair, bones, name="hair", expression=None, top=False):
    """The strands into the file as a part of lines (glTF's mode 1) with
    their widths (_WIDTH), colours and bones, on a node of its own with a
    skin of `bones` (node numbers); `expression`: its one expression's
    name, moving the points as they say. Returns the node's number."""
    attributes = {
        "POSITION": floats(doc, blob, hair.pos, "VEC3", bounds=True),
        "_WIDTH": floats(doc, blob, hair.width, "SCALAR"),
        "COLOR_0": floats(doc, blob, hair.colour, "VEC3"),
        "JOINTS_0": add_numbers(doc, blob, struct.pack("<%dH" % (len(hair.joints) * 4),
                                                       *[c for v in hair.joints for c in v]),
                                "VEC4", len(hair.joints), 5123, 34962),
        "WEIGHTS_0": floats(doc, blob, hair.weights, "VEC4"),
    }
    part = {"attributes": attributes, "mode": 1,
            "indices": add_numbers(doc, blob, struct.pack("<%dI" % len(hair.pairs), *hair.pairs),
                                   "SCALAR", len(hair.pairs), 5125, 34963)}
    doc["materials"].append({"name": name, "pbrMetallicRoughness": {
        "baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.6}})
    part["material"] = len(doc["materials"]) - 1
    mesh = {"name": name, "primitives": [part]}
    if expression:
        part["targets"] = [{"POSITION": floats(doc, blob, hair.moves, "VEC3", bounds=True)}]
        mesh["extras"] = {"targetNames": [expression]}
    doc["meshes"].append(mesh)
    bind = []
    for b in bones:
        w = world_of(doc, b)
        bind += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1]
    doc.setdefault("skins", []).append({
        "joints": list(bones),
        "inverseBindMatrices": add_numbers(doc, blob, struct.pack("<%df" % len(bind), *bind),
                                           "MAT4", len(bones), 5126)})
    doc["nodes"].append({"name": name, "mesh": len(doc["meshes"]) - 1, "skin": len(doc["skins"]) - 1})
    if top:
        doc["scenes"][0]["nodes"].append(len(doc["nodes"]) - 1)
    else:
        doc["nodes"][0].setdefault("children", []).append(len(doc["nodes"]) - 1)
    return len(doc["nodes"]) - 1


def add_part(doc, blob, shape, colour, parent, at, name, more=None, picture=None):
    """An ordinary part of triangles (points, facings, triangles) hung on
    the node `parent` at `at`; `more`: more for its material; `picture`:
    its colour picture (its UVs, PNG bytes)."""
    pos, nrm, ix = shape
    attributes = {"POSITION": floats(doc, blob, pos, "VEC3", bounds=True),
                  "NORMAL": floats(doc, blob, nrm, "VEC3")}
    pbr = {"baseColorFactor": list(colour) + [1], "metallicFactor": 0, "roughnessFactor": 0.5}
    if picture:
        uv, png = picture
        attributes["TEXCOORD_0"] = floats(doc, blob, uv, "VEC2")
        while len(blob) % 4:
            blob.append(0)
        doc["bufferViews"].append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(png)})
        blob.extend(png)
        doc.setdefault("images", []).append({"bufferView": len(doc["bufferViews"]) - 1,
                                             "mimeType": "image/png"})
        doc.setdefault("textures", []).append({"source": len(doc["images"]) - 1})
        pbr["baseColorTexture"] = {"index": len(doc["textures"]) - 1}
    doc["materials"].append({"name": name, "pbrMetallicRoughness": pbr, **(more or {})})
    doc["meshes"].append({"name": name, "primitives": [{
        "attributes": attributes, "material": len(doc["materials"]) - 1,
        "indices": add_numbers(doc, blob, struct.pack("<%dI" % len(ix), *ix), "SCALAR", len(ix), 5125, 34963)}]})
    doc["nodes"].append({"name": name, "translation": list(at), "mesh": len(doc["meshes"]) - 1})
    doc["nodes"][parent].setdefault("children", []).append(len(doc["nodes"]) - 1)
    return len(doc["nodes"]) - 1


def add_animation(doc, blob, name, channels):
    """One more animation of its own: [(node, "rotation" or "translation",
    times, values flat)]."""
    samplers, chans = [], []
    for node, path, times, values in channels:
        width = 4 if path == "rotation" else 3
        t = add_numbers(doc, blob, struct.pack("<%df" % len(times), *times), "SCALAR", len(times), 5126,
                        more={"min": [min(times)], "max": [max(times)]})
        v = add_numbers(doc, blob, struct.pack("<%df" % len(values), *values), "VEC%d" % width,
                        len(values) // width, 5126)
        samplers.append({"input": t, "output": v, "interpolation": "LINEAR"})
        chans.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})
    doc.setdefault("animations", []).append({"name": name, "samplers": samplers, "channels": chans})


# Where arms_character's head ball is and how big, at rest (the world).
HEAD_AT, HEAD_SIZE = (0.0, 1.68, 0.0), (0.1, 0.11, 0.1)
# How far the hair's expression lifts the hair on top of its head.
LIFT = 0.15
# How far the "Tilt" animation tips its neck over (degrees, towards the
# picture's left).
TILT = 30
# How wide the few wide hairs of a coat of thin ones are (hair_character's
# `wide_every`; metres).
WIDE_HAIR = 0.006


def hair_character(path, vrm=True, standin=False, behind=0, fan=0, width=0.006, crown=2400, locks=300, tuft=200,
                   wide_every=0):
    """arms_character with real hair, magenta:
      - on top of its head `crown` strands standing out all round, drooping
        a little, hung on its head; its one expression ("lift": a VRM's
        "happy") lifts them all LIFT up;
      - a lock of `locks` strands down each side, from its head past its
        neck to its chest, each point hung on those bones in turn (so it
        bends as they turn);
      - a tuft of `tuft` strands standing on its left hand, hung on it;
      - `behind`: that many more, white, hanging behind its body (never
        seen from the front);
      - `fan`: that many more standing in a sheet well behind its head,
        from its neck to above its head and wider than it, hung on its
        neck (so they're no part of its head);
      - `standin`: stand-in hair too - a yellow cap, an ordinary part
        marked {"direbot": {"hair_standin": true}} in its material's
        extras.
    Every hair `width` wide (the file's units: metres) - but with
    `wide_every`, every so-manyth hair of its crown, its locks and its tuft
    WIDE_HAIR wide: a coat of thin hairs with a few wide ones among them.
    An animation of its own, "Tilt", holds its neck tipped TILT degrees
    over. Returns how many strands the file has."""
    arms_character(path, vrm=vrm)

    def wide(i):
        return WIDE_HAIR if wide_every and i % wide_every == 0 else width
    doc, blob = open_glb(path)
    names = {n.get("name"): i for i, n in enumerate(doc["nodes"])}
    bones = [names["head"], names["neck"], names["chest"], names["leftHand"]]
    HEAD, NECK, CHEST, HAND = range(4)
    hair = Strands()
    cx, cy, cz = HEAD_AT
    rx, ry, rz = HEAD_SIZE
    # (Spread evenly: each a golden angle round from the last, as a
    # sunflower's seeds.)
    for i in range(crown):
        up = 1 - (i + 0.5) / crown * (1 - math.cos(math.radians(62)))
        out = math.sqrt(1 - up * up)
        turn = i * 2.399963
        d = (out * math.cos(turn), up, out * math.sin(turn))
        root = (cx + rx * d[0] * 0.97, cy + ry * d[1] * 0.97, cz + rz * d[2] * 0.97)
        points = []
        for k in range(6):
            t = k / 5
            points.append((root[0] + d[0] * 0.13 * t, root[1] + d[1] * 0.13 * t - 0.05 * t * t,
                           root[2] + d[2] * 0.13 * t))
        hair.add(points, wide(i), MAGENTA, [[(HEAD, 1.0)]] * 6, move=(0, LIFT, 0))
    for side in (1, -1):
        for i in range(locks):
            a, b = (i * 0.61803) % 1, (i * 0.75488) % 1
            root = (side * rx * 0.96, cy - 0.03 + 0.07 * a, -0.03 + 0.06 * b)
            bend = (side * 0.2, cy - 0.07, 0.03)
            tip = (side * (0.19 + 0.05 * b), 1.30 + 0.05 * a, 0.1)
            points, held = [], []
            for k in range(10):
                t = k / 9
                points.append(tuple((1 - t) ** 2 * root[n] + 2 * t * (1 - t) * bend[n] + t * t * tip[n]
                                    for n in range(3)))
                head = max(0.0, 1 - 2 * t)
                chest = max(0.0, 2 * t - 1)
                held.append([(HEAD, head), (NECK, 1 - head - chest), (CHEST, chest)])
            hair.add(points, wide(i), MAGENTA, held)
    hand = world_of(doc, names["leftHand"])
    for i in range(tuft):
        a, b = (i * 0.61803) % 1, (i * 0.75488) % 1
        root = (hand[0] + 0.02 + 0.06 * a, hand[1] + 0.015, -0.025 + 0.05 * b)
        points = [(root[0] + (a - 0.5) * 0.04 * t, root[1] + 0.1 * t, root[2] + 0.01 * t * t)
                  for t in (0, 0.5, 1)]
        hair.add(points, wide(i), MAGENTA, [[(HAND, 1.0)]] * 3)
    chest = world_of(doc, names["chest"])
    for i in range(behind):
        a, b = (i * 0.61803) % 1, (i * 0.75488) % 1
        x = -0.07 + 0.14 * a
        points = [(x, chest[1] + 0.2 - 0.18 * t, -0.14 - 0.02 * b - 0.03 * t * (1 - t)) for t in (0, 0.5, 1)]
        hair.add(points, max(width, 0.006), WHITE, [[(CHEST, 1.0)]] * 3)
    for i in range(fan):
        a, b = (i * 0.61803) % 1, (i * 0.75488) % 1
        x = -0.2 + 0.4 * a
        points = [(x, 1.55 + (0.53 + 0.04 * b) * t, -0.2 - 0.04 * t * (1 - t)) for t in (0, 0.5, 1)]
        hair.add(points, width, MAGENTA, [[(NECK, 1.0)]] * 3)
    node = add_strands(doc, blob, hair, bones, expression="lift")
    if vrm:
        doc["extensions"]["VRMC_vrm"]["expressions"] = {"preset": {"happy": {
            "morphTargetBinds": [{"node": node, "index": 0, "weight": 1}]}}}
    if standin:
        add_part(doc, blob, ball_points(0, 0, 0, 0.13, 0.09, 0.13), YELLOW, names["head"], (0, 0.15, 0),
                 "cap", more={"extras": {"direbot": {"hair_standin": True}}})
    half = math.radians(TILT) / 2
    add_animation(doc, blob, "Tilt", [(names["neck"], "rotation", [0, 1],
                                       [0, 0, math.sin(half), math.cos(half)] * 2)])
    save_glb(path, doc, blob)
    return hair.count


def magenta(px):
    return px[0] > 50 and px[2] > 50 and px[1] * 2 < min(px[0], px[2])


def faint_magenta(px):
    """Any hair at all, however thin (over nothing: the darker, the more
    see-through)."""
    return px[0] > 6 and px[2] > 6 and px[1] * 2 < min(px[0], px[2])


def yellow(px):
    return px[0] > 100 and px[1] > 0.75 * px[0] and px[2] < 0.5 * px[0]


def white(px):
    return min(px) > 150 and max(px) - min(px) < 40


def orange(px):
    return px[0] > 150 and 0.4 * px[0] < px[1] < 0.72 * px[0] and px[2] < 0.5 * px[1]


def red(px):
    return px[0] > 120 and px[1] < 60 and px[2] < 60


def green(px):
    return px[1] > 100 and px[1] > 2 * px[0] and px[1] > 2 * px[2]


def lime(px):
    return px[1] > 150 and px[1] > 1.3 * px[0] and px[0] > 2 * px[2] and px[0] > 60


def blue(px):
    return px[2] > 120 and px[2] > 1.6 * px[0] and px[2] > 1.4 * px[1]


def count(rows, test, box=None):
    return middle_of(rows, test, box)[2]


def per_metre(rows):
    """How many pixels a metre of the character is in the picture: by its
    blue body, 0.4 tall."""
    return sum(1 for row in rows if sum(1 for px in row if blue(px)) >= 3) / 0.4


def top_of(rows, test, least=3):
    """The highest row with `least` of `test`'s pixels."""
    for y, row in enumerate(rows):
        if sum(1 for px in row if test(px)) >= least:
            return y
    return None


def differing(a, b, by=24):
    """How many pixels of two pictures differ by more than `by`."""
    assert len(a) == len(b) and len(a[0]) == len(b[0])
    return sum(1 for ra, rb in zip(a, b) for pa, pb in zip(ra, rb)
               if max(abs(x - y) for x, y in zip(pa, pb)) > by)


# A character in its own spot, all of it in the picture, holding still;
# the hair budget off (the graphics card's clock then changes nothing, so
# the same picture comes every time).
SIZE = 400
SETTINGS = {"width": SIZE, "height": SIZE, "size": 100, "framing": "whole", "style": "drawn", "mirror": False,
            "smoothing": 0, "follow_move": 0, "follow_grow": 0, "follow_tilt": 0, "body_follow": 0,
            "hair_budget_ms": 0, **STILL}


@pytest.fixture(scope="module")
def run():
    o = Obs()
    yield o
    if o.proc.poll() is None:
        o.close()
    if not os.environ.get("DIREBOT_OBS_KEEP"):
        shutil.rmtree(o.home, ignore_errors=True)


def settles(read, hold=4.0, within=90.0, step=0.1):
    """Waits for a number to stop changing: what it is once it has stayed
    the same for `hold` seconds (None: it never did, in `within`)."""
    end = time.time() + within
    last, since = read(), time.time()
    while time.time() < end:
        time.sleep(step)
        now = read()
        if now != last:
            last, since = now, time.time()
        elif time.time() - since >= hold:
            return last
    return None


class Stage:
    """A scene of face avatars in the running OBS, taken away after."""

    def __init__(self, run, scene):
        self.c = obsws.ReqClient(host="127.0.0.1", port=run.ws_port, password="", timeout=10)
        self.scene = scene + " scene"
        self.names = []
        send(self.c, "CreateScene", {"sceneName": self.scene})
        send(self.c, "SetCurrentProgramScene", {"sceneName": self.scene})

    def add(self, name, model, **more):
        send(self.c, "CreateInput", {"sceneName": self.scene, "inputName": name,
                                     "inputKind": "direbot_face_avatar",
                                     "inputSettings": {**SETTINGS, "model": model, **more}})
        self.names.append(name)

    def state(self, name):
        return vendor(self.c, "FaceGetState", {"source": name})

    def model(self, name):
        return self.state(name)["model"]

    def hair(self, name):
        return self.model(name)["hair"]

    def set(self, name, **settings):
        send(self.c, "SetInputSettings", {"inputName": name, "inputSettings": settings})

    def cue(self, name, **data):
        return vendor(self.c, "FaceReact", {"source": name, **data})

    def reloaded(self, name):
        """After a change that loads its file again: wait for it."""
        time.sleep(0.3)
        assert wait_for(lambda: not self.model(name)["loading"] and self.model(name)["shown"], 30, 0.1), \
            self.model(name)
        time.sleep(0.4)

    def shown(self, *names):
        for name in names or self.names:
            assert wait_for(lambda: self.model(name)["shown"], 30, 0.2), self.model(name)

    def shot(self, name, keep=None):
        rows = screenshot(self.c, name)
        if keep:
            keep_shot(rows, keep)
        return rows

    def close(self):
        for name in self.names:
            try:
                send(self.c, "RemoveInput", {"inputName": name})
            except Exception:
                pass
        self.c.disconnect()


def test_hair_strands_are_drawn(run):
    """A character with hair strands: FaceGetState says how many its file
    has, how many are kept and how many are drawn now, and the picture has
    its hair where it grows - on top of its head, down its sides, on its
    hand. Stand-in hair (a part marked so in its file) isn't shown while
    its strands are."""
    plain = os.path.join(run.home, "hair.vrm")
    strands = hair_character(plain)
    capped = os.path.join(run.home, "hair-cap.vrm")
    assert hair_character(capped, standin=True) == strands
    bald = os.path.join(run.home, "bald.vrm")
    arms_character(bald)
    s = Stage(run, "Hair")
    try:
        s.add("Hair", plain)
        s.add("Hair cap", capped)
        s.add("Bald", bald)
        s.shown()
        assert wait_for(lambda: s.hair("Hair")["drawn"] > 0 and s.hair("Hair cap")["drawn"] > 0, 10, 0.1), \
            (s.hair("Hair"), s.hair("Hair cap"))
        time.sleep(0.5)
        for name in ("Hair", "Hair cap"):
            h = s.hair(name)
            print(f"{name}: {h}")
            assert h["detail"] == "full" and h["strands"] == strands, h
            assert h["loaded"] == strands and h["shown_strands"] == strands and h["shown_loaded"] == strands, h
            # (Wide hairs, big in the picture: every one of them drawn.)
            assert h["drawn"] == strands and h["pieces"] > strands and h["draws"] > 0, h
            assert h["standin"] == (name == "Hair cap"), h
            assert h["share"] == 1 and h["budget_ms"] == 0, h
        assert "problem" not in s.model("Hair"), s.model("Hair")
        # No hair: nothing of it.
        h = s.hair("Bald")
        assert h["strands"] == 0 and h["loaded"] == 0 and h["drawn"] == 0 and h["pieces"] == 0, h
        assert not h["standin"] and h["shown_strands"] == 0, h

        rows = s.shot("Hair")
        bald_rows = s.shot("Bald")
        capped_rows = s.shot("Hair cap")
        assert count(bald_rows, magenta) == 0 and count(bald_rows, orange) > 300
        # On top of its head and round it: plenty, its middle over its
        # head's; a lock down each side of its body; a tuft on its hand
        # (the picture's right).
        hx, hy, head = middle_of(rows, orange)
        assert head > 200, head
        mx, my, n = middle_of(rows, magenta, (0, 0, SIZE, int(hy)))
        print(f"hair: {count(rows, magenta)} magenta pixels, {n} above its head's middle "
              f"({mx:.0f}, {my:.0f}); its head at ({hx:.0f}, {hy:.0f}), {head} pixels showing")
        assert n > 2000 and abs(mx - hx) < 10 and my < hy - 10, (mx, my, n, hx, hy, head)
        bx, by, body = middle_of(rows, blue)
        left = count(rows, magenta, (0, int(by) - 20, int(bx), int(by) + 20))
        right = count(rows, magenta, (int(bx), int(by) - 20, SIZE, int(by) + 20))
        assert left > 150 and right > 150 and abs(left - right) < 0.25 * left, (left, right)
        rx, ry, hand = middle_of(rows, red, (int(bx), 0, SIZE, SIZE))
        assert hand > 20, hand
        near_hand = (int(rx) - 5, int(ry) - 40, int(rx) + 60, int(ry) + 25)
        assert count(rows, magenta, near_hand) > 300, count(rows, magenta, near_hand)
        # Its stand-in hair isn't shown with its strands: the same picture
        # as the one with none.
        assert count(capped_rows, yellow) == 0, count(capped_rows, yellow)
        assert differing(capped_rows, rows) <= 20, differing(capped_rows, rows)
    finally:
        s.close()


def test_the_hair_detail_setting(run):
    """The hair detail setting ("hair_detail"). Light: fewer strands kept
    (a thick head of hair no longer all of it) and fewer drawn, each more
    solid, and half the hair budget. Today: a character with stand-in hair
    wears that instead - no strands loaded or drawn - and one with none
    keeps its strands, as Light does. Full again brings them back, and a
    setting it doesn't know is Full."""
    plain = os.path.join(run.home, "detail.vrm")
    strands = hair_character(plain)
    capped = os.path.join(run.home, "detail-cap.vrm")
    hair_character(capped, standin=True)
    thick = os.path.join(run.home, "detail-thick.vrm")
    many = hair_character(thick, crown=14000)
    thin = os.path.join(run.home, "detail-thin.vrm")
    hair_character(thin, width=0.0004)
    s = Stage(run, "Detail")
    names = ("Plain", "Capped", "Thick", "Thin", "Plan")
    try:
        s.add("Plain", plain)
        s.add("Capped", capped)
        s.add("Thick", thick, width=200, height=200)
        s.add("Thin", thin)
        # (The setting's own budget, only read here: with it on, how much
        # is drawn goes by the graphics card's clock.)
        s.add("Plan", plain, width=100, height=100, hair_budget_ms=-1)
        s.shown()
        assert wait_for(lambda: all(s.hair(n)["drawn"] > 0 for n in names), 10, 0.1), [s.hair(n) for n in names]
        time.sleep(0.5)
        full = {n: s.hair(n) for n in names}
        full_rows = {n: s.shot(n, "detail full " + n) for n in ("Plain", "Capped")}
        for n in names:
            print(f"full, {n}: {full[n]}")
            assert full[n]["detail"] == "full", full[n]
        assert full["Thick"]["strands"] == many and full["Thick"]["loaded"] == many, full["Thick"]
        assert full["Plan"]["budget_ms"] == 1, full["Plan"]
        assert full["Plain"]["drawn"] == strands, full["Plain"]
        assert 16 < full["Thin"]["drawn"] < strands, full["Thin"]
        in_full = count(full_rows["Plain"], magenta)
        assert in_full > 3000 and count(full_rows["Capped"], yellow) == 0

        def change(to):
            for n in names:
                s.set(n, hair_detail=to)
            for n in names:
                s.reloaded(n)
            assert wait_for(lambda: all(s.model(n)["shown"] for n in names), 20, 0.1)
            time.sleep(0.5)
            now = {n: s.hair(n) for n in names}
            for n in names:
                print(f"{to}, {n}: {now[n]}")
            return now

        # Light.
        light = change("light")
        for n in names:
            assert light[n]["detail"] == "light" and light[n]["strands"] == full[n]["strands"], light[n]
        assert light["Plan"]["budget_ms"] == 0.5, light["Plan"]
        # (Posed by the CPU here it keeps 60,000 points at most: the thick
        # one has more.)
        assert 0.3 * many < light["Thick"]["loaded"] < 0.95 * many, light["Thick"]
        assert light["Thick"]["shown_loaded"] == light["Thick"]["loaded"], light["Thick"]
        assert light["Plain"]["loaded"] == strands and light["Plain"]["drawn"] == strands, light["Plain"]
        assert 16 <= light["Thin"]["drawn"] < 0.8 * full["Thin"]["drawn"], (light["Thin"], full["Thin"])
        rows = s.shot("Plain", "detail light Plain")
        assert abs(count(rows, magenta) - in_full) < 0.1 * in_full, (count(rows, magenta), in_full)
        assert count(s.shot("Capped"), yellow) == 0

        # Today: its stand-in hair instead of its strands.
        today = change("today")
        for n in names:
            assert today[n]["detail"] == "today" and today[n]["strands"] == full[n]["strands"], today[n]
        h = today["Capped"]
        assert h["standin"] and h["loaded"] == 0 and h["drawn"] == 0 and h["pieces"] == 0, h
        assert h["shown_strands"] == 0 and h["shown_loaded"] == 0 and h["draws"] == 0, h
        rows = s.shot("Capped", "detail today Capped")
        cap = count(rows, yellow)
        print(f"today: {cap} pixels of its stand-in hair, {count(rows, magenta)} of its strands")
        assert cap > 800 and count(rows, magenta) == 0, (cap, count(rows, magenta))
        # No stand-in hair: its strands stay, as Light has them.
        for n in ("Plain", "Thick", "Thin"):
            assert not today[n]["standin"] and today[n]["loaded"] == light[n]["loaded"], (today[n], light[n])
            assert today[n]["drawn"] == light[n]["drawn"], (today[n], light[n])
        assert today["Plan"]["budget_ms"] == 0.5, today["Plan"]
        rows = s.shot("Plain", "detail today Plain")
        assert abs(count(rows, magenta) - in_full) < 0.1 * in_full, (count(rows, magenta), in_full)

        # Full again, and a setting it doesn't know: all as it was.
        for to in ("full", "bushy"):
            again = change(to)
            for n in names:
                assert again[n]["detail"] == "full", again[n]
                for key in ("strands", "loaded", "standin", "shown_strands", "shown_loaded", "budget_ms"):
                    assert again[n][key] == full[n][key], (n, key, again[n], full[n])
            for n in ("Plain", "Capped", "Thick", "Thin"):
                assert again[n]["drawn"] == full[n]["drawn"], (again[n], full[n])
            for n in ("Plain", "Capped"):
                rows = s.shot(n, f"detail {to} {n}")
                assert differing(rows, full_rows[n]) <= 20, (n, differing(rows, full_rows[n]))
    finally:
        s.close()


def test_hairs_are_hidden_behind_solid_parts(run):
    """Hairs behind a solid part don't show through it: strands hanging
    behind its body are drawn (FaceGetState counts them) yet the picture
    from the front is the same as the character with no hair at all -
    and from round the back they're there."""
    bald = os.path.join(run.home, "no-hair.vrm")
    arms_character(bald)
    back = os.path.join(run.home, "hair-behind.vrm")
    strands = hair_character(back, crown=0, locks=0, tuft=0, behind=400)
    assert strands == 400
    s = Stage(run, "Behind")
    try:
        s.add("None behind", bald)
        s.add("Behind", back)
        s.add("Behind seen", back, views=json.dumps([{"name": "Round", "yaw": 120}]), view="1", view_glide=0)
        s.shown()
        assert wait_for(lambda: s.hair("Behind")["drawn"] > 0 and s.hair("Behind seen")["drawn"] > 0, 10, 0.1)
        time.sleep(0.5)
        h = s.hair("Behind")
        print(f"behind its body: {h}")
        assert h["loaded"] == strands and h["drawn"] == strands and h["pieces"] >= strands, h
        front, none = s.shot("Behind"), s.shot("None behind")
        seen = s.shot("Behind seen")
        off = differing(front, none)
        print(f"from the front: {count(front, white)} white pixels, {off} pixels unlike the one with no hair; "
              f"from round the back: {count(seen, white)} white")
        assert count(none, white) == 0 and count(front, white) == 0, count(front, white)
        assert off <= 4, off
        assert count(seen, white) > 300, count(seen, white)
    finally:
        s.close()


RAYS = 10


def ray_character(path, width):
    """arms_character with a few long straight hairs standing in the air
    over its head, well apart, each leaning a little (so it crosses the
    picture's columns), `width` wide. (Three points each: a hair of only
    two has a test of its own.)"""
    arms_character(path)
    doc, blob = open_glb(path)
    names = {n.get("name"): i for i, n in enumerate(doc["nodes"])}
    hair = Strands()
    for i in range(RAYS):
        x = -0.36 + 0.08 * i
        hair.add([(x + 0.04 * t, 1.86 + 0.44 * t, 0.0) for t in (0, 0.5, 1)], width, MAGENTA, [[(0, 1.0)]] * 3)
    add_strands(doc, blob, hair, [names["head"]])
    save_glb(path, doc, blob)


def trace(px):
    """The least trace of hair over nothing."""
    return px[0] >= 2 and px[2] >= 2 and px[1] * 2 <= min(px[0], px[2])


def runs_of(row, test):
    """A row's runs of `test`'s pixels: each one's red, added up, and its
    brightest."""
    out, inside = [], False
    for px in row:
        if test(px):
            if inside:
                out[-1] = (out[-1][0] + px[0], max(out[-1][1], px[0]))
            else:
                out.append((px[0], px[0]))
            inside = True
        else:
            inside = False
    return out


def test_thin_hairs_are_smooth_not_grainy(run):
    """A hair far thinner than a pixel is drawn see-through, not as a
    sprinkle of solid dots. The measure: a few thin hairs standing apart
    over nothing. Each one is an unbroken line - every row of the picture
    it crosses has some of it - of steady strength (the colour it leaves
    in a row never far from what it usually leaves), and much fainter than
    the same hair drawn wide. Dots would be the other way about: rows with
    none of it between rows at full strength."""
    thin = os.path.join(run.home, "rays-thin.vrm")
    ray_character(thin, 0.0006)
    wide = os.path.join(run.home, "rays-wide.vrm")
    ray_character(wide, 0.02)
    s = Stage(run, "Rays")
    try:
        s.add("Thin rays", thin)
        s.add("Wide rays", wide)
        s.shown()
        assert wait_for(lambda: s.hair("Thin rays")["drawn"] > 0 and s.hair("Wide rays")["drawn"] > 0, 10, 0.1)
        time.sleep(0.5)
        # (So few that all of them are drawn, as thin as they are.)
        assert s.hair("Thin rays")["drawn"] == RAYS and s.hair("Wide rays")["drawn"] == RAYS, s.hair("Thin rays")
        rows, wide_rows = s.shot("Thin rays"), s.shot("Wide rays")
        solid = max(px[0] for row in wide_rows for px in row if magenta(px))
        ys = [y for y, row in enumerate(rows) if any(faint_magenta(px) for px in row)]
        assert ys and ys[-1] - ys[0] > 60, ys[:3]
        # (Not its tip, which fades away, nor its very root.)
        first, last = ys[0] + (ys[-1] - ys[0]) // 5, ys[-1] - (ys[-1] - ys[0]) // 10
        left, brightest, broken = [], 0, []
        for y in range(first, last):
            found = runs_of(rows[y], trace)
            if len(found) != RAYS:
                broken.append((y, len(found)))
            left += [total for total, _ in found]
            brightest = max([brightest] + [most for _, most in found])
        left.sort()
        usual = left[len(left) // 2]
        shades = len({px for y in range(first, last) for px in rows[y] if trace(px)})
        print(f"thin hairs: rows {first}-{last}, {len(broken)} without all {RAYS} of them; each leaves "
              f"{left[0]} to {left[-1]} of red in a row, usually {usual}; its brightest pixel {brightest}, "
              f"the same hair wide {solid}; {shades} shades")
        assert not broken, broken[:10]
        assert solid > 180 and brightest < 0.3 * solid, (brightest, solid)
        assert left[0] > 0.3 * usual and left[-1] < 2 * usual, (left[0], usual, left[-1])
        assert shades >= 12, shades
    finally:
        s.close()


def ink(rows):
    """How much hair a picture has in all: its hair pixels' red, added up
    (see-through hair counts for as much as shows)."""
    return sum(px[0] for row in rows for px in row if faint_magenta(px))


def test_thin_hairs_are_sent_as_lines(run):
    """Hair well under a pixel wide is drawn a pixel wide and see-through -
    which is what a line is, for half the work: so a part whose hairs are
    that thin goes to the graphics card as lines, and FaceGetState says how
    many of its pieces did. The picture has as much hair in it either way
    (the same hairs told never to be lines: "hair_lines" 0, a number the
    settings window doesn't show), each hair still an unbroken line. Wide
    hairs stay ribbons - the same picture as told never - and only told
    always (2) are they lines: thin ones, with less hair to see."""
    thin = os.path.join(run.home, "lines-thin.vrm")
    strands = hair_character(thin, width=0.0006)
    wide = os.path.join(run.home, "lines-wide.vrm")
    assert hair_character(wide) == strands
    rays = os.path.join(run.home, "lines-rays.vrm")
    ray_character(rays, 0.0006)
    s = Stage(run, "Lines")
    try:
        s.add("Thin", thin)
        s.add("Thin never", thin, hair_lines=0)
        s.add("Thin always", thin, hair_lines=2)
        s.add("Wide", wide)
        s.add("Wide never", wide, hair_lines=0)
        s.add("Wide always", wide, hair_lines=2)
        s.add("Rays", rays)
        s.add("Rays never", rays, hair_lines=0)
        s.shown()
        assert wait_for(lambda: all(s.hair(name)["drawn"] > 0 for name in s.names), 10, 0.1)
        time.sleep(0.5)
        h = {name: s.hair(name) for name in s.names}
        for name in s.names:
            print(f"{name}: {h[name]}")
        rows = {name: s.shot(name) for name in s.names}
        # Thin hairs: lines unless told never - every piece drawn, the same
        # hairs either way.
        assert h["Thin"]["pieces"] > strands and h["Thin"]["lines"] == h["Thin"]["pieces"], h["Thin"]
        assert h["Thin never"]["lines"] == 0 and h["Thin never"]["pieces"] == h["Thin"]["pieces"], h["Thin never"]
        assert h["Thin always"]["lines"] == h["Thin"]["pieces"], h["Thin always"]
        assert h["Thin"]["drawn"] == h["Thin never"]["drawn"] == h["Thin always"]["drawn"], h
        # As much hair in the picture, over as much of it.
        lines, ribbons = ink(rows["Thin"]), ink(rows["Thin never"])
        over, over_ribbons = count(rows["Thin"], faint_magenta), count(rows["Thin never"], faint_magenta)
        print(f"thin hair as lines: {lines} of red over {over} pixels; as ribbons {ribbons} over {over_ribbons}")
        assert ribbons > 20000 and over_ribbons > 2000, (ribbons, over_ribbons)
        assert 0.9 * ribbons < lines < 1.1 * ribbons, (lines, ribbons)
        assert 0.85 * over_ribbons < over < 1.15 * over_ribbons, (over, over_ribbons)
        assert differing(rows["Thin always"], rows["Thin"]) == 0
        # A few thin hairs standing apart: each an unbroken line as lines
        # too, leaving as much in a row.
        assert h["Rays"]["lines"] == h["Rays"]["pieces"] > 0 and h["Rays never"]["lines"] == 0, (h["Rays"],
                                                                                              h["Rays never"])
        ys = [y for y, row in enumerate(rows["Rays never"]) if any(faint_magenta(px) for px in row)]
        first, last = ys[0] + (ys[-1] - ys[0]) // 5, ys[-1] - (ys[-1] - ys[0]) // 10
        left = {}
        for name in ("Rays", "Rays never"):
            found = [runs_of(rows[name][y], trace) for y in range(first, last)]
            broken = [(first + i, len(f)) for i, f in enumerate(found) if len(f) != RAYS]
            assert not broken, (name, broken[:10])
            left[name] = sum(total for f in found for total, _ in f)
        print(f"thin hairs standing apart: as lines {left['Rays']} of red in rows {first}-{last}, "
              f"as ribbons {left['Rays never']}")
        assert 0.9 * left["Rays never"] < left["Rays"] < 1.1 * left["Rays never"], left
        # Wide hairs: ribbons, the same picture as told never; told always,
        # lines - thin ones.
        assert h["Wide"]["lines"] == 0 and h["Wide never"]["lines"] == 0, (h["Wide"], h["Wide never"])
        assert h["Wide always"]["lines"] == h["Wide"]["pieces"] > strands, h["Wide always"]
        assert differing(rows["Wide"], rows["Wide never"]) == 0
        wide_ribbons, wide_lines = count(rows["Wide"], magenta), count(rows["Wide always"], magenta)
        print(f"wide hair: {wide_ribbons} magenta pixels as ribbons, {wide_lines} told to be lines")
        unlike = differing(rows["Wide always"], rows["Wide"])
        assert wide_ribbons > 3000 and wide_lines < 0.95 * wide_ribbons and unlike > 1000, (wide_ribbons, wide_lines,
                                                                                         unlike)
        for name in s.names:
            assert "problem" not in s.model(name), s.model(name)
    finally:
        s.close()


def test_thin_hairs_among_wide_ones_are_lines(run):
    """A coat of many thin hairs with a few wide ones among them (an
    animal's) is never thin enough as a whole. Its thin hairs' pieces go to
    the graphics card as lines all the same, and its wide ones stay
    ribbons: FaceGetState counts the thin ones' pieces as lines, and the
    picture has as much hair as with none of it lines ("hair_lines" 0) -
    its wide hairs as wide as ever. Told always (2), the wide ones go thin
    too."""
    figure = os.path.join(run.home, "lines-among.vrm")
    strands = hair_character(figure, width=0.0006, wide_every=5)
    s = Stage(run, "Among")
    try:
        s.add("Among", figure)
        s.add("Among never", figure, hair_lines=0)
        s.add("Among always", figure, hair_lines=2)
        s.shown()
        assert wait_for(lambda: all(s.hair(name)["drawn"] > 0 for name in s.names), 10, 0.1)
        time.sleep(0.5)
        h = {name: s.hair(name) for name in s.names}
        for name in s.names:
            print(f"{name}: {h[name]}")
        rows = {name: s.shot(name) for name in s.names}
        # Four hairs in five are thin: their pieces lines, the rest ribbons.
        pieces = h["Among"]["pieces"]
        assert pieces > strands and h["Among"]["drawn"] == h["Among never"]["drawn"] == h["Among always"]["drawn"], h
        assert h["Among never"]["pieces"] == pieces and h["Among always"]["pieces"] == pieces, h
        assert 0.78 * pieces < h["Among"]["lines"] < 0.82 * pieces, h["Among"]
        assert h["Among never"]["lines"] == 0 and h["Among always"]["lines"] == pieces, h
        # As much hair in the picture, over as much of it - and its wide
        # hairs (solid magenta) as before.
        lines, ribbons = ink(rows["Among"]), ink(rows["Among never"])
        over, over_ribbons = count(rows["Among"], faint_magenta), count(rows["Among never"], faint_magenta)
        solid, solid_ribbons = count(rows["Among"], magenta), count(rows["Among never"], magenta)
        print(f"thin hairs among wide ones, the thin as lines: {lines} of red over {over} pixels, {solid} of them "
              f"solid; all ribbons {ribbons} over {over_ribbons}, {solid_ribbons} solid")
        assert ribbons > 20000 and solid_ribbons > 1500, (ribbons, solid_ribbons)
        assert 0.95 * ribbons < lines < 1.05 * ribbons, (lines, ribbons)
        assert 0.9 * over_ribbons < over < 1.1 * over_ribbons, (over, over_ribbons)
        assert 0.97 * solid_ribbons < solid < 1.03 * solid_ribbons, (solid, solid_ribbons)
        # Told always: its wide hairs lines too - thin ones.
        solid_always = count(rows["Among always"], magenta)
        print(f"told always: {solid_always} solid pixels")
        assert solid_always < 0.95 * solid_ribbons, (solid_always, solid_ribbons)
        for name in s.names:
            assert "problem" not in s.model(name), s.model(name)
    finally:
        s.close()


def test_hair_is_laid_out_as_drawn_only_on_the_card(run):
    """Hair posed on the graphics card can lie in its memory in the order
    it's drawn in ("hair_layout" 1, a number the settings window doesn't
    show: test_real_windows.py). Posed here, by the CPU - as on Linux and
    the Mac - there's nothing to lay out: FaceGetState's "laid" stays 0,
    and the picture is the same."""
    figure = os.path.join(run.home, "laid-here.vrm")
    strands = hair_character(figure, width=0.0006, wide_every=5)
    s = Stage(run, "Laid here")
    try:
        s.add("By point", figure, hair_layout=0)
        s.add("Laid out", figure, hair_layout=1)
        s.shown()
        assert wait_for(lambda: all(s.hair(name)["drawn"] > 0 for name in s.names), 10, 0.1)
        time.sleep(0.5)
        h = {name: s.hair(name) for name in s.names}
        for name in s.names:
            print(f"{name}: {h[name]}")
            assert not s.model(name)["on_card"] and "problem" not in s.model(name), s.model(name)
            assert h[name]["laid"] == 0 and h[name]["laid_mb"] == 0 and h[name]["pieces"] > strands, h[name]
        for key in ("drawn", "pieces", "lines", "draws"):
            assert h["By point"][key] == h["Laid out"][key], (key, h)
        a, b = s.shot("By point"), s.shot("Laid out")
        off = sum(1 for ra, rb in zip(a, b) for x, y in zip(ra, rb) if tuple(x) != tuple(y))
        print(f"posed here, by point and told laid out: {off} pixels differ")
        assert off == 0 and count(a, magenta) > 1500, (off, count(a, magenta))
    finally:
        s.close()


def test_hair_into_one_pixel_for_measuring(run):
    """For measuring what hair costs but for its pixels ("hair_measure" 1,
    a number the settings window doesn't show): every piece is sent as
    ever, and drawn into one pixel of the picture's corner - no hair is in
    the picture, the rest of the character is. Left alone, or 0: as ever."""
    figure = os.path.join(run.home, "measured.vrm")
    strands = hair_character(figure, width=0.0006, wide_every=5)
    s = Stage(run, "Measured")
    try:
        s.add("As ever", figure)
        s.add("Told 0", figure, hair_measure=0)
        s.add("One pixel", figure, hair_measure=1)
        s.shown()
        assert wait_for(lambda: all(s.hair(name)["drawn"] > 0 for name in s.names), 10, 0.1)
        time.sleep(0.5)
        h = {name: s.hair(name) for name in s.names}
        for name in s.names:
            print(f"{name}: {h[name]}")
            assert "problem" not in s.model(name), s.model(name)
        for key in ("drawn", "pieces", "lines", "draws"):
            assert h["As ever"][key] == h["One pixel"][key] == h["Told 0"][key], (key, h)
        assert h["As ever"]["pieces"] > strands, h
        a, b, c = s.shot("As ever"), s.shot("Told 0"), s.shot("One pixel")
        off = sum(1 for ra, rb in zip(a, b) for x, y in zip(ra, rb) if tuple(x) != tuple(y))
        hair_a, hair_c = count(a, magenta), count(c, magenta)
        solid_a, solid_c = (sum(1 for row in p for px in row if any(px)) for p in (a, c))
        print(f"as ever {hair_a} pixels of hair, {solid_a} drawn; into one pixel {hair_c}, {solid_c} drawn; "
              f"told 0: {off} pixels differ")
        assert off == 0 and hair_a > 1500 and hair_c <= 1, (off, hair_a, hair_c)
        # (The character under its hair is still there.)
        assert 0.2 * solid_a < solid_c < solid_a, (solid_a, solid_c)
    finally:
        s.close()


def test_fewer_hairs_are_drawn_the_smaller_it_is(run):
    """The same character drawn smaller has fewer of its strands drawn
    (each wider, to cover as much): FaceGetState's count falls with its
    size, while its hair covers about the same share of it in the
    picture."""
    figure = os.path.join(run.home, "sized.vrm")
    strands = hair_character(figure, width=0.0006)
    sizes = (100, 45, 20)
    s = Stage(run, "Sizes")
    try:
        for size in sizes:
            s.add(f"Size {size}", figure, size=size)
        s.shown()
        assert wait_for(lambda: all(s.hair(f"Size {size}")["drawn"] > 0 for size in sizes), 10, 0.1)
        time.sleep(0.5)
        drawn, hairy, body = {}, {}, {}
        for size in sizes:
            h = s.hair(f"Size {size}")
            rows = s.shot(f"Size {size}")
            drawn[size] = h["drawn"]
            hairy[size] = count(rows, lambda px: px[0] > 30 and px[2] > 30 and px[1] * 2 < min(px[0], px[2]))
            body[size] = count(rows, blue)
            print(f"size {size}: {h['drawn']} of {h['loaded']} strands drawn in {h['pieces']} pieces; "
                  f"{hairy[size]} pixels of hair, {body[size]} of its body")
            assert h["loaded"] == strands and h["share"] == 1, h
        assert drawn[100] > 0.9 * strands, drawn
        assert drawn[45] < 0.75 * drawn[100] and drawn[20] < 0.6 * drawn[45] and drawn[20] >= 16, drawn
        # Its hair's share of the picture next to its body's: about the
        # same at each size.
        cover = {size: hairy[size] / body[size] for size in sizes}
        print(f"hair for each pixel of body: {cover}")
        for size in sizes[1:]:
            assert 0.7 * cover[100] < cover[size] < 1.4 * cover[100], cover
    finally:
        s.close()


def test_the_hair_budget(run):
    """The hair budget ("hair_budget_ms", a number the settings window
    doesn't show: under 0 the hair detail setting's own, 0 none, over 0
    that many milliseconds of the graphics card's time a frame): far too
    small a budget brings the share of the hairs drawn down to its least
    and no further, and the count drawn with it; none lets them all
    back."""
    figure = os.path.join(run.home, "budget.vrm")
    strands = hair_character(figure)
    s = Stage(run, "Budget")
    try:
        s.add("Budget", figure, width=300, height=300)
        s.shown()
        assert wait_for(lambda: s.hair("Budget")["drawn"] == strands, 10, 0.1), s.hair("Budget")
        h = s.hair("Budget")
        assert h["share"] == 1 and h["budget_ms"] == 0, h
        # The graphics card's clock answers here (its time for the hair,
        # read a few frames late).
        clock = wait_for(lambda: s.hair("Budget")["ms"] > 0, 8, 0.1)
        print(f"hair's time on the graphics card: {s.hair('Budget')['ms']} ms "
              f"(its clock {'answers' if clock else 'is silent'})")
        s.set("Budget", hair_budget_ms=0.001)
        assert wait_for(lambda: abs(s.hair("Budget")["budget_ms"] - 0.001) < 1e-6, 5, 0.05), s.hair("Budget")
        if clock:
            # It starts low and drops while it's over, down to its least,
            # and stays there: still over, and never fewer (fewer, each
            # as much wider, and a coat is shards).
            assert wait_for(lambda: s.hair("Budget")["share"] < 0.5, 5, 0.02), s.hair("Budget")
            least = settles(lambda: s.hair("Budget")["share"])
            h = s.hair("Budget")
            print(f"a budget far too small: its least share {least}: {h}")
            assert least is not None and 0 < least < 0.25, h
            assert h["share"] == least and h["ms"] > h["budget_ms"], h
            assert h["loaded"] == strands and h["shown_strands"] == strands, h
            # That share of its strands, each as much wider.
            assert 16 <= h["drawn"] < 0.25 * strands, h
            assert abs(h["drawn"] - least * strands) <= 0.02 * strands, (h, least)
            rows = s.shot("Budget", "budget least")
            # (Each hair as much wider: still a head of hair.)
            assert count(rows, magenta) > 500, count(rows, magenta)
        else:
            # No clock to go by: the budget leaves the hair as it is.
            time.sleep(2)
            h = s.hair("Budget")
            assert h["share"] == 1 and h["drawn"] == strands and h["ms"] == -1, h
        # The setting's own: Full's.
        s.set("Budget", hair_budget_ms=-1)
        assert wait_for(lambda: s.hair("Budget")["budget_ms"] == 1, 5, 0.05), s.hair("Budget")
        # None: all of them again.
        s.set("Budget", hair_budget_ms=0)
        assert wait_for(lambda: s.hair("Budget")["share"] == 1 and s.hair("Budget")["drawn"] == strands, 5, 0.05), \
            s.hair("Budget")
        assert s.hair("Budget")["budget_ms"] == 0
    finally:
        s.close()


def test_hair_moves_with_its_expression_and_its_bones(run):
    """Its expression moves its hair (the file's own move of the strand
    part's points: the hair on top of its head lifted), and its bones take
    their hair with them: the hair on its head tips over with its neck,
    the locks down its sides bend (their tips, on its chest, stay), and
    the tuft on its hand is at its hand wherever its arm is."""
    figure = os.path.join(run.home, "moves.vrm")
    hair_character(figure)
    plain = os.path.join(run.home, "moves-plain.glb")
    hair_character(plain, vrm=False)
    s = Stage(run, "Moves")
    try:
        # (Smaller, so there's room over its head for its hair to lift into.)
        s.add("Moves", figure, size=60)
        s.add("Moves plain", plain, size=60)
        s.shown()
        assert wait_for(lambda: s.hair("Moves")["drawn"] > 0 and s.hair("Moves plain")["drawn"] > 0, 10, 0.1)
        time.sleep(0.5)
        rest = s.shot("Moves", "moves rest")
        metre = per_metre(rest)
        top = top_of(rest, magenta)
        bx, by, _ = middle_of(rest, blue)
        low = (0, int(by) - 20, SIZE, SIZE)
        locks = count(rest, magenta, low)
        assert 150 < metre < 300 and top is not None and locks > 150, (metre, top, locks)

        # Its expression: the hair on its head up by LIFT, the rest where
        # it was; then back.
        answer = s.cue("Moves", do="face", face="happy", seconds=6)
        assert answer["ok"] and answer["shows"], answer
        assert wait_for(lambda: s.model("Moves")["expressions"].get("happy", 0) >= 0.99, 5, 0.05), \
            s.model("Moves")["expressions"]
        time.sleep(0.3)
        lifted = s.shot("Moves", "moves lifted")
        up = top - top_of(lifted, magenta)
        print(f"its expression: the top of its hair up {up} pixels ({up / metre:.3f} m; the file says {LIFT})")
        assert abs(up / metre - LIFT) < 0.02, (up, metre)
        assert abs(count(lifted, magenta, low) - locks) < 0.05 * locks, (count(lifted, magenta, low), locks)
        assert s.cue("Moves", do="stop")["ok"]
        assert wait_for(lambda: s.model("Moves")["expressions"].get("happy", 0) == 0, 8, 0.05), \
            s.model("Moves")["expressions"]
        time.sleep(0.3)
        assert differing(s.shot("Moves"), rest) <= 20

        # Its neck tipped over (its own animation, holding still): the
        # hair on its head goes with it.
        def head_and_hair(rows):
            """Its head's middle (x), and the middle of its hair above its
            body."""
            body_top = top_of(rows, blue)
            return middle_of(rows, orange)[0], middle_of(rows, magenta, (0, 0, SIZE, body_top))[0]
        hx, mx = head_and_hair(rest)
        s.set("Moves", animation="Tilt")
        assert wait_for(lambda: s.model("Moves")["animation"] == "Tilt", 5, 0.1), s.model("Moves")
        time.sleep(0.6)
        tilted = s.shot("Moves", "moves tilted")
        tx, ux = head_and_hair(tilted)
        print(f"its neck tipped {TILT} degrees: its head from x {hx:.0f} to {tx:.0f}, its hair from "
              f"{mx:.0f} to {ux:.0f} ({metre:.0f} pixels a metre)")
        # (Its head's middle is 0.18 over its neck: tipped 30 degrees it
        # goes half that to the side - and its hair, higher up, further.)
        assert 0.05 * metre < hx - tx < 0.13 * metre, (hx, tx, metre)
        assert 0.07 * metre < mx - ux < 0.2 * metre and mx - ux > 0.9 * (hx - tx), (mx, ux, hx, tx, metre)
        assert abs(count(tilted, magenta) - count(rest, magenta)) < 0.2 * count(rest, magenta)
        # The locks' tips, on its chest, stay where they were.
        tips = (0, int(by), SIZE, SIZE)
        assert abs(count(tilted, magenta, tips) - count(rest, magenta, tips)) < 0.25 * count(rest, magenta, tips)
        s.set("Moves", animation="")
        assert wait_for(lambda: s.model("Moves")["animation"] == "", 5, 0.1)

        # The tuft on its hand: by its hand with its arm down (a VRM's
        # arms hang) and with it straight out (the plain file's T).
        hands = {}
        for name, rows in (("Moves", rest), ("Moves plain", s.shot("Moves plain", "moves plain"))):
            cx = middle_of(rows, blue)[0]
            rx, ry, n = middle_of(rows, red, (int(cx), 0, SIZE, SIZE))
            assert n > 15, n
            tuft = count(rows, magenta, (int(rx) - 30, int(ry) - 45, int(rx) + 60, int(ry) + 30))
            hands[name] = (rx - cx, ry, tuft)
            print(f"{name}: its hand {rx - cx:.0f} right of its body, at y {ry:.0f}; {tuft} pixels of hair by it")
            assert tuft > 150, tuft
        assert hands["Moves plain"][0] > hands["Moves"][0] + 0.15 * metre, hands
        assert hands["Moves"][1] > hands["Moves plain"][1] + 0.15 * metre, hands
    finally:
        s.close()


def holed_cards(path, near_first):
    """Two see-through parts (glTF's BLEND), one behind the other: a green
    card, and nearer the camera a red one whose picture is empty in its
    middle half (a frame). `near_first`: the red one is drawn first (its
    part's middle is further off than the green one's: more of it, wholly
    empty, lies behind them both) - else the green one is."""
    # A picture solid round its edge, empty (alpha 0) in its middle half.
    frame = [bytes(v for x in range(16) for v in ((255, 255, 255, 0) if 4 <= x < 12 and 4 <= y < 12
                                                   else (255, 255, 255, 255))) for y in range(16)]
    png = png_bytes(path + ".png", 16, 16, frame)

    def card(z, half, uv, hidden):
        pos = [(-half, -half, z), (half, -half, z), (half, half, z), (-half, half, z)]
        uvs = list(uv)
        ix = [0, 1, 2, 0, 2, 3]
        if hidden:
            # More of the part, behind them both and wholly empty (its
            # picture's middle): never seen, but it puts the part's middle
            # further off than the other's.
            extra, extra_uv = [], []
            for k in range(6):
                z_back = -0.02 - 0.002 * k
                extra += [(-0.05, -0.05, z_back), (0.05, -0.05, z_back), (0.05, 0.05, z_back), (-0.05, 0.05, z_back)]
                extra_uv += [(0.5, 0.5)] * 4
            ix = [i + k * 4 for k in range(6) for i in ix] + [24 + i for i in ix]
            pos, uvs = extra + pos, extra_uv + uvs
        return pos, [(0, 0, 1)] * len(pos), ix, uvs
    whole = [(0, 1), (1, 1), (1, 0), (0, 0)]
    solid = [(0.05, 0.05), (0.15, 0.05), (0.15, 0.15), (0.05, 0.15)]
    far = card(0.0, 0.1, solid, not near_first)
    near = card(0.05, 0.08, whole, near_first)
    write_glb(path, [far[:3] + ((0.05, 0.6, 0.08), 0.6), near[:3] + ((0.8, 0.02, 0.02), 0.6)],
              materials_more={0: {"alphaMode": "BLEND"}, 1: {"alphaMode": "BLEND"}},
              pictures={0: (far[3], png), 1: (near[3], png)})


def test_empty_pixels_of_a_see_through_part_hide_nothing(run):
    """A see-through part's empty pixels (its picture's alpha 0: the gaps
    of a hair card) no longer hide what's drawn after them. Two
    see-through cards, a red frame in front of a green card: the green
    shows through the frame's empty middle whichever of them is drawn
    first (before, the frame drawn first cut its middle out of the green
    one)."""
    files = {}
    for name, near_first in (("Far first", False), ("Near first", True)):
        files[name] = os.path.join(run.home, name.lower().replace(" ", "-") + ".glb")
        holed_cards(files[name], near_first)
    s = Stage(run, "Cards")
    try:
        for name, path in files.items():
            s.add(name, path, width=200, height=200, look="flat")
        s.shown()
        time.sleep(0.5)
        shots = {name: s.shot(name, "cards " + name) for name in files}
        for name, rows in shots.items():
            mid = [rows[y][x] for y in range(85, 115) for x in range(85, 115)]
            through = sum(1 for px in mid if green(px))
            # (Dark, inside the cards: where neither shows.)
            dark = sum(1 for y in range(30, 170) for x in range(30, 170) if max(rows[y][x]) < 60)
            print(f"{name}: {through} of {len(mid)} pixels in the frame's middle green; "
                  f"{count(rows, red)} red pixels, {count(rows, green)} green in all; {dark} dark inside")
            assert through == len(mid), (name, through, mid[:5])
            # The frame itself, and the green card round it.
            assert all(red(rows[y][x]) for x, y in ((40, 100), (160, 100), (100, 40), (100, 160))), name
            assert all(green(rows[y][x]) for x, y in ((16, 100), (184, 100), (100, 16), (100, 184))), name
            assert count(rows, red) > 12000 and count(rows, green) > 10000, name
    finally:
        s.close()


def test_a_3d_hand_in_front_hides_the_hair_behind_it(run):
    """A thing shown with the character (the headpat's 3D hand, here a
    green ball) hides the hairs behind its solid parts: a sheet of hair
    standing behind its head shows round the hand on its head, never
    through it - nor through its head."""
    figure = os.path.join(run.home, "fan.vrm")
    strands = hair_character(figure, crown=0, locks=0, tuft=0, fan=900)
    hand = os.path.join(run.home, "ball-hand.glb")
    write_glb(hand, [ball_points(0, 0.02, 0, 0.07, 0.07, 0.07) + ((0.05, 0.6, 0.08), 0.6)])
    s = Stage(run, "Fan")
    try:
        s.add("Fan", figure, size=70, react_hand_3d=hand)
        s.shown()
        assert wait_for(lambda: s.state("Fan")["react"]["hand_3d"], 20, 0.1), s.state("Fan")["react"]
        assert wait_for(lambda: s.hair("Fan")["drawn"] == strands, 10, 0.1), s.hair("Fan")
        time.sleep(0.5)
        rest = s.shot("Fan", "fan rest")
        # Its head in front of the sheet: all there, no hair through it.
        hx, hy, head = middle_of(rest, orange)
        r = math.sqrt(count(rest, lambda px: px[0] > 100 and px[1] > 40 and px[2] < 0.6 * px[1]) / math.pi)
        inside = [rest[y][x] for y in range(int(hy - r), int(hy + r)) for x in range(int(hx - r), int(hx + r))
                  if (x - hx) ** 2 + (y - hy) ** 2 < (0.8 * r) ** 2]
        print(f"its head: {head} orange pixels round ({hx:.0f}, {hy:.0f}), about {r:.0f} across the half; "
              f"{sum(1 for px in inside if magenta(px))} of {len(inside)} inside it magenta")
        assert r > 12 and not any(magenta(px) for px in inside), (r, sum(1 for px in inside if magenta(px)))
        assert magenta(rest[int(hy)][int(hx - 1.6 * r)]) and magenta(rest[int(hy)][int(hx + 1.6 * r)])
        assert magenta(rest[int(hy - 1.6 * r)][int(hx)])
        assert count(rest, green, (0, 0, SIZE, int(hy))) == 0

        # The hand on its head, in front of the sheet.
        assert s.cue("Fan", do="pat", count=3, side=1, face="")["ok"]
        shots = quick_shots(s.c, "Fan", 1.6)
        over = (0, 0, SIZE, int(hy))
        _, rows = max(shots, key=lambda shot: count(shot[1], green, over))
        keep_shot(rows, "fan patted")
        gx, gy, n = middle_of(rows, green, over)
        assert n > 300, n
        gr = math.sqrt(n / math.pi)
        ball = [rows[y][x] for y in range(int(gy - gr), int(gy + gr)) for x in range(int(gx - gr), int(gx + gr))
                if (x - gx) ** 2 + (y - gy) ** 2 < (0.75 * gr) ** 2 and y < hy - r]
        through = sum(1 for px in ball if magenta(px))
        before = sum(1 for y in range(int(gy - gr), int(gy + gr)) for x in range(int(gx - gr), int(gx + gr))
                     if (x - gx) ** 2 + (y - gy) ** 2 < (0.75 * gr) ** 2 and y < hy - r and magenta(rest[y][x]))
        print(f"the hand: {n} green pixels round ({gx:.0f}, {gy:.0f}); in its middle {through} of {len(ball)} "
              f"pixels magenta (with no hand there: {before})")
        assert len(ball) > 100 and before > 0.9 * len(ball), (len(ball), before)
        assert through == 0 and sum(1 for px in ball if green(px)) > 0.97 * len(ball), through
        # ...and the sheet still shows beside it.
        assert count(rows, magenta, over) > 0.5 * count(rest, magenta, over)
        assert wait_for(lambda: not s.state("Fan")["react"]["patting"], 8, 0.1)
    finally:
        s.close()


def wolf_with_hair(path, strands=400):
    """The test wolf (tests/data/companion) with a tuft of lime hair
    standing on its head, hung on its head bone."""
    shutil.copy(WOLF, path)
    doc, blob = open_glb(path)
    names = {n.get("name"): i for i, n in enumerate(doc["nodes"])}
    head = world_of(doc, names["head"])
    hair = Strands()
    for i in range(strands):
        a, b = (i * 0.61803) % 1, (i * 0.75488) % 1
        root = (-0.14 + 0.28 * a, head[1] + 0.18, head[2] - 0.12 + 0.24 * b)
        points = [(root[0] * (1 + 0.6 * t), root[1] + 0.3 * t, root[2]) for t in (0, 0.5, 1)]
        hair.add(points, 0.01, LIME, [[(0, 1.0)]] * 3)
    add_strands(doc, blob, hair, [names["head"]], top=True)
    save_glb(path, doc, blob)
    return hair.count


def test_a_companions_hair(run):
    """A companion with hair strands of its own has them drawn beside the
    character's, and FaceGetState counts both while it's here."""
    figure = os.path.join(run.home, "with-pet.vrm")
    strands = hair_character(figure)
    wolf = os.path.join(run.home, "hairy-wolf.glb")
    its = wolf_with_hair(wolf)
    w, h = 600, 300
    s = Stage(run, "Pet")
    try:
        s.add("Pet", figure, width=w, height=h, size=60)
        s.shown()
        assert wait_for(lambda: s.hair("Pet")["drawn"] == strands, 10, 0.1), s.hair("Pet")
        time.sleep(0.3)
        alone = s.shot("Pet", "pet alone")
        assert s.hair("Pet")["shown_strands"] == strands and count(alone, lime) == 0
        answer = s.cue("Pet", do="companion", action="come", file=wolf, instant=True,
                       place={"size": 0.7, "side": -1, "distance": 0.45}, view={"zoom": 0.8, "x": 0.05})
        assert answer["ok"] and answer["came"], answer
        assert wait_for(lambda: s.state("Pet")["companion"]["phase"] == "out" and
                        s.state("Pet")["companion"]["drawn"], 20, 0.05), s.state("Pet")["companion"]
        assert wait_for(lambda: s.hair("Pet")["shown_strands"] == strands + its, 5, 0.05), s.hair("Pet")
        time.sleep(1.0)  # the camera's glide
        hair = s.hair("Pet")
        print(f"with its companion: {hair}")
        # (The character's own file still has only its own.)
        assert hair["strands"] == strands and hair["loaded"] == strands, hair
        assert hair["shown_loaded"] == strands + its and hair["drawn"] == strands + its, hair
        rows = s.shot("Pet", "pet with companion")
        bx = middle_of(rows, blue)[0]
        lx, ly, n = middle_of(rows, lime)
        print(f"its companion's hair: {n} lime pixels round ({lx:.0f}, {ly:.0f}); the character at x {bx:.0f}")
        assert n > 150 and lx < bx - 40, (lx, ly, n, bx)
        assert count(rows, magenta) > 800, count(rows, magenta)
        # Gone again: only the character's.
        assert s.cue("Pet", do="companion", action="go", instant=True)["went"]
        assert wait_for(lambda: s.state("Pet")["companion"]["phase"] == "away", 3, 0.05)
        assert wait_for(lambda: s.hair("Pet")["shown_strands"] == strands, 5, 0.05), s.hair("Pet")
        time.sleep(0.3)
        assert count(s.shot("Pet"), lime) == 0
        assert s.cue("Pet", do="companion", action="off")["ok"]
    finally:
        s.close()


def test_a_hair_of_only_two_points(run):
    """The shortest strand a file can have - two points, one straight
    piece (short fur is exported so) - loads and is drawn like any other.
    (In an OBS of its own: this one took OBS down with it.)"""
    figure = os.path.join(run.home, "two-points.vrm")
    arms_character(figure)
    doc, blob = open_glb(figure)
    names = {n.get("name"): i for i, n in enumerate(doc["nodes"])}
    hair = Strands()
    for i in range(600):
        a, b = (i * 0.61803) % 1, (i * 0.75488) % 1
        x = -0.1 + 0.2 * a
        hair.add([(x, 1.78, -0.05 + 0.1 * b), (x * 1.5, 1.95, -0.05 + 0.1 * b)], 0.006, MAGENTA,
                 [[(0, 1.0)]] * 2)
    add_strands(doc, blob, hair, [names["head"]])
    save_glb(figure, doc, blob)
    own = Obs()
    try:
        s = Stage(own, "Two points")
        try:
            s.add("Two points", figure)
            assert wait_for(lambda: s.model("Two points")["shown"], 30, 0.2), s.model("Two points")
            assert wait_for(lambda: s.hair("Two points")["drawn"] > 0, 10, 0.1), s.hair("Two points")
        except Exception as e:
            # (A moment for it to be gone, if it's going.)
            try:
                own.proc.wait(10)
            except Exception:
                pass
            if own.proc.poll() is not None:
                raise AssertionError("OBS went down loading a strand of two points (exit code %s)"
                                     % own.proc.returncode) from e
            raise
        time.sleep(0.5)
        h = s.hair("Two points")
        print(f"strands of two points: {h}")
        assert h["strands"] == 600 and h["loaded"] == 600 and h["drawn"] == 600 and h["pieces"] == 600, h
        assert count(s.shot("Two points", "two points"), magenta) > 500
        s.close()
    finally:
        if own.proc.poll() is None:
            own.close()
        else:
            own.xvfb.terminate()
        if not os.environ.get("DIREBOT_OBS_KEEP"):
            shutil.rmtree(own.home, ignore_errors=True)


def test_closing_obs_leaves_nothing_behind(run):
    assert run.close() == 0
    log = run.log()
    assert "Number of memory leaks: 0" in log
    # Every value a hair technique reads was set just before it (Windows
    # won't draw one with any missing), and every number a point comes with
    # is read by its technique (OpenGL won't draw one with any left out).
    assert "Windows won't draw it" not in log
    assert "Could not find attribute" not in log and "device_draw" not in log
