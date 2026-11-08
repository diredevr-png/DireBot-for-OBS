"""Google MediaPipe's own answers for the body tracker's test pictures.

Writes tests/data/body/expected.json, which tests/unit_body.cpp checks our
body tracker (src/face/body.cpp) against:

  - "pictures": MediaPipe's Holistic Landmarker (IMAGE mode, a new one for
    each picture, the same holistic_landmarker.task as data/face) on
    astronaut.ppm, the same mirrored, and camera.pgm: the body's 33 points
    (picture pixels, how likely each is in sight), the same in metres, and
    each hand if it finds one.
  - "following": the same in VIDEO mode, astronaut.ppm four times 67 ms
    apart (each look from where the body was, smoothed).
  - "models": each model on its own through LiteRT (Google's own TFLite
    runtime) on a piece of astronaut.ppm, for our engine to match number
    for number: the body finder, the body model, the hand-square model,
    the hand model, the lighter hand model and the palm finder (data/face).
  - "hands": MediaPipe's Hand Landmarker (its palm finder, then its hand
    model; the same two models as data/face, put together as its .task) on
    thumb_up.ppm, hands.ppm and hands.ppm mirrored: each hand's 21 points
    and how sure it is it's a right hand, on a first look (IMAGE mode) and
    after six looks running (VIDEO mode: each hand followed from its own
    points from the second look) - and on astronaut.ppm and camera.pgm,
    where it finds none.

Test only - MediaPipe and LiteRT are never part of DireBot for OBS:
    pip install mediapipe ai-edge-litert
    python tools/body-expected.py

Holistic finds no hands on astronaut.ppm and camera.pgm: the hand steps
from the body are checked on the hand models' numbers, and the hands
found on their own (the palm finder, then the hand model) on Google's own
pictures of hands, thumb_up.ppm and hands.ppm (tests/data/body/README.md).
"""
import io
import json
import os
import zipfile

import numpy as np
import mediapipe as mp
from mediapipe.tasks import python as mpt
from mediapipe.tasks.python import vision
from ai_edge_litert.interpreter import Interpreter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TASK = os.path.join(ROOT, "data", "face", "holistic_landmarker.task")
LITE = os.path.join(ROOT, "data", "face", "hand_landmark_lite.tflite")
PALM = os.path.join(ROOT, "data", "face", "palm_detection_full.tflite")
DATA = os.path.join(ROOT, "tests", "data", "body")


def read_pnm(path):
    data = open(path, "rb").read()
    magic, w, h, top, rest = data.split(maxsplit=4)
    assert magic in (b"P5", b"P6") and top == b"255"
    w, h = int(w), int(h)
    if magic == b"P5":
        grey = np.frombuffer(rest[: w * h], np.uint8).reshape(h, w)
        return np.repeat(grey[:, :, None], 3, axis=2)
    return np.frombuffer(rest[: w * h * 3], np.uint8).reshape(h, w, 3)


def r(v, digits=4):
    return round(float(v), digits)


def holistic(px):
    h, w = px.shape[:2]
    image = mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(px))
    landmarker = vision.HolisticLandmarker.create_from_options(vision.HolisticLandmarkerOptions(
        base_options=mpt.BaseOptions(model_asset_path=TASK), running_mode=vision.RunningMode.IMAGE))
    res = landmarker.detect(image)
    landmarker.close()
    out = {"w": w, "h": h, "found": bool(res.pose_landmarks)}
    if res.pose_landmarks:
        out["points"] = [[r(p.x * w), r(p.y * h), r(p.z * w), r(p.visibility or 0)] for p in res.pose_landmarks]
        out["world"] = [[r(p.x, 5), r(p.y, 5), r(p.z, 5)] for p in res.pose_world_landmarks]
    for key, hand in (("left_hand", res.left_hand_landmarks), ("right_hand", res.right_hand_landmarks)):
        out[key] = [[r(p.x * w), r(p.y * h), r(p.z * w)] for p in hand] if hand else []
    return out


def following(px, looks=4, ms=67):
    """MediaPipe's VIDEO mode on the same picture `looks` times, `ms` apart:
    each look after the first starts from where the body was, and its own
    smoothing (One Euro) is on."""
    h, w = px.shape[:2]
    landmarker = vision.HolisticLandmarker.create_from_options(vision.HolisticLandmarkerOptions(
        base_options=mpt.BaseOptions(model_asset_path=TASK), running_mode=vision.RunningMode.VIDEO))
    image = mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(px))
    out = []
    for i in range(looks):
        res = landmarker.detect_for_video(image, i * ms)
        out.append([[r(p.x * w), r(p.y * h)] for p in res.pose_landmarks])
    landmarker.close()
    return {"ms": ms, "looks": out}


def hand_task():
    """MediaPipe's Hand Landmarker's .task from data/face's own files: the
    palm finder (the same model as the hand_detector.tflite in Google's
    hand_landmarker.task) and the hand model from holistic_landmarker.task."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("hand_detector.tflite", open(PALM, "rb").read())
        z.writestr("hand_landmarks_detector.tflite",
                   zipfile.ZipFile(TASK).read("hand_landmarks_detector.tflite"))
    return out.getvalue()


def hands(px, looks=6, ms=33):
    """MediaPipe's Hand Landmarker (two hands at most) on a picture: a first
    look, and `looks` looks running `ms` apart. Each hand: its 21 points
    (picture pixels) and how sure the hand model is that it's a right hand
    (its own number: MediaPipe calls a hand "Right" when it's over 0.5)."""
    h, w = px.shape[:2]
    image = mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(px))
    task = hand_task()

    def each(res):
        out = []
        for hd, lm in zip(res.handedness, res.hand_landmarks):
            right = hd[0].score if hd[0].category_name == "Right" else 1 - hd[0].score
            out.append({"right": r(right), "points": [[r(p.x * w), r(p.y * h)] for p in lm]})
        return out
    landmarker = vision.HandLandmarker.create_from_options(vision.HandLandmarkerOptions(
        base_options=mpt.BaseOptions(model_asset_buffer=task), running_mode=vision.RunningMode.IMAGE,
        num_hands=2))
    first = each(landmarker.detect(image))
    landmarker.close()
    landmarker = vision.HandLandmarker.create_from_options(vision.HandLandmarkerOptions(
        base_options=mpt.BaseOptions(model_asset_buffer=task), running_mode=vision.RunningMode.VIDEO,
        num_hands=2))
    for i in range(looks):
        res = landmarker.detect_for_video(image, i * ms)
    landmarker.close()
    return {"w": w, "h": h, "first": first, "looks": looks, "ms": ms, "followed": each(res)}


def run(model, inp):
    net = Interpreter(model_content=model)
    net.allocate_tensors()
    d = net.get_input_details()[0]
    net.set_tensor(d["index"], inp[None].astype(np.float32))
    net.invoke()
    return [net.get_tensor(o["index"]).reshape(-1) for o in net.get_output_details()], \
        [list(map(int, o["shape"])) for o in net.get_output_details()]


def models(astronaut):
    task = zipfile.ZipFile(TASK)
    px = astronaut.astype(np.float32)
    out = {}
    # The body finder: 224 x 224 from (144, 144), -1 to 1. Its 8 surest
    # anchors: the score (before the sigmoid) and the 12 box numbers.
    finder, shapes = run(task.read("pose_detector.tflite"), px[144:368, 144:368] * (2 / 255) - 1)
    boxes, scores = (finder[0], finder[1]) if shapes[0][-1] == 12 else (finder[1], finder[0])
    best = np.argsort(-scores)[:8]
    out["finder"] = {"from": [144, 144], "size": 224,
                     "best": [{"anchor": int(i), "score": r(scores[i]),
                               "box": [r(v) for v in boxes[i * 12:(i + 1) * 12]]} for i in best]}
    # The body model: 256 x 256 from (128, 0), 0 to 1. Its points, how
    # sure, and in metres - and its heat map, each point's highest value
    # and where.
    body, shapes = run(task.read("pose_landmarks_detector.tflite"), px[0:256, 128:384] / 255)
    heat = body[3].reshape(shapes[3][1] * shapes[3][2], shapes[3][3])
    out["body"] = {"from": [128, 0], "size": 256, "points": [r(v) for v in body[0]],
                   "presence": r(body[1][0], 6), "world": [r(v, 6) for v in body[4]],
                   "heat_top": [r(v) for v in heat.max(axis=0)],
                   "heat_at": [int(i) for i in heat.argmax(axis=0)]}
    # The hand-square model, same piece.
    recrop, _ = run(task.read("hand_roi_refinement.tflite"), px[0:256, 128:384] / 255)
    out["recrop"] = {"from": [128, 0], "size": 256, "points": [r(v) for v in recrop[0]]}
    # The hand models: 224 x 224 from (150, 200), 0 to 1, all they say.
    for key, model in (("hand", task.read("hand_landmarks_detector.tflite")), ("lite", open(LITE, "rb").read())):
        hand, shapes = run(model, px[200:424, 150:374] / 255)
        sizes = [int(np.prod(s)) for s in shapes]
        out[key] = {"from": [150, 200], "size": 224, "sizes": sizes,
                    "outputs": [[r(v, 6) for v in o] for o in hand]}
    # The palm finder: 192 x 192 from (160, 160), 0 to 1. Its 8 surest
    # anchors: the score (before the sigmoid) and the 18 box numbers.
    palm, shapes = run(open(PALM, "rb").read(), px[160:352, 160:352] / 255)
    boxes, scores = (palm[0], palm[1]) if shapes[0][-1] == 18 else (palm[1], palm[0])
    best = np.argsort(-scores)[:8]
    out["palm"] = {"from": [160, 160], "size": 192,
                   "best": [{"anchor": int(i), "score": r(scores[i]),
                             "box": [r(v) for v in boxes[i * 18:(i + 1) * 18]]} for i in best]}
    return out


def main():
    astronaut = read_pnm(os.path.join(DATA, "astronaut.ppm"))
    camera = read_pnm(os.path.join(DATA, "camera.pgm"))
    thumb = read_pnm(os.path.join(DATA, "thumb_up.ppm"))
    two = read_pnm(os.path.join(DATA, "hands.ppm"))
    result = {"pictures": {"astronaut": holistic(astronaut),
                           "mirrored": holistic(astronaut[:, ::-1].copy()),
                           "camera": holistic(camera)},
              "following": following(astronaut),
              "models": models(astronaut),
              "hands": {"thumb_up": hands(thumb), "hands": hands(two),
                        "hands_mirrored": hands(two[:, ::-1].copy()),
                        "astronaut": hands(astronaut), "camera": hands(camera)},
              "mediapipe": mp.__version__}
    with open(os.path.join(DATA, "expected.json"), "w") as f:
        json.dump(result, f, separators=(",", ":"))
    for name, p in result["pictures"].items():
        print(name, "found" if p["found"] else "not found", "hands:", len(p["left_hand"]), len(p["right_hand"]))
    for name, p in result["hands"].items():
        print(name, "hands found by the palm finder:", len(p["first"]), "then", len(p["followed"]))


if __name__ == "__main__":
    main()
