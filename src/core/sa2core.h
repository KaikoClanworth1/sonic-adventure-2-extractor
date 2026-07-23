// Sonic Adventure 2 (PC / Steam) asset library.
//
// The formats implemented here were reverse-engineered and validated against
// the retail Steam build; see docs/FORMATS.md for the derivation and the batch
// regression numbers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <array>

namespace sa2 {

const char* version();

// ---------------------------------------------------------------- PRS
// SEGA "PRS" LZ compression. Control bits are consumed LSB-first from lazily
// refilled control bytes:
//   1        -> literal byte
//   0 0 b b  -> short copy, len = bb + 2, 8-bit negative offset
//   0 1      -> long copy, LE u16 w; offset = (w>>3)-8192, len = w&7
//               (len==0 -> extra byte + 1, else len += 2). w==0 ends the stream.
bool prs_looks_valid(const uint8_t* data, size_t size);
std::vector<uint8_t> prs_decompress(const uint8_t* data, size_t size);

// ---------------------------------------------------------------- PAK
// SA2PC "\x01pak": header, metadata table, then concatenated payloads.
struct PakEntry {
    std::string long_path;   // original build path
    std::string name;        // archive-relative path
    uint32_t offset = 0;
    uint32_t length = 0;
};
struct PakArchive {
    std::vector<PakEntry> entries;
    bool parse(const uint8_t* data, size_t size);
};

// ---------------------------------------------------------------- textures
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;         // width*height*4
    std::string name;
    bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

// GVR (GameCube VR). `data` may start at GBIX/GCIX or directly at GVRT.
bool gvr_decode(const uint8_t* data, size_t size, Image& out);
// GVM archive: LE chunk length, BE flags/count/entries.
bool gvm_extract(const uint8_t* data, size_t size, std::vector<Image>& out);
// DDS: DXT1/3/5 and uncompressed BGRA/BGR.
bool dds_decode(const uint8_t* data, size_t size, Image& out);
// PNG via stb_image.
bool png_decode(const uint8_t* data, size_t size, Image& out);

// Loads every texture out of whatever container `path` holds.
bool load_textures(const std::string& path, std::vector<Image>& out);

// ---------------------------------------------------------------- models
// NJS_OBJECT: 0x34 bytes { u32 flags, u32 attach, f32 pos[3], s32 rot[3](BAMS),
//                          f32 scale[3], u32 child, u32 sibling }
struct Node {
    uint32_t ptr = 0;
    uint32_t flags = 0;
    uint32_t attach_ptr = 0;
    float pos[3]{0, 0, 0};
    int32_t rot[3]{0, 0, 0};      // BAMS: 0x10000 == 360 degrees
    float scale[3]{1, 1, 1};
    int parent = -1;
    int index = 0;
    std::string name;
    float world[16]{};            // model-space matrix, row-vector convention
};

// A run of triangles sharing one texture/material.
struct MeshPart {
    std::vector<float> positions;    // xyz triples
    std::vector<float> normals;      // xyz triples
    std::vector<float> uvs;          // uv pairs
    std::vector<uint32_t> colors;    // ARGB, 0xFFFFFFFF when absent
    std::vector<uint32_t> indices;
    std::vector<int> vertex_node;    // node supplying each vertex (skin bind)
    int texture_id = -1;
    int node_index = 0;
    bool double_sided = false;
    bool use_alpha = false;         // alpha blending on
    bool ignore_light = false;      // unlit / vertex-colour only
    bool env_map = false;           // spherical environment mapping
    int blend_src = 4;              // GX blend factor (4 = SrcAlpha)
    int blend_dst = 5;              // GX blend factor (5 = InvSrcAlpha)
    uint32_t diffuse = 0xFFFFFFFFu; // ARGB material colour
};

struct Model {
    std::vector<Node> nodes;
    std::vector<MeshPart> parts;
    std::string name;
    bool empty() const { return parts.empty(); }
    void bounds(float lo[3], float hi[3]) const;
    size_t triangle_count() const;
    size_t vertex_count() const;
};

// ---------------------------------------------------------------- motions
struct Key3 { int frame; float v[3]; };
struct MotionChannel {
    std::vector<Key3> pos;
    std::vector<Key3> rot;     // radians
    std::vector<Key3> scale;
};
struct Motion {
    int frame_count = 0;
    int node_count = 0;
    uint16_t type = 0;
    std::string name;
    std::map<int, MotionChannel> channels;   // animated-node index -> keys
    bool empty() const { return channels.empty(); }
};

// ---------------------------------------------------------------- containers
// A Ninja data blob plus the base address its pointers are relative to.
class NinjaBlob {
public:
    NinjaBlob(std::vector<uint8_t> data, uint32_t base, bool big_endian);

    bool ok(uint32_t p) const {
        return p != 0 && p >= base_ && p < base_ + (uint32_t)data_.size();
    }
    uint32_t off(uint32_t p) const { return p - base_; }
    size_t size() const { return data_.size(); }
    const uint8_t* data() const { return data_.data(); }
    const std::vector<uint8_t>& bytes() const { return data_; }

    uint32_t u32(size_t o) const;
    int32_t  i32(size_t o) const;
    uint16_t u16(size_t o) const;
    int16_t  i16(size_t o) const;
    float    f32(size_t o) const;

    // Depth-first NJS_OBJECT walk; rejects structurally impossible nodes.
    std::vector<Node> read_tree(uint32_t root) const;
    // True when `ptr` really points at an NJS_CNK_MODEL.
    bool valid_attach(uint32_t ptr) const;
    // Builds model-space geometry. Chunk models share one vertex cache across
    // the whole node tree, so this happens in a single traversal.
    bool build_model(uint32_t root, Model& out) const;
    // As build_model, but poses the skeleton with `motion` at `frame` first
    // (motion == nullptr gives the bind pose). Used for animation playback.
    bool build_model_posed(uint32_t root, const Motion* motion, float frame,
                           Model& out) const;
    bool read_motion(uint32_t ptr, int node_count, Motion& out) const;
    // Count of nodes that a motion animates (those without the NoAnimate flag).
    int count_animated(uint32_t root) const;
    // Structural scan for NJS_OBJECT roots that own a real attach.
    std::vector<uint32_t> find_model_roots() const;

    // --- GC "Ginja" models (stage geometry) ---
    // True when `ptr` points at a GCAttach (0x24 bytes, skin==0, a Position
    // vertex set, sane mesh counts and bounding radius).
    bool valid_gc_attach(uint32_t ptr) const;
    // Walk a node tree whose attaches are GC attaches; append model-space parts.
    bool build_gc_model(uint32_t root, Model& out) const;

    const std::vector<uint8_t>& raw() const { return data_; }
    uint32_t base() const { return base_; }
    bool big_endian() const { return be_; }

private:
    std::vector<uint8_t> data_;
    uint32_t base_;
    bool be_;
};

// ---------------------------------------------------------------- REL / stages
// Applies a GameCube REL module's self-module ADDR32 relocations in place, so
// intra-module pointers become direct file offsets (pointer base = 0). Returns
// false if the buffer is not a v1/v2/v3 REL. `out` is the relocated image.
bool rel_relocate(const uint8_t* data, size_t n, std::vector<uint8_t>& out);

// A LandTable found inside a relocated stage REL.
struct LandTableInfo {
    uint32_t addr = 0;        // pointer to the LandTable struct
    int col_count = 0;
    int visible_count = 0;    // COL index < this => visual, >= this => collision
    float clip = 0.0f;
    uint32_t col_ptr = 0;
    std::string texture_name;  // e.g. "landtx13" -> LANDTX13.PRS
};

// Signature-scans a relocated REL image for LandTables, largest first.
std::vector<LandTableInfo> find_landtables(const NinjaBlob& blob);

// Builds every visual COL of a landtable into one Model (GC geometry, world
// space). Collision-only COLs are skipped.
bool build_landtable(const NinjaBlob& blob, const LandTableInfo& lt, Model& out);

// ---------------------------------------------------------------- Chao stages
// Decodes Chao World stage geometry (ChaoStgLobby/Karate/Kinder .prs, already
// PRS-decompressed) into world-space meshes. These files pack triangle-strip
// meshes with interleaved pos+normal vertex blocks and no pointer tables (see
// src/core/sa2_chao.cpp). Returns false if no meshes decode (e.g. the Hero/Dark
// gardens, which use a different pos-only format).
bool load_chao_stage(const std::vector<uint8_t>& data, Model& out);

// ---------------------------------------------------------------- SET / objects
// One placed object from a SETxxxx_?.BIN file.
struct SetObject {
    uint16_t id = 0;            // object id (low 12 bits) + clip level (high 4)
    int16_t rot[3]{0, 0, 0};   // BAMS
    float pos[3]{0, 0, 0};
    float scale[3]{1, 1, 1};   // often repurposed as per-object parameters
    int object_id() const { return id & 0x0FFF; }
    int clip_level() const { return (id >> 12) & 0x0F; }
};

// Parses a SET placement file: u32 count (BE), 0x1C pad, then count*0x20 records.
bool parse_set_file(const uint8_t* d, size_t n, std::vector<SetObject>& out);

// ---------------------------------------------------------------- Unity / VRChat
// Writes, into `dir`:
//   <base>.materials.json  - one entry per unique material (texture, unlit,
//                            env-map, blend, double-sided, diffuse)
//   SA2Stage.shader        - a Unity shader reproducing SA2 stage shading,
//                            authored to be VRChat-safe (no GrabPass)
//   apply_materials.py      - a Blender/Unity-agnostic helper that maps the
//                            JSON onto imported meshes by material name
// The FBX/PNG exports already carry the geometry and textures; this adds the
// material intent Unity needs. Returns false on I/O error.
bool export_unity_materials(const std::string& dir, const std::string& base,
                            const Model& model, const std::vector<Image>& textures,
                            std::string* error = nullptr);

// Event files are big-endian and hold absolute GameCube RAM pointers. Retail
// cutscenes load at 0x8125FE60; a few (e.g. e0350) are the older "dcgc" layout
// at 0x812FFE60. detect_event_base() picks whichever puts more pointers in range.
constexpr uint32_t kEventBase = 0x8125FE60u;
constexpr uint32_t kEventBaseDcGc = 0x812FFE60u;
uint32_t detect_event_base(const uint8_t* d, size_t n);

std::vector<std::pair<uint32_t, uint32_t>> read_mdl_table(const uint8_t* d, size_t n);
struct MtnEntry { int index; int node_count; uint32_t ptr; };
std::vector<MtnEntry> read_mtn_table(const uint8_t* d, size_t n);

// ---------------------------------------------------------------- game index
enum class AssetKind {
    Unknown, TextureArchive, CharacterModel, CharacterMotion, EventScene,
    EventTexture, EventMotion, PakArchive, Texture, Audio, Video, Message, Level,
    Stage, SetPlacement, ExeModel, ChaoStage
};
const char* kind_name(AssetKind k);

// User-facing sections shown as tabs in the viewer.
enum class Section { Maps, Characters, Objects, Particles, Audio, Enemies, Other, COUNT };
const char* section_name(Section s);

struct AssetEntry {
    std::string path;          // absolute
    std::string rel_path;      // relative to the game folder
    std::string name;          // file name
    std::string display_name;  // friendly name (from the game where possible)
    std::string subtitle;      // secondary label (e.g. the raw file name)
    uint64_t size = 0;
    AssetKind kind = AssetKind::Unknown;
    Section section = Section::Other;
    bool compressed = false;   // PRS
    uint32_t exe_root = 0;     // for ExeModel: the model root's virtual address
};

// Friendly names pulled from sonic2app.exe: stage number -> stage name (from the
// compiled source paths + the in-game stage-name strings).
struct NameTable {
    std::map<int, std::string> stages;   // 13 -> "City Escape"
    bool loaded = false;
};
NameTable load_name_table(const std::string& game_root);

// Best-effort friendly name for a character model/motion file (sonicmdl.prs ->
// "Sonic"). Works from the file name alone.
std::string friendly_character_name(const std::string& file_name);

class GameIndex {
public:
    bool scan(const std::string& game_folder);
    const std::vector<AssetEntry>& entries() const { return entries_; }
    const std::string& root() const { return root_; }
    const NameTable& names() const { return names_; }
    std::vector<int> search(const std::string& query, int limit = 2000) const;
    // Indices of entries in a section, optionally filtered by a query.
    std::vector<int> in_section(Section s, const std::string& query = "",
                                int limit = 5000) const;
    int section_count(Section s) const;
    // The flat virtual image of sonic2app.exe (empty if not found), so the viewer
    // can build the ExeModel entries without re-reading the executable.
    const std::vector<uint8_t>& exe_image() const { return exe_image_; }
    uint32_t exe_base() const { return exe_base_; }
private:
    std::string root_;
    std::vector<AssetEntry> entries_;
    NameTable names_;
    std::vector<uint8_t> exe_image_;
    uint32_t exe_base_ = 0;
};

// Tries to locate a Sonic Adventure 2 install (Steam library folders, common
// paths). Returns an empty string if none is found.
std::string autodetect_game_folder();

std::vector<uint8_t> read_file(const std::string& path);
// Reads a file and PRS-decompresses it when it is compressed.
std::vector<uint8_t> load_file(const std::string& path);

// ---------------------------------------------------------------- audio (ADX)
struct AudioClip {
    int sample_rate = 0;
    int channels = 0;
    std::vector<int16_t> pcm;   // interleaved 16-bit signed
    bool valid() const { return sample_rate > 0 && channels > 0 && !pcm.empty(); }
    double seconds() const {
        return channels ? (double)pcm.size() / channels / sample_rate : 0.0;
    }
};
// Decode a CRI ADX (type 2/3/4, standard 2-coefficient ADPCM) into PCM16.
bool decode_adx(const uint8_t* d, size_t n, AudioClip& out);
// Write a 16-bit PCM WAV file.
bool write_wav(const std::string& path, const AudioClip& clip);

// ---------------------------------------------------------------- exe models
// Build a flat virtual image from a PE (sonic2app.exe) so NinjaBlob(image, base,
// /*be=*/false) can resolve the absolute virtual pointers of the little-endian
// Ninja models compiled into the executable.
bool load_pe_image(const std::string& path, std::vector<uint8_t>& image, uint32_t& base);
// Embedded model roots with >= min_tris triangles, most-detailed first.
std::vector<uint32_t> find_exe_models(const std::vector<uint8_t>& image, uint32_t base,
                                      int min_tris, std::vector<int>* tri_counts);

// Highest-level helper: pull every model (and, for characters, every motion)
// out of one asset file.
struct LoadedAsset {
    std::vector<Model> models;
    std::vector<Motion> motions;
    std::vector<Image> textures;
    std::string source;

    // Placed objects for a loaded stage (parsed from its setNNNN_s.bin), in the
    // same world space as the stage geometry. Empty for non-stage assets.
    std::vector<SetObject> objects;

    // Decoded audio, for ADX assets. Invalid for everything else.
    AudioClip audio;

    // Re-pose context for animation playback: the decompressed model data, and
    // per-model the root pointer and animated-node count. The viewer rebuilds a
    // posed mesh with NinjaBlob(anim_data, 0, true).build_model_posed(root, ...).
    std::vector<uint8_t> anim_data;
    std::vector<uint32_t> model_roots;
    std::vector<int> model_anim_count;

    // Motions that apply to model `mi` (node_count == its animated count).
    std::vector<int> motions_for(int mi) const {
        std::vector<int> out;
        if (mi < 0 || mi >= (int)model_anim_count.size()) return out;
        for (int i = 0; i < (int)motions.size(); i++)
            if (motions[i].node_count == model_anim_count[mi]) out.push_back(i);
        return out;
    }
};
bool load_asset(const AssetEntry& e, const GameIndex& idx, LoadedAsset& out,
                std::string* error = nullptr);

// ---------------------------------------------------------------- FBX
struct FbxExportOptions {
    float scale = 1.0f;
    bool write_textures = true;      // dump PNGs beside the .fbx
    bool y_up = true;
};

// Binary FBX 7.4: meshes, materials, textures, skeleton, skin clusters and one
// animation stack per motion.
bool export_fbx(const std::string& out_path,
                const Model& model,
                const std::vector<Image>& textures,
                const std::vector<Motion>& motions,
                const FbxExportOptions& opts,
                std::string* error = nullptr);

bool write_png(const std::string& path, const Image& img);

}  // namespace sa2
