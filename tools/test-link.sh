#!/usr/bin/env bash
# Builds and runs the tests for DireBot for OBS that don't need OBS:
# the unit tests, then the link's network tests through obsws-python.
# (tests/test_real_obs.py runs it inside a real OBS - see its top.)
#   pip install pytest obsws-python websocket-client
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${BUILD_DIR:-$here/build-tests}"
mkdir -p "$out"
cxx="${CXX:-g++}"
flags=(-std=c++17 -O1 -g -Wall -Wextra -pthread -I"$here/deps")
link_src=("$here/src/link/crypto.cpp" "$here/src/link/ws.cpp" "$here/src/link/protocol.cpp")

"$cxx" "${flags[@]}" -o "$out/unit" "$here/tests/unit.cpp" "${link_src[@]}" "$here/src/obs/address.cpp"
"$out/unit"

"$cxx" "${flags[@]}" -o "$out/harness" "$here/tests/harness.cpp" "$here/src/link/server.cpp" "${link_src[@]}"
DIREBOT_LINK_HARNESS="$out/harness" python3 -m pytest -q "$here/tests/test_link.py" "$@"
