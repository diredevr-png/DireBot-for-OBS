"""Our face tracker against Google MediaPipe's own, on any video.

    pip install mediapipe numpy        (test only, never part of the add-on)
    python tools/face-video-check.py my-clip.mp4

Both follow the face through the video (MediaPipe's video mode, the same
face_landmarker.task as data/face), and it says how often each found the
face, how far apart their points, expressions and head turns are, and how
long a look took. The video stays where it is: nothing is copied into the
repo (a streamer's own face isn't test data to share).

Needs ffmpeg and a C++17 compiler (it builds tools/face-look.cpp).
Fast moves can differ by a pixel or two even with the very same sums: a
look starts from the last one, so the smallest rounding difference grows
for a few frames and settles again.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TASK = os.path.join(ROOT, "data", "face", "face_landmarker.task")
RECORD = 4 + 478 * 3 + 52 + 16


def build(out):
    src = [os.path.join(ROOT, "src", "face", f) for f in ("tracker.cpp", "pose.cpp")]
    nn = os.path.join(ROOT, "src", "face", "nn")
    cxx = os.environ.get("CXX", "g++")
    flags = ["-std=c++17", "-O2", "-pthread", "-I" + os.path.join(ROOT, "deps")]
    objs = []
    for name in ("net", "kernels", "kernels-sse2", "kernels-avx2"):
        obj = os.path.join(out, name + ".o")
        extra = ["-mavx2", "-mfma"] if name == "kernels-avx2" else []
        subprocess.run([cxx, *flags, *extra, "-c", os.path.join(nn, name + ".cpp"), "-o", obj], check=True)
        objs.append(obj)
    exe = os.path.join(out, "face-look")
    subprocess.run([cxx, *flags, "-o", exe, os.path.join(HERE, "face-look.cpp"), *src, *objs], check=True)
    return exe


def video_size(path):
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                            "stream=width,height,r_frame_rate", "-of", "csv=p=0", path],
                           check=True, capture_output=True, text=True).stdout.split(",")
    num, den = probe[2].strip().split("/")
    return int(probe[0]), int(probe[1]), float(num) / float(den)


def frames(path, w, h, fps):
    return subprocess.Popen(["ffmpeg", "-v", "error", "-i", path, "-vf", "scale=%d:%d,fps=%g" % (w, h, fps),
                             "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)


def mediapipe(path, w, h, fps, out):
    import mediapipe as mp
    from mediapipe.tasks import python as mpt
    from mediapipe.tasks.python import vision
    landmarker = vision.FaceLandmarker.create_from_options(vision.FaceLandmarkerOptions(
        base_options=mpt.BaseOptions(model_asset_path=TASK), running_mode=vision.RunningMode.VIDEO,
        num_faces=1, output_face_blendshapes=True, output_facial_transformation_matrixes=True))
    video = frames(path, w, h, fps)
    records = []
    i = 0
    while True:
        buf = video.stdout.read(w * h * 3)
        if len(buf) < w * h * 3:
            break
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=np.frombuffer(buf, np.uint8).reshape(h, w, 3).copy())
        start = time.perf_counter()
        r = landmarker.detect_for_video(image, int(round(i * 1000 / fps)))
        rec = np.zeros(RECORD, np.float32)
        rec[2] = (time.perf_counter() - start) * 1000
        if r.face_landmarks:
            rec[0] = 1
            rec[4:4 + 478 * 3] = np.array([[p.x * w, p.y * h, p.z * w] for p in r.face_landmarks[0]]).ravel()
            rec[4 + 478 * 3:4 + 478 * 3 + 52] = [c.score for c in r.face_blendshapes[0]]
            rec[-16:] = np.array(r.facial_transformation_matrixes[0]).ravel()
        records.append(rec)
        i += 1
    video.wait()
    np.array(records, np.float32).tofile(out)


def report(ours, theirs):
    a = np.fromfile(ours, np.float32).reshape(-1, RECORD)
    b = np.fromfile(theirs, np.float32).reshape(-1, RECORD)
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    fa, fb = a[:, 0] > 0, b[:, 0] > 0
    print("%d frames: the face found on %.1f%% (ours), %.1f%% (MediaPipe); they agree on %.1f%%"
          % (n, fa.mean() * 100, fb.mean() * 100, np.mean(fa == fb) * 100))
    both = fa & fb
    if both.any():
        pa = a[both, 4:4 + 478 * 3].reshape(-1, 478, 3)[:, :, :2]
        pb = b[both, 4:4 + 478 * 3].reshape(-1, 478, 3)[:, :, :2]
        d = np.abs(pa - pb).max(axis=(1, 2))
        size = (pb.max(1) - pb.min(1)).mean(1)
        print("points apart (the most on each frame): median %.2f px, 99%% of frames under %.2f px (the face is about %d px)"
              % (np.median(d), np.percentile(d, 99), np.median(size)))
        e = np.abs(a[both, 4 + 1434:4 + 1434 + 52] - b[both, 4 + 1434:4 + 1434 + 52]).max(1)
        print("expressions apart: median %.4f, 99%% under %.4f (of 0 to 1)" % (np.median(e), np.percentile(e, 99)))
        r = np.abs(a[both, -16:].reshape(-1, 4, 4)[:, :3, :3] - b[both, -16:].reshape(-1, 4, 4)[:, :3, :3]).max((1, 2))
        print("head turn apart: median %.4f, 99%% under %.4f (of the turn's numbers, -1 to 1)"
              % (np.median(r), np.percentile(r, 99)))
    print("a look took %.1f ms (ours, median; %.1f for 95%% of looks), %.1f ms (MediaPipe)"
          % (np.median(a[:, 2]), np.percentile(a[:, 2], 95), np.median(b[:, 2])))
    print("ours ran its face finder on %d looks" % int(a[:, 3].sum()))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("video")
    p.add_argument("--width", type=int, default=960, help="looked at this wide (the add-on: 960 at most)")
    p.add_argument("--fps", type=float, default=30,
                   help="looks a second (the add-on: 30, or 15 with Easy on my PC)")
    args = p.parse_args()
    w0, h0, fps0 = video_size(args.video)
    w = min(args.width, w0) // 2 * 2
    h = round(h0 * w / w0 / 2) * 2
    fps = min(args.fps, fps0)
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        ours, theirs = os.path.join(tmp, "ours.f32"), os.path.join(tmp, "theirs.f32")
        video = frames(args.video, w, h, fps)
        subprocess.run([exe, TASK, ours, str(w), str(h), str(fps)], stdin=video.stdout, check=True)
        video.wait()
        mediapipe(args.video, w, h, fps, theirs)
        print("%s at %d x %d, %g frames a second:" % (os.path.basename(args.video), w, h, fps))
        report(ours, theirs)


if __name__ == "__main__":
    sys.exit(main())
