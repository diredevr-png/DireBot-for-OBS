# 3D things thrown: how they're drawn, what they cost, and how that was found

This is the working record of how **DireBot Face Avatar** draws something 3D that isn't a character (0.12.0): a
thing thrown at you and the pieces it throws off, a headpat's hand, gaming hands' keyboard, mouse and pad. Read it
before changing `src/face/prop-mesh.hpp`, `Prop3D` and `Avatar3D::warm` in `src/face/avatar3d.cpp`, the `Thing`
techniques of `data/effects/direbot-face-3d.effect`, or `warm_things` in `src/face/face-avatar.cpp`.

0.12.0 added twenty 3D things to throw (81 files with their pieces: 481,055 points, 929,200 triangles, almost no
pictures). The first build of it drew them right and cost too much: OBS held about 150 MB more with them loaded and
190 MB more once each had been thrown, the first throw stuttered, and sixteen thrown at once took OBS's drawing
from 21 ms a frame to 116 ms (software drawing; the table is in section 6). This is what was wrong and what the
code does now.

## 1. What was wrong

1. **Every point of every thing in the air was moved by the CPU, every frame.** `Prop3D::place` added the thing's
   shapes, turned each point, facing and tangent to where the thing was, and sent the lot to the graphics card
   again (`gs_vertexbuffer_flush_direct`): about 0.3 ms and 1.2 MB a frame for one pizza, for each place it was
   drawn in. Sixteen in the air: about 5 ms of one core and 20 MB sent, a frame.
2. **Each thing was kept four or five times over.** The file as read (its points, its triangles with four bytes a
   corner, its shapes point by point), `Prop3D`'s own copy of all of that to pose from, the buffers handed to OBS
   (which keeps them in memory too, to make them again if the card is lost), and the card's copy. Every picture a
   thing throws off (splats, drips, mist: 41 of them) also kept a small copy in memory that only a character's
   picture needs.
3. **The first throw paid for everything a graphics card does the first time.** With a picture for a character the
   3D effect was only read and built when the first 3D thing was drawn; and each thing's first draw has its own
   first-time work (its points and pictures made ready, drawing code built for its kind of part).

## 2. What's kept of a thing's file (`prop-mesh.hpp`)

The loader thread reads the file (`load_model`), makes a `PropMesh` of it and lets the file as read go at once.
A `PropMesh` is exactly what goes to the card, nothing more:

| of each part | bytes a point | what |
|---|---|---|
| where it is, its facing | 16 + 16 | as its file has them: where its node puts it (a bound part: its strongest bone, as it was bound). Four numbers each, as OBS has them |
| its colour | 4 | sRGB bytes |
| its UVs | 8 | |
| its tangent | 16 | only a part whose material has a normal map |
| its shapes | 48 | only a part that a kept shape moves: the file's first two shapes (`PROP_SHAPES`), how far each moves the point and turns its facing, packed into three fours |
| its triangles | 2 a corner | 4 for a part of more than 65,536 points |

Hair strands in a thing aren't drawn and aren't kept. Only the first two shapes are kept because nothing uses more:
a thing thrown has "wobble" and "wobble back", a piece has "open", the hand has "lift". `Prop3D::shape` finds a
shape by its name among those kept, and one beyond them reads as not there.

`Prop3D` (graphics thread) hands a `PropMesh`'s numbers to OBS once, in buffers that never change, and keeps
nothing itself but the buffers, the materials and a few numbers. `Prop3D::points_sent()` counts every point ever
sent: it goes up when a thing is loaded and must never go up while things are thrown (the tests look).

## 3. How it's drawn: the card puts it where it is

A draw of a thing is now a few numbers:

- its place goes onto OBS's matrix stack (`gs_matrix_mul`), so the effect's `ViewProj` puts its points, and into
  `cam` (the view times its place), which takes them to the camera's view for the light;
- `turn` takes its facings to the view: the inverse of its place turned rows for columns, times the view's turn
  (`prop_turn`). That is right for a thing squashed as it hits or mirrored, where its place alone would light it
  wrong. The vertex stage makes each facing length 1 again, as the CPU did;
- `thing` carries how much of its two shapes show, and -1 for a thing its place mirrors (`prop_handed`): its
  tangents' which-way-up turns over and the other side of each triangle is left out.

The techniques (`direbot-face-3d.effect`; the `-skin` effect Windows uses includes that file, so it has them too):

| technique | for |
|---|---|
| `Thing`, `ThingBumped` | a part as `Draw` and `DrawBumped` draw a character's |
| `ThingDepth` | its solid parts' distances, for the hairs behind it |
| `ThingShaped`, `ThingBumpedShaped`, `ThingDepthShaped` | the same for a part with shapes, while any of its shapes shows: the shapes are added in the vertex stage, from three more fours a point (`TEXCOORD1` to `3`) |

A shaped part is drawn through the plain techniques whenever no shape shows (most frames), so its shapes' numbers
aren't even read. The same thing can be in any number of places in one frame, each with its own shapes, for no
work a point on the CPU. Everything else is as it was: a fading thing shows only its front (its distances laid
first, then drawn where it's nearest), a glowing part is all drawn at its middle's depth, see-through parts go far
to near with the hair.

The sums the card does are the CPU's old sums, number for number. `tests/unit_prop.cpp` holds both (the old
`Prop3D::place` as it was, and the effect's sums written in C++) and compares them for a thing as made, turned and
sized, squashed, mirrored, and with its shapes showing.

## 4. The first throw: warmed as it's loaded

`FaceAvatar::warm_things` (graphics thread) draws each newly loaded thing once, one a frame, into a 32 x 32
picture nobody sees (`Avatar3D::warm`): solid, see-through, fading, its distances, and with its shapes showing if
it has any. The scene that draws things over the character (`overlay3d`) is made for this as soon as there is a 3D
thing, so the 3D effect is read and built then (a long frame, once in an OBS session, where the character is a
picture; a 3D character has built it already). If the look changes (flat colours and back), they're all warmed
again, because some parts are then drawn by other code.

`DIREBOT_FACE_NO_WARM=1` in OBS's environment switches the warming off, to measure what it saves.

## 5. Memory

- Per thing: what section 2 lists, twice (the card's copy, and OBS's own in memory). No copy of ours.
- A picture that's only ever drawn (a thing thrown, a splat, a drip, a hand) keeps no small copy; a character's
  picture and its outfit layers do (`Picture`'s `shape`), because their shape is read from it.
- Things over the character are drawn into pictures of their own, twice the source's size each way while smooth
  edges are on (a 1920 x 1080 source: about 66 MB on the card for colours and depth, and 8 MB more for the halved
  one). They used to be kept from the first throw on. Now they're let go of 30 seconds after the last thing was
  drawn (`OVERLAY_KEEP_NS`) and made again at the next throw. Whether the card's memory goes down then is the
  driver's business (section 6).

`FaceGetState`'s `react.things` says what's held: `loaded` (things on the card), `points`, `triangles`, `bytes`
(the card's, not counting pictures), `warm` (warming is on), `warmed`, `to_warm`, `drawn_into_bytes` (the pictures
things are drawn into, now) and `points_sent`.

## 6. Numbers

**Software drawing** (OBS 30.2 on Linux with Mesa's llvmpipe, 1280 x 720 at 30 fps, a 600 x 600 face avatar with a
picture for a character, DireBot's own twenty things sent as DireBot sends them). Here the "graphics card" is the
CPU and its memory is ordinary memory, so every drawing cost is exaggerated and the card's copy of everything
counts as OBS's memory:

| | 0.11.0 (its six things are pictures) | 0.12.0 as first built | now | now, warming off |
|---|---|---|---|---|
| OBS's memory with no things | 378 to 382 MB | 377 to 378 MB | 380 to 383 MB | 384 MB |
| with the things loaded | 390 to 393 MB | 527 to 536 MB | 494 to 504 MB | 468 MB |
| after each has been thrown | 393 to 397 MB | 569 to 578 MB | 496 to 514 MB | 514 MB |
| loading them | 0.11 s | 0.34 to 0.43 s | 1.3 to 1.4 s | 0.44 s |
| the first throw: OBS's frame time at its worst, frames skipped | 18.7 ms, 0 | 42.4 ms, 22 | 29.6 ms, 4 | 57.4 ms, 29 |
| the other nineteen things, each thrown once: frames skipped in all | 1 | 15 | 22 | 20 |
| sixteen of one thing at once: frame time at its worst, frames skipped | 21.5 ms, 0 | 116.3 ms, 71 | 121.8 ms, 74 | |
| sixteen different things at once | 20.3 ms, 0 | 98.1 ms, 71 | 116.6 ms, 79 | |

(Frame time: obs-websocket's `averageFrameRenderTime`, OBS's own running average, at its highest while the throw
was on. With nothing thrown it reads 15 to 16.7 ms in every column.)

What this table shows and doesn't:

- **Memory** is down by 30 to 70 MB with the things loaded and 55 to 80 MB after throwing them, of the 150 and 190
  MB they added. Much of what's left here is the card's own copy, which a real card holds in its own memory, and
  the 3D effect itself (built at the first throw before, as the things are loaded now: that is why "loaded" reads
  higher with warming on than off, and the same after).
- **The first throw** no longer waits for the 3D effect to be built and for each thing's first draw: 4 frames
  skipped for 22. The same work now makes loading take about a second longer, once.
- **Frame time while things are in the air didn't change, and can't here.** Software drawing does the card's work
  on the CPU, a point at a time, whoever asked for it: sixteen pizzas are 416,000 points a frame through Mesa's
  vertex stage either way, about 100 ms, and what `Prop3D::place` used to add to that (about 5 ms) is within the
  run-to-run spread. A real card does those points in well under a millisecond. So the saving of section 1's
  first point only shows on a real card.

**On a real card** (OBS 33.0.1 on Windows, Direct3D 11, an AMD Radeon RX 9070 XT, 1920 x 1080 at 60 fps, a
1920 x 1080 face avatar, the same twenty things; each column one fresh OBS). The frame times are OBS's own
(`averageFrameRenderTime`): in the middle of the stretch, and at its worst. "A storm" is eight pizzas thrown
every half second for twelve seconds. Memory is what the things added to OBS over the same OBS with none (OBS
itself held between 4.4 and 8.3 GB here: its scenes had a 3D character with hair in another source):
its private bytes, and its memory on the card.

| | 0.12.0 as first built | now | now, warming off |
|---|---|---|---|
| **a picture for a character** | | | |
| frame time with nothing thrown | 0.40 ms | 0.37 ms | 0.42 ms |
| sixteen pizzas at once | 1.47 ms, 3.01 ms | 0.45 ms, 0.56 ms | 0.52 ms, 0.62 ms |
| sixteen different things at once | 1.26 ms, 2.18 ms | 0.84 ms, 1.22 ms | 0.80 ms, 1.17 ms |
| a storm | 4.46 ms, 5.07 ms | 0.76 ms, 0.82 ms | 0.76 ms, 0.82 ms |
| OBS's share of a CPU in the storm, as it says | 1.63% | 0.99% | 1.00% |
| the first throw: frame time at its worst, frames skipped | 0.53 ms, 0 | 0.47 ms, 0 | 0.47 ms, 0 |
| the other nineteen, each thrown once | 0.65 ms, 0 | 0.54 ms, 0 | 0.55 ms, 0 |
| loading them; all warmed; frames skipped meanwhile | 0.32 s; -; 5 | 0.36 s; 1.44 s; 3 | 0.36 s; -; 4 |
| memory added with the things loaded: private, on the card | 221 MB, 96 MB | 133 MB, 57 MB | 119 MB, 56 MB |
| after each has been thrown | 300 MB, 180 MB | 207 MB, 139 MB | 196 MB, 138 MB |
| after the storm | 338 MB, 209 MB | 228 MB, 148 MB | 217 MB, 147 MB |
| **a 3D character** (12.8 ms a frame by itself) | | | |
| sixteen pizzas at once | 13.47 ms, 15.63 ms | 13.46 ms, 14.12 ms | |
| a storm, frames skipped | 16.51 ms, 17.06 ms, 7 | 12.98 ms, 13.64 ms, 0 | |
| memory added with the things loaded | 209 MB, 96 MB | 126 MB, 58 MB | |
| after the storm | 330 MB, 207 MB | 218 MB, 150 MB | |

What it shows:

- **Things in the air cost a real card almost nothing now.** A storm took OBS from 0.4 ms a frame to 4.5 ms
  before, and takes it to 0.8 ms now: what's left of `Prop3D::place` is gone, and the points are no longer sent
  each frame. Beside a 3D character that already takes most of a 60 fps frame, that was the difference between
  seven frames skipped in the storm and none.
- **Memory**: the things cost about 40% less loaded and a third less after throwing, in OBS's own memory and on
  the card. `points_sent` stood still through every throw.
- **The first throw didn't stutter on this card, before or after** (no frame skipped; the worst frame 0.53 ms
  against 0.47 ms). The warming is for slower cards and software drawing, where the first throw skipped 22
  frames. Here it costs a second more until the last thing is warmed, about 13 MB, and no frames.
- **Letting go of the pictures things are drawn into** (71 MB for this source, section 5): the add-on let go of
  them half a minute after the last throw (`drawn_into_bytes` 0), but Windows' counters for OBS didn't move in
  the ten seconds after, in its own memory or on the card. It isn't the add-on or OBS holding on: with
  Direct3D's software driver (GitHub's Windows machines) OBS's private bytes go up by those 71 MB while the
  pictures are there and come all the way back when they're let go of (258.7, 330.0, 258.7 MB;
  `tests/test_real_windows.py` checks it), and in software drawing on Linux the same. So on this card the driver
  keeps what's freed, for a while or to use again. How long wasn't measured.

## 7. How to measure it again

- `react.things` (above) while throwing: `points_sent` must stand still.
- OBS's own numbers through obs-websocket's `GetStats`: `averageFrameRenderTime` and `renderSkippedFrames`, with
  nothing thrown, as one thing is thrown for the first time, and with sixteen at once (`FaceReact`'s `count`).
- OBS's memory: the process's own (private bytes on Windows), with no things, with them loaded, and after each
  has been thrown; on a real card also the card's memory for the process.
- **That a change draws the same picture:** build the old and the new add-on with a clock for throws that only
  steps when told (in `tick`, where `react.step` is called: fixed steps of 1/60 s, as many as a file says), throw
  each thing in both, stop both at the same steps and compare the screenshots. Two runs of one build come out the
  same to the last bit that way, so any difference is the change. That is how the move to the card was checked:
  180 pictures (twenty things, nine moments each), 30.7 million drawn pixels, 21 of them differing by more than 8
  in 255 and none by more than 96 (edges, where the card's sums round differently from the CPU's). The clock is a
  test-only change and isn't in the code.
- `tests/test_real_throws.py` (`things_on_the_card`) runs in a real OBS: sent once, warmed, shapes on the card
  (whichever of the two a shape is), the pictures let go of and made again. `tests/test_real_windows.py` runs the
  same on Direct3D, and looks at OBS's memory while a full-screen source's pictures are there and once they're
  let go of.

## 8. Not done, and why

- **Things shared between sources.** A face avatar and the "DireBot Throws" filter on a camera each load the
  things DireBot sends them, so a setup with both holds them twice. One copy on the card could serve both
  (`Prop3D` would split into what's on the card and where this source has it).
- **More than two shapes on the card.** Nothing has them. A third would be another four numbers a point and a
  wider `thing`.
- **Smaller pictures for things over the character.** They're as big as the source however small the thing is.
  Drawing only the part of the picture a thing covers would save the card clearing and halving the rest.
