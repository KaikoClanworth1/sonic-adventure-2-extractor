// GameCube REL relocation, LandTable location, and GC "Ginja" model parsing.
//
// Stage geometry in SA2 lives in stgXXD.rel: genuine GameCube REL modules whose
// data is big-endian. After applying the module's own ADDR32 relocations in
// place, every intra-module pointer is a direct file offset (pointer base 0),
// because a REL section's stored offset already IS its file offset. The visual
// models are the GC "Ginja" format; the GX display lists inside them stay
// big-endian on every platform.
#include "sa2core.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <set>
#include <unordered_map>

namespace sa2 {

// ---------------------------------------------------------------- REL
static uint32_t be32(const uint8_t* d, size_t o) {
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
           ((uint32_t)d[o + 2] << 8) | d[o + 3];
}
static void wr32be(uint8_t* d, size_t o, uint32_t v) {
    d[o] = (uint8_t)(v >> 24); d[o + 1] = (uint8_t)(v >> 16);
    d[o + 2] = (uint8_t)(v >> 8); d[o + 3] = (uint8_t)v;
}

enum {
    R_PPC_ADDR32 = 1,
    R_DOLPHIN_NOP = 201,
    R_DOLPHIN_SECTION = 202,
    R_DOLPHIN_END = 203,
};

bool rel_relocate(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    if (!src || n < 0x4C) return false;
    uint32_t version = be32(src, 0x1C);
    if (version < 1 || version > 3) return false;
    uint32_t module_id = be32(src, 0x00);
    uint32_t num_sections = be32(src, 0x0C);
    uint32_t sinfo = be32(src, 0x10);
    uint32_t imp_off = be32(src, 0x28);
    uint32_t imp_size = be32(src, 0x2C);
    if (num_sections == 0 || num_sections > 64) return false;
    if (sinfo + num_sections * 8 > n) return false;
    if (imp_off + imp_size > n || imp_size % 8) return false;

    out.assign(src, src + n);   // relocate in place; the file is its own image

    // section file offsets (low 2 bits are flags)
    std::vector<uint32_t> sec_off(num_sections);
    for (uint32_t i = 0; i < num_sections; i++)
        sec_off[i] = be32(src, sinfo + i * 8) & ~3u;

    // apply only the self-module relocation sub-stream
    for (uint32_t k = 0; k < imp_size / 8; k++) {
        uint32_t mod = be32(src, imp_off + k * 8);
        uint32_t off = be32(src, imp_off + k * 8 + 4);
        if (mod != module_id) continue;      // DOL refs are meaningless here
        size_t p = off;
        uint32_t cursor = 0;
        uint32_t guard = 0;
        while (p + 8 <= n) {
            if (++guard > 8000000u) break;
            uint16_t skip = (uint16_t)((src[p] << 8) | src[p + 1]);
            uint8_t rtype = src[p + 2];
            uint8_t sect = src[p + 3];
            uint32_t addend = be32(src, p + 4);
            p += 8;
            if (rtype == R_DOLPHIN_END) break;
            if (rtype == R_DOLPHIN_SECTION) {
                cursor = (sect < num_sections) ? sec_off[sect] : 0;
                continue;
            }
            cursor += skip;
            if (rtype == R_DOLPHIN_NOP) continue;
            if (rtype == R_PPC_ADDR32) {
                uint32_t sbase = (sect < num_sections) ? sec_off[sect] : 0;
                if (cursor + 4 <= out.size())
                    wr32be(out.data(), cursor, sbase + addend);
            }
            // 16-bit and branch relocations only matter for code we never run
        }
    }
    return true;
}

// ---------------------------------------------------------------- GC model
// GX attribute slots, in index-descriptor order.
enum { GC_POS = 1, GC_NORMAL = 2, GC_COLOR0 = 3, GC_TEX0 = 5 };

struct GCVertexSet {
    uint8_t attribute = 0;
    uint8_t stride = 0;
    uint16_t count = 0;
    uint32_t data_ptr = 0;
    std::vector<float> f;     // pos/normal: xyz triples; uv: uv pairs
    std::vector<uint32_t> c;  // color: RGBA words
};

static void parse_vertex_sets(const NinjaBlob& b, uint32_t ptr,
                              std::unordered_map<int, GCVertexSet>& out) {
    uint32_t o = b.off(ptr);
    size_t n = b.size();
    for (int i = 0; i < 32; i++) {
        if (o + 16 > n) break;
        uint8_t attr = b.raw()[o];
        if (attr == 0xFF || attr == 0) break;
        GCVertexSet vs;
        vs.attribute = attr;
        vs.stride = b.raw()[o + 1];
        vs.count = b.u16(o + 2);
        vs.data_ptr = b.u32(o + 8);
        if (b.ok(vs.data_ptr) && vs.count > 0 && vs.count <= 65535 && vs.stride > 0) {
            uint32_t base = b.off(vs.data_ptr);
            for (int k = 0; k < vs.count; k++) {
                size_t e = base + (size_t)k * vs.stride;
                if (e + vs.stride > n) break;
                if (attr == GC_POS || attr == GC_NORMAL) {
                    vs.f.push_back(b.f32(e));
                    vs.f.push_back(b.f32(e + 4));
                    vs.f.push_back(b.f32(e + 8));
                } else if (attr == GC_COLOR0) {
                    const uint8_t* p = b.raw().data() + e;
                    vs.c.push_back(((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                                   ((uint32_t)p[1] << 8) | p[2]);   // ARGB
                } else if (attr == GC_TEX0) {
                    float u = b.i16(e) / 256.0f;
                    float v = (vs.stride >= 4) ? b.i16(e + 2) / 256.0f : 0.0f;
                    vs.f.push_back(u);
                    vs.f.push_back(v);
                }
            }
        }
        out[attr] = std::move(vs);
        o += 16;
    }
}

// A per-mesh corner is (posIdx, normIdx, colIdx, uvIdx); -1 = attribute absent.
struct Corner { int p, nrm, col, uv; };

struct GCDraw {
    std::vector<std::array<Corner, 3>> tris;
    int texture_id = -1;
    uint32_t ambient = 0xFFFFFFFFu;
    bool use_alpha = false;
    bool ignore_light = false;
    bool env_map = false;
    bool double_sided = false;
    int blend_src = 4, blend_dst = 5;
};

static void emit_tris(uint8_t op, const std::vector<Corner>& c,
                      std::vector<std::array<Corner, 3>>& out) {
    if (op == 0x90) {                          // triangles
        for (size_t i = 0; i + 2 < c.size(); i += 3)
            out.push_back({c[i], c[i + 1], c[i + 2]});
    } else if (op == 0x98) {                   // strip
        for (size_t i = 2; i < c.size(); i++) {
            std::array<Corner, 3> t = (i & 1) ? std::array<Corner, 3>{c[i - 1], c[i - 2], c[i]}
                                              : std::array<Corner, 3>{c[i - 2], c[i - 1], c[i]};
            if (t[0].p != t[1].p && t[1].p != t[2].p && t[0].p != t[2].p)
                out.push_back(t);
        }
    } else if (op == 0xA0) {                    // fan
        for (size_t i = 1; i + 1 < c.size(); i++)
            out.push_back({c[0], c[i], c[i + 1]});
    }
}

static void parse_meshes(const NinjaBlob& b, uint32_t ptr, int count,
                         uint32_t& index_flags, bool translucent,
                         std::vector<GCDraw>& out) {
    size_t n = b.size();
    int cur_tex = -1;
    uint32_t ambient = 0xFFFFFFFFu;
    bool use_alpha = translucent, ignore_light = false, env_map = false;
    int blend_src = 4, blend_dst = 5;
    uint32_t mo = b.off(ptr);
    for (int m = 0; m < count; m++) {
        size_t base = mo + (size_t)m * 16;
        if (base + 16 > n) break;
        uint32_t param_ptr = b.u32(base);
        int32_t param_count = b.i32(base + 4);
        uint32_t prim_ptr = b.u32(base + 8);
        uint32_t prim_size = b.u32(base + 0x0C);

        if (b.ok(param_ptr) && param_count >= 0 && param_count < 100000) {
            uint32_t po = b.off(param_ptr);
            for (int p = 0; p < param_count; p++) {
                size_t pe = po + (size_t)p * 8;
                if (pe + 8 > n) break;
                uint8_t ptype = b.raw()[pe];
                uint32_t data = b.u32(pe + 4);
                switch (ptype) {
                    case 1: index_flags = data; break;                 // IndexFlags
                    case 8: cur_tex = (int)(data & 0xFFFF); break;     // Texture
                    case 5: ambient = data; break;                     // Colour
                    case 2: ignore_light = ((data >> 8) & 0x01) != 0; break;
                    case 4:                                            // BlendAlpha
                        blend_dst = (int)((data >> 8) & 7);
                        blend_src = (int)((data >> 11) & 7);
                        use_alpha = use_alpha || ((data & 0x4000) != 0);
                        break;
                    case 10: env_map = (((data >> 4) & 0xFF) == 1); break;
                }
            }
        }

        // corner layout from the (sticky) index flags
        int reader_slot[13];
        bool reader_is16[13];
        int nread = 0, corner_size = 0;
        static const int kSlots = 13;
        for (int k = 0; k < kSlots; k++) {
            if (!((index_flags >> (2 * k + 1)) & 1)) continue;
            bool is16 = ((index_flags >> (2 * k)) & 1) != 0;
            reader_slot[nread] = k;
            reader_is16[nread] = is16;
            corner_size += is16 ? 2 : 1;
            nread++;
        }
        GCDraw draw;
        draw.texture_id = cur_tex;
        draw.ambient = ambient;
        draw.use_alpha = use_alpha;
        draw.ignore_light = ignore_light;
        draw.env_map = env_map;
        draw.blend_src = blend_src;
        draw.blend_dst = blend_dst;

        if (b.ok(prim_ptr) && corner_size > 0) {
            size_t p = b.off(prim_ptr);
            size_t end = std::min(p + prim_size, n);
            std::vector<Corner> corners;
            while (p + 3 <= end) {
                uint8_t op = b.raw()[p];
                if (op == 0) break;
                uint16_t vc = b.u16(p + 1);
                p += 3;
                corners.clear();
                bool bad = false;
                for (int v = 0; v < vc; v++) {
                    if (p + corner_size > end) { bad = true; break; }
                    Corner cc{-1, -1, -1, -1};
                    for (int rk = 0; rk < nread; rk++) {
                        int val;
                        if (reader_is16[rk]) { val = b.u16(p); p += 2; }
                        else { val = b.raw()[p]; p += 1; }
                        switch (reader_slot[rk]) {
                            case 1: cc.p = val; break;
                            case 2: cc.nrm = val; break;
                            case 3: cc.col = val; break;
                            case 5: cc.uv = val; break;
                        }
                    }
                    corners.push_back(cc);
                }
                if (bad) break;
                emit_tris(op, corners, draw.tris);
            }
        }
        if (!draw.tris.empty()) out.push_back(std::move(draw));
    }
}

bool NinjaBlob::valid_gc_attach(uint32_t ptr) const {
    if (!ok(ptr)) return false;
    size_t a = off(ptr);
    if (a + 0x24 > data_.size()) return false;
    uint32_t vtx = u32(a), skin = u32(a + 4);
    uint32_t opq = u32(a + 8), trn = u32(a + 0x0C);
    uint16_t ocnt = u16(a + 0x10), tcnt = u16(a + 0x12);
    float r = f32(a + 0x20);
    if (skin != 0) return false;
    if (!ok(vtx)) return false;
    if (ocnt == 0 && tcnt == 0) return false;
    if (ocnt > 4096 || tcnt > 4096) return false;
    if (ocnt && !ok(opq)) return false;
    if (tcnt && !ok(trn)) return false;
    if (!std::isfinite(r) || r < 0.0f || r > 1e7f) return false;
    // first vertex set must be a Position/XYZ/Float32 descriptor
    size_t v = off(vtx);
    if (v + 16 > data_.size()) return false;
    if (data_[v] != GC_POS || data_[v + 1] != 12) return false;
    if (u32(v + 4) != 0x41) return false;
    return true;
}

static void xform_pt(const float* m, const float* p, float* o) {
    o[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    o[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    o[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
}
static void xform_dir(const float* m, const float* p, float* o) {
    o[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8];
    o[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9];
    o[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10];
}

// Parse one GC attach and append its geometry to `out`, transformed by `world`.
static void gc_attach_to_parts(const NinjaBlob& b, uint32_t attach_ptr,
                               const float world[16], int node_index,
                               Model& out) {
    size_t a = b.off(attach_ptr);
    uint32_t vtx_ptr = b.u32(a);
    uint32_t opq_ptr = b.u32(a + 8), trn_ptr = b.u32(a + 0x0C);
    uint16_t ocnt = b.u16(a + 0x10), tcnt = b.u16(a + 0x12);

    std::unordered_map<int, GCVertexSet> sets;
    parse_vertex_sets(b, vtx_ptr, sets);
    auto pit = sets.find(GC_POS);
    if (pit == sets.end() || pit->second.f.empty()) return;
    const GCVertexSet& pos = pit->second;
    const GCVertexSet* nrm = sets.count(GC_NORMAL) ? &sets[GC_NORMAL] : nullptr;
    const GCVertexSet* col = sets.count(GC_COLOR0) ? &sets[GC_COLOR0] : nullptr;
    const GCVertexSet* uv = sets.count(GC_TEX0) ? &sets[GC_TEX0] : nullptr;
    int npos = (int)(pos.f.size() / 3);

    std::vector<GCDraw> draws;
    uint32_t index_flags = 0;
    if (ocnt && b.ok(opq_ptr)) parse_meshes(b, opq_ptr, ocnt, index_flags, false, draws);
    if (tcnt && b.ok(trn_ptr)) parse_meshes(b, trn_ptr, tcnt, index_flags, true, draws);

    for (auto& d : draws) {
        MeshPart part;
        part.texture_id = d.texture_id;
        part.node_index = node_index;
        part.double_sided = d.double_sided;
        part.use_alpha = d.use_alpha;
        part.ignore_light = d.ignore_light;
        part.env_map = d.env_map;
        part.blend_src = d.blend_src;
        part.blend_dst = d.blend_dst;
        part.diffuse = d.ambient;
        std::unordered_map<uint64_t, uint32_t> remap;
        auto add = [&](const Corner& c) -> uint32_t {
            int pi = c.p;
            if (pi < 0 || pi >= npos) return UINT32_MAX;
            uint64_t key = ((uint64_t)(uint32_t)pi << 32) ^
                           ((uint32_t)(c.uv + 1) * 2654435761u) ^ (uint32_t)c.col;
            auto it = remap.find(key);
            if (it != remap.end()) return it->second;
            uint32_t idx = (uint32_t)(part.positions.size() / 3);
            remap[key] = idx;
            float wp[3];
            xform_pt(world, &pos.f[(size_t)pi * 3], wp);
            part.positions.insert(part.positions.end(), {wp[0], wp[1], wp[2]});
            if (nrm && c.nrm >= 0 && c.nrm * 3 + 2 < (int)nrm->f.size()) {
                float wn[3];
                xform_dir(world, &nrm->f[(size_t)c.nrm * 3], wn);
                float len = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
                if (len > 1e-8f) { wn[0] /= len; wn[1] /= len; wn[2] /= len; }
                part.normals.insert(part.normals.end(), {wn[0], wn[1], wn[2]});
            } else {
                part.normals.insert(part.normals.end(), {0.0f, 1.0f, 0.0f});
            }
            if (uv && c.uv >= 0 && c.uv * 2 + 1 < (int)uv->f.size()) {
                part.uvs.push_back(uv->f[(size_t)c.uv * 2]);
                part.uvs.push_back(uv->f[(size_t)c.uv * 2 + 1]);
            } else {
                part.uvs.push_back(0.0f);
                part.uvs.push_back(0.0f);
            }
            uint32_t cc = 0xFFFFFFFFu;
            if (col && c.col >= 0 && c.col < (int)col->c.size()) cc = col->c[c.col];
            part.colors.push_back(cc);
            part.vertex_node.push_back(node_index);
            return idx;
        };
        for (auto& t : d.tris) {
            uint32_t i0 = add(t[0]), i1 = add(t[1]), i2 = add(t[2]);
            if (i0 == UINT32_MAX || i1 == UINT32_MAX || i2 == UINT32_MAX) continue;
            part.indices.insert(part.indices.end(), {i0, i1, i2});
        }
        if (!part.indices.empty()) out.parts.push_back(std::move(part));
    }
}

bool NinjaBlob::build_gc_model(uint32_t root, Model& out) const {
    out.nodes = read_tree(root);
    if (out.nodes.empty()) return false;
    for (const auto& nd : out.nodes) {
        if (!valid_gc_attach(nd.attach_ptr)) continue;
        gc_attach_to_parts(*this, nd.attach_ptr, nd.world, nd.index, out);
    }
    return !out.parts.empty();
}

// ---------------------------------------------------------------- LandTable
static std::string read_cstr(const NinjaBlob& b, uint32_t ptr) {
    if (!b.ok(ptr)) return "";
    size_t o = b.off(ptr), e = o;
    const auto& d = b.raw();
    while (e < d.size() && d[e] && e - o < 64) e++;
    return std::string((const char*)d.data() + o, e - o);
}

std::vector<LandTableInfo> find_landtables(const NinjaBlob& b) {
    std::vector<LandTableInfo> out;
    const auto& d = b.raw();
    size_t n = d.size();
    for (size_t o = 0; o + 0x20 <= n; o += 4) {
        int16_t colcount = b.i16(o);
        if (colcount <= 0 || colcount > 20000) continue;
        int16_t vis = b.i16(o + 2);
        if (vis < -1) continue;
        float clip = b.f32(o + 0x0C);
        if (!(clip >= 1.0f && clip <= 1e7f)) continue;
        uint32_t colptr = b.u32(o + 0x10);
        if (!b.ok(colptr)) continue;
        uint32_t texname_ptr = b.u32(o + 0x18);
        if (texname_ptr && !b.ok(texname_ptr)) continue;
        size_t c = b.off(colptr);
        if (c + (size_t)colcount * 0x20 > n) continue;
        int checks = std::min<int>(colcount, 8), good = 0;
        for (int k = 0; k < checks; k++) {
            size_t e = c + (size_t)k * 0x20;
            float r = b.f32(e + 0x0C);
            uint32_t obj = b.u32(e + 0x10);
            if (!(r >= 0.0f && r < 1e7f)) break;
            if (obj && !b.ok(obj)) break;
            good++;
        }
        if (good < checks) continue;
        // Reject false positives (garbage in the relocation/import tables that
        // happens to look like a COL array) by requiring that a fair share of
        // the visual COLs actually resolve to a real GC or Chunk model.
        int sampled = 0, hits = 0;
        for (int k = 0; k < colcount && sampled < 12; k++) {
            bool visual = (vis < 0) || (k < vis);
            if (!visual) continue;
            uint32_t obj = b.u32(c + (size_t)k * 0x20 + 0x10);
            if (!b.ok(obj)) continue;
            sampled++;
            auto tree = b.read_tree(obj);
            for (const auto& nd : tree) {
                if (!b.ok(nd.attach_ptr)) continue;
                if (b.valid_gc_attach(nd.attach_ptr) || b.valid_attach(nd.attach_ptr))
                    hits++;
                break;
            }
        }
        if (sampled == 0 || hits * 2 < sampled) continue;
        LandTableInfo lt;
        lt.addr = b.base() + (uint32_t)o;
        lt.col_count = colcount;
        lt.visible_count = vis;
        lt.clip = clip;
        lt.col_ptr = colptr;
        lt.texture_name = read_cstr(b, texname_ptr);
        out.push_back(lt);
    }
    std::sort(out.begin(), out.end(),
              [](const LandTableInfo& a, const LandTableInfo& c) {
                  return a.col_count > c.col_count;
              });
    // Distinct landtables have distinct COL arrays; drop duplicates that alias
    // the same array (keeping the first, i.e. the largest).
    std::vector<LandTableInfo> uniq;
    std::set<uint32_t> seen;
    for (auto& lt : out) {
        if (seen.insert(lt.col_ptr).second) uniq.push_back(lt);
    }
    return uniq;
}

// ---------------------------------------------------------------- SET files
bool parse_set_file(const uint8_t* d, size_t n, std::vector<SetObject>& out) {
    if (!d || n < 0x20) return false;
    uint32_t count = be32(d, 0);
    // SET files are big-endian; sanity-check the record count against the size.
    if ((size_t)0x20 + (size_t)count * 0x20 != n) {
        // some tools store little-endian; try that before giving up
        uint32_t le = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                      ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
        if ((size_t)0x20 + (size_t)le * 0x20 == n) count = le;
        else return false;
    }
    if (count > 100000) return false;
    auto f32be = [&](size_t o) {
        uint32_t v = be32(d, o);
        float f; memcpy(&f, &v, 4); return f;
    };
    auto s16be = [&](size_t o) { return (int16_t)((d[o] << 8) | d[o + 1]); };
    out.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        size_t o = 0x20 + (size_t)i * 0x20;
        SetObject s;
        s.id = (uint16_t)((d[o] << 8) | d[o + 1]);
        s.rot[0] = s16be(o + 2);
        s.rot[1] = s16be(o + 4);
        s.rot[2] = s16be(o + 6);
        s.pos[0] = f32be(o + 8);
        s.pos[1] = f32be(o + 12);
        s.pos[2] = f32be(o + 16);
        s.scale[0] = f32be(o + 20);
        s.scale[1] = f32be(o + 24);
        s.scale[2] = f32be(o + 28);
        out.push_back(s);
    }
    return true;
}

bool build_landtable(const NinjaBlob& b, const LandTableInfo& lt, Model& out) {
    size_t cbase = b.off(lt.col_ptr);

    // A landtable is uniformly one model format. Sample the first visual COLs to
    // decide: SA2B stages use GC "Ginja", but some (e.g. City Hall, Sky Rail)
    // ship Dreamcast Chunk models inside the REL.
    int gc_votes = 0, chunk_votes = 0;
    for (int i = 0; i < lt.col_count && (gc_votes + chunk_votes) < 12; i++) {
        size_t e = cbase + (size_t)i * 0x20;
        if (e + 0x20 > b.size()) break;
        bool visual = (lt.visible_count < 0) || (i < lt.visible_count);
        if (!visual) continue;
        uint32_t obj = b.u32(e + 0x10);
        if (!b.ok(obj)) continue;
        auto tree = b.read_tree(obj);
        for (const auto& nd : tree) {
            if (!b.ok(nd.attach_ptr)) continue;
            if (b.valid_gc_attach(nd.attach_ptr)) gc_votes++;
            else if (b.valid_attach(nd.attach_ptr)) chunk_votes++;
            break;
        }
    }
    bool use_gc = gc_votes >= chunk_votes;

    for (int i = 0; i < lt.col_count; i++) {
        size_t e = cbase + (size_t)i * 0x20;
        if (e + 0x20 > b.size()) break;
        uint32_t obj = b.u32(e + 0x10);
        if (!b.ok(obj)) continue;
        bool visual = (lt.visible_count < 0) || (i < lt.visible_count);
        if (!visual) continue;   // collision-only Basic model, skip
        Model m;
        bool built = use_gc ? b.build_gc_model(obj, m) : b.build_model(obj, m);
        if (!built) built = use_gc ? b.build_model(obj, m) : b.build_gc_model(obj, m);
        if (built) {
            int nbase = (int)out.nodes.size();
            for (auto nd : m.nodes) {
                if (nd.parent >= 0) nd.parent += nbase;
                nd.index += nbase;
                out.nodes.push_back(nd);
            }
            for (auto& part : m.parts) {
                part.node_index += nbase;
                for (auto& vn : part.vertex_node) vn += nbase;
                out.parts.push_back(std::move(part));
            }
        }
        if (out.parts.size() > 200000) break;
    }
    return !out.parts.empty();
}

}  // namespace sa2
