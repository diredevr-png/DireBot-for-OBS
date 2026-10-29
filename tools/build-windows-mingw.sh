#!/usr/bin/env bash
# Builds direbot-for-obs.dll for 64-bit Windows from Linux, with MinGW,
# and packs the zip DireBot's Install button takes.
#
#   OBS_SOURCE=/path/to/obs-studio-31.1.1 tools/build-windows-mingw.sh
#
# Only OBS's headers are used (libobs/ and frontend/api/ from the OBS
# source at the version in buildspec.json); OBS itself isn't built. The
# two OBS DLLs are linked through import libraries made here from just
# the functions the plugin calls. The release build is the GitHub Actions
# one (CMake + Visual Studio, .github/workflows); this is for a quick
# test build, and for checking the code builds for Windows at all.
#
# Needs: x86_64-w64-mingw32-g++ (GCC 13+, apt install mingw-w64),
# python3, zip.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
obs="${OBS_SOURCE:?set OBS_SOURCE to the obs-studio source folder (the version in buildspec.json)}"
out="${BUILD_DIR:-$here/build-windows}"
prefix="${MINGW_PREFIX:-x86_64-w64-mingw32-}"
name="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["name"])' "$here/buildspec.json")"
version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$here/buildspec.json")"
IFS=. read -r v_major v_minor v_patch <<<"$version"

rm -rf "$out"
mkdir -p "$out/gen" "$out/obj" "$out/lib"

# What OBS's own Windows build puts in obsconfig.h.
cat >"$out/gen/obsconfig.h" <<'EOF'
#pragma once
#define OBS_DATA_PATH "../../data"
#define OBS_PLUGIN_PATH "../../obs-plugins/64bit"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
EOF
sed -e "s/@CMAKE_PROJECT_NAME@/$name/" -e "s/@CMAKE_PROJECT_VERSION@/$version/" \
	"$here/src/plugin-support.c.in" >"$out/gen/plugin-support.c"

includes=(-I"$out/gen" -I"$obs/libobs" -I"$obs/frontend/api" -I"$here/src" -I"$here/deps")
defines=(-D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE)
warn=(-Wall -Wextra -Wno-unused-parameter -Wno-attributes -Wno-missing-field-initializers)
cxxflags=(-std=c++17 -O2 -g0 "${warn[@]}" "${defines[@]}" "${includes[@]}")
cflags=(-std=c17 -O2 -g0 "${warn[@]}" "${defines[@]}" "${includes[@]}")

sources=(
	src/plugin-main.cpp
	src/link/crypto.cpp src/link/ws.cpp src/link/protocol.cpp src/link/server.cpp
	src/obs/websocket.cpp src/obs/meters.cpp src/obs/usage.cpp src/obs/link-file.cpp src/obs/address.cpp
	src/multistream/multistream.cpp src/multistream/places.cpp src/obs/keystore.cpp src/obs/settings-window.cpp
)
objects=()
for src in "${sources[@]}"; do
	obj="$out/obj/$(echo "$src" | tr '/' '_').o"
	"${prefix}g++" "${cxxflags[@]}" -c "$here/$src" -o "$obj"
	objects+=("$obj")
done
"${prefix}gcc" "${cflags[@]}" -c "$out/gen/plugin-support.c" -o "$out/obj/plugin-support.o"
objects+=("$out/obj/plugin-support.o")

# Import libraries: every OBS function the plugin calls, found in OBS's
# headers. A name in neither header is left for the C runtime / Windows.
python3 - "$obs" "$out" "${prefix}nm" "${objects[@]}" <<'EOF'
import os, re, subprocess, sys
obs, out, nm, objects = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
undefined = set()
for o in objects:
    for line in subprocess.run([nm, "-u", o], capture_output=True, text=True, check=True).stdout.splitlines():
        sym = line.split()[-1]
        if re.fullmatch(r"[a-z][a-z0-9_]*", sym):
            undefined.add(sym)
def exported(folder):
    names = set()
    for root, _, files in os.walk(folder):
        for f in files:
            if f.endswith((".h", ".hpp")):
                text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
                names.update(re.findall(r"\bEXPORT\b[^;{]*?\b([a-z][a-z0-9_]*)\s*\(", text, re.S))
    return names
frontend = exported(os.path.join(obs, "frontend", "api"))
libobs = exported(os.path.join(obs, "libobs")) - frontend
for dll, names in (("obs", libobs), ("obs-frontend-api", frontend)):
    used = sorted(undefined & names)
    with open(os.path.join(out, "lib", dll + ".def"), "w") as f:
        f.write(f"LIBRARY {dll}.dll\nEXPORTS\n" + "".join(f"    {n}\n" for n in used))
    print(f"  {dll}.dll: {len(used)} functions")
EOF
for dll in obs obs-frontend-api; do
	"${prefix}dlltool" -d "$out/lib/$dll.def" -l "$out/lib/lib$dll.dll.a" -D "$dll.dll"
done

# Only the functions OBS looks for are exported.
cat >"$out/lib/$name.def" <<EOF
LIBRARY $name.dll
EXPORTS
    obs_module_set_pointer
    obs_current_module
    obs_module_ver
    obs_module_load
    obs_module_post_load
    obs_module_unload
    obs_module_set_locale
    obs_module_free_locale
    obs_module_get_string
    obs_module_name
    obs_module_description
    obs_module_author
EOF

# File > Properties > Details on Windows.
cat >"$out/gen/resource.rc" <<EOF
1 VERSIONINFO
 FILEVERSION $v_major,$v_minor,$v_patch,0
 PRODUCTVERSION $v_major,$v_minor,$v_patch,0
 FILEFLAGSMASK 0x0L
 FILEFLAGS 0x0L
 FILEOS 0x40004L
 FILETYPE 0x2L
 FILESUBTYPE 0x0L
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName", "DireBot"
            VALUE "FileDescription", "DireBot for OBS"
            VALUE "FileVersion", "$version"
            VALUE "InternalName", "$name"
            VALUE "LegalCopyright", "(C) $(date +%Y) DireBot. GPL-2.0-or-later."
            VALUE "OriginalFilename", "$name.dll"
            VALUE "ProductName", "DireBot for OBS"
            VALUE "ProductVersion", "$version"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
EOF
"${prefix}windres" "$out/gen/resource.rc" -O coff -o "$out/obj/resource.o"

dll="$out/$name.dll"
"${prefix}g++" -shared -o "$dll" "${objects[@]}" "$out/obj/resource.o" "$out/lib/$name.def" \
	"$out/lib/libobs.dll.a" "$out/lib/libobs-frontend-api.dll.a" -lws2_32 -lbcrypt -lcrypt32 \
	-static-libgcc -static-libstdc++ -static \
	-Wl,--no-undefined -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -s

# The zip, laid out the way OBS's legacy installers do it (DireBot's
# Install button moves it to where your OBS wants it).
pkg="$out/package"
mkdir -p "$pkg/$name/bin/64bit" "$pkg/$name/data"
cp "$dll" "$pkg/$name/bin/64bit/"
cp -r "$here/data/." "$pkg/$name/data/"
cp "$here/LICENSE" "$pkg/$name/" 2>/dev/null || true
zip_name="$name-$version-windows-x64.zip"
(cd "$pkg" && TZ=UTC find "$name" -exec touch -t 202601010000 {} + && zip -qrX "$out/$zip_name" "$name")
(cd "$out" && sha256sum "$zip_name" >"$zip_name.sha256")
echo "Built $dll"
echo "Packed $out/$zip_name"
cat "$out/$zip_name.sha256"
