"""Google MediaPipe's own answers for the face tracker's test pictures.

Writes tests/data/face/expected.json, which tests/unit_face.cpp checks our
tracker against: the face's 478 points, its 52 expressions, its head pose
and the face finder's box, from MediaPipe's Face Landmarker and Face
Detector (IMAGE mode, the same face_landmarker.task as data/face).

Test only - MediaPipe is never part of DireBot for OBS:
    pip install mediapipe
    python tools/face-expected.py

The pictures are tests/data/face/portrait.ppm and three made from it with
whole-number sums, so the C++ test makes exactly the same pixels:
mirrored, turned (by the angle whose cosine is 12/13, nearest pixel), and
half size (each 2 x 2 averaged).
"""
import json
import os

import numpy as np
import mediapipe as mp
from mediapipe.tasks import python as mpt
from mediapipe.tasks.python import vision

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TASK = os.path.join(ROOT, "data", "face", "face_landmarker.task")
DATA = os.path.join(ROOT, "tests", "data", "face")


def read_ppm(path):
    data = open(path, "rb").read()
    magic, w, h, top, rest = data.split(maxsplit=4)
    assert magic == b"P6" and top == b"255"
    w, h = int(w), int(h)
    return np.frombuffer(rest[: w * h * 3], np.uint8).reshape(h, w, 3)


def mirrored(px):
    return px[:, ::-1].copy()


def turned(px):
    # Each pixel takes the one the turn lands it on: in doubled
    # coordinates from the middle, (12 dx + 5 dy, -5 dx + 12 dy) / 13.
    h, w = px.shape[:2]
    out = np.zeros_like(px)
    for y in range(h):
        for x in range(w):
            dx, dy = 2 * x + 1 - w, 2 * y + 1 - h
            sx = (12 * dx + 5 * dy + 13 * (w - 1) + 13) // 26
            sy = (-5 * dx + 12 * dy + 13 * (h - 1) + 13) // 26
            if 0 <= sx < w and 0 <= sy < h:
                out[y, x] = px[sy, sx]
    return out


def halved(px):
    h, w = px.shape[0] // 2 * 2, px.shape[1] // 2 * 2
    p = px[:h, :w].astype(np.int32)
    s = p[0::2, 0::2] + p[0::2, 1::2] + p[1::2, 0::2] + p[1::2, 1::2]
    return ((s + 2) // 4).astype(np.uint8)


def answers(px):
    h, w = px.shape[:2]
    image = mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(px))
    landmarker = vision.FaceLandmarker.create_from_options(vision.FaceLandmarkerOptions(
        base_options=mpt.BaseOptions(model_asset_path=TASK), running_mode=vision.RunningMode.IMAGE,
        num_faces=1, output_face_blendshapes=True, output_facial_transformation_matrixes=True))
    r = landmarker.detect(image)
    out = {"w": w, "h": h, "found": bool(r.face_landmarks)}
    if r.face_landmarks:
        out["points"] = [[round(p.x * w, 4), round(p.y * h, 4), round(p.z * w, 4)] for p in r.face_landmarks[0]]
        out["expressions"] = [round(c.score, 6) for c in r.face_blendshapes[0]]
        out["names"] = [c.category_name for c in r.face_blendshapes[0]]
        out["pose"] = [round(float(v), 6) for v in np.array(r.facial_transformation_matrixes[0]).ravel()]
    # The face finder alone, from the same file.
    import zipfile
    finder = zipfile.ZipFile(TASK).read("face_detector.tflite")
    detector = vision.FaceDetector.create_from_options(vision.FaceDetectorOptions(
        base_options=mpt.BaseOptions(model_asset_buffer=finder), running_mode=vision.RunningMode.IMAGE))
    out["finds"] = [{"box": [d.bounding_box.origin_x, d.bounding_box.origin_y, d.bounding_box.width,
                             d.bounding_box.height],
                     "score": round(d.categories[0].score, 6),
                     "keys": [[round(k.x * w, 4), round(k.y * h, 4)] for k in d.keypoints]}
                    for d in detector.detect(image).detections]
    return out


def main():
    px = read_ppm(os.path.join(DATA, "portrait.ppm"))
    result = {name: answers(make(px)) for name, make in
              [("plain", lambda p: p), ("mirrored", mirrored), ("turned", turned), ("halved", halved)]}
    result["mediapipe"] = mp.__version__
    with open(os.path.join(DATA, "expected.json"), "w") as f:
        json.dump(result, f, separators=(",", ":"))
    for name in ("plain", "mirrored", "turned", "halved"):
        print(name, "found" if result[name]["found"] else "not found", len(result[name]["finds"]), "finds")


if __name__ == "__main__":
    main()
