// Chao World stage geometry (ChaoStgLobby / Karate / Kinder .prs).
//
// These files are a packed stream of meshes with NO pointer tables. Each mesh is
//   [ ~44-byte material header ending in a u16 group count ]
//   [ group count triangle-strip groups ]
//   [ zero padding up to the vertex block's alignment ]
//   [ 24-byte interleaved position+normal vertex block ]
// A group is a triangle STRIP: { s16 count, count corners }, each corner three
// u16s (positionIdx, normalIdx, uvIdx) in GameCube display-list style; a negative
// count reverses the winding. The strip references the position column into the
// vertex block, whose length is maxPositionIndex + 1. Big-endian, pointer base 0.
//
// We can't rely on a table, so we anchor on the geometry: enumerate every maximal
// run of pos+normal vertices, scan for each candidate group count, strip-parse it,
// map it to the vertex block that follows (after alignment padding), and keep the
// parse producing the most triangles per block. A final edge-length filter drops
// incoherent (garbage) parses. This mirrors tools/chaobuild8.py, validated in
// Blender against ChaoStgLobby (10 meshes, clean arched walls + platform).
//
// The Hero/Dark gardens use a different, pos-only display-list format and are not
// handled here.
#include "sa2core.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace sa2 {

namespace {

struct BE {
    const uint8_t* d;
    size_t n;
    uint16_t u16(size_t o) const { return (uint16_t)((d[o] << 8) | d[o + 1]); }
    int16_t  i16(size_t o) const { return (int16_t)u16(o); }
    float f32(size_t o) const {
        uint32_t v = ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
                     ((uint32_t)d[o + 2] << 8) | d[o + 3];
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    bool isvert(size_t o) const {
        if (o + 24 > n) return false;
        float px = f32(o), py = f32(o + 4), pz = f32(o + 8);
        float nx = f32(o + 12), ny = f32(o + 16), nz = f32(o + 20);
        float ln = nx * nx + ny * ny + nz * nz;
        return ln > 0.9f && ln < 1.1f &&
               std::fabs(px) < 5000 && std::fabs(py) < 5000 && std::fabs(pz) < 5000;
    }
};

// A corner is { u16 positionIdx, u16 u, u16 v }. The trailing two are NOT
// indices: they are the corner's texture coordinates in SA's fixed-point form,
// decoded by dividing by 255 (so 0/127/255 give 0.0/0.5/1.0 exactly, and values
// above 255 tile) - the same Decode255 the SA model tools use. That is why the
// file contains no UV array to point at.
struct Corner { int p; uint16_t u, v; };
struct Group { bool rev; std::vector<Corner> corners; };

// Parse the strip groups starting at a group-count offset. Returns false unless
// every group parses within bounds.
bool parse_groups(const BE& b, size_t G, std::vector<Group>& groups, int& maxpos,
                  size_t& end) {
    int gc = b.u16(G);
    if (gc < 1 || gc > 6000) return false;
    size_t p = G + 2;
    groups.clear();
    maxpos = -1;
    for (int g = 0; g < gc; g++) {
        if (p + 2 > b.n) return false;
        int sc = b.i16(p);
        p += 2;
        int cnt = std::abs(sc);
        if (cnt < 1 || cnt > 6000 || p + (size_t)cnt * 6 > b.n) return false;
        Group grp;
        grp.rev = sc < 0;
        grp.corners.reserve(cnt);
        for (int v = 0; v < cnt; v++) {
            Corner c{b.u16(p), b.u16(p + 2), b.u16(p + 4)};
            p += 6;
            grp.corners.push_back(c);
            if (c.p > maxpos) maxpos = c.p;
        }
        groups.push_back(std::move(grp));
    }
    end = p;
    return true;
}

void strip_tris(const std::vector<Group>& groups, std::vector<Corner>& tris) {
    for (const auto& g : groups) {
        const auto& c = g.corners;
        for (size_t k = 0; k + 2 < c.size(); k++) {
            Corner a = c[k], b2 = c[k + 1], cc = c[k + 2];
            if (((k % 2 == 1) ? 1 : 0) ^ (g.rev ? 1 : 0)) std::swap(a, b2);
            if (a.p != b2.p && b2.p != cc.p && a.p != cc.p) {
                tris.push_back(a); tris.push_back(b2); tris.push_back(cc);
            }
        }
    }
}

// Each mesh's material block opens with a word of the form 0x25xx00tt / 0x21xx00tt
// (it sits a fixed distance before the group count, after the node's 1,1,1/0,0,0
// scale+rotation). The low half is the mesh's texture index.
int texture_index_before(const BE& b, size_t group_count_off) {
    size_t lo = group_count_off > 0x40 ? group_count_off - 0x40 : 0;
    for (size_t a = group_count_off - 4; a > lo; a -= 2) {
        if (a + 4 > b.n) continue;
        uint8_t top = b.d[a];
        if ((top == 0x25 || top == 0x21) && b.d[a + 2] == 0x00) {
            int t = b.d[a + 3];
            if (t < 256) return t;
        }
    }
    return -1;
}

}  // namespace

bool load_chao_stage(const std::vector<uint8_t>& data, Model& out) {
    BE b{data.data(), data.size()};
    size_t n = data.size();
    if (n < 64) return false;

    // 1) maximal vertex blocks (start -> vertex count). Back- and forward-extend
    //    so a scan that starts a few bytes off the grid doesn't split a block.
    std::vector<std::pair<size_t, int>> blocks;
    {
        size_t o = 0;
        while (o + 24 <= n) {
            if (b.isvert(o)) {
                size_t s = o;
                while (s >= 24 && b.isvert(s - 24)) s -= 24;
                size_t e = s;
                while (b.isvert(e)) e += 24;
                blocks.emplace_back(s, (int)((e - s) / 24));
                o = std::max(e, o + 4);
            } else {
                o += 4;
            }
        }
        std::sort(blocks.begin(), blocks.end());
        blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    }
    if (blocks.empty()) return false;
    std::vector<size_t> starts;
    std::map<size_t, int> size_of;
    for (auto& pr : blocks) { starts.push_back(pr.first); size_of[pr.first] = pr.second; }

    auto block_for = [&](size_t p, int need) -> size_t {
        // nearest block start in [p, p+4096] big enough for `need` verts; isvert
        // undercounts blocks, so allow a 20% shortfall and trust the index count.
        size_t i = std::lower_bound(starts.begin(), starts.end(), p) - starts.begin();
        while (i < starts.size() && starts[i] <= p + 4096) {
            size_t V = starts[i];
            if (size_of[V] >= need * 0.8 && V + (size_t)need * 24 <= n) return V;
            i++;
        }
        return SIZE_MAX;
    };

    // 2) scan every group-count candidate, keep the most-triangles parse per block
    struct Best { size_t ntris; int need; std::vector<Corner> tris; int tex; };
    std::map<size_t, Best> best;
    for (size_t G = 4; G + 8 < n; G += 2) {
        int gc = b.u16(G);
        if (gc < 1 || gc > 6000) continue;
        int first = std::abs(b.i16(G + 2));           // first strip count may be negative
        if (first < 1 || first > 6000) continue;
        std::vector<Group> groups;
        int maxpos = 0;
        size_t end = 0;
        if (!parse_groups(b, G, groups, maxpos, end)) continue;
        if (maxpos < 3 || groups.empty()) continue;
        int need = maxpos + 1;
        size_t V = block_for((end + 3) & ~size_t(3), need);
        if (V == SIZE_MAX) continue;
        std::vector<Corner> tris;
        strip_tris(groups, tris);
        auto it = best.find(V);
        if (it == best.end() || tris.size() > it->second.ntris)
            best[V] = Best{tris.size(), need, std::move(tris), texture_index_before(b, G)};
    }

    // 3) emit one MeshPart per block, dropping incoherent parses
    for (auto& kv : best) {
        size_t V = kv.first;
        std::vector<Corner>& tris = kv.second.tris;

        // coherence filter: real strips have short, uniform edges; a garbage parse
        // links distant verts. Drop long-edge triangles; reject the whole mesh if
        // most of its triangles are long.
        if (tris.size() >= 24) {
            std::vector<float> elen;
            elen.reserve(tris.size() / 3);
            auto dist = [&](int a, int c) {
                float dx = b.f32(V + a * 24) - b.f32(V + c * 24);
                float dy = b.f32(V + a * 24 + 4) - b.f32(V + c * 24 + 4);
                float dz = b.f32(V + a * 24 + 8) - b.f32(V + c * 24 + 8);
                return std::sqrt(dx * dx + dy * dy + dz * dz);
            };
            for (size_t t = 0; t + 2 < tris.size(); t += 3)
                elen.push_back(std::max({dist(tris[t].p, tris[t + 1].p),
                                         dist(tris[t + 1].p, tris[t + 2].p),
                                         dist(tris[t].p, tris[t + 2].p)}));
            std::vector<float> sorted = elen;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            float med = sorted[sorted.size() / 2];
            float thresh = std::max(med * 8.0f, 1e-3f);
            std::vector<Corner> keep;
            for (size_t e = 0; e < elen.size(); e++)
                if (elen[e] <= thresh) {
                    keep.push_back(tris[e * 3]); keep.push_back(tris[e * 3 + 1]);
                    keep.push_back(tris[e * 3 + 2]);
                }
            if (keep.size() < tris.size() / 2) continue;   // mostly garbage -> drop
            tris.swap(keep);
        }
        if (tris.empty()) continue;

        // UVs live on the corner, so a vertex is (position, u, v): split shared
        // positions that carry different texture coordinates.
        MeshPart part;
        std::map<uint64_t, uint32_t> seen;
        for (const Corner& c : tris) {
            uint64_t key = ((uint64_t)(uint32_t)c.p << 32) | ((uint32_t)c.u << 16) | c.v;
            auto it = seen.find(key);
            if (it == seen.end()) {
                uint32_t vi = (uint32_t)(part.positions.size() / 3);
                size_t e = V + (size_t)c.p * 24;
                part.positions.push_back(b.f32(e));
                part.positions.push_back(b.f32(e + 4));
                part.positions.push_back(b.f32(e + 8));
                part.normals.push_back(b.f32(e + 12));
                part.normals.push_back(b.f32(e + 16));
                part.normals.push_back(b.f32(e + 20));
                part.uvs.push_back(c.u / 255.0f);
                part.uvs.push_back(c.v / 255.0f);
                it = seen.emplace(key, vi).first;
            }
            part.indices.push_back(it->second);
        }
        part.texture_id = kv.second.tex;
        part.double_sided = true;   // Chao stage walls are single-sided in-game
        part.node_index = (int)out.parts.size();
        out.parts.push_back(std::move(part));
    }
    if (out.parts.empty()) return false;
    out.nodes.push_back(Node{});    // one identity node so the model is well-formed
    return true;
}

// ---------------------------------------------------------------------------
// GC "Ginja" Chao areas (everything except the Lobby): once the module's
// self-relocations are applied the ordinary GC model walker can read them, so we
// only need to locate the node roots and pull the texture-list names.

namespace {

// A node whose attach is a valid GC attach, and which nothing else references as
// a child/sibling, is a model root.
std::vector<uint32_t> find_gc_roots(const NinjaBlob& b) {
    size_t n = b.size();
    std::vector<uint32_t> nodes;
    for (size_t a = 0; a + 0x34 <= n; a += 4) {
        uint32_t attach = b.u32(a + 4);
        if (!b.ok(attach) || !b.valid_gc_attach(attach)) continue;
        uint32_t child = b.u32(a + 0x2C), sib = b.u32(a + 0x30);
        if (child && !b.ok(child)) continue;
        if (sib && !b.ok(sib)) continue;
        nodes.push_back((uint32_t)a);
    }
    // drop anything reachable as a child/sibling: those are not roots
    std::vector<uint32_t> child_of;
    for (uint32_t a : nodes) {
        uint32_t c = b.u32(a + 0x2C), s = b.u32(a + 0x30);
        if (c) child_of.push_back(c);
        if (s) child_of.push_back(s);
    }
    std::sort(child_of.begin(), child_of.end());
    std::vector<uint32_t> roots;
    for (uint32_t a : nodes)
        if (!std::binary_search(child_of.begin(), child_of.end(), a))
            roots.push_back(a);
    return roots;
}

// NJS_TEXLIST: { NJS_TEXNAME* names, u32 count }, each name { char* , 0, 0 }.
// Returns the longest well-formed list, which is the stage's own.
std::vector<std::string> find_texture_names(const NinjaBlob& b) {
    size_t n = b.size();
    const uint8_t* d = b.raw().data();
    std::vector<std::string> best;
    auto cstr = [&](uint32_t p, std::string& s) {
        if (!b.ok(p)) return false;
        size_t o = b.off(p);
        s.clear();
        while (o < n && d[o] && s.size() < 40) {
            char c = (char)d[o++];
            if (c < 32 || c > 126) return false;
            s.push_back(c);
        }
        return s.size() >= 3;
    };
    for (size_t a = 0; a + 8 <= n; a += 4) {
        uint32_t p = b.u32(a), cnt = b.u32(a + 4);
        if (!b.ok(p) || cnt == 0 || cnt > 512) continue;
        if (b.off(p) + (size_t)cnt * 12 > n) continue;
        std::vector<std::string> names;
        bool ok = true;
        for (uint32_t i = 0; i < cnt && ok; i++) {
            std::string s;
            if (!cstr(b.u32(b.off(p) + (size_t)i * 12), s)) ok = false;
            else names.push_back(s);
        }
        if (ok && names.size() > best.size()) best = std::move(names);
    }
    return best;
}

}  // namespace

bool load_chao_stage_gc(const std::vector<uint8_t>& relocated, Model& out,
                        std::vector<std::string>& texture_names) {
    NinjaBlob b(relocated, 0, /*big_endian=*/true);
    texture_names = find_texture_names(b);
    for (uint32_t root : find_gc_roots(b)) {
        Model m;
        if (!b.build_gc_model(root, m)) continue;
        int nbase = (int)out.nodes.size();
        for (auto nd : m.nodes) {
            if (nd.parent >= 0) nd.parent += nbase;
            nd.index += nbase;
            out.nodes.push_back(nd);
        }
        for (auto& part : m.parts) {
            part.node_index += nbase;
            out.parts.push_back(std::move(part));
        }
        if (out.parts.size() > 8000) break;
    }
    return !out.parts.empty();
}

}  // namespace sa2
