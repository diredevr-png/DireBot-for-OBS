The body tracker's test pictures (tests/unit_body.cpp).

astronaut.ppm: "astronaut.png" from scikit-image's sample pictures
(skimage/data, version 0.24.0): NASA's portrait of astronaut Eileen
Collins, a work of the US government, in the public domain.

camera.pgm: "camera.png" from the same sample pictures: a photographer
with a camera, CC0 by the photographer (Lav Varshney).

thumb_up.ppm: "thumb_up.jpg" from Google MediaPipe's own test pictures
(https://storage.googleapis.com/mediapipe-assets/thumb_up.jpg, the
pictures MediaPipe's hand tests use), unchanged but for the file type: a
right hand, thumb up. Copyright The MediaPipe Authors, under the Apache
License 2.0 (data/face/LICENSE).

hands.ppm: "right_hands.jpg" from the same test pictures, shrunk from
720 x 382 to 540 x 286: two right hands, side by side (mirrored, two
left ones). Same copyright and licence.

expected.json: Google MediaPipe's own answers on them (its Holistic
Landmarker: the body's 33 points, in the picture and in metres, and any
hands it finds; on astronaut.ppm, mirrored too, and in VIDEO mode four
looks running; its Hand Landmarker: each hand's 21 points on the first
look and after six looks running, on thumb_up.ppm, hands.ppm and
hands.ppm mirrored, and none on the other two), and each body and hand
model and the palm finder on its own through LiteRT, Google's own TFLite
runtime, on a piece of astronaut.ppm. Made by tools/body-expected.py
(MediaPipe and LiteRT for the test only, never part of DireBot for OBS).
