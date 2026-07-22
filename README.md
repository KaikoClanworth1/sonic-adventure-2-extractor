# Sonic Adventure 2 — Extractor & Model Viewer

A native Windows tool for browsing, extracting, viewing and exporting the assets
of **Sonic Adventure 2 (PC / Steam, 2012)**.

![Screenshot](docs/screenshot.png)

It reads the game's own formats directly — no intermediate tools, no runtime
dependencies. Everything was reverse-engineered from the retail build and is
verified by a batch regression that runs every parser over every shipped file.

---

## What it does

* **Browses** the whole game folder — 4,351 files indexed and classified.
* **Global search** across every filename.
* **Decompresses PRS** and reads **PAK** archives.
* **Decodes GVR/GVM textures** (GameCube CMPR, RGB5A3, indexed) plus DDS and PNG.
* **Renders models** in a textured, lit 3D viewport (orbit + zoom).
* **Exports to binary FBX** with meshes, UVs, materials, textures, the node
  skeleton, skin clusters and every animation as its own take.
* **Exports textures** as PNG.
* Persisted game-folder setting and a **UI scale slider** for high-DPI displays.

## Verified coverage

`sa2cli regress` over the retail Steam build:

```
  files indexed      : 4351
  PRS decompressed   : 2451 ok, 0 failed
  PAK archives       : 231 ok, 0 failed
  GVM archives       : 606 ok, 0 failed (16057 textures)
  character models   : 23 ok, 0 failed
  character motions  : 17 ok, 0 failed (817 motions, 345380 keys)
  event scenes       : 67 ok, 0 failed
  geometry           : 1051417 vertices, 1118404 triangles
  suspect bounds     : 0
  RESULT             : ALL PASS
```

## Getting it

Grab `sa2-extractor-v1.0.0-win64.zip` from
[Releases](../../releases) — a single statically linked `.exe`, no vcredist
required. Run `sa2viewer.exe`, click **Browse...** and point it at your
Sonic Adventure 2 folder (e.g.
`C:\Program Files (x86)\Steam\steamapps\common\Sonic Adventure 2`).

## Command line

```
sa2cli list      <game>            list and classify every asset
sa2cli search    <game> <query>    search asset names
sa2cli info      <file>            identify one file
sa2cli extract   <file> <outdir>   extract textures / PAK contents
sa2cli model     <file>            summarise the models in a file
sa2cli fbx       <file> <out.fbx>  export models + skeleton + anims to FBX
sa2cli regress   <game>            batch-test every parser on every file
```

## Building

Needs MSVC (2019 or newer) and CMake. All dependencies are vendored in
`extern/` — Dear ImGui, GLFW (prebuilt static, `/MT`) and stb.

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output lands in `build\bin\`. The exe links the static CRT, so it runs on a
clean machine.

## Where the assets live

| Asset | Location |
|---|---|
| Playable character models | `resource/gd_PC/*mdl.prs` (23 files) |
| Character animations | `resource/gd_PC/*mtn.prs` (17 files, 817 motions) |
| Cutscene scenes | `resource/gd_PC/event/eNNNN.prs` (67 files) |
| Textures | `resource/gd_PC/**.prs` → GVM, and `*.pak` → DDS/PNG |

## FBX exports are checked in Blender, not just written

`tools/validate_fbx.py` imports an export with headless Blender (`pip install bpy`)
and asserts meshes, UVs, materials, image sizes, armature bones, skin weights that
resolve to real bones, and actions with actual keyframes.
`tools/render_fbx.py` renders one independently — this is Knuckles' head straight
out of `knuckmdl.prs`, imported and rendered by Blender with no manual fixing:

![Blender render](docs/blender-knuckles.png)

Across a random sample of exports: **6/6 pass**, e.g. `milesmdl` → 18/18 meshes with
UVs and vertex groups, 60 bones, 0 dangling groups, 30 actions, 83,910 keyframes.

## Known limitations

* **Stage geometry is not supported.** Level landtables live in `stgXXD.rel`,
  which are genuine GameCube REL modules; pulling models out needs section
  relocation processing that this tool does not implement.
* **Cutscene animations are not exported.** Event geometry loads fine, but the
  motions live in a separate `eNNNNmotion.bin` stream that is not parsed, so
  cutscene FBX files contain 0 actions.
* **Animations are matched by exact filename.** `sonicmdl.prs` picks up
  `sonicmtn.prs`, but variants like `bknuckmdl.prs` or `twalk1mdl.prs` have no
  same-named `*mtn.prs`, so they export with no actions. Export the base
  character to get the animations.
* A model container holds many independent models all rooted at the origin, so
  `sa2cli fbx` exports model 0 by default. Pass an index, or `-1` to merge
  everything (they will overlap).
* **Object models compiled into `sonic2app.exe`** use the GC "Ginja" format
  (little-endian structs, big-endian GX display lists) at absolute virtual
  addresses. Not implemented — this tool handles the Ninja **Chunk** models,
  which is what every model in `gd_PC` actually uses.
* Skinning in SA2 is rigid: each mesh is bound to the node that owns it. That
  is how the game itself works (Dreamcast-era segmented characters), and it is
  what the FBX skin clusters reproduce. Weighted chunk vertex types (32/33) are
  parsed but are essentially unused by the retail data.
* Audio, video and message tables are indexed and classified but not decoded.

## Format notes

The reverse-engineering write-up — PRS, PAK, GVM/GVR, the Ninja chunk format
and its unusual per-field-size endianness, and the animation layout — is in
[docs/FORMATS.md](docs/FORMATS.md).

## Legal

This tool ships **no game data**. You need your own copy of Sonic Adventure 2.
Sonic Adventure 2 is © SEGA. This project is unaffiliated with SEGA and is
released under the [MIT License](LICENSE) for interoperability, preservation
and modding purposes.
