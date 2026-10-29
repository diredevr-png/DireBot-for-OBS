#!/usr/bin/env bash
# Builds direbot-for-obs.so against the OBS installed on this Linux PC
# (apt install obs-studio libobs-dev), for trying the plugin in a real OBS
# here - tests/test_real_obs.py does that. Not a release build.
#
# An OBS built from source elsewhere (the vertical picture needs OBS 31.1
# or newer, newer than Ubuntu's):
#   OBS_PREFIX=/path/to/obs-install tools/build-linux-test.sh
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
out="${BUILD_DIR:-$here/build-linux}"
name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$here/buildspec.json")"
version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$here/buildspec.json")"
mkdir -p "$out/gen"
sed -e "s/@CMAKE_PROJECT_NAME@/$name/" -e "s/@CMAKE_PROJECT_VERSION@/$version/" \
	"$here/src/plugin-support.c.in" >"$out/gen/plugin-support.c"
prefix="${OBS_PREFIX:-/usr}"
libs=()
[ "$prefix" != /usr ] && libs=(-L"$prefix/lib" -Wl,-rpath,"$prefix/lib")
flags=(-O1 -g -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
	-I"$prefix/include/obs" -I"$here/src" -I"$here/deps" ${EXTRA_FLAGS:-})
gcc -std=c17 "${flags[@]}" -c "$out/gen/plugin-support.c" -o "$out/plugin-support.o"
g++ -std=c++17 "${flags[@]}" -shared -o "$out/$name.so" \
	"$here/src/plugin-main.cpp" "$here"/src/link/{crypto,ws,protocol,server}.cpp \
	"$here"/src/obs/{websocket,meters,usage,link-file,address,keystore,settings-window}.cpp \
	"$here"/src/multistream/{multistream,places}.cpp "$here"/src/follow/{tracker,follow-cam}.cpp \
	"$here"/src/vertical/{rules,options,canvas,outputs,settings,vertical}.cpp "$out/plugin-support.o" \
	"${libs[@]}" -lobs -lobs-frontend-api -pthread -Wl,--no-undefined
echo "Built $out/$name.so"
