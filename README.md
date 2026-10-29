# DireBot for OBS

An OBS add-on that connects OBS to [DireBot](https://github.com/diredevr-png/DireBot)
by itself, and shows what's slowing OBS down.

## What it does

**DireBot Link.** When OBS opens, DireBot finds it and connects without a
websocket password or port to type in. The link only answers on this PC,
never the network. Its password is new each time OBS starts, and it's
kept in a file only your Windows account can read (beside OBS's own
websocket settings). The file goes when OBS closes.

The link speaks obs-websocket's language, and obs-websocket (the websocket
add-on that comes with OBS) answers every request. So the link can do what
OBS's websocket can and no more. It changes nothing in OBS's websocket
settings, and other tools that use OBS's websocket keep working.

**What's slowing OBS.** DireBot can ask how long each source and filter
takes OBS every frame, measured by OBS's own source profiler (OBS 31.1 and
newer), with the numbers from OBS's Stats window: frames missed from
rendering lag, frames skipped from encoding lag, CPU and memory. Measuring
only runs while DireBot's page is open, and stops a minute after. Reports
never include web addresses, file paths or device names.

**Audio levels.** DireBot's talking avatar follows your mic. The link sends
OBS's audio levels (the same numbers as OBS's Audio Mixer) only while
something is listening.

The link never hands out your stream key: `GetStreamServiceSettings`
through it shows the key (and a custom server's password or token) as
`********`, and settings sent back as they were read keep the real one.
Putting a new key in works as usual. Apart from that, the link passes on
every obs-websocket request, and it works whether or not OBS's own
websocket server is switched on, so whoever has the link file's password
can do what OBS's websocket allows. The file is what keeps OBS safe: on
Windows it's in your own AppData, elsewhere it's made readable by you alone
(0600).

## Installing

DireBot puts it in for you: **Studio → Set up OBS → DireBot for OBS →
Install**. DireBot only installs a copy signed by DireBot's publisher.

By hand: close OBS, unzip `direbot-for-obs-<version>-windows-x64.zip` into
`C:\ProgramData\obs-studio\plugins\`, and open OBS again. In OBS, **Help →
Log Files → View Current Log** has the line
`[direbot-for-obs] DireBot Link is on (port 4466, this PC only).`

Needs OBS 31.1.1 or newer, 64-bit Windows.

## For DireBot (and other tools)

The link file is `%APPDATA%\obs-studio\plugin_config\direbot-for-obs\link.json`:

```json
{"format": 1, "port": 4466, "password": "...", "pid": 1234,
 "obs": "32.0.2", "websocket": "5.6.3", "plugin": "0.1.0", "started": 1798000000}
```

Connect to `ws://127.0.0.1:<port>` exactly as to obs-websocket (protocol 5,
RPC version 1) with that password. Check `pid` is still running first: a
file left behind by a crash has a dead one.

The add-on's own requests go through `CallVendorRequest` with
`vendorName: "direbot"` (over the link or OBS's normal websocket):

| requestType | answers |
|---|---|
| `GetInfo` | `plugin` version, `api` (1), `obs` version, `link` on?, `link_port`, `features` |
| `GetUsage` | `measuring` (first 3 seconds), `fps`, `fps_now`, `frame_ms`, `frame_budget_ms`, `frames`, `frames_lagged`, `encoded`, `encoded_skipped`, `cpu`, `memory_mb`, `stream_frames`, `stream_dropped`, and `sources`: per input and filter `name`, `uuid`, `kind`, `type`, `active`, `showing`, `measured`, `tick_ms`, `render_ms`, `gpu_ms` (+ `_max_ms`), `fps_in`/`fps_shown` for cameras and media, `parent`/`enabled` for filters (and `own_ms`/`own_gpu_ms`: a filter's time takes in what it draws on, so this is its own part), and `hints` (browser size/fps/"shut down when hidden", whether it shows a page on this PC and its port, a short fingerprint of a camera to spot the same one added twice) |

## Building

Windows (what the releases are): GitHub Actions builds it with CMake and
Visual Studio from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)'s
scripts (`.github/workflows/build.yaml`).

A quick test build from Linux with MinGW, against OBS's headers only:

    OBS_SOURCE=/path/to/obs-studio-31.1.1 tools/build-windows-mingw.sh

Tests (Linux):

    tools/test-link.sh                       # protocol, WebSocket, network
    BUILD_DIR=/tmp/b tools/build-linux-test.sh
    DIREBOT_OBS_PLUGIN=/tmp/b/direbot-for-obs.so pytest tests/test_real_obs.py

`test_real_obs.py` runs a real OBS (`apt install obs-studio libobs-dev xvfb`)
with its own empty settings on a pretend screen, and checks the link with
obsws-python, the client DireBot uses.

## Releasing

1. Put the new version in `buildspec.json` (`"version"`), and push a tag
   `vX.Y.Z`: GitHub Actions builds `direbot-for-obs-X.Y.Z-windows-x64.zip`
   (Actions → the run → Artifacts).
2. Sign it on the signing PC, from DireBot's folder:
   `python tools\sign_obs_plugin.py direbot-for-obs-X.Y.Z-windows-x64.zip`.
   The signed zip is in `obs-plugin\release\`, with its SHA-256.
3. Make the GitHub release for the tag and attach that signed zip.
   DireBot's Install/Update button takes the newest release's
   `-windows-x64.zip`, checks GitHub's fingerprint of it, and only puts it
   in when it's signed by the same publisher as DireBot.

## How it's laid out

    src/plugin-main.cpp   start and stop with OBS; the clock for meters
    src/link/             the link: WebSocket, obs-websocket's protocol, the server
    src/obs/              OBS's side: obs-websocket bridge, audio meters,
                          usage report, link file
    deps/                 nlohmann/json (MIT), obs-websocket's API header (GPL-2.0)

Later parts (multistream, vertical, auto-duck, captions) each get their own
folder under `src/`, their own `direbot` requests and a line in
`features`.

## Licence

GPL-2.0-or-later, like OBS. See LICENSE.
