The body tracker's test pictures (tests/unit_body.cpp).

astronaut.ppm: "astronaut.png" from scikit-image's sample pictures
(skimage/data, version 0.24.0): NASA's portrait of astronaut Eileen
Collins, a work of the US government, in the public domain.

camera.pgm: "camera.png" from the same sample pictures: a photographer
with a camera, CC0 by the photographer (Lav Varshney).

expected.json: Google MediaPipe's own answers on them (its Holistic
Landmarker: the body's 33 points, in the picture and in metres, and any
hands it finds; on astronaut.ppm, mirrored too, and in VIDEO mode four
looks running), and each body and hand model on its own through LiteRT,
Google's own TFLite runtime, on a piece of astronaut.ppm. Made by
tools/body-expected.py (MediaPipe and LiteRT for the test only, never part
of DireBot for OBS).

Neither picture has a hand MediaPipe finds, so the hand steps are checked
two ways: the hand models number for number against LiteRT here, and the
whole tracker against MediaPipe on photos with hands by hand (photos we
can't keep in the repo). To do that: run tools/body-expected.py's
holistic() and BodyTracker (smoothing off, forget() before each picture)
on the same photo and compare each hand's 21 points; on clear hands they
agree within a pixel or so.
