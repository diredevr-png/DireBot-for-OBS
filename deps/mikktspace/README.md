MikkTSpace - the common way of working out tangents from a model's UVs,
the one Blender (and most tools that bake normal maps) use, and the one
glTF asks for when a file has none of its own.

mikktspace.c and mikktspace.h are Morten S. Mikkelsen's, unchanged, from
https://github.com/mmikk/MikkTSpace (commit 3e895b49d05ea07e4c2133156cfa94369e19e409).
More at http://www.mikktspace.com/.

Copyright (C) 2011 by Morten S. Mikkelsen, under the zlib licence (in the
top of both files): free to use, with this notice kept. DireBot for OBS
calls it from src/face/model.cpp (tangents_of).
