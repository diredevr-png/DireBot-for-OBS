# Hair, strand by strand: how it's drawn, what it costs, and how that was found

This is the working record of the strand hair in **DireBot Face Avatar** (0.10.0): what the
code does now, the numbers it was built on, what was tried and dropped and why, and how to measure it again. Read it
before changing anything in `src/face/hair.hpp`, the strand parts of `src/face/model.cpp` and
`src/face/avatar3d.cpp`, or the strand techniques of `data/effects/direbot-face-3d*.effect`.

Every number called *measured* here was read on a real card: a Radeon RX 9070 XT, OBS 33.0.1 on Windows 11, a
1920 x 1080 picture, the character filling it, no game running unless it says so. Two test characters carry most of
them:

- **the wolf**: a white coat of thin hairs with a few wide ones among them. 660,157 strands in the file, 450,271
  kept (the 2 million point cap), 435,836 drawn with no limit (the cap of 1.5 million pieces a frame). Its colours
  carry a soft shading baked in as a stand-in, because hair casts no shadow yet;
- **the beard**: a man's head with a black beard and a braid, drawn head and shoulders. 66,820 strands in the file,
  50,590 kept, 39,559 drawn with no limit (the same cap of pieces: its hairs are long, 38 pieces each on average
  against the wolf's 3.5).

Anything worked out and not measured is called an estimate.

## 1. What the file has to hold

A glTF or VRM character's line parts are its hair (`model.cpp`, `open_strands`):

| in the file | what it is |
|---|---|
| a primitive with `mode` 1 (`LINES`: pairs of points) or 3 (`LINE_STRIP`), with or without `indices` | each connected line is one hair, root first |
| `_WIDTH` (one float a point) | the hair's width there, in the file's units (metres). None: 0.1 mm |
| `COLOR_0` (3 or 4 numbers a point, linear as glTF says) | its tint; the fourth is how see-through |
| `JOINTS_0` + `WEIGHTS_0` | its bones, as any skinned part. Every point is posed by these and nothing else |
| the mesh's morph targets (`POSITION`, sparse or not) and `extras.direbot.strand_morphs` on the primitive (either or both: their moves add up) | how expressions move it. The second form is per strand: `strand_start` (an accessor: each strand's first point, one more entry than strands) and `targets` (one per morph target: `strands`, the rising list of strands it moves, and `POSITION`, one move each) |
| a material whose `extras` hold `{"direbot": {"hair_standin": true}}` on an ordinary part | stand-in hair: strips or pictures drawn *instead* of the strands on the "Simple hair" setting, and hidden while strands are drawn |

Limits when reading: 20 million hair points in a file; a strand part's expressions move it by at most 2,047 spots
(points close together that move alike share one: `Strands::MOST_ANCHORS`); a character posed on the graphics card
has at most 255 joints.

## 2. What loading does to it

All in `model.cpp` (`LoadOptions` in `model.hpp` holds the numbers; `hair_load_options` in `hair.hpp` picks them for
a setting).

1. **Points that hardly bend a hair are left out** (`shape_strand`). A point stays if leaving it out would move the
   hair more than 0.03% of the character's size (`strand_bend`), turn it more than 20 degrees (`strand_turn`),
   change its width by a tenth of its widest or its colour by a visible shade (more than 6 of 255), or stop its
   bones swinging it in a curve (`strand_swing`). It also stays if an expression moves it further than that same
   0.03% from where the kept points beside it would carry it: a hair pressed aside in its middle by a shape keeps
   the pressed point and the points either side of it. (Moves given a strand at a time move a whole hair alike and
   keep nothing more.)
2. **A cap on points.** 2 million on Full, 800,000 on Lighter, posed on the card; 150,000 and 60,000 where the CPU
   poses it (OpenGL). A file with more has hairs left out evenly all over (`spread_order`), more of them where the
   coat is many hairs deep (`strand_cover`, `strand_uneven`), and each kept hair is drawn that much wider
   (`Strands::rate`, `full`, `mean_width`) so the coat covers as much.
3. **A draw order that never needs sorting hair by hair.** Strands are grouped by the bone they hang from
   (`Strands::groups`); each group's pieces (a piece: one point to the next) are split into 5 layers by how far they
   are from their roots (`layers`); a run is one group's one layer (`runs`), its pieces by strand so that "the first
   N strands" is always the start of every run. Each frame the groups are ordered far to near and each group's
   layers drawn roots first.
4. **Two sorts.** A coat of many thin hairs and a few wide ones keeps each run's wide pieces first and its thin ones
   after (`Strands::thin`, when at least half its pieces are thin: `THIN_MOST`, a piece being thin when no wider
   than 1.5 times the width half the coat's length is under: `THIN_WIDTH`), so the thin ones can go to the card as
   lines while the wide ones stay ribbons.
5. **How wide a pixel must be for lines to be safe** is worked out once (`line_pixel`, `line_pixel_most`,
   `line_pixel_at`, and the same for the thin sort).

## 3. What drawing does each frame

In `avatar3d.cpp` (`draw_strands`, `draw_hull`, the form loop before them) with the rules in `hair.hpp`.

- **Ribbons.** A piece is two triangles facing the camera, see-through at its edges. A hair thinner than a pixel is
  drawn a pixel wide and as see-through as it is thinner.
- **Fewer, wider hairs when small** (`hair_share`). Only as many strands are drawn as leaves each at least 40% solid
  (65% on Lighter: `HairPlan::solid`); the ones drawn are widened so the coat covers the same.
- **Lines** (`hair_form`). A line is a pixel wide, so it can stand for a hair up to about 0.7 to 1 pixel wide and
  costs the card half the corners. A part goes as lines when that loses under 1% of what its hairs cover, and stays
  lines until a pixel is a tenth smaller (so it does not flicker at the edge). A part in two sorts sends only its
  thin pieces as lines by the same rule.
- **The hull** (`strand_hull`, `draw_hull`). The inner layers of a quarter of the strands drawn, each as much wider,
  are drawn into the depth picture of the solid parts. Hair well behind other hair is hidden by it, which is what
  lets layers stand in for true sorting.
- **Hidden hair left out** (`HairPlan::skip_hidden`; Direct3D, posed on the card). The vertex stage reads that depth
  picture (`card-picture.cpp` binds it there) and drops a piece whose two ends are both hidden.
- **Laid out as it's drawn** (`HairPlan::layout` 1; posed on the card). Each form a part is drawn in (a sort as
  ribbons or as lines, and the hull) gets its own vertex buffer with the corners in the order the pieces go to the
  card, so the card reads straight through its memory (`lay_strands`, `lay_hull`, `strand_layout` in `hair.hpp`).
  A form is made off the drawing thread (`Avatar3D::LaidJob`); the drawing waits for one only the first time a
  part's hair is drawn, when there is nothing else to draw it from. While a new form is being made the part goes on
  in the form it had. A form not drawn from for 1,800 frames is freed (`free_idle`, `HairPlan::idle`), and the
  by-point copy of the hair is not kept on the card at all.
- **A cap of 1.5 million pieces a frame** (750,000 on Lighter), all the hair in the picture together.
- **The budget** (`HairBudget`). Hair's time on the card is read every frame from the card's own clock (a GPU timer
  query). Over the limit (1 ms on Full, 0.5 ms on Lighter) fewer, wider hairs are drawn, never under a tenth of
  them. A new character starts at a quarter and works up. A clock that gives no reading for 120 frames counts as
  no clock: every hair is drawn.
  - *The first rule* (`budget_rule` 0) holds anywhere from three quarters of the limit up to it.
  - *The other rule* (`budget_rule` 1, Full's) climbs while hair takes under 0.9 of the limit, aims at 0.95, and
    remembers the share it went over at so a jump in cost is found once and not crossed again and again.
  - *Lines kept* (`line_loss`, 0.4 on Full: `HAIR_LINE_LOSS_KEPT`). Under a budget the hairs drawn are wider, and a
    line cannot be wider. The first way turned a coat back into ribbons as soon as its lines lost a twentieth of
    what they cover; Full now keeps them lines until they would lose 40%.
- **One limit for everything in the picture.** A companion beside a character is drawn in the character's picture
  and by its settings (`draw_strands` and `draw_hull` take the owner), and the two share the limit.

The settings a person sees (`hair_detail`): **Full**, **Lighter** (fewer, wider hairs, half the limit, the first
rule) and **Simple hair** (the stand-in hair; a character with none keeps its strands as on Lighter).

## 4. What it costs (measured)

Hair's time on the card a frame, median over a held state. "By point" is the first form of the code: one copy of
each point, the pieces' corners pointing into it.

| the wolf, no limit (435,836 hairs, 1.5 million pieces) | ms |
|---|---|
| by point, all ribbons | 2.57 to 2.71 |
| by point, as the build chooses (thin hairs lines, hidden hair left out) | 2.39 to 2.54 |
| laid out as drawn, all ribbons | 1.71 to 1.78 |
| laid out, as the build chooses, hull by point | 1.39 |
| laid out, as the build chooses, hull laid out too (what the build does now) | **1.32** |

Where the 1.32 ms goes: pixels about 0.58; the hull 0.15; the rest is set-up and the card's work on every piece
before any pixel (0.73 ms with every piece left out). 277 draws; 274 more draws cost 0.14 ms (about half a
microsecond a draw).

Held at a share of the hairs, laid out: as lines about 0.40 ms + 0.21 ms for every 100,000 hairs; as ribbons about
0.39 ms + 0.26 ms for every 100,000.

| the wolf under the 1 ms limit | hairs drawn | ms | steady? |
|---|---|---|---|
| by point, the first rule | 104,000 to 124,000 | 0.84 to 0.92 | no (99,000 to 112,000 between passes of one round) |
| laid out, the first rule | 158,648 | 0.81 | yes, a fifth of the limit unused |
| laid out, the other rule alone | a share from 0.55 to 0.73 | 0.79 to 0.97, worst reading 1.016 | **no**: it goes back and forth where the lines begin |
| laid out, the other rule, lines kept (loss 0.2) | 256,543 | 0.94 | yes, after 1 to 6 s |
| laid out, the other rule, lines kept (loss 0.4: **Full as built**) | **253,093** | **0.93** | yes |

| the beard (39,559 hairs) | ms |
|---|---|
| no limit, by point, ribbons | 0.91 |
| no limit, laid out, as the build chooses (lines at that size) | 0.65 |
| 1 ms limit, by point, the first rule | 0.76, 32,060 hairs |
| 1 ms limit, as built | 0.65, every hair (the same picture to the pixel with either rule) |

Making the layouts: the wolf's took 270 ms when first drawn and 87 ms more when its thin hairs first became lines;
the beard's 216 ms. That was measured while they were made on the drawing thread, one after another, with every
form kept (495 MB and 412 MB of the card's memory). Since then they are made off the drawing thread side by side
and unused forms are freed. As built (the game test below): the work before a character's hair is first drawn took
278 to 320 ms for the wolf and 217 ms for the beard; held on screen the wolf's hair keeps 224 MB, the beard's 156
MB, the two together 380 MB.

### With a game

Earlier game tests (a heavy shooter at 1080p, recording in OBS, the same card): posing on the card instead of the
CPU cost the game nothing measurable and halved OBS's own time a frame. With no limit the wolf took 2.6 ms and
about 25 frames a second off the game in a match; at the 1 ms limit there was no clear cost (the game itself swings
by 15 frames a second from minute to minute).

The build as it is now (11 November 2026; someone playing; six states of 31 s, five times round: two rounds in
menus, one at the start of a match, two in a match; the card 60 to 108% busy):

| state | hair ms (worst sampled) | hairs drawn | draws | OBS's share of the card | OBS render ms | frames OBS missed |
|---|---|---|---|---|---|---|
| nothing shown (the capture only) | | | | 3.9% | 0.94 | 0 |
| the beard, as built | 0.556 (0.61) | 39,559, every hair | 20 | 8.0% | 1.95 | 2 |
| the wolf, as built | 0.857 (0.963); 0.82 to 0.95 by round | 318,672; 297,201 to 364,546 by round | 275 | 10.3% | 1.66 | 3 |
| the wolf, the first rule | 0.845 (0.972) | 313,787 | 275 | 10.1% | 1.73 | 1 |
| the beard with the wolf as its companion | 0.737 (0.795) | 303,246 of 500,861, the share 1 | 295 | 9.4% | 2.70 | 5 |
| the wolf, merged draws | 0.851 (1.073) | 314,890 | 10 | 9.9% | 1.47 | 0 |

- The limit held. Read twice a second no state as built went over 1 ms. The budget reads every frame, and in the
  match rounds it met sets of readings over the limit (the wolf at shares 0.76 and 0.81, the pair at 1) and drew
  fewer: the pair went to 0.97 of its hairs for a while.
- No cost to the game could be read. With nothing shown the game ran at 85 and at 115 frames a second in the two
  match rounds; the states with hair read from 10 under to 23 over the "nothing" of their round, and two states
  that cost the card the same (the wolf by either rule) were 9 frames a second apart. By share: a character with
  its hair took 4 to 6% of the card.
- OBS missed 1 to 5 frames a state (of about 9,300) with hair shown, all in the match rounds with the card at 95%
  or more; none with nothing shown or with merged draws. The avatar in use before this work was not in the test.
- **The same hairs read 10 to 20% cheaper than with no game**: the wolf held at a share of 0.70 took 0.85 ms (1.06
  with no game, the same hairs, pieces and lines to within 1%), the beard 0.556 (0.653), and the distances pass,
  whose drawing had not changed, 0.243 (0.269) and 0.177 (0.216). The hull was drawn (the same share of the pieces
  as before). So the likely cause is the card: it runs faster while a game keeps it busy than when OBS alone draws
  a little each frame. **Not proven**: the two were different builds on different hours. A reading with no game
  from the same build in the same session settles it, and every game test should take one. Until then the no-game
  numbers above are an upper bound for real use, not a measure of it.
- Because hair read cheaper, **the first rule did as well as the other** here (it never reached the share under
  0.65 where a coat's thin hairs stop being lines and it gets stuck). The other rule's gain is for where hair costs
  more: a slower card, a heavier coat.
- The encoder's load was not measured (its counter gave nothing). Lighter was not in the test.

## 5. What worked

- **Sorted layers and a hull instead of sorting hairs.** No per-frame sort of 1.5 million pieces; only the groups
  are ordered.
- **Laying the corners out in the order they're drawn.** The biggest single saving (2.39 to 1.32 ms for the same
  picture, 0 pixels different). What led to it: drawing everything in one layer was twice as fast although it drew
  the same pieces, so the cost was the card jumping about in memory, not the work on each piece.
- **Lines for hairs under a pixel wide**, and **two sorts** so an animal's coat can use them without its wide hairs
  losing cover. Worth about 7% by point; laid out, 1.78 ms as ribbons against 1.39 as the build chooses. Decisive
  under a budget, because lines cost less for each hair.
- **The other budget rule together with lines kept.** 253,000 hairs instead of 159,000 in the same 1 ms.
- **Timing hair on the card itself** rather than guessing from counts: the same number of pieces cost 2.6 ms on the
  wolf and 0.9 ms on the beard.
- **Posing on the card** (`direbot-face-3d-skin.effect`): frees the CPU and lets a character keep 2 million points.

## 6. What did not work, and why

| tried | what happened | now |
|---|---|---|
| One triangle a piece (a zig-zag strip) | fills in the bows of bent hairs | dropped |
| Fewer points a hair for far hair (bend 0.003, turn 90 degrees) | more hairs fitted the cap and it cost *more* (2.67 ms); 49% of the pixels changed | dropped |
| Leaving hidden hair out before the pixel stage | half the wolf's pieces are left out, but it saves 0.16 ms on ribbons and nothing on lines, and the test itself costs 0.09 ms | kept on (small gain, same picture to within 19 pixels) |
| Lines by width alone (a looser rule than 1%) | fine on a white coat, but a beard loses 4 to 8% of its cover and shows specks of skin under the lip | the 1% rule, and two sorts |
| One layer instead of five | half the time, but the coat is drawn darker on a third of the pixels | not used; it showed that memory order was the cost |
| The other budget rule without lines kept | never settles: the share steps over the point where hairs turn into lines and back, and readings go over the limit | never use rule 1 with `line_loss` under about 0.2 |
| The first rule on a coat that changes form | can cycle for ever at the same jump, and leaves up to a quarter of the limit unused | kept for Lighter only (unmeasured there with the other) |
| Estimating the card's cost from a software renderer | the direction was right and the sizes wrong every time (a 40% saving predicted, 5% measured; 1.57 ms predicted, 0.80 measured) | measure on a real card before deciding |
| Every one of the wolf's hairs inside 1 ms | not reached: 1.32 ms with all of them | stopped at 253,000 hairs by decision; see below |

Built but **off**: **merged draws** (`HairPlan::merge` 1, the hidden setting `hair_merge`; `StrandMerge` in
`hair.hpp`, `Avatar3D::draw_merged`). A layer's groups go to the card in one draw from an index buffer made in the
groups' order; rows that still hold are reused when the order turns and the buffer is made again after a rest. The
picture is the same to the pixel on Direct3D (40 draws become 5 in the test). The estimate from the cost of a draw
above was 0.13 ms saved on the wolf. **Measured in the game test it saved nothing on the card**: 275 draws became
10 and hair took 0.851 ms against 0.857 at the same share. It saved OBS's own thread 0.19 ms a frame (1.47 against
1.66) and missed no frames. But it was not steady: in its first two rounds the share changed 17 times (2 for the
wolf as built) between 0.52 and 0.82, and one reading was 1.073 ms. Likely each change of share makes its index
buffers again, which costs, which changes the share; not looked into. It stays off until that is understood.

Ideas not built: a smaller record a corner (the card reads 52 bytes a corner; reading 16 with every piece left out
took 0.29 ms less, but exact floats need the 52, so it would need a lossy record and tolerance tests); cheaper
colour work in the strand pixel shader; a cheaper hull.

## 7. How to measure it again

The source has hidden settings for this (set them with obs-websocket's `SetInputSettings`; -1 means "as the setting
has it"). They are in no settings window.

| setting | values |
|---|---|
| `hair_budget_ms` | the limit in ms; 0 none |
| `hair_budget_rule` | 0 the first rule, 1 the other |
| `hair_line_loss` | 0.05 to 0.4: how much of their cover a budget's lines may lose |
| `hair_line_cost` | 0.1 to 1: what a ribbon costs against a line (the first rule's exchange) |
| `hair_share` | hold the budget's share at this (up to 1) whatever the clock says |
| `hair_lines` | 0 never lines, 1 where thin enough, 2 always |
| `hair_skip_hidden` | 0 off, 1 on, 2 draw only what would be left out, 3 leave every piece out, 4 make the test and leave nothing out |
| `hair_layout` | 0 by point, 1 laid out as drawn |
| `hair_merge` | 0 a draw a group, 1 a layer's groups together, 2 the same and never made again |
| `hair_idle` | frames before an unused form is freed |
| `hair_layers` | how many layers (reloads the character) |
| `hair_solid` | 0.05 to 1: how solid a thin hair is left before fewer are drawn |
| `hair_bend`, `hair_turn` | the loader's limits for leaving points out (reloads) |
| `hair_measure` | 1 hair into one pixel (everything but the pixels), 2 and 3 every piece left out and only 32 or 16 bytes a corner read, 4 no hull, 5 every run in two draws |

The vendor request `FaceGetState` (vendor `direbot`, `{"source": name}`) answers with `model.on_card` and
`model.hair`: `strands` and `loaded` (in the file, kept), `drawn`, `pieces`, `lines`, `laid`, `laid_mb`, `laid_ms`,
`forming`, `point_mb`, `merged`, `merges`, `skips_hidden`, `draws`, `hull_pieces`, `hull_draws`, `depth_ms`, `ms`
(hair's time on the card last read), `budget_ms`, `share`, `share_changes`, `form_changes`, `over` (how far the
worst reading went over the limit).

The method that gave the numbers above:

1. A scene of its own with one face avatar source a state, 1920 x 1080, everything that moves switched off
   (`smoothing`, `breathing`, `sway` 0, `eye_moves` false) so two states can be compared pixel by pixel.
2. Each state is shown for 25 to 31 s; the first 6 s are thrown away (15 s or more for a state under a budget: the
   share needs time to settle) and the rest read about four times a second. Report the median, the worst, and how
   often the share and the form changed.
3. Go round all the states at least twice and compare the passes. A number that differs between passes is not a
   result.
4. A screenshot of every state (`GetSourceScreenshot`); count the pixels that differ by more than 8 of 255 between
   two states. Pairs that must be the same picture (by point against laid out, with and without the merged draws)
   must differ by 0.
5. With a game: someone plays normally (standing still makes some games idle at 30 frames a second) while the
   states change by themselves; the game's frame times come from Windows' own present events; each state is
   compared with the "nothing shown" state of the same pass, because the game changes from minute to minute; and the
   scene holds the same screen or game capture as when live, or the card carries less than it would. Say which
   rounds were in a match and which in menus. Take the same states once with no game in the same session (the card
   may read cheaper under a game), and record `hull_pieces`, `hull_draws` and `depth_ms` for every state, so a
   difference can be traced to its part.
6. A test build is unsigned: back up the installed add-on first, put it back after, and check its files' hashes and
   signature.

## 8. Which test proves what

- `tests/unit_hair.cpp` (14,579 checks): reading, leaving points out, the cap, the order, runs and sorts, the share
  by size, lines, both budget rules, the layouts, the hull, merged draws. `tools/test-link.sh` runs it.
- `tests/test_real_hair.py` (17 tests inside a real OBS on Linux, posed by the CPU): what is drawn, the settings,
  the budget on a clock the test drives, expressions and bones, a companion's hair.
- `tests/test_real_windows.py` (inside OBS 31, 32 and 33 on Windows, Direct3D, posed on the card):
  `test_hair_strands_on_the_graphics_card`, `test_hidden_hair_is_left_out_on_the_card`,
  `test_hair_laid_out_as_drawn_on_the_card`, `test_a_layer_of_hair_in_one_draw_on_the_card`,
  `test_a_companions_hair_is_drawn_as_its_owners`, `test_hair_on_the_card_keeps_only_what_its_drawn_from`. These
  are the only proof of the card path: every "same picture" above is a 0-pixel check there.

OpenGL (Linux, Mac) poses hair on the CPU and uses none of section 3's card-only parts, so a change to them is not
tested until the Windows jobs have run.

## 9. Limits that stand

- Every hair of a coat as big as the wolf's does not fit 1 ms on this card (1.32 ms with no game); at the limit it
  draws 253,000 of 436,000 with no game and 297,000 to 365,000 under one, each wider. Kept lines make such a coat
  very slightly darker and thinner than ribbons would.
- The first time a character's hair is drawn the picture waits for its layouts (about 0.2 s for the beard, 0.3 s
  for the wolf; the length of the pause itself was not timed, only the work).
- With the card full (a game at 95% or more) OBS missed up to 5 frames in two and a half minutes with hair shown.
- Lighter keeps the first budget rule; the other has not been measured at 0.5 ms.
- Hair casts no shadow on itself or the body.
- The layers are not true depth: crossing locks (a braid) show through each other a little, and two see-through
  parts that interleave hide each other at their soft edges.
- An expression moves a strand part by 2,047 spots at most. A part with more points that each move their own way
  has them share spots (the mean of their moves); a file that needs every point exact splits such hair over parts
  of at most 2,047 moved points each.
- Posed by the CPU a character keeps far fewer hairs (150,000 points).
