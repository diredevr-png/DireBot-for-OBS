# Companion test files (placeholders)

Made-up stand-ins for the tests of a companion beside the 3D character and
of moves that stay (tests/unit_companion.cpp, tests/test_real_companions.py).
They are boxes, not real models.

- wolf-test.glb: a box "wolf" (glTF, not a VRM) made for the picture's left
  of its character, its origin on the floor where it sits. Animations come
  (3.5 s, in from -x), idle (6 s loop), pet (3 s), go (3 s, out to -x) and
  scared (2.5 s); a node "pat" under "head". About 1.27 tall sitting.
- dragon-test.vrm: a tiny box person (VRM 1.0) with a box "dragon" kept
  hidden in its hips at rest (dragon_root at 0.001 of its size, bound at
  full size). Animations idle, dragon_come, dragon_idle, dragon_pet,
  dragon_go (4, 4, 4, 3 and 4 s).
