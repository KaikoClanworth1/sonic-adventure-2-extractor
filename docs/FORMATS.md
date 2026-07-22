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

## 8. What is not implemented

* **Stage geometry** lives in `stgXXD.rel` — genuine GameCube REL modules
  (big-endian PowerPC relocatables, module ID at 0x00). Extracting landtables
  from these needs section relocation processing, which this project does not do.
* **Object/stage models compiled into `sonic2app.exe`** use the GC "Ginja" format
  (little-endian structs, big-endian GX display lists) at absolute VAs with
  ImageBase 0x00400000. Also not implemented.
* Audio (`.adx`, `.csb`), video (`.sfd`, `.m1v`) and message tables are indexed
  and classified but not decoded.
