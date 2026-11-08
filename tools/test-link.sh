#!/usr/bin/env bash
# Builds and runs the tests for DireBot for OBS that don't need OBS:
# the unit tests (the link's, the vertical picture's, Follow me's, clean recordings', the face tracker's,
# the face avatar's 2D and 3D characters', following your hands and arms, hand signs and face moves, how their pictures
# shrink, what it does on cue, the mouth from your mic's), then the link's
# network tests through obsws-python.
# (tests/test_real_obs.py runs it inside a real OBS - see its top.)
#   pip install pytest obsws-python websocket-client
# A sanitizer build: EXTRA_FLAGS=-fsanitize=thread tools/test-link.sh
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${BUILD_DIR:-$here/build-tests}"
mkdir -p "$out"
cxx="${CXX:-g++}"
flags=(-std=c++17 -O1 -g -Wall -Wextra -pthread -I"$here/deps" ${EXTRA_FLAGS:-})
link_src=("$here/src/link/crypto.cpp" "$here/src/link/ws.cpp" "$here/src/link/protocol.cpp")

"$cxx" "${flags[@]}" -o "$out/unit" "$here/tests/unit.cpp" "${link_src[@]}" "$here/src/obs/address.cpp" \
	"$here/src/multistream/places.cpp" "$here/src/obs/keystore.cpp"
"$out/unit"

"$cxx" "${flags[@]}" -o "$out/unit-vertical" "$here/tests/unit_vertical.cpp" "$here/src/vertical/rules.cpp" \
	"$here/src/vertical/options.cpp" "$here/src/obs/keystore.cpp"
"$out/unit-vertical"

"$cxx" "${flags[@]}" -o "$out/unit-follow" "$here/tests/unit_follow.cpp" "$here/src/follow/tracker.cpp"
"$out/unit-follow"

"$cxx" "${flags[@]}" -o "$out/unit-record" "$here/tests/unit_record.cpp" "$here/src/record/rules.cpp" \
	"$here/src/moves/plan.cpp"
"$out/unit-record"

# The face tracker: its engine built as the add-on builds it (-O2, the AVX2
# part with its own flags), the tests against the files in the repo.
face_obj=()
for src in net kernels kernels-sse2 kernels-avx2; do
	extra=()
	[ "$src" = kernels-avx2 ] && extra=(-mavx2 -mfma)
	"$cxx" "${flags[@]}" -O2 ${extra[@]+"${extra[@]}"} -c "$here/src/face/nn/$src.cpp" -o "$out/face-$src.o"
	face_obj+=("$out/face-$src.o")
done
"$cxx" "${flags[@]}" -DDIREBOT_SOURCE_DIR="\"$here\"" -o "$out/unit-face" "$here/tests/unit_face.cpp" \
	"$here/src/face/tracker.cpp" "$here/src/face/pose.cpp" "${face_obj[@]}"
"$out/unit-face"
"$cxx" "${flags[@]}" -DDIREBOT_SOURCE_DIR="\"$here\"" -o "$out/unit-avatar" "$here/tests/unit_avatar.cpp" \
	"$here/src/face/avatar2d.cpp" "$here/src/face/tracker.cpp" "$here/src/face/pose.cpp" "${face_obj[@]}"
"$out/unit-avatar"
# MikkTSpace (zlib licence), as it comes: C.
"${CC:-gcc}" -std=c17 -O2 -c "$here/deps/mikktspace/mikktspace.c" -o "$out/mikktspace.o"
"$cxx" "${flags[@]}" -DDIREBOT_SOURCE_DIR="\"$here\"" -o "$out/unit-model" "$here/tests/unit_model.cpp" "$here/src/face/model.cpp" \
	"$here/src/face/rig.cpp" "$here/src/face/alive.cpp" "$here/src/face/react.cpp" "$here/src/face/voice.cpp" "$here/src/face/setup.cpp" "$here/src/face/tracker.cpp" "$here/src/face/pose.cpp" "${face_obj[@]}" "$out/mikktspace.o"
"$out/unit-model"
# Following your hands and arms: the body and hand models against Google's
# own runtime, the body tracker against MediaPipe, the arm and finger solver.
"$cxx" "${flags[@]}" -DDIREBOT_SOURCE_DIR="\"$here\"" -o "$out/unit-body" "$here/tests/unit_body.cpp" \
	"$here/src/face/body.cpp" "$here/src/face/arms.cpp" "${face_obj[@]}"
"$out/unit-body"
# Hand signs and face moves: made hands, Google's pictures through the body
# tracker, waves, the watch's holds and sides.
"$cxx" "${flags[@]}" -DDIREBOT_SOURCE_DIR="\"$here\"" -o "$out/unit-signs" "$here/tests/unit_signs.cpp" \
	"$here/src/face/signs.cpp" "$here/src/face/body.cpp" "$here/src/face/tracker.cpp" "$here/src/face/pose.cpp" "${face_obj[@]}"
"$out/unit-signs"
"$cxx" "${flags[@]}" -o "$out/unit-react" "$here/tests/unit_react.cpp" "$here/src/face/react.cpp"
"$out/unit-react"
"$cxx" "${flags[@]}" -O2 -o "$out/unit-pictures" "$here/tests/unit_pictures.cpp" "$here/src/face/pictures.cpp"
"$out/unit-pictures"
"$cxx" "${flags[@]}" -O2 -o "$out/unit-voice" "$here/tests/unit_voice.cpp" "$here/src/face/voice.cpp" \
	"$here/src/face/tracker.cpp" "$here/src/face/pose.cpp" "${face_obj[@]}"
"$out/unit-voice"

"$cxx" "${flags[@]}" -o "$out/harness" "$here/tests/harness.cpp" "$here/src/link/server.cpp" "${link_src[@]}"
DIREBOT_LINK_HARNESS="$out/harness" python3 -m pytest -q "$here/tests/test_link.py" "$@"
