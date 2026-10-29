#!/usr/bin/env bash
# Builds direbot-for-obs.so against the OBS installed on this Linux PC
# (apt install obs-studio libobs-dev), for trying the plugin in a real OBS
# here - tests/test_real_obs.py does that. Not a release build.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${BUILD_DIR:-$here/build-linux}"
name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$here/buildspec.json")"
version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$here/buildspec.json")"
mkdir -p "$out/gen"
sed -e "s/@CMAKE_PROJECT_NAME@/$name/" -e "s/@CMAKE_PROJECT_VERSION@/$version/" \
	"$here/src/plugin-support.c.in" >"$out/gen/plugin-support.c"
flags=(-O1 -g -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
	-I/usr/include/obs -I"$here/src" -I"$here/deps" ${EXTRA_FLAGS:-})
gcc -std=c17 "${flags[@]}" -c "$out/gen/plugin-support.c" -o "$out/plugin-support.o"
g++ -std=c++17 "${flags[@]}" -shared -o "$out/$name.so" \
	"$here/src/plugin-main.cpp" "$here"/src/link/{crypto,ws,protocol,server}.cpp \
	"$here"/src/obs/{websocket,meters,usage,link-file,address}.cpp "$out/plugin-support.o" \
	-lobs -lobs-frontend-api -pthread -Wl,--no-undefined
echo "Built $out/$name.so"
