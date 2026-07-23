# Sonic Adventure 2 (PC / Steam) file formats

Everything here was derived from the retail Steam build and verified by batch
regression over every shipped file. Where a number is quoted it came out of the
data, not out of a wiki.

SA2 PC is a port of the GameCube release, so most asset data is **big-endian**
GameCube data shipped verbatim. Textures are **GVR** (GameCube VR), not the
Dreamcast PVR that SADX uses — there are zero `.pvm`/`.pvr` files in the game.

---

## 1. PRS compression (`.prs`)

SEGA's LZ77 variant. Control bits are consumed **LSB-first** from lazily
refilled control bytes:

| Control bits | Meaning |
|---|---|
| `1` | literal byte |
| `0 0 b b` | short copy: `len = bb + 2`, next byte is an 8-bit negative offset |
| `0 1` | long copy: LE `u16 w`; `offset = (w >> 3) - 8192`, `len = w & 7`. If `len == 0` an extra byte gives `len + 1`, else `len += 2`. **`w == 0` ends the stream.** |

Copies are byte-at-a-time from the output buffer — overlapping copies are legal
and are how runs get encoded, so `memcpy` is wrong here.

Sniff test: `size > 3 && (data[0] & 1) == 1 && last two bytes are 0`.

**Result: 2451 / 2451 files decompress, 176.2 MB → 335.2 MB (1.90x).**

---

## 2. PAK archive (`.pak`)

The PC port's texture-replacement container. All little-endian.

```
0x00  char[4]  magic = 01 'p' 'a' 'k'
0x04  u8[33]   zero
0x25  u32      entry count (mirror)
0x29  u32      total size of all payloads
0x2D  u32      total size of all payloads (mirror)
0x31  u32      0
0x35  u32      0
0x39  u32      entry count          <- read the count from here
0x3D  entry metadata table:
        u32 n; char longPath[n];    (original build path)
        u32 m; char name[m];        (archive-relative path)
        u32 length; u32 length;     (stored twice)
      ... then every payload back to back, in table order.
```

Payloads are DDS (2573), `.inf` index files (141) and PNG (70).

**Result: 230 / 230 archives parse with exact byte accounting** — the computed
end of the last payload equals the file size in every case.

---

## 3. GVM texture archive + GVR textures

### GVM directory — mixed endianness, which is the classic trap

```
0x00  char[4] "GVMH"
0x04  u32 LE  headerLength     (first texture is at 8 + headerLength)
0x08  u16 BE  flags
0x0A  u16 BE  entry count
0x0C  entry table
```

Flag bits: `0x1` global indexes, `0x2` dimensions, `0x4` formats, `0x8` filenames.
With all four set an entry is 38 bytes: `u16 index`, `char[28] name`,
`u8 fmt1, u8 fmt2`, `u16 dims`, `u32 globalIndex`. Global indexes are based at
100000.

### GVR texture

```
optional "GBIX"/"GCIX" chunk: magic, u32 LE len, u32 BE globalIndex, pad
"GVRT"
  +0x04  u32 LE  dataLength
  +0x0A  u8      (paletteFormat << 4) | dataFlags
  +0x0B  u8      dataFormat
  +0x0C  u16 BE  width
  +0x0E  u16 BE  height
  +0x10  [CLUT if indexed] then pixel data
```

`dataFlags`: `0x1` mipmaps, `0x2` external palette, `0x8` internal palette.

Mip chain size uses a **32-byte floor per level**, all the way down to 1x1:
`total = max(32, raw(W,H))` then `for (s = W>>1; s; s >>= 1) total += max(32, raw(s,s))`.

Formats actually used by SA2 (16,057 textures across 606 archives):

| ID | Format | Count |
|---|---|---|
| 0x0E | DXT1 / GameCube CMPR | 13,827 |
| 0x05 | RGB5A3 | 2,212 |
| 0x08 | Index4 | 18 |

GameCube tiling is fixed-size tiles, row-major inside a tile and tiles
row-major across the image: 8x8 for 4bpp, 8x4 for 8bpp, 4x4 for 16bpp, and 4x4
with a separate AR plane then GB plane for ARGB8888.

**CMPR is not PC DXT1**: 8x8 macroblocks of four 4x4 sub-blocks, the two RGB565
endpoints are **big-endian**, and the 2-bit selectors are read **MSB-first**
(`(byte >> (6 - x*2)) & 3`).

---

## 4. Model containers

| Container | Endian | Pointer base |
|---|---|---|
| `*mdl.prs` / `*mtn.prs` (characters) | big | **0** — direct file offsets |
| `event/eNNNN.prs` (cutscenes) | big | **0x8125FE60** — GameCube RAM addresses |

The event base was recovered from the data itself, without assuming it: find
byte patterns that look like `NJS_OBJECT` (scale == 1,1,1), then take the mode of
`pointer − candidate_offset` across every pointer in the file. All files agree on
`0x8125FE60`. (A naive "which base makes the most pointers land in range" scan is
ambiguous by ±0x34 — exactly one `NJS_OBJECT` — because nodes are stored in
contiguous arrays.)

```
MDL:  repeat { u32 index; u32 objectOffset } until index == 0xFFFFFFFF
MTN:  repeat { i16 index; u16 nodeCount; u32 motionOffset } until index == -1
```

---

## 5. NJS_OBJECT node — 0x34 bytes

```
0x00  u32     flags
0x04  u32     attach pointer (0 = none)
0x08  f32[3]  position
0x14  s32[3]  rotation, BAMS (0x10000 == 360 degrees)
0x20  f32[3]  scale
0x2C  u32     first child
0x30  u32     next sibling
```

Flag bits: `0x01` no position, `0x02` no rotation, `0x04` no scale, `0x08` hide,
`0x10` skip children, `0x20` alternate euler order, `0x40` **skip in the motion
node index**.

Local transform is `Scale * Rotation * Translation` (row-vector). The default
euler order is XYZ; with flag `0x20` SA2 evaluates **Z, then X, then Y**.

A node whose position or scale is non-finite or absurd is not a node — reject it,
otherwise one bad node poisons the world transform of every descendant.

---

## 6. Ninja Chunk model (`NJS_CNK_MODEL`) — 0x18 bytes

```
0x00  u32     vlist pointer   (Sint32*)
0x04  u32     plist pointer   (Sint16*)
0x08  f32[3]  bounding sphere centre
0x14  f32     bounding sphere radius
```

**The key endianness insight.** SA2B/SA2PC data is the original little-endian
Dreamcast data converted to big-endian *per declared field size*. Because the
struct declares `Sint32 *vlist` and `Sint16 *plist`, the **vertex list is
byte-swapped in 4-byte units and the polygon list in 2-byte units**. So:

```
vertex chunk header : BE u32 W -> head = W & 0xFF, flag = (W>>8)&0xFF,
                                  size = W >> 16          (u32 words)
vertex index header : BE u32 W -> indexOffset = W & 0xFFFF, nbVertex = W >> 16
poly chunk header   : BE u16 w -> head = w & 0xFF, flag = w >> 8
                      BE u16   -> size                    (u16 words)
```

Read the vertex header byte-by-byte and you get nonsense; read it as one u32 and
everything lines up.

### Vertex stride is self-describing

`size` counts u32 words after the 4-byte header, and the first of those is the
index header, so `stride = (size - 1) / nbVertex`. Deriving it from the chunk
beats any hardcoded table — a wrong table entry (VertexDiffuse8 is 4 words, not 5)
silently walks the parser off the rails and produces coordinates near FLT_MAX.

Types actually used: 44 `VertexNormalNinjaFlags`, 41 `VertexNormal`,
35 `VertexDiffuse8`, 42 `VertexNormalDiffuse8`.

### Polygon chunks

`1..5` Bits, `8..9` texture ID (`id = u16 & 0x1FFF`), `16..31` material
(bit 0 diffuse, bit 1 ambient, bit 2 specular — each one ARGB u32, swapped as u16
pairs), `56..58` volume, `64..75` strip, `255` end.

Strip chunk: `u16 header` where `nbStrip = h & 0x3FFF` and `userFlagSize = h >> 14`,
then per strip a `s16 length` (**negative means reversed winding**) followed by
that many `u16` indices, each optionally trailed by a `s16` UV pair
(divide by 256, or 1024 for the "UVH" variants).

Strip counts, indices and UVs must all be bounded by the chunk's declared size,
or a corrupt chunk will read megabytes of garbage.

### The vertex cache is shared across the whole node tree

This is the part that is easy to get wrong. A vertex chunk writes into a single
cache at `indexOffset`, and a node's polygon chunks may index vertices uploaded by
an **ancestor** node. So the tree must be walked once, depth-first, transforming
each node's vertices into model space as they are written. Parsing each attach in
isolation loses geometry and produces dangling indices.

---

## 7. NJS_MOTION — 16 bytes

```
0x00  u32  mdata pointer
0x04  i32  frame count
0x08  u16  type      (channel mask: 1 position, 2 rotation, 4 scale, ...)
0x0A  u16 inp_fn     (low nibble = channel count, bits 6-7 = interpolation)
```

`mdata` is one entry per animated node: **all N pointers first, then all N
counts**, in channel-flag order. Keys are 16 bytes: `i32 frame` plus either three
floats (position/scale) or three BAMS `i32` (rotation).

Motions bind to nodes by depth-first traversal index, skipping nodes flagged
`0x40`. The node count lives in the MTN table entry, not in the motion.

**Result: 820 motions, 345,380 keyframes across 17 character motion sets.**

---

## 8. Stage geometry: GameCube REL + LandTables + GC "Ginja" models

Stages live in `stgXXD.rel` — genuine GameCube REL modules, big-endian.

### REL relocation

```
0x00 u32 moduleId    0x0C u32 numSections   0x10 u32 sectionInfoOffset
0x1C u32 version     0x24 u32 relOffset     0x28 u32 impOffset  0x2C u32 impSize
section info: numSections x { u32 offset (low 2 bits = flags), u32 size }
imp table:    impSize/8 x { u32 moduleId, u32 relStreamOffset }
reloc entry (8 bytes): u16 skip, u8 type, u8 section, u32 addend
```

Apply only the sub-stream whose `moduleId == header moduleId` (the other targets
the DOL). Relocation is a running cursor: `R_DOLPHIN_SECTION (202)` resets the
cursor to a section start, `R_DOLPHIN_NOP (201)` advances it, `R_DOLPHIN_END (203)`
ends the stream. For `R_PPC_ADDR32 (1)`, write `sectionOffset + addend` at the
cursor. **A REL section's stored offset already IS its file offset**, so after
relocating in place every intra-module pointer is a plain file offset (base 0) —
no repacking needed. The 16-bit/branch relocations only matter for code we never
run.

**Result: 64/64 stage RELs relocate and yield a LandTable (2 have none, as
expected), ~1.0M triangles total.**

### LandTable (SA2/SA2B) — 0x20 bytes

```
0x00 i16 colCount        0x02 i16 visibleModelCount   0x0C f32 clippingDistance
0x10 u32 COL array       0x18 u32 textureName (cstr)  0x1C u32 texlist
```

`textureName` is e.g. `"landtx13"` → `LANDTX13.PRS` (a GVM). A stage also carries
auxiliary landtables for animated scenery, named by suffix: `_uv` (UV scroll),
`_ani`, `_x`. Each is extracted as a separate model.

### COL — 0x20 bytes

```
0x00 f32[3] centre   0x0C f32 radius   0x10 u32 NJS_OBJECT
0x18 u32 blockBits   0x1C i32 surfaceFlags
```

COL index `< visibleModelCount` is a visual model; `>= visibleModelCount` is a
collision-only Basic model (skipped by the viewer). Most SA2B stages use GC
"Ginja" visual models; a few (e.g. City Hall) ship Dreamcast Chunk models inside
the REL, so the loader detects the format per landtable by sampling.

**Locating the LandTable without hardcoded addresses:** signature-scan for the
struct, then require a share of its COLs to resolve to real models. That last
check is what rejects the garbage "landtables" that otherwise appear inside the
relocation/import tables (an in-place relocated REL contains far more non-geometry
bytes than a repacked section image would).

### GC "Ginja" model

```
GCAttach (0x24): u32 vertexSets, u32 skin(=0 in SA2), u32 opaqueMeshes,
                 u32 translucentMeshes, u16 opaqueCount, u16 transCount,
                 f32[3] centre, f32 radius
GCVertexSet (16, ends at attribute 0xFF/0): u8 attribute (1=Pos,2=Normal,
                 3=Color0,5=Tex0), u8 stride, u16 count,
                 u32 structure (structType=&0xF, dataType=(>>4)&0xF),
                 u32 dataPtr, u32 dataLen
GCMesh (16): u32 paramPtr, i32 paramCount, u32 primPtr, u32 primSize
GCParameter (8): u8 type (0=VtxAttrFmt,1=IndexFlags,2=Lighting,4=BlendAlpha,
                 5=Colour,8=Texture,10=TexCoordGen), u32 data
```

Positions/normals are float32 XYZ; colours RGBA8; UVs are int16/256. The display
list is **big-endian GX** everywhere: `u8 opcode` (0x90 triangles, 0x98 strip,
0xA0 fan), `u16 count`, then per-vertex indices whose presence and width come from
the **sticky** IndexAttributeFlags (2 bits per GX slot; the value persists across
meshes until another parameter changes it).

## 9. SET object placement

`setXXXX_s.bin` / `_u.bin` (and `_2p_`, `_hd_` variants), big-endian:

```
0x00 u32 count, 0x04 u8[0x1C] pad, then count x SETEntry (0x20):
  0x00 u16 id (low 12 bits = object id, high 4 = clip level)
  0x02 i16[3] rotation (BAMS)   0x08 f32[3] position   0x14 f32[3] scale
```

**Result: parses byte-exact on all 240 SET files** (the size equation
`0x20 + count*0x20 == filesize` holds). The object id → model mapping is *not* in
the file — it indexes a per-stage object list that SA Tools carries as curated
`objdefs` metadata, so the tool exports placement (id + transform) as a scene
JSON rather than resolving models.

## 10. Friendly names (from the executable)

The UI's friendly names come straight out of `sonic2app.exe`, not a bundled
table:

* **Stage number -> name** from the compiled source paths, e.g.
  `..\src\stg13_cityescape`, `..\src\stg34_CannonsCoreSonic`. These give a
  reliable `stgNN -> codename` map (and, for Cannon's Core, the character).
* **Clean display names** from the in-game English stage-name strings (the
  `"<Name> BGM."` sound-test block: `City Escape BGM.`, `Metal Harbor BGM.` ...).

Each codename is matched to a clean name by normalized containment
(`cityescape` -> "City Escape", `pumpkin` -> "Pumpkin Hill"), falling back to a
prettified codename; rival battles (`sonicvsshadow`) become "Sonic vs Shadow".
Character names come from the model file prefix (`sonicmdl` -> Sonic,
`milesmdl` -> Tails, `metalsonicmdl` -> Metal Sonic), and boss arenas from a
small keyed table (`bigfoot` -> Big Foot, `last1` -> Biolizard).

## 11. What is not implemented

* **Object/enemy models compiled into `sonic2app.exe`** (GC "Ginja" at absolute
  VAs, ImageBase 0x00400000) — the placement side is done, the model resolution
  is not.
* **Stage motion** (UV scroll rates, moving-platform paths) is compiled game code,
  not replayable keyframe data.
* **In-game particle systems** are hardcoded; only effect textures (extractable
  via the texture path) and `sp_info` appearance parameters are data.
* Audio (`.adx`, `.csb`), video (`.sfd`, `.m1v`) and message tables are indexed
  and classified but not decoded.
