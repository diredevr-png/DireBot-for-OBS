The face tracker's test pictures (tests/unit_face.cpp, tests/test_real_face.py).

portrait.ppm: a 256 x 256 piece (from x 96, y 0) of "astronaut.png" in
scikit-image's sample pictures (skimage/data, version 0.24.0): NASA's
portrait of astronaut Eileen Collins, a work of the US government, in the
public domain.

expected.json: Google MediaPipe's own answers on that picture and three
made from it (mirrored, turned, half size): the face's 478 points, its 52
expressions, its head pose and the face finder's boxes. Made by
tools/face-expected.py (MediaPipe for the test only, never part of DireBot
for OBS).
