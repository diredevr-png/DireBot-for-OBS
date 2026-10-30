# DireBot for OBS

An OBS add-on that connects OBS to [DireBot](https://github.com/diredevr-png/DireBot)
by itself, shows what's slowing OBS down, streams to more than one site at
once, and makes a tall (9:16) picture for TikTok, Shorts and Reels.

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

**Multistream.** The add-on sends your stream to YouTube, Kick, TikTok
or any other site at the same time as your main stream. **Tools →
DireBot Multistream** opens its window: add a site, paste its stream key
(and, for "Another site", its server). Each site sends the same picture
as your stream, a shared **Lighter** picture (one smaller encoder every
Lighter site shares), or a picture of its own; and the stream's sound or
one of OBS's audio tracks, so each site can get its own mix of sounds
(DireBot's Studio → Multistream → Sounds for each site sets them up). Sites
start and end with your stream unless you untick that, and reconnect by
themselves when the connection drops. The keys are saved with the
profile, locked to your Windows account (DPAPI) - on another PC or
account the window asks for them again. Two sites with the same server
and key are flagged. Sites from **Multiple RTMP outputs** or **Aitum
Multistream** can be brought over once, keys and all: the add-on reads
their saved settings itself and the sites arrive switched off. Nothing
about a site's server or key is ever sent to DireBot - only whether one
is filled in.

**Vertical.** A second, tall picture in OBS - **DireBot Vertical**, 1080 ×
1920 to start with (720 × 1280, 1080 × 1350 and 1440 × 2560 too) - with
scenes of its own, which can switch with your usual ones. It needs OBS
31.1 or newer (it's an OBS "canvas"). **Tools → DireBot Vertical** opens
its window: up to three places it streams to (TikTok, YouTube, Instagram,
Kick, Facebook, X or another), each with its server and stream key, locked
the same way as multistream's; its own bitrate and encoder (the same kind
as your stream's unless you pick another); which of OBS's audio tracks
each place, and its recording, sends; whether it starts and ends with
your stream; and a recording of its own beside OBS's. The tall picture is
encoded once however many places it goes to, and isn't drawn at all
while nothing uses it. Twitch's phone viewers get it too: pick DireBot
Vertical as Enhanced Broadcasting's **Additional canvas** in OBS's
Settings → Stream.

**Clean recordings (0.3.0).** Records single sources - your camera, your
game - each to its own file with nothing else on it, beside OBS's normal
recording. Each source gets a small private picture of its own (OBS 31.1
and newer), drawn only while it records, at the source's own size or
fitted inside 1080p or 720p, with OBS's recording quality and audio
track. They start with your recording, while you're live, or only when
asked. DireBot sets them up (Studio → Live → Clean recordings); the
choice is kept in `direbot-record.json` in the OBS profile folder, and
the files go next to OBS's recordings, named after the source.

**DireBot Move (0.3.0).** A scene transition: whatever is in both scenes
slides and grows from where it was to where it is, everything else fades.
Add it once in OBS (the **+** under Scene Transitions → DireBot Move);
DireBot's **Smooth** transition uses it.

**DireBot Blur (0.3.0).** A filter that blurs or pixelates a source
(right-click it → Filters → **+** → DireBot Blur).

**DireBot Face Avatar (0.4.0).** A source that puts a character on screen
wearing your real eyes and mouth, cut out of your camera, and moves,
tilts and turns it with your head. Your face is followed by Google's
MediaPipe face models (in `data/face`, Apache 2.0) run by our own small
engine (`src/face/nn`: no MediaPipe, TensorFlow or other library, nothing
sent anywhere): 478 points on your face, 52 expressions (blinks, mouth
open, smile...) and which way your head is turned, 30 times a second (15
with **Easy on my PC**) on a thread of its own. The steps around the
models are MediaPipe's own, so the answers are theirs (the tests check
that). **Steady or snappy** sets the smoothing; **Set** (look straight at
the camera with a resting face) makes your turn and expressions count
from how you sit. When your face is lost the eyes and mouth are held for
a moment, from the last picture they were seen in (never your room), then
the character shows its own drawn eyes and mouth. Optional pictures for
turned left and turned right, and outfit layers in front of or behind the
character. DireBot sets it up (Avatar → Face avatar).

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
| `GetMultistream` | `ok`, `api`, `profile`, `file_problem`, `window_open`, `main` (your stream: `live`, `multitrack`, `video_kbps`, `audio_kbps`, `audio_track`, `encoder`, `encoder_name`, `codec`, `width`, `height`, `fps`, `delay`), `lighter` (`kbps`, `height`, `encoder`), `tracks` (`track`, `name`), `encoders` (`id`, `name`), `sites` (`key`, `label`, `where`), and `places`: `id`, `name`, `site`, `label`, `host`, `protocol`, `on`, `with_stream`, `stop_with_stream`, `key_set`, `key_lost`, `server_set`, `video` (`same`/`lighter`/`own`), `kbps`, `height`, `encoder`, `track` (0: the stream's sound), `came_from`, `twin` (an earlier place with the same server and key), `output`, `state` (`off`/`starting`/`sending`/`reconnecting`/`stopping`), `error`, `ready`, and while running `bytes`, `frames`, `dropped`, `congestion`, `seconds`, `sending_video`, `sending_kbps` - never a server or a key |
| `MultistreamStart` / `MultistreamStop` `{id}` | `ok`, `note` / `error` in words ("Go live first..." for a place that sends the stream's own picture or sound) |
| `MultistreamStopAll` | `ok`, `stopped` |
| `MultistreamAdd` `{site, open}` | `ok`, `id`, `name` (its window opens on it when `open`) |
| `MultistreamSet` `{id, changes}` | `ok`, `place`; `changes` may have `name`, `on`, `with_stream`, `stop_with_stream`, `video`, `kbps`, `height`, `encoder`, `track` - never `server` or `key`. With `id` `"lighter"`: the Lighter picture's `kbps`, `height`, `encoder` |
| `MultistreamOpen` `{id}` | `ok` - the window opens (on that place) |
| `MultistreamImport` `{from: "multi_rtmp" / "aitum"}` | `ok`, `added` (`id`, `name`) - brought over switched off |
| `VerticalGetState` | `exists`, `uuid`, `name`, `width`, `height`, `fps`, `current` scene, `scenes` (`name`, `uuid`), `follow` (`on`, `links`), `video`, `awake`, `streaming`, `recording`, `twitch` (Twitch's phone viewers get it), `settings_open`, `with_main`, `problem`, `options` (`kbps`, `encoder`, `audio_track`, `with_stream`, `record_with_main`, and `places`: `slot`, `platform`, `enabled`, `key_set`, `server_set`, `server_ok`, `host`, `audio_track`) and `outputs` (`encoder`, `streams`: `slot`, `active`, `live`, `connecting`, `reconnecting`, `kbps`, `dropped`, `track`, `problem`; `recording`: `active`, `paused`, `seconds`, `file`, `track`, `problem`) - never a server or a key |
| `VerticalSetUp` `{width, height}` / `VerticalSetSize` `{width, height}` | makes the tall picture / changes its size (not while it's sending or recording) |
| `VerticalSetScene` / `VerticalRemoveScene` `{scene}`, `VerticalCreateScene` `{name}`, `VerticalRenameScene` `{scene, to}` | its scenes (`VerticalCreateScene` answers the new `scene`'s `name` and `uuid`) |
| `VerticalSetFollow` `{on, links: [{main, tall}]}` | switch with your usual scenes |
| `VerticalSetOptions` | `kbps`, `encoder`, `audio_track`, `with_stream`, `record_with_main`, `places` (`slot`, `platform`, `enabled`, `audio_track`) - never a server or a key |
| `VerticalStartStream` / `VerticalStopStream` `{slot}` (none: all), `VerticalStartRecord` / `VerticalStopRecord`, `VerticalPauseRecord` `{pause}` | `ok`, or `error` in words |
| `VerticalOpenSettings`, `VerticalLook` `{seconds}` (draw it for a picture), `VerticalGetEncoders` | the window opens / `ok` / `encoders` (`id`, `name`) and `auto` |
| `RecordGetState` | clean recordings: `options` (`when`: `recording`/`stream`/`hand`, `encoder`, `picks`: `source`, `uuid`, `height` (0: the source's own), `quality`, `audio_track`, `enabled`), `running` (`source`, `file` name, `recording`, `seconds`), `problems`, `sources` (what can be picked), `encoders` (`id`, `name`), `auto_encoder` |
| `RecordSetOptions` `{options}` | saves them (the same shape as `options` above); answers the state |
| `RecordStart` / `RecordStop` | the state, plus `note` when some couldn't start, or `error` in words when none could. The `RecordChanged` vendor event sends the state whenever it changes |
| `FaceSet` `{source, clear?}` | "Set": the next looks that find the face are its rest, kept in the source's `rest` setting (`setting` while it's taking them); `clear` forgets it. `ok`, or `error` in words (for one, the face avatar isn't shown) |
| `FaceGetState` `{source, points?}` | `camera`, `ready`, `showing`, `found`, `presence` (0-1), `set`, `setting`, `turn` (`yaw`, `pitch`, `roll` in degrees, since "Set"), `turned` (-1, 0, 1), `moves` (the 51 expressions, 0-1 since "Set", ARKit's names), `parts` (first eye, second eye, mouth: `x`, `y`, `w`, `h` shares of the camera's picture), `shown`, `holding`, `looks_per_second`, `ms`, `problem`; with `points`, the 478 `points` (`x`, `y` shares) |
| `GetUsage` | `measuring` (first 3 seconds), `fps`, `fps_now`, `frame_ms`, `frame_budget_ms`, `frames`, `frames_lagged`, `encoded`, `encoded_skipped`, `cpu`, `memory_mb`, `stream_frames`, `stream_dropped`, and `sources`: per input and filter `name`, `uuid`, `kind`, `type`, `active`, `showing`, `measured`, `tick_ms`, `render_ms`, `gpu_ms` (+ `_max_ms`), `fps_in`/`fps_shown` for cameras and media, `parent`/`enabled` for filters (and `own_ms`/`own_gpu_ms`: a filter's time takes in what it draws on, so this is its own part), and `hints` (browser size/fps/"shut down when hidden", whether it shows a page on this PC and its port, a short fingerprint of a camera to spot the same one added twice) |

## Building

Windows (what the releases are): GitHub Actions builds it with CMake and
Visual Studio from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)'s
scripts (`.github/workflows/build.yaml`).

A quick test build from Linux with MinGW, against OBS's headers only:

    OBS_SOURCE=/path/to/obs-studio-31.1.1 tools/build-windows-mingw.sh

Tests (Linux):

    tools/test-link.sh                       # protocol, WebSocket, network, the face tracker
    BUILD_DIR=/tmp/b tools/build-linux-test.sh
    DIREBOT_OBS_PLUGIN=/tmp/b/direbot-for-obs.so pytest tests/test_real_obs.py tests/test_real_face.py

`test_real_obs.py` runs a real OBS 31.1 or newer (`ppa:obsproject/obs-studio`, then `apt install obs-studio xvfb`)
with its own empty settings on a pretend screen, and checks the link with
obsws-python, the client DireBot uses. `test_real_obs_multistream.py` and
`test_real_vertical.py` stream to ffmpeg on the same PC. The vertical
picture needs OBS 31.1 or newer, newer than some Linux PCs have: build
OBS from source and point the build at it with `OBS_PREFIX=/path/to/obs`
(and `DIREBOT_OBS_PRELOAD` for libraries of its own that must load first).

The face tracker's tests (`tests/unit_face.cpp`) check it against
MediaPipe's own answers on a photo (`tests/data/face`), mirrored, turned
and half size. `tools/face-expected.py` makes those answers again (it
needs `pip install mediapipe`, for the test only).

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
                          usage report, link file, the key store, the
                          DireBot Multistream window
    src/multistream/      the extra streams: places (and their file), and
                          the outputs, encoders and services that send them
    src/vertical/         the tall picture: its canvas and scenes, its
                          places and recording, its window in OBS
    src/record/           clean recordings: a private picture and file per source
    src/moves/            the DireBot Move transition and its plan
    src/filters/          the DireBot Blur filter
    src/face/             the DireBot Face Avatar source and its face tracker
    src/face/nn/          our own engine for the face models
    data/effects/         the shaders the transition, the filter and the face avatar draw with
    data/face/            Google's MediaPipe face models (Apache 2.0, see its NOTICE)
    deps/                 nlohmann/json (MIT), obs-websocket's API header (GPL-2.0)

Later parts each get their own folder under `src/`, their own `direbot`
requests and a line in `features`.

## Licence

GPL-2.0-or-later, like OBS. See LICENSE.

`data/face/face_landmarker.task` is Google's MediaPipe Face Landmarker,
unchanged: Copyright The MediaPipe Authors, under the Apache License 2.0
(`data/face/LICENSE`, `data/face/NOTICE`).
