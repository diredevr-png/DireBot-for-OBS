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
character. Or a **3D character** instead of the pictures: a `.glb`/`.gltf`
(Blender and most 3D tools) or `.vrm` (VRoid Studio, VRM 0.x and 1.0),
read by our own code (`src/face/model.cpp`) and drawn with OBS's own
graphics: lit glossy, as a cartoon (VRM's MToon, with its outlines) or
flat, see-through round it. It turns and nods with you (a VRM's neck and
head bones, or the whole of it round its head), makes your 52 expressions
(parts named as Apple's ARKit names them, or VRM's expressions worked out
from yours), looks where you look, and its swinging parts swing: VRM's
spring bones, or any bones that hang free (a chain of links, a doll's
ears and legs). Characters standing in a T get their arms down. Its file's
own animation (an idle, a tail's wag: glTF animations, as Blender saves
them) plays over and over if one is chosen (the `animation` setting, its
name); your head, eyes and its swinging parts move from where it has them.
A file of more than one character (each its own part) can keep one's face
still: the `still_faces` setting names them, a part's name a line ("#n" for
an unnamed n-th node). Shapes your face moves start from nothing, whatever
strength the file was saved with. With two characters sharing a blink or a
mouth, your real eyes and mouth go on the one round the head bone.
A VRM's fingers rest relaxed (the `fingers` setting: 0 as made, 50
relaxed, 100 curled; fingers its file already bends are bent only the rest
of the way, and ones its own animation moves are left to it).
Your real
eyes and mouth go on its face too, soft-edged and in its skin's colour,
turning half as far as it does, while its own stay still under them (or
its own drawn face instead, as you choose). It finds where they go from its
blink and mouth shapes or eye bones, or its set-up says. In its own
spot, or worn on your head over the camera (a VRM's eyes on yours), with
a hidden head shape so what's behind your head stays behind it.
Outfit pictures ride on its head (placed on its front picture, in front
of it or behind it). DireBot sets it up (Avatar → Face avatar), including a character
imported from a Lens Studio project: its set-up (the `model_setup`
setting, JSON in the model's own units) says which way it faces, what
it turns round, where its eyes and mouth are, which bone chains swing and
which way they're pulled, and which parts stay upright as your head turns
(trailing after it), and which parts are its own drawn eyes and mouth
(hidden while your real ones are on it). DireBot's starter characters
come with one. Set-up it can't read counts as none.

**Tune your face, and alive when you're still (0.5.0).** Google's face
models read some faces and cameras muted (a blink that never quite
closes, a mouth that barely opens), so for each of the face's main moves
(`blink`, `mouth`, `smile`, `round`, `brows_up`, `brows_down`, `look`)
the `tune_<name>_from` and `tune_<name>_to` settings (0-100) say where the
camera's reading starts to move the character and where it's all the way;
`linked_blink` blinks both eyes together, `open_when_smiling` keeps its
eyes open in a big smile (a real blink still shuts them), and `eye_move`
(0-200) is how far its eyes move. A character that holds perfectly still
looks frozen, so it breathes (`breathing` 0-100, a breath every
`breath_time` seconds: VMagicMirror's sin-squared breath), sways and looks
about a little (`sway`), and its eyes make small moves (`eye_moves`).
When it can't see you it holds your last look for a second, then eases
into a gentle idle - looking about, blinking by itself - and is back with
you as soon as it sees you; `auto_blink` makes it blink by itself while it
sees you too. A VRM's chest and shoulders rise with each breath and its
body takes some of your head's turn (`body_follow` 0-100); a 3D character
with no bones grows a touch with each breath, and a 2D one rises.

**The mouth from your mic, lights you choose, and camera views (0.6.0).**
Its mouth moves from your mic as well as your camera (`mouth_from`:
`camera`, `mic` or `both` - in both, the camera's mouth until you talk,
then the mic's): it listens to the mic you pick in OBS (`mic`, a source's
name; empty, your first mic) and finds the vowel you're saying - A, I,
U, E or O, the mouth shapes every VRM has - from where the voice's two
lowest resonances are, and how loud it is: the mouth starts to open at
`mic_start` dB and is open all the way at `mic_full` dB (a gate below
that), times `mic_amount` (0-200%). OBS's own mute and volume count, as
its meter shows them. An "s" or a click opens it a little, teeth
together. "Say each vowel" (`FaceMicCalibrate`) listens to you say each
for a moment and keeps where your own voice puts them (`mic_profile`).
The lights on a 3D character (`light_preset`: `studio` - as it always
was - `daylight`, `cozy`, `night`, `neon`, or `own`: `light_side` and
`light_height` in degrees, `light_colour` and `light_brightness`, the
room's `room_colour` and `room_brightness`, a rim light's `rim_colour` and
`rim_brightness`) sit where your camera is, so every camera view is lit
the same, and your real eyes and mouth on it take its light. Its file's
normal maps (bumps and creases) and occlusion maps (shadows in folds) are
drawn (`detail_maps`; its own tangents, or worked out as Blender does); `soft_skin`
(0-100%) lets light reach round into the shadow side of its skin, warmer -
on the materials named like skin (`skin`, `face`, `body`, `head`, and
from 0.8.0 a whole word `ear`, `neck`, `nose`, `lip`, `hand`, `arm`, `leg`,
`foot` or `torso`, so `Ears_L` but not `Earring`; never its eyes, hair,
teeth or clothes), or the ones `skin_parts` names, a name
a line. Up to three saved camera views (`views`, JSON: `name`, `yaw` and
`pitch` in degrees round it, `zoom` (1 as framed), `x` and `y` - shares of
the picture to move it) - `view` shows one (`1`-`3`, or `main`), gliding
there in `view_glide` ms.

**Faces, moves, things thrown at it and headpats (0.7.0).** On cue - a
deck pad, a chat command, a redeem or an alert, through `FaceReact` - a 3D
character makes a face for a few seconds (happy, angry, sad, relaxed,
surprised, tongue out, or one of its file's own expressions or shapes by
name), easing in and out, and a new one takes over from the one before.
Any character nods, shakes its head, tilts it or bounces; a VRM waves or
cheers with its arms, plays one of its file's own animations, or plays a
VRM animation file (`.vrma`: `react_moves`, JSON: `name` and `file` each,
up to 32) - the standard way VRM moves are shared, carried over onto its
own bones as tall or short as it is. Things thrown at it (`react_items`,
JSON: a `file` each, a picture, up to 24) fly in from a side, growing
smaller as they go (thrown from where you watch), spinning; each hits its
face - its head knocked back where it hits, then springing back and
settling, its hair and ears thrown about by their own springs, a flash
where it hit (`react_hit`, a picture), its eyes screwed shut a moment and
a face if you pick one - then bounces off and falls away. A volley comes
one after another, at most 8 in the air and 16 waiting. A headpat
(`react_hand`, a picture of a hand) comes down onto the top of its head
(on a character that fills its picture to the top, far enough down that
it's seen) and pats it, its head bobbing with each pat, while it squints
happily.

Also in 0.7.0, a 3D character drawn more faithfully to how it was made: a
part shows only its front unless its material is double-sided, as glTF
says (and VRM 0.x's MToon culling) - so a hand-made outline (a dark shell
turned inside out) is a rim, not a dark cover, and inner faces stay hidden;
cut-out pictures (lashes, stubble, hair cards) stay as full small as at
full size, and colour pictures shrink as light (fine dark detail no
darker small); a see-through part in the glossy look keeps its shine (a
clear lens, visor or eye's front), while its picture's empty parts stay
empty; a relaxed thumb bends towards the first finger, not down under
the hand. A character file made again in its place loads again when OBS
is next told about it, and files in a folder named in other letters load
on Windows. `FaceGetState` says which parts with bumps had no tangents in
their file (worked out instead; from 0.8.0 with MikkTSpace, the way Blender
and most other tools work them out, so its bumps look as they did there).

**Your hands and arms (0.8.0).** A 3D VRM character in its own spot (not
worn on your head) can follow your arms, hands and fingers from the same
camera: its upper arms and forearms point where yours do, its elbows bend
your elbows' way, its hands turn as yours (its forearms take half the
twist, so its wrists don't pinch thin) and each finger joint bends as
yours, within each joint's natural limits and how far a wrist bends (the
hand model reads a fist's knuckles less bent than they are, 45 to 70
degrees, not 90, and its tips more, so a joint read as far as its fist
reads is bent all the way: a fist closes, its thumb across it). An
arm raised above level lifts its shoulder too (a third of the way, at
most 25 degrees, as a real shoulder blade turns), so the top of a raised
arm doesn't fold into its neck. It
follows your body as you face the camera - against your chest, so leaning
or turning in your chair doesn't swing its arms, and they ride on its own
breathing and turning body - mirrored as its face is (`mirror`). Near your
head, its hand goes to the same place by its own head, whatever its size:
a hand on your cheek is on its cheek. It uses more of your PC, so it's
off until you turn it on (`follow_arms`); `arms_hands` follows your hands
and fingers too (off: your arms only, quicker); `arms_light` looks 10
times a second instead of 15, with a lighter hand model. `arms_reach`
(50-200%) is how far it reaches towards and away from the camera (the
camera judges that least well), `arms_spread` (0-45 degrees) holds its
arms out from its body when they hang down, `arms_hold` (ms) is how long
a hand or arm it loses stays as it was before it rests (arms down,
fingers relaxed; found again, it's back in a quarter of a second), and
`arms_smoothing` (0-100) is its own steady-or-snappy. A hand must be seen
two looks running before it moves anything, and a hand is only looked
for at your own wrist, must be where your wrist is and counts once if
it's found twice - so a hand in the background, or a "ghost" hand, never
moves it. Only an arm it can see is followed: an arm hanging below the
picture, or the body model's guess at an arm out of sight (its elbow out
of the picture and no hand found at its wrist - with `arms_hands` off it
still looks for one there), rests. Calibrate (`FaceArms`): hold a hand up
by your ear, open, palm to the camera, and it learns how your open hand
looks (so its fingers open all the way when yours do, and still close
all the way in a fist) and where your hand is by your ear (so its hand
goes by its ear); it's kept in `arms_calibration`. A move on cue
(a wave, a cheer, an animation) takes over its arms while it plays.
`FaceGetState`'s `hands_and_arms` says what the camera sees of you, for
DireBot's camera check: your head, shoulders, elbows, wrists and hips in
the picture.

Google's MediaPipe Holistic models do the seeing (its body finder, body
model, hand-square model and hand model, `data/face/holistic_landmarker.task`,
and the lighter hand model, `data/face/hand_landmark_lite.tflite`), run by
the same engine as the face's, with the steps around them as MediaPipe's
Holistic Landmarker does them: the tests check each model against Google's
own runtime and the body's points against MediaPipe's own answers.

Also in 0.8.0, a character file with bumps but no tangents has them worked
out with MikkTSpace (as Blender, Unity and Unreal do) instead of our own
guess, so its bumps and creases fall the way they were made.

And from 0.8.0 a character's bones that follow others move as its file
says (VRM's node constraints, `VRMC_node_constraint` 1.0, as the VRM
Add-on for Blender and UniVRM save them): twist bones turn with its arms
(round their own length only, so a twisted forearm or upper arm doesn't
pinch thin), a bone aimed at another stays pointed at it (a sleeve at its
elbow), and a bone copying another's turn copies it. They're worked out
after everything else moves it - your head, arms and fingers, a move on
cue, its own animation - and before anything swings from them, each after
the ones it waits on. One a file shouldn't have (following itself, or in
a loop of them) is left out. `FaceGetState`'s `model` says how many it
follows (`following_bones`) and how many it left out
(`following_bones_left_out`).

**In its own spot, all of it as it stands (0.8.1).** "All of it"
(`framing` `whole`, and anything not a VRM) frames a 3D character as it
stands - its arms down by its sides, with a little room round it - not by
the box of the T most are made in (as wide as its arms reach, its hands
hanging out of it once they're down). **Size** goes past 100%, up to
300%: bigger than its spot, its top kept in it and the rest cut off at the
bottom (head and chest out of all of it). `FaceGetState`'s `model` says
the shape of what's framed (`framed`: `w`, `h`), so DireBot can fit the
source's box to it - a tall, narrow box round a figure standing up, with
no empty sides. And a 3D character in a scene that isn't shown - or
switched off in the one that is - loads all the same: OBS only draws
what's shown, and it waited to be drawn, saying it was loading until it
was.

**A real headpat, 3D things and chat's emotes thrown (0.8.1).** A
headpat's hand comes in from a side (`pat`'s `side`, or either at random),
palm down, rests on the top of its head and pats it - lifted from the
wrist and let fall, a couple of times a second - while its head dips and
squishes with each pat (shorter and a little wider, from its neck - a
picture character all of it, from its bottom edge - springing back) and
leans into the hand, its eyes shut happily till a moment after the hand
has gone. The hand is a 3D one (`react_hand_3d`, a
glTF hand: fingers along -x, thumb +y, palm +z, its palm's middle at 0, in
metres, with a `lift` shape) laid on the top of the head as it's shaped
(hair and all), turned to come from its side; its parts whose material's
name has `skin` or `sleeve` in it are coloured by `react_hand_tint` (JSON:
`skin` and `sleeve`, each red, green and blue to multiply by, linear,
0-4). Over a 2D character it's drawn over the picture. Without one, a
picture of a hand (`react_hand`) comes in from the side and turns at its
wrist; `react_hand_place` (JSON: `touch` and `wrist`, shares of the
picture as it comes from the left, and `width` in heads) says where it
touches the head and bends - with none, the middle of its bottom edge
touches. Things to throw can be 3D (`react_items` with a `.glb`, `.gltf`
or `.vrm` file): loaded on a thread of their own, drawn lit in 3D,
spinning as they fly, at most 400,000 points each. A model coloured from
a small palette picture (a low-poly pack's: a few colours, often one
pixel each and not a power of two across) keeps each colour exact: a
picture of 64 colours or fewer that has to be made bigger is copied
pixel for pixel, to at least 8 times across (512 at most), never
smoothed into the colour beside it - a 3D character's too. And one of chat's
emotes is thrown by its picture's file (`throw`'s `file`: a PNG, JPEG,
WebP or GIF - a moving GIF moves), the newest 64 kept open. A throw can
stick (`stick`: seconds, up to 10) - where it hits, moving and tipping
with its head, then sliding down and falling off; at most 12 stuck at once,
the one stuck longest falling first - and come as a bonk (`from`:
`above`): it shows up over its head, rises a little tipping back, and
comes down on top of it - its bottom onto the top of its head, however
tall its picture or model is - its head nodded down and squished. `count` (1-16)
throws a barrage of it, one after another; `stop` makes what's stuck fall
off.

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
 "obs": "32.0.2", "websocket": "5.6.3", "plugin": "0.4.1", "started": 1798000000}
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
| `FaceGetState` `{source, points?}` | `camera`, `ready`, `showing`, `found`, `presence` (0-1), `set`, `setting`, `turn` (`yaw`, `pitch`, `roll` in degrees, since "Set"), `turned` (-1, 0, 1), `moves` (the 51 expressions, 0-1 since "Set", ARKit's names), `parts` (first eye, second eye, mouth: `x`, `y`, `w`, `h` shares of the camera's picture), `shown`, `holding`, `looks_per_second`, `ms`, `problem`; with `points`, the 478 `points` (`x`, `y` shares); with a 3D character, `model`: `file`, `loaded`, `loading`, `shown`, `kind` (`gltf`, `vrm0`, `vrm1`), `title`, `author`, `points`, `triangles`, `springs`, `animations` (its own animations: `name` each), `animation` (the one playing, or ""), `swinging_chain`, `arkit_parts`, `blinks` and `talks` (once it's rigged: whether its own blink and mouth shapes move its face enough to see, at least 0.1% of its height; tiny marker shapes don't count), `set_up` (it has a set-up), `upright_parts`, `cards` (your eyes and mouth drawn on it last frame, 0-3), `outfit_layers` (outfit pictures drawn on it last frame), `spots` (`left_eye`, `right_eye`, `mouth`: `x`, `y`, `w`, `h` shares of its picture from the front, `from` where it found them: `expressions`, `eye bones`, `set up`, `guessed`), `expressions` (VRM's, 0-1), `yaw`, `pitch`, `x`, `y` (its middle in the picture, pixels), `finger_joints` (the finger joints it curls), `body` (it has a spine or chest that breathes and follows your head), `following_bones` and `following_bones_left_out` (0.8.0: its bones that follow others, VRM's node constraints, and those its file has that can't be), `framed` (0.8.1: `w`, `h` - how wide and tall what's framed in its own spot is, in the model's units, once it's loaded), `problem` in words when it couldn't be read. Always (0.5.0): `tune`: `reading` and `shows` (each tune group, 0-1: what the camera reads and what the character shows from it) and `reading_peak`, `shows_peak` (the same, held at their highest for 0.6 s); `alive`: `idle` (0 following you to 1 idle), `breath` (0-1); `camera_check`: `seen`, `box` (`x`, `y`, `w`, `h`: your face in the camera's picture, shares of it), `facing` (`yaw`, `pitch`, `roll`: which way you face the camera itself, degrees, not since "Set"), `light` (`face`, `left`, `right` - each side of your face as the picture shows it - and `room`, the rest of the picture: 0 black to 1 white), `camera_width`, `camera_height`. Always (0.6.0): `mic`: `from`, `mic` (as set), `found`, `listening_to` (the mic it hears), `hearing`, `muted`, `level` and `peak` (dB), `start`, `full`, `heard` (louder than the start), `voiced`, `talking`, `open` (0-1), `share` (how much of its mouth is the mic's, 0-1), `vowels` (`a`, `i`, `u`, `e`, `o`: 0-1), `f1`, `f2` (Hz), `own_voice` (a saved "say each vowel"), `calibration` (`running`, `vowel`, `number`, `listening`, `left` seconds, `done`, and once done `heard` per vowel and `own`); `lights`: `preset`, `side`, `height`, `colour`, `brightness`, `room_colour`, `room`, `rim_colour`, `rim`, `detail`, `soft_skin`; `camera_view`: `view` (the one asked for), `showing`, `gliding`, `views` (`name`, `yaw`, `pitch`, `zoom`, `x`, `y`); and in `model`: `materials` (`name`, `bumps`, `shadows`, `skin` each; 0.7.0: `own_tangents`, its bumps on the file's own tangents), `bump_parts`, `shadow_parts`, `skin_parts`, `guessed_tangent_parts` (0.7.0: parts with bumps whose file had no tangents, so they're worked out). Always (0.7.0): `react`: `faces` (the faces it makes: `name` each), `shapes` (its file's own shapes: `name` each), `arms` (it waves and cheers with them), `vrm_moves` (it plays VRM animation files), `moves` (each of `react_moves`: `name`, `length` seconds, `problem` in words when its file couldn't be read), `items` (each of `react_items`: `loaded`; 0.8.1: a 3D one `three_d`, and `problem` in words when it can't be read), `hand`, `hit` (their pictures loaded), `hits` (so far), `flying` (in the air now), `busy` (anything on); 0.8.1: `hand_3d` (the 3D hand loaded), `hand_3d_problem`, `emotes` (emote pictures kept open), `patting`, `stuck` (things stuck to it now), `squint`, `squish` (0-1, a headpat's or a bonk's), `hand_now` (while it's shown: `side`, `rise`, `away`, `lift`, `opacity`). Always (0.8.0): `hands_and_arms`: `on`, `hands`, `light` (as set), `can` (it can follow them on this character now), `why_not` (`off`, `worn`, `no_3d`, `not_vrm`, `no_arms`, or ""), `loading`, `ready`, `problem` (the body models), `looks_per_second`, `ms`, `found` (a body), `following` (`left_arm`, `right_arm`, `left_hand`, `right_hand`: yours, followed now), `calibrating`, `calibrated`, `calibration` (`ok`, `no hand`, or "" - the last one's result), `framing` (`found`, and with a body: `head` (in the picture), `shoulders`, `elbows`, `wrists`, `hips` (how many, 0-2), `hands` (found), `shoulder_width` and `shoulder_height` (shares of the picture)); with `points`, `points` (your top half's 25 points: `x`, `y` shares of the picture, `seen` 0-1) and `left_hand_points`, `right_hand_points` (21 each, when found) |
| `FaceMicCalibrate` `{source, do}` | "Say each vowel": `do` `start` (it asks for each vowel in turn - follow `mic.calibration` in `FaceGetState` - and saves your voice's places in `mic_profile` when it's done), `stop`, or `forget` (back to the usual places). `ok`, or `error` in words (no mic found, or its mouth is from the camera only) |
| `FaceReact` `{source, do, ...}` | what it does on cue. `do`: `face` (`face`, its name in any case, "" eases the one on off; `seconds`, up to 60; `amount` 0-1) answers `face` (its own name for it), `shows`, and `why` in words when it won't show (not a 3D character, or no face called that); `move` (`move`: `nod`, `shake`, `tilt`, `bounce`, `wave`, `cheer`, or a `react_moves` name or one of its file's own animations, any case) answers `kind` (`head`, `arms`, `file`, `own`) and `shows` (false for `wave` and `cheer` without a VRM's arms, and for a `react_moves` file's move on a character that isn't a VRM); `throw` (`item`, the number in `react_items` from 0, or (0.8.1) `file`, a picture's full path - one of chat's emotes; (0.8.1) `stick`, seconds stuck where it hits, up to 10 (0 bounces off); `from`: `above` a bonk on top of its head, else from a side; `count`, 1-16, a barrage of it; `side` -1 its left, 1 its right, 0 either; `strength` 0.1-2; `size` 0.2-3 (1: a little under half its head); `face` and `face_seconds` (up to 10), the face it makes when it's hit) answers `hits_in` (seconds until it hits, for the hit's sound) and (0.8.1) `thrown` (how many of `count` were taken: never more than 16 waiting) and `every` (seconds between them); `pat` (`count`, 1-20 pats; `size` 0.3-3 (1: about its head's width); `face`, "happy" unless given, "" none; (0.8.1) `side`, -1 from the picture's left, 1 its right, 0 either at random); `stop` (everything eased off quickly, nothing waiting). `ok`, or `error` in words |
| `FaceArms` `{source, do}` | Your hands and arms (0.8.0): `do` `calibrate` (hold a hand up by your ear, open, palm to the camera; follow `hands_and_arms.calibrating` in `FaceGetState` - it's saved in `arms_calibration` when it's done), `stop`, or `forget`. `ok`, or `error` in words (not on, your hands not followed, not a 3D VRM in its own spot, the body models not ready, or the avatar not shown) |
| `FaceModelFront` `{source, width?, close?}` | its 3D character straight from the front as it's made: `width`, `height`, `rgba` (base64, plain colour), `box` (`left`, `top`, `width`, `height` in the model), `region` (`x`, `y`, `w`, `h`: the part of the box drawn, as shares of it), `spots` (as in `FaceGetState`) - its front picture, which spots and outfit places are shares of. `close`: just the part round its face, for its eyes and mouth up close (`spots` are still shares of the whole box) |
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
needs `pip install mediapipe`, for the test only). The body tracker's
(`tests/unit_body.cpp`) check each body and hand model against LiteRT,
Google's own runtime, and the body against MediaPipe's own answers on two
photos (`tests/data/body`); `tools/body-expected.py` makes those (`pip
install mediapipe ai-edge-litert`). Inside OBS, `test_real_face.py` has a
VRM character follow the arms in one of those photos, a bone copying its
arm's turn going round with it.

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
    src/face/             the DireBot Face Avatar source, its face tracker,
                          and its body tracker and arm solver (body, arms)
    src/face/nn/          our own engine for the face and body models
    data/effects/         the shaders the transition, the filter and the face avatar draw with
    data/face/            Google's MediaPipe face, body and hand models (Apache 2.0, see its NOTICE)
    deps/                 nlohmann/json (MIT), obs-websocket's API header (GPL-2.0),
                          MikkTSpace (zlib)

Later parts each get their own folder under `src/`, their own `direbot`
requests and a line in `features`.

## Licence

GPL-2.0-or-later, like OBS. See LICENSE.

The mouth from your mic is our own code; its first steps (bringing the
sound down to a lower rate and looking at it 10 ms at a time) follow
uLipSync's (Hecomi, MIT licence) as a recipe, with no code copied. Its
vowels' starting places are the averages Hillenbrand et al. measured
(J. Acoust. Soc. Am. 97, 1995).

VRM animation files (`.vrma`) are carried onto a character's bones as the
VRM Consortium's VRM Animation spec says, the way pixiv's three-vrm (MIT
licence) does it: its steps as a recipe, with no code copied. The same
goes for bones that follow others (VRM's node constraints) and the order
they're worked out in.

`data/face/face_landmarker.task` is Google's MediaPipe Face Landmarker,
`data/face/holistic_landmarker.task` its Holistic Landmarker and
`data/face/hand_landmark_lite.tflite` its lighter hand model, all
unchanged: Copyright The MediaPipe Authors, under the Apache License 2.0
(`data/face/LICENSE`, `data/face/NOTICE`).

Following your arms and fingers is our own code; its maths (each part of
an arm pointing where yours does, a hand turned to your palm's way, each
finger joint bent as far as the angle between your finger's bones) follow
Kalidokit's (yeemachine, MIT licence) as a recipe, with no code
copied.

`deps/mikktspace` is Morten S. Mikkelsen's MikkTSpace, unchanged, under
the zlib licence (in its own files and `deps/mikktspace/README.md`): it
works out a character's tangents when its file has none.
