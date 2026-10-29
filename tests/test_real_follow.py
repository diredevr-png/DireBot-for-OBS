"""
"DireBot Follow Cam" inside a real OBS: a made-up camera (a video of a
white box that walks to the right and stops) shown through a follow cam,
which has to end up with the box in its picture - and without following,
it isn't. OBS runs on a pretend screen with its own empty settings, like
test_real_obs.py.

Skipped unless OBS, Xvfb, ffmpeg, Pillow and the test build are there:

    BUILD_DIR=/tmp/b tools/build-linux-test.sh
    DIREBOT_OBS_PLUGIN=/tmp/b/direbot-for-obs.so \\
        python3 -m pytest tests/test_real_follow.py
"""

import base64
import io
import os
import shutil
import subprocess
import time

import pytest

obsws = pytest.importorskip("obsws_python")
Image = pytest.importorskip("PIL.Image")

from test_real_obs import Obs, vendor, wait_for  # noqa: E402
from test_real_obs import pytestmark as needs_obs  # noqa: E402

pytestmark = [needs_obs, pytest.mark.skipif(
    not shutil.which("ffmpeg"), reason="needs ffmpeg to make the video")]

SCENE = "Follow test"


def make_video(path):
    """10 seconds of a dark room where a white box (you) walks from the
    left to near the right edge in two seconds and then stands still."""
    subprocess.run(
        ["ffmpeg", "-v", "error", "-y",
         "-f", "lavfi", "-i", "color=c=0x202020:s=1280x720:r=30:d=10",
         "-f", "lavfi", "-i", "color=c=white:s=120x200:r=30:d=10",
         "-filter_complex",
         "[0][1]overlay=x='min(100+t*400\\,1000)':y=260:shortest=1",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", path],
        check=True, timeout=60)


def send(c, request, data=None):
    return c.send(request, data or {}, raw=True)


def white_share(c, source):
    """How much of the source's picture is the white box, and where it is
    across (a share of the width, or None)."""
    shot = send(c, "GetSourceScreenshot", {
        "sourceName": source, "imageFormat": "png", "imageWidth": 180})
    data = shot["imageData"].split(",", 1)[1]
    img = Image.open(io.BytesIO(base64.b64decode(data))).convert("L")
    w, h = img.size
    px = img.load()
    xs = [x for y in range(h) for x in range(w) if px[x, y] > 200]
    return len(xs) / float(w * h), (sum(xs) / len(xs) / w if xs else None)


@pytest.fixture(scope="module")
def run():
    o = Obs()
    yield o
    if o.proc.poll() is None:
        o.close()
    if not os.environ.get("DIREBOT_OBS_KEEP"):
        shutil.rmtree(o.home, ignore_errors=True)


def test_it_keeps_you_in_the_picture(run):
    video = os.path.join(run.home, "walk.mp4")
    make_video(video)
    c = run.client()
    try:
        assert vendor(c, "GetInfo")["features"]["follow_cam"] is True
        send(c, "CreateScene", {"sceneName": SCENE})
        send(c, "CreateInput", {
            "sceneName": SCENE, "inputName": "Walker",
            "inputKind": "ffmpeg_source",
            "inputSettings": {"local_file": video, "looping": False,
                              "clear_on_media_end": False,
                              "restart_on_activate": True}})
        for name, follow in (("Follow Cam", True), ("Still Cam", False)):
            send(c, "CreateInput", {
                "sceneName": SCENE, "inputName": name,
                "inputKind": "direbot_follow_cam",
                "inputSettings": {"source": "Walker", "width": 360,
                                  "height": 640, "follow": follow}})
        send(c, "SetCurrentProgramScene", {"sceneName": SCENE})
        # Its size is the box's, whatever the camera's.
        assert wait_for(lambda: send(c, "GetSceneItemTransform", {
            "sceneName": SCENE, "sceneItemId": send(c, "GetSceneItemId", {
                "sceneName": SCENE, "sourceName": "Follow Cam"})[
                    "sceneItemId"]})["sceneItemTransform"]["sourceWidth"]
            == 360, 5)
        time.sleep(6)
        share, across = white_share(c, "Follow Cam")
        assert share > 0.03, "the box isn't in the follow cam's picture"
        # Near the middle, not trailing behind where it walked.
        assert 0.3 < across < 0.7
        # Not following: the middle of the camera, where the box isn't.
        share, _ = white_share(c, "Still Cam")
        assert share < 0.005
        # Closer: still in the picture.
        send(c, "SetInputSettings", {"inputName": "Follow Cam",
                                     "inputSettings": {"zoom": 200}})
        time.sleep(1)
        share, across = white_share(c, "Follow Cam")
        # All of it still in the picture (it's a third of it, this close).
        assert share > 0.3 and 0.25 < across < 0.75
        # The camera renamed: it keeps following it.
        send(c, "SetInputName", {"inputName": "Walker",
                                 "newInputName": "My camera"})
        got = send(c, "GetInputSettings", {"inputName": "Follow Cam"})
        assert got["inputSettings"]["source"] == "My camera"
        time.sleep(1.5)
        assert white_share(c, "Follow Cam")[0] > 0.03
        # Hidden a while and shown again: it starts from where you were.
        item = send(c, "GetSceneItemId", {
            "sceneName": SCENE, "sourceName": "Follow Cam"})["sceneItemId"]
        for on in (False, True):
            send(c, "SetSceneItemEnabled", {
                "sceneName": SCENE, "sceneItemId": item,
                "sceneItemEnabled": on})
            time.sleep(1.5)
        share, across = white_share(c, "Follow Cam")
        assert share > 0.3 and 0.25 < across < 0.75
    finally:
        c.disconnect()


def test_closing_obs_leaves_nothing_behind(run):
    assert run.close() == 0
    log = run.log()
    assert "[direbot-for-obs] unloaded" in log
    assert "Number of memory leaks: 0" in log
