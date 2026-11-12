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
read by our own code (`src/face/model.cpp`; files up to 1 GB, 0.10.2:
512 MB before) and drawn with OBS's own graphics: lit glossy, as a
cartoon (VRM's MToon, with its outlines) or flat, see-through round it.
It turns and nods with you (a VRM's neck and head bones, or the whole of
it round its head), makes your 52 expressions
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
body takes some of your head's turn (`body_follow` 0-100) - except while
a move that holds it still plays (see "A body held still while a move
plays" below); a 3D character with no bones grows a touch with each
breath, and a 2D one rises.

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
and fingers too (off: your arms only, quicker); `arms_light` looks at
your hands 15 times a second instead of 30 (your body 10 instead of 15),
with a lighter hand model. `arms_reach`
(50-200%) is how far it reaches towards and away from the camera (the
camera judges that least well), `arms_spread` (0-45 degrees) holds its
arms out from its body when they hang down, `arms_hold` (ms) is how long
a hand or arm it loses stays as it was before it rests (arms down,
fingers relaxed; found again, it's back in a quarter of a second), and
`arms_smoothing` (0-100) is its own steady-or-snappy. A hand must be seen
two looks running before it moves anything, must be one the hand model
is sure of and within your arm's reach, and counts once if it's found
twice - so a hand in the background, or a "ghost" hand, never moves it. Only an arm it can see is followed: an arm hanging below the
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
the lighter hand model, `data/face/hand_landmark_lite.tflite`, and from
0.9.0 the palm finder, `data/face/palm_detection_full.tflite`), run by
the same engine as the face's, with the steps around them as MediaPipe's
Holistic Landmarker (and, for your hands, its Hand Landmarker) does
them: the tests check each model against Google's own runtime, the
body's points against MediaPipe's own answers and the hands' against its
Hand Landmarker's.

**Hands found on their own (0.9.0).** Each hand is found by itself, not
only where the body model thinks your wrist is (in a dim room, or with an
arm raised, the body model often has your wrist somewhere else), as
MediaPipe's Hand Landmarker finds them: a hand found is followed from
where it was on each look, and one that isn't is looked for at your
wrist and then by Google's palm finder on the whole picture (when a hand
has just been lost, and twice a second while one isn't followed; it
costs as much as a look at a hand, so not on every look). Left or right
comes from the hand itself (the hand model's own answer, over a few
looks), checked against your body's wrists and shoulders when the body
model is sure of you. Your hands are looked at up to 30 times a second
and your body up to 15 (`arms_light`: 15 and 10; your arms only: 15),
so quick hand moves keep up. The body model sometimes has you turned
round (your back to the camera) with both hands up; you face the camera
to stream, so its left and right are swapped back. And where a hand is
found away from where the body model has its wrist, its arm follows the
hand: the wrist goes to the hand and the elbow is worked out from your
shoulder, bent the way the body model had it. `FaceGetState`'s
`hands_and_arms` also says how often it looks at most
(`most_per_second`), how often the body model ran (`body_per_second`),
whether the palm finder is there (`palm_finder`), and how often each of
your hands was found over the last few seconds and how lit it was then
(`hands_seen`); its `camera_check`, how many new pictures a second the
camera sends (`camera_fps` - a camera in a dim room often drops to 15).
For DireBot's camera check.

**Hand signs and face moves that set things off (0.9.0).** DireBot
tells the face avatar which signs to watch for (`hand_signs` on, and
`signs`: a JSON list of `{id, what, side, hold_ms, raised, strength}`)
and runs a routine for each one it hears about. Hand signs: `open_palm`,
`fist`, `thumbs_up`, `thumbs_down`, `point_up`, `finger_gun`, `peace`,
`middle_finger`, `horns`, `ok`, and `wave`; face moves: `mouth_open`,
`brows_up`, `smile`, `kiss`, `wink`. `side`: `either`, `left`, `right` or
`both` (your own hands; a wink's eye). Each one held `hold_ms` (a wave as
soon as it's seen) sends a `FaceSign` vendor event once - held on, it
isn't sent again until it's let go. Your hands are looked at for this
even with `follow_arms` off, and the character's arms don't move for
them. The shapes are our own rules on each hand's 21 points in metres
(signs.cpp): how far each finger reaches against its own length and how
much it bends, where the thumb is, and which way the thumb or first
finger points - the same near the camera or far, turned any way, either
hand. `raised`: only with the hand up by your face or higher (a hand
holding a mouse, a controller or food looks like a fist). A wave is an
open hand going side to side, three swings within a second and a half.
Face moves are your face's own through your tuning (`tune_*`), so
"mouth wide open" is as wide as you've tuned the mouth; `strength`, 10 to
100, is how much of it. With `signs_hidden`, it watches while it's
hidden too (a PNGtuber's: the avatar in no scene that's shown) - the
camera is looked at as it would be if it were shown, and held "shown" so
a camera set to stop when it isn't keeps going; `FaceSet` works then
too. `FaceGetState`'s `signs` says what it sees.

**Your face from a phone (0.9.0).** With `face_from` set to `phone`, the
face comes from an app on your phone instead of the camera, over your
Wi-Fi (`src/face/phone.cpp`: one UDP socket on a thread of its own).
`phone_app` `vtube_studio` (VTube Studio's phone app, iPhone or Android,
with "3rd Party PC Clients" on) and `meowface` (Android) are asked on the
phone's port 21412 about once a second for three seconds of looks
(`{"messageType":"iOSTrackingDataRequest","time":3,"sentBy":"DireBot",
"ports":[ours]}`) and send JSON back, about 60 a second; `ifacialmocap`
(iPhone) is sent its hello on port 49983 and sends its lines to our
49983. `phone_address` is the phone's address as the app shows it (with
`:port` or without; a name works too), and only packets from that address
are taken. The 52 ARKit face moves come in Apple's names, with Apple's
left and right the other way round to MediaPipe's (Apple's
`eyeBlinkLeft` is your right eye): they're swapped, so a wink is the same
eye from a phone as from the camera, and the head's turn is made the
same as the camera's (yaw towards the picture's right, pitch up, roll
clockwise). How the turns, places and sides compare was checked against
programs that convert one to the other every day (Inochi2D's facetrack-d,
BSD; VTube-IFacial-Link and ExpressionAppBridge, MIT) - none of their
code is in here. The look then goes through the tracker as a camera's
does, so "Set", the smoothing, the tuning, alive when you're still and
the hand signs' face moves all work the same. What a phone can't give:
your real eyes and mouth (the character's own face shows), being worn on
your head (`worn` is ignored), and your hands - the camera (`source`) is
only looked at for those, when it's set and `follow_arms` or a hand sign
wants it. On Windows, the first time, Windows asks whether OBS may use
the network (tick Private networks). `FaceGetState`'s `face_from` and
`phone` say how it's going.

**Moves from another tracker app (0.9.0).** Apps that follow you - XR
Animator, Webcam Motion Capture, VSeeFace, VirtualMotionCapture, mocopi's
and SlimeVR's bridges, Waidayo and other phone apps - send their
character's bones and face over VMC (OSC over UDP), and a face avatar
listens for them on this PC's `vmc_port` (39539 unless you pick another;
`src/face/vmc.cpp`: our own OSC reader, one UDP socket on a thread of its
own, shared by every face avatar on that port). It reads
`/VMC/Ext/Bone/Pos` (each bone by Unity's name, its place and turn),
`/VMC/Ext/Blend/Val` and `/VMC/Ext/Blend/Apply` (its face's shapes:
VRM's - `A`, `Blink_L`, `Joy`... or 1.0's `aa`, `blinkLeft`, `happy` -
and Apple's ARKit names, "perfect sync") and `/VMC/Ext/OK`; it takes one
app at a time (the first that sends; another is counted until the first
has been quiet a second). Each part as you pick:

- `face_from` `vmc`: your face from it, through the tracker as a phone's
  (so "Set", the tuning, alive when you're still, the hand signs' face
  moves and a 2D character all work): its ARKit shapes as they are,
  VRM's blinks, vowels and looks made into them; its eye bones as its
  looks when its shapes don't say; its head's turn from its neck and head
  bones. Its faces (happy, angry, sad, relaxed, surprised) and its own
  shapes are shown by their names on a character that has them.
- `vmc_body`: its hips, spine and chest and its legs (its hips' place
  as far for your character's height, drifting back to its own spot so
  it doesn't walk out of it); `vmc_arms`: its shoulders, arms, hands and
  fingers. These two need a 3D VRM character in its own spot (VRM's
  bones: each bone's turn is VRM's "normalized" turn, as a VRM animation
  file's).

Unity is left-handed (x its right, y up, z its front): a place
(x, y, z) is ours (-x, y, z) and a turn (x, y, z, w) ours (x, -y, -z, w).
`vmc_mirror` swaps its left and right (for an app that doesn't mirror
you). A move on cue still takes the bones it moves (0.10.1: and so
does the idle a move that stays plays, as it shows -
riding a mount, its hands stay on their holds and its hips on the mount,
the app's body easing back in as it gets off; your head from a camera is
turned against the app's chest only as far as its chest is the app's,
not a move's); bones the app doesn't
send stay as they were (your camera's arms, if they follow you, or
hanging by its sides), and each part eases in and out over a quarter of a
second as the app starts and stops sending. `FaceSet` with the face from
it fails, in words, when it isn't sending your face; with its body,
"Set" also makes where its hips are now its own spot. `FaceGetState`'s
`vmc` says how it's going.

**Gaming hands (0.9.0).** Off until you turn it on (`game_hands`): a 3D
VRM character in its own spot puts its hands on a keyboard and mouse or
a pad of its own while you play - its keyboard hand's fingers pressing
as you press keys (both hands typing once your mouse has been still a
moment), its mouse hand following your mouse (as far as yours went, its
mouse drifting back to the middle of its mat) and clicking, both hands
on its pad with its thumbs on the sticks and buttons and its first
fingers on the triggers. On Windows it reads your keyboard and mouse
with Raw Input (`RIDEV_INPUTSINK`, so it works while your game has the
focus) and your pads with XInput (`src/face/devices.cpp`, its own window
and thread, opened while any face avatar has it on and closed when none
does). It only ever notices that a key went down or came up and how many
are held - never which one - so what you type (a password, a chat
message) can't be told from what it keeps: counts, in memory only,
never saved or logged. Elsewhere it reads nothing and says so. Pads
are XInput's: Xbox pads, and others through Steam Input or DS4Windows
(up to four). A game running as administrator may keep its input from
an OBS that isn't (Windows' UIPI).

Which it uses (`game_device`): `auto` whichever you used last (a few
presses or a moment's playing switches it, so a stray key doesn't),
`keyboard`, or `pad`. `game_mouse` `right` or `left` (your mouse hand;
mirrored, as everything is, when the character mirrors you), `game_props`
its own keyboard, mouse, mat and pad shown (`data/face/gaming`, made from
code by `tools/make-gaming-props.py`; off, its hands just move as if
they were there), `game_rest` seconds with nothing used before its hands
rest (0: never), `game_height` and `game_distance` (-40 to 40, shares of
its arm's length) raise the desk and move it away. Each hand glides from
one to the other (lifted over on its way); everything's laid out for a
real person's arm and grown with the character's arms, the pad (and how
it's held) with its hands - and each finger reaches its own spot on it
(its thumb onto its stick's top as it leans, or its buttons; its first
finger on its trigger, up on its bumper when that's held; the others round
under its grip), so it holds it right whatever its hands are like. Its
spot's head-and-shoulders framing takes in the desk, seen from a little
above. A move on cue still takes its arms, and a move that stays (a
ride) puts its desk away till it ends; it's over your arms from the
camera and a tracker app's while it's on. `FaceGame` `do` `show` pretends
14 seconds of typing, a game on the keyboard and mouse, then one on a
pad (to see it without playing), `stop` ends it. `FaceGetState`'s
`gaming` says how it's going.

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

**What a thing does as it hits, and throws at your camera (0.9.0).** A
row of `react_items` can say what it does as it hits (`hit`: JSON,
`effect` `splat`, `feathers` or `pieces`, and each picture by its full
path). A splat (`mark`, `drip`, `bits`, `seconds` 0.5-15) is left on the
character where it hit - only on the character, never the air round it -
spreading a little, juice (`bits`) thrown off and falling, a few drips
(`drip`, hung from their top) running down from it; it moves and tilts
with its head, and after `seconds` slides down a little and fades. The
thing itself is crushed into it and never bounces off. Feathers
(`bits`) puff out a little and drift down past its face, slowed by the
air, swaying side to side. Pieces (`bits`, `puff`, `mark`) burst out and
fall, two puffs grow and fade, and `mark` is left on it for `seconds`.
`squishy` (with any of them, or none) squashes the thing flat as it hits
and wobbles it back. At most 24 marks and 240 bits at once; a picture
that isn't there leaves nothing (the thing is still thrown). `stop`
fades them all at once. `FaceGetState`'s `react` says `marks` and `bits`
(how many now), and each item's `hit` (`effect`, `loaded`).

"DireBot Throws" (`direbot_throws_filter`) is a filter for a camera: the
same throws land on your real face in it, for streamers who don't use a
face avatar. DireBot puts it on, with `react_items`, `react_hit` and
`camera_shake` (on to start with: the camera jolts a little as each
thing hits, zoomed in just enough that its edges never show). It finds
your face in the camera (held while it can't see you; until it first
does, a guess a little above the middle), and what's thrown, and what
it leaves, follows your head as it moves and tilts. Asleep it leaves the
camera exactly as it is and looks at nothing: it wakes when something
is asked of it and sleeps 4 seconds after it's all over. It's asked as a
face avatar is, with `{"source": the camera, "filter": its name}`
(`FaceReact` throws and `stop`; `companion` and `stay` are a face
avatar's only; `FaceGetState` adds `camera`: `awake`, `face`, `seen`, `x`,
`y` and `head` (pixels), `roll`, `shake`). A face avatar that reads that
camera - to find your face, or to show it behind itself worn - always
gets it as it is, never with the throws on it.

**A companion beside it, and moves that stay (0.9.0).** A 3D character
can have a companion: a pet of its own file (`.glb`, `.gltf` or `.vrm`,
with its own animations) that comes in beside it - a wolf that trots in,
sits at its side, gets a headpat and trots off again - on the floor the
character stands on, sized to it (`size`: its height a share of the
character's), on the picture's left or right (`side`; its file faces the
way glTF says, and it's mirrored for the side it wasn't made for), a
share of the character's height across (`distance`) and towards the
camera (`forward`), turned towards it (`turn`, degrees), in the same
camera and light. Its animations are picked by name - `come`, `idle`
(played over and over while it's there), `pet`, `go` and `scared` - or
worked out from the usual words (arrive, enter; sit, loop, breathe; pat,
happy; leave, exit; flee, run, bolt); one with only one animation sits in
it. Its "come" ends where its idle starts, so it never jumps; asked to go
while it's coming, it goes once it's there. It stays as the character
changes (another file, another outfit) until it's sent off. `follow`
(0-1) is how much it moves with the character as your head moves it. A
headpat (`pet`) brings the character's 3D hand (`react_hand_3d`) down onto
the companion's node called `pat` (else its head) from the character's
side, while its `pet` animation plays. Any other of its own animations
plays once as a move of its own (`move`: a wolf's howl, say) while it's
there, eased in over its idle and back out, never while it's coming,
going or already in the middle of one. With a camera view of its own
(`view`) the camera glides out to it as it comes and back as it goes.
**Move it across the picture** (`spot_x`, -50 to 50%, to a tenth of a
percent or finer) shifts a 3D character across its spot, to leave room
beside it. A move that stays (`stay`) is
one of the character's own animations played once - riding up onto a
mount, say - that then stays in another of them (`idle`) over and over,
cross-faded where they meet, until it's ended (with a move of its own, or
straight back); its camera view glides to it while it stays, its
companion can go (or bolt, scared) as it starts, and what's framed stays
as the character stands, so a mount much wider than it isn't squeezed in.

**Parts a move brings out and puts away (0.10.0).** A part shown or
put away by its size (0.001 while it's away: a sword in the hand, a
wolf's dark lip as it howls) is part-size for a moment while its move
eases in or out over how the character stood. A bone its file marks
`"extras": {"direbot": {"switch": true}}` never is: its size track that
jumps (glTF's `STEP`) plays whole as soon as the move plays at all.
Bones not marked, and size tracks that don't jump, ease as before.

**Shapes a move plays (0.10.0).** A character's or a companion's
own animation can change its mesh's shapes (its morph targets: glTF's
`weights` track) - a throat that hugs an opening jaw, a chest that
swells - when the node holding the mesh is marked `"extras": {"direbot":
{"shapes": true}}`. A track that jumps (`STEP`) plays whole as soon as
the move plays at all; any other eases with the move as its bones do;
with nothing playing it the shapes are as the file has them. Hair
strands in the mesh follow (their own targets, as with expressions).
Not marked, such tracks aren't read, as before: a face's shapes are
moved by your face.

**A body held still while a move plays (0.10.1).** A 3D character's
spine and chest take some of your head's turn
(`body_follow`), its own slow sway and each breath's lean and rise - on
top of whatever a move does with them, so a hand the move puts on
something (holding on as it rides a mount, a hand on a sword's hilt)
drifts off it as you turn your head. A move can ask for its body to be
held still: its animation marked `"extras": {"direbot": {"hold_torso":
true}}` (or `1`, as Blender may write a ticked box). While it plays - as
much as it shows, so it eases in and out with the move - your head's
turn, the sway and the breath leave its spine and chest alone, its neck
and head take all of your head's turn, and its hands stay where the move
puts them. It works for a character's own animations (on cue, or the idle
a move that stays plays over and over: fading to one that isn't marked
lets go as it fades) and for a VRM animation file's. A move that isn't
marked never lets go of one that is: a wave while it rides keeps it held.
Not marked, its body follows, sways and breathes as before.

**Smoother into and out of a move that stays (0.10.1).** Its idle now
cross-fades in under its move while the move still
plays in full, and is all there before the move eases out, so riding up
onto a mount lands on the mount instead of dipping back towards standing
(0.9.0: a quarter of the way, halfway through), and riding off lands on
the ground instead of sliding back up and dropping at the end. Any
cross-fade between two of a character's own animations now eases what
only the old one moves back to how the character rests (its arms down by
its side), instead of holding it to the end and letting go at once.

**Glowing parts: fire, sparks and smoke (0.9.0).** A see-through part
(glTF's `BLEND`) whose material has `"extras": {"direbot": {"glow":
{"soft": 1.5, "ragged": 0}}}` is drawn as a soft glowing gas - a
dragon's fire breath, its sparks and smoke. Its light (its emissive
colour, times its emissive picture and its points' colour) is added
over what's behind it however see-through it is, and it hides what's
behind it only as much as its base colour's alpha says (soot a lot, a
white-hot flame hardly at all). Seen edge on it fades out (`soft`, a
power: 0 never, higher sooner); with `ragged` (0 to 1) the fading eats
into the thin places of its picture (its alpha) first, so its outline
breaks up into wisps and tongues instead of fading like a smooth ball.
Every point of it is drawn at the depth of its middle, so a puff never
cuts into itself or into a wall behind it, and the parts are drawn far
to near. It gets no outline and no lighting; a part that isn't `BLEND`
is drawn as it always was.

**Real hair, strand by strand (0.10.0).** A 3D
character's line parts (glTF's `LINES` or `LINE_STRIP`: a groom exported
as curves) are its hairs: each line a hair, as wide along its length as
its `_WIDTH` says (in the file's units; 0.1 mm without one), tinted by
its `COLOR_0`, held by its bones and moved by its expressions like the
rest of it, so a beard follows the jaw and the lips. Each hair is drawn
as a ribbon facing you, see-through at its edges and where it's thinner
than a pixel, so a coat's edge is soft instead of jagged. The hairs are
drawn back to front in layers (the hair on far bones first, and on each
bone the pieces near the root before the pieces near the tip), with the
inner layers of a share of the hairs standing in as a solid core, so
hair behind hair behind skin sorts itself out without the card sorting
every hair every frame.

It's made cheap as it loads and as it's drawn. As it loads, points that
hardly bend their hair are left out (a point stays where leaving it out
would move the hair more than 0.03% of the coat's size, turn it more
than 20 degrees, change its width or colour enough to see, let its bones
swing it wrong, or let an expression move it unlike the points beside
it), and a character keeps at most 2 million points (800,000 on Lighter;
where the CPU poses it, 150,000 and 60,000): a file with more has hairs
left out evenly all over, more of them where the coat is many hairs deep
and fewer where it's thin, and the ones kept are drawn that much wider
so the coat covers as much.

As it's drawn, a coat that's small on screen draws fewer, wider hairs (a
hair thinner than a pixel is drawn a pixel wide and as see-through as
it's thinner, and only as many are drawn as leaves each at least 40%
solid; 65% on Lighter), and no more than 1.5 million pieces of hair in a
frame (750,000 on Lighter). Hairs well under a pixel wide go to the card
as lines, which is the same picture from half the corners; a coat of
thin hairs with a few wide ones among them sends only its thin ones so.
Posed on the graphics card (Windows), hair that can't show behind a
solid part or deep under other hair is left out before the card cuts it
into pixels, and each form a part is drawn in lies in the card's memory
in the order it's drawn, so the card reads straight through it - made
off the drawing thread, and freed again when it hasn't been drawn from
for a while.

And the hair budget: the add-on times hair on the graphics card every
frame (the card's own clock), and when it takes more than 1 ms (0.5 ms on
Lighter) it draws fewer, wider hairs until it doesn't, down to a tenth
of them - so hair gives way before your game does. A new character
starts at a quarter and works up. On Full the budget aims just under its
limit and remembers where it went over, and thin hairs stay lines under
it. A companion's hair is drawn by its character's settings, and the two
share the one limit. (Measured on a Radeon RX 9070 XT, a wolf's coat of
436,000 hairs filling a 1080p picture: 1.3 ms with every hair, and
253,000 of them at the 1 ms limit.) How all of it works, what was
measured, what was tried and dropped, and how to measure it again:
[docs/hair.md](docs/hair.md).

`hair_detail` (in the source's settings, "Hair detail") picks `full`,
`light` ("Lighter": fewer, wider hairs, half the budget) or `today`
("Simple hair"). A character can carry simpler hair for the third
choice: parts whose material has `"extras": {"direbot": {"hair_standin":
true}}` (strips and pictures, as hair was made before) are hidden while
its strands are drawn, and drawn instead of them on Simple hair. A
character without any keeps its strands there, as on Lighter.

What it can't do yet: hair casts no shadows on itself or the body (so a
white coat is flatter than in Blender, and a braid's crossing locks show
through each other a little); where the CPU poses a character (OpenGL:
Linux and Mac) it keeps far fewer hairs than on Windows; the first time
a character's hair is drawn the picture waits a moment for it to be laid
out; and expressions move each strand part by 2,047 spots at most (hairs
close together that move alike share one).

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

Needs OBS 31.1.1 or newer, 64-bit Windows. OBS 33 works from 0.8.2:
OBS 33 took the picture loader the face avatar used out of OBS and put a
new one in, and Windows wouldn't load 0.8.1 at all in OBS 33. From 0.8.2
the add-on finds whichever loader the OBS it's in has, when it runs.

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
| `FaceGetState` `{source, points?}` | `camera`, `ready`, `showing`, `found`, `presence` (0-1), `set`, `setting`, `turn` (`yaw`, `pitch`, `roll` in degrees, since "Set"), `turned` (-1, 0, 1), `moves` (the 51 expressions, 0-1 since "Set", ARKit's names), `parts` (first eye, second eye, mouth: `x`, `y`, `w`, `h` shares of the camera's picture), `shown`, `holding`, `looks_per_second`, `ms`, `problem`; with `points`, the 478 `points` (`x`, `y` shares); with a 3D character, `model`: `file`, `loaded`, `loading`, `shown`, `kind` (`gltf`, `vrm0`, `vrm1`), `title`, `author`, `points`, `triangles`, `springs`, `animations` (its own animations: `name` each), `animation` (the one playing, or ""), `swinging_chain`, `arkit_parts`, `blinks` and `talks` (once it's rigged: whether its own blink and mouth shapes move its face enough to see, at least 0.1% of its height; tiny marker shapes don't count), `set_up` (it has a set-up), `upright_parts`, `cards` (your eyes and mouth drawn on it last frame, 0-3), `outfit_layers` (outfit pictures drawn on it last frame), `spots` (`left_eye`, `right_eye`, `mouth`: `x`, `y`, `w`, `h` shares of its picture from the front, `from` where it found them: `expressions`, `eye bones`, `set up`, `guessed`), `expressions` (VRM's, 0-1), `yaw`, `pitch`, `x`, `y` (its middle in the picture, pixels), `finger_joints` (the finger joints it curls), `body` (it has a spine or chest that breathes and follows your head), `following_bones` and `following_bones_left_out` (0.8.0: its bones that follow others, VRM's node constraints, and those its file has that can't be), `framed` (0.8.1: `w`, `h` - how wide and tall what's framed in its own spot is, in the model's units, once it's loaded), (0.10.0) `on_card` (it's posed on the graphics card) and `hair` (`detail` as set, `strands` in its file and `loaded` (kept), `standin` (it has simpler hair to wear instead), and for what's shown now `shown_strands`, `shown_loaded`, `drawn` (strands drawn last frame), `pieces`, `draws`, `share` (the hair budget's say, 0-1), `ms` (hair's time on the graphics card a frame; -1: no reading) and `budget_ms`), `problem` in words when it couldn't be read. Always (0.5.0): `tune`: `reading` and `shows` (each tune group, 0-1: what the camera reads and what the character shows from it) and `reading_peak`, `shows_peak` (the same, held at their highest for 0.6 s); `alive`: `idle` (0 following you to 1 idle), `breath` (0-1); `camera_check`: `seen`, `box` (`x`, `y`, `w`, `h`: your face in the camera's picture, shares of it), `facing` (`yaw`, `pitch`, `roll`: which way you face the camera itself, degrees, not since "Set"), `light` (`face`, `left`, `right` - each side of your face as the picture shows it - and `room`, the rest of the picture: 0 black to 1 white), `camera_width`, `camera_height`, and (0.9.0) `camera_fps` (how many new pictures a second the camera sends; 0: the same picture over and over, a photo). Always (0.6.0): `mic`: `from`, `mic` (as set), `found`, `listening_to` (the mic it hears), `hearing`, `muted`, `level` and `peak` (dB), `start`, `full`, `heard` (louder than the start), `voiced`, `talking`, `open` (0-1), `share` (how much of its mouth is the mic's, 0-1), `vowels` (`a`, `i`, `u`, `e`, `o`: 0-1), `f1`, `f2` (Hz), `own_voice` (a saved "say each vowel"), `calibration` (`running`, `vowel`, `number`, `listening`, `left` seconds, `done`, and once done `heard` per vowel and `own`); `lights`: `preset`, `side`, `height`, `colour`, `brightness`, `room_colour`, `room`, `rim_colour`, `rim`, `detail`, `soft_skin`; `camera_view`: `view` (the one asked for), `showing`, `gliding`, `views` (`name`, `yaw`, `pitch`, `zoom`, `x`, `y`), (0.9.0) `cue` (`companion` or `stay` while one has the camera, else ""); and in `model`: `materials` (`name`, `bumps`, `shadows`, `skin` each; 0.7.0: `own_tangents`, its bumps on the file's own tangents), `bump_parts`, `shadow_parts`, `skin_parts`, `guessed_tangent_parts` (0.7.0: parts with bumps whose file had no tangents, so they're worked out). Always (0.7.0): `react`: `faces` (the faces it makes: `name` each), `shapes` (its file's own shapes: `name` each), `arms` (it waves and cheers with them), `vrm_moves` (it plays VRM animation files), `moves` (each of `react_moves`: `name`, `length` seconds, `problem` in words when its file couldn't be read), `items` (each of `react_items`: `loaded`; 0.8.1: a 3D one `three_d`, and `problem` in words when it can't be read), `hand`, `hit` (their pictures loaded), `hits` (so far), `flying` (in the air now), `busy` (anything on); 0.8.1: `hand_3d` (the 3D hand loaded), `hand_3d_problem`, `emotes` (emote pictures kept open), `patting`, `stuck` (things stuck to it now), `squint`, `squish` (0-1, a headpat's or a bonk's), `hand_now` (while it's shown: `side`, `rise`, `away`, `lift`, `opacity`); 0.9.0: `moving` (one of its moves playing). Always (0.9.0): `companion`: `file`, `loaded`, `loading`, `problem`, `animations` (`name` each), `clips` (the animation it plays for each, "" none), `pat` (where a headpat lands: its node's name), `phase`, `shown`, `drawn`, `busy` (coming or going), `petting`, `patting`, `camera` (it has the camera now), `height` (as drawn, sitting, in the world's units), `moving` (its own move playing now, or ""); `stay`: `on`, `swapping` (its move playing into the one it stays in), `idle`. Always (0.8.0): `hands_and_arms`: `on`, `hands`, `light` (as set), `can` (it can follow them on this character now), `why_not` (`off`, `worn`, `no_3d`, `not_vrm`, `no_arms`, or ""), `loading`, `ready`, `problem` (the body models), `looks_per_second`, `ms`, `found` (a body), `following` (`left_arm`, `right_arm`, `left_hand`, `right_hand`: yours, followed now), `calibrating`, `calibrated`, `calibration` (`ok`, `no hand`, or "" - the last one's result), `framing` (`found`, and with a body: `head` (in the picture), `shoulders`, `elbows`, `wrists`, `hips` (how many, 0-2), `hands` (found), `shoulder_width` and `shoulder_height` (shares of the picture), and (0.9.0) `turned` (the body model had you turned round: put right)); (0.9.0) `most_per_second` (how often it looks at most: your hands 30, light 15; arms only 15, light 10), `body_per_second` (how often the body model ran), `palm_finder` (loaded), `hands_seen` (`left`, `right` - yours: `found`, 0-1, how often over the last few seconds, and once found `light` and `around`, its palm and just round it, 0 black to 1 white); with `points`, `points` (your top half's 25 points: `x`, `y` shares of the picture, `seen` 0-1) and `left_hand_points`, `right_hand_points` (21 each, when found). Always (0.9.0): `face_from` (`camera`, `phone` or `vmc`), `phone`: `on` (set to ask a phone), `listening` (its socket is open), `way` (`vtube_studio` or `ifacialmocap`), `address` (as set), `phone` (where it asks, `host:port`), `port` (ours), `problem` (in words, or ""), `packets` (read since it started), `per_second`, `since` (seconds since the last one; -1: none yet), `found` (the last one saw your face), `shapes` (how many of the 52 it sent), `others` and `other` (packets from another address, and the last one), `unread` (from the phone, but not readable); `looking_hidden` (it's looking at the camera while it's hidden: `signs_hidden`), and `signs`: `on`, `rules` (how many), `watching` (looking now), `hands_looked_at`, `hidden`, `looking_hidden`, `hands` (`left`, `right` - yours: `found`, `sign` (the one it holds, or null), `sure` 0-1, `raised`, `waving`), `face_found`, `face` (`mouth_open`, `brows_up`, `smile`, `kiss`, `wink`, `wink_left`, `wink_right`: 0-1), `sent` (how many it's sent), `last` (the last one's id); `vmc` (another tracker app): `on` (listening for one: its face, body or arms is set to come from it), `face`, `body`, `arms`, `mirror` (as set), `listening` (its socket is open), `port`, `problem` (in words, or ""), `packets` (read since it started), `per_second`, `since` (seconds since the last one; -1: none yet), `from` (where the app sending is: "this PC" or its address), `others` and `other` (packets from another app while it sends, and where), `unread` (not OSC), `loaded` and `calibration` (as the app says: 1 loaded, 3 calibrated; -1 when it hasn't), `sends` (lately: `bones` (how many), `body`, `legs`, `head`, `eyes`, `arms`, `fingers`, `shapes` (how many), `perfect_sync` (ARKit's)), `using` (taken now: `face`, `body`, `arms`, `fingers`), `why_not` (its bones can't be taken: `worn`, `no_3d`, `not_vrm`, `no_bones`, or ""); `gaming` (gaming hands): `on`, `device` (`auto`, `keyboard`, `pad`), `mouse_hand`, `props`, `rest` (as set), `reading` (`keyboard`, `mouse`, `pads`: read on this PC, each with its `_problem` in words when it isn't; `pads_in`: how many are plugged in), `shown` (its hands are on its desk now), `why_not` (`off`, `worn`, `no_3d`, `not_vrm`, `no_arms`, or ""), `using` (`keyboard_mouse`, `pad` or `none`), `typing`, `hands_on` (0-1), `used` (`keys`, `mouse`, `pad`: each used in the last second), `show_me`, `show_left` (seconds), `props_loaded`, `props_wanted`, `props_problem` - never which keys |
| `FaceMicCalibrate` `{source, do}` | "Say each vowel": `do` `start` (it asks for each vowel in turn - follow `mic.calibration` in `FaceGetState` - and saves your voice's places in `mic_profile` when it's done), `stop`, or `forget` (back to the usual places). `ok`, or `error` in words (no mic found, or its mouth is from the camera only) |
| `FaceReact` `{source, do, ...}` | what it does on cue. `do`: `face` (`face`, its name in any case, "" eases the one on off; `seconds`, up to 60; `amount` 0-1) answers `face` (its own name for it), `shows`, and `why` in words when it won't show (not a 3D character, or no face called that); `move` (`move`: `nod`, `shake`, `tilt`, `bounce`, `wave`, `cheer`, or a `react_moves` name or one of its file's own animations, any case) answers `kind` (`head`, `arms`, `file`, `own`) and `shows` (false for `wave` and `cheer` without a VRM's arms, and for a `react_moves` file's move on a character that isn't a VRM); `throw` (`item`, the number in `react_items` from 0, or (0.8.1) `file`, a picture's full path - one of chat's emotes; (0.8.1) `stick`, seconds stuck where it hits, up to 10 (0 bounces off); `from`: `above` a bonk on top of its head, else from a side; `count`, 1-16, a barrage of it; `side` -1 its left, 1 its right, 0 either; `strength` 0.1-2; `size` 0.2-3 (1: a little under half its head); `face` and `face_seconds` (up to 10), the face it makes when it's hit) answers `hits_in` (seconds until it hits, for the hit's sound) and (0.8.1) `thrown` (how many of `count` were taken: never more than 16 waiting) and `every` (seconds between them); `pat` (`count`, 1-20 pats; `size` 0.3-3 (1: about its head's width); `face`, "happy" unless given, "" none; (0.8.1) `side`, -1 from the picture's left, 1 its right, 0 either at random); `stop` (everything eased off quickly, nothing waiting); (0.9.0) `companion` (`action`: `come` - `file`, `clips` (`come`, `idle`, `pet`, `go`, `scared`: its animations by name, "" worked out), `place` (`size` 0.05-3, `side` -1 the picture's left or 1 its right, `made_for` the side its file is made for, `distance` and `forward` -3 to 3, `turn` -180 to 180), `follow` 0-1, `view` (`yaw`, `pitch`, `zoom`, `x`, `y`, or none), `instant` - answers `came`, `loading` (its file still loading: it comes once it's loaded), `phase` (`away`, `coming`, `out`, `going`) and `why` when the character isn't 3D; `place`, the same with nothing coming or going; `go` (`scared`, `instant`) answers `went` and `phase`; `pet` (`count`, `size`) answers `pets`, and `why` when it isn't there or is coming or going; `move` (`move`: one of its own animations by name, any case) answers `moves`, `length` (seconds) and `why` in words when it won't (it isn't there, its file is still loading, it's coming or going, or it's in the middle of another move) - `error` when it has no animation of that name; `off`, gone at once, its file let go of); (0.9.0) `stay` (`action` `start`: `move` (one of its own animations, "" none), `idle` (the one it stays in), `view` (or none), `companion` (`go` or `scared`: its companion goes as it starts); `end`: `move` ("" none) - answers `companion` (its phase) when it sent it off; `error` when the character isn't 3D or has no animation of that name). `ok`, or `error` in words |
| `FaceGame` `{source, do}` | Gaming hands (0.9.0): `do` `show` (14 seconds pretending: typing, then a game on the keyboard and mouse, then on a pad - follow `gaming.show_me` and `show_left` in `FaceGetState`) answers `seconds`; `stop` ends it. `ok`, or `error` in words (not on, not a 3D VRM in its own spot, no arms, just starting, or the avatar not shown) |
| `FaceArms` `{source, do}` | Your hands and arms (0.8.0): `do` `calibrate` (hold a hand up by your ear, open, palm to the camera; follow `hands_and_arms.calibrating` in `FaceGetState` - it's saved in `arms_calibration` when it's done), `stop`, or `forget`. `ok`, or `error` in words (not on, your hands not followed, not a 3D VRM in its own spot, the body models not ready, or the avatar not shown) |
| (vendor event) `FaceSign` | 0.9.0: a sign it watches for was held its time: `source`, `id` (the rule's), `what`, `side` (the hand that made it: `left`, `right`, `both`; a wink's eye; "" for the other face moves) |
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
    DIREBOT_OBS_PLUGIN=/tmp/b/direbot-for-obs.so pytest tests/test_real_obs.py tests/test_real_face.py tests/test_real_hair.py

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
photos and its hands on two more (`tests/data/body`);
`tools/body-expected.py` makes those (`pip
install mediapipe ai-edge-litert`). Inside OBS, `test_real_face.py` has a
VRM character follow the arms in one of those photos, a bone copying its
arm's turn going round with it.

Your face from a phone: `tests/unit_phone.cpp` reads each app's packets
(made up from the apps' published formats) and a pretend phone on the
same PC; inside OBS, `test_real_face.py` (and on Windows
`test_real_windows.py`) has a pretend phone of each kind send a face to
a face avatar, which follows its turn and its wink.

Moves from another tracker app: `tests/unit_vmc.cpp` reads OSC (every
type, bundles in bundles, packets cut short and random bytes), the bones
by Unity's names, its turns made ours (checked against sums worked out by
hand in Unity's own terms), its face (perfect sync, VRM 0.x and 1.0's
names, eye bones, its head's turn) through the tracker, and the link
with pretend apps on the same PC; `tests/unit_model.cpp` has a VRM take
an app's bones (and a move on cue over them, its hips, your head against
its chest). Inside OBS, `test_real_face.py` has a pretend app (30 frames a
second, as XR Animator sends them) move a VRM's arm and hips and send its
face; on Windows, `test_real_windows.py` has one send a face over
Windows' own sockets, two avatars share its port, and a port another app
has is said.

Real hair: `tests/unit_hair.cpp` loads made-up grooms (coats thick and
thin, curls, hooks, hairs held by bones) and checks which points and
strands are kept and why, that parts thin alike, the draw order's groups
and layers, how many are drawn for a size on screen, the budget against
a made-up clock, and expressions strand by strand. Inside OBS,
`test_real_hair.py` has a character with strands drawn at each hair
detail (its simpler hair on "Simple hair"), hidden behind solid parts
and a 3D hand, smooth where its hairs are thin, fewer the smaller it is
and the smaller its budget, moved by its expression and its bones, on a
companion too - and the empty pixels of a see-through part hiding
nothing behind them.

Tests (Windows, in GitHub Actions): the Windows zip goes into a real OBS
for Windows - 31.1.1, 32.2.2 and 33.0.1 - which has to load it and open
a face avatar's pictures, a PNG and an animated GIF
(`tests/test_real_windows.py`), and draw strand hair posed on the
graphics card the same as posed by the CPU. Before that, `tools/check-obs-imports.py`
checks every function the DLL calls is in each OBS's own `obs.dll` and
`obs-frontend-api.dll` (one missing and Windows won't load the add-on).
When a new OBS comes out, add it to the list in `.github/workflows/build.yaml`.

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
                          its body tracker and arm solver (body, arms),
                          its hand signs and face moves (signs), your
                          face from a phone (phone), moves from
                          another tracker app over VMC (vmc), and
                          gaming hands (devices: your keyboard, mouse
                          and pads counted; gamehands: its hands on its
                          own)
    src/face/nn/          our own engine for the face and body models
    data/effects/         the shaders the transition, the filter and the face avatar draw with
    data/face/            Google's MediaPipe face, body and hand models (Apache 2.0, see its NOTICE)
    data/face/gaming/     gaming hands' keyboard, mouse, mat and pad (our own, made by tools/make-gaming-props.py)
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
`data/face/holistic_landmarker.task` its Holistic Landmarker,
`data/face/hand_landmark_lite.tflite` its lighter hand model and
`data/face/palm_detection_full.tflite` its palm finder (the Hand
Landmarker's own), all
unchanged: Copyright The MediaPipe Authors, under the Apache License 2.0
(`data/face/LICENSE`, `data/face/NOTICE`).

Following your arms and fingers is our own code; its maths (each part of
an arm pointing where yours does, a hand turned to your palm's way, each
finger joint bent as far as the angle between your finger's bones) follow
Kalidokit's (yeemachine, MIT licence) as a recipe, with no code
copied.

Your face from a phone is our own code. The apps' formats are as
DenchiSoft's VTubeStudioBlendshapeUDPReceiverTest (MIT licence) and
iFacialMocap's own notes give them; how their turns, places and sides
compare to ours was checked against Inochi2D's facetrack-d (BSD
licence), xuan25's VTube-IFacial-Link and DrBomb's ExpressionAppBridge
(MIT licence), with no code copied.

Moves from another tracker app (VMC) are our own code, OSC reader and
all. How a VMC receiver takes them - Unity's bone names, its left-handed
turns, Blend/Val held until Blend/Apply - follows the VMC protocol's own
page and gpsnmeajp's EVMC4U (a VMC receiver for Unity, MIT licence) as a
recipe, with no code copied.

Gaming hands are our own code, props and all. How a character's hands
go on a keyboard, mouse and pad - its hands gliding from one to the other,
a finger down for each key, a typing hand wandering a little, its mouse
hand following yours, its pad bumped about by what's pressed - follows
VMagicMirror's (Baxter, MIT licence) as a recipe, with no code copied.

`deps/mikktspace` is Morten S. Mikkelsen's MikkTSpace, unchanged, under
the zlib licence (in its own files and `deps/mikktspace/README.md`): it
works out a character's tangents when its file has none.
