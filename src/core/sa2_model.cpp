// NJS_OBJECT trees, Ninja chunk models and NJS_MOTION animation.
//
// Endianness note: SA2B/SA2PC data is the original little-endian Dreamcast data
// converted to big-endian *per declared field size*, matching NJS_CNK_MODEL:
//     typedef struct { Sint32 *vlist; Sint16 *plist; NJS_POINT3 center; Float r; }
// so the vertex list is byte-swapped in 4-byte units and the polygon list in
// 2-byte units. Concretely, for a big-endian file:
//     vertex chunk header : BE u32 W -> head = W & 0xFF, flag = (W>>8)&0xFF,
//                                       size = W >> 16   (counted in u32 words)
//     vertex index header : BE u32 W -> indexOffset = W & 0xFFFF, nb = W >> 16
//     poly chunk header   : BE u16 w -> head = w & 0xFF, flag = w >> 8
//                           BE u16   -> size (counted in u16 words)
#include "sa2core.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <set>
#include <unordered_map>

namespace sa2 {

static constexpr float kBams = 6.28318530717958647692f / 65536.0f;
static constexpr uint32_t kOne = 0x3F800000u;
static constexpr size_t kObjSize = 0x34;
static constexpr size_t kAttachSize = 0x18;

// ------------------------------------------------------------ chunk enums
static bool is_vertex_chunk(int t) { return t >= 32 && t <= 50; }
static bool is_strip_chunk(int t) { return t >= 64 && t <= 75; }
static bool is_poly_chunk(int t) {
    return t == 0 || t == 255 || (t >= 1 && t <= 5) || t == 8 || t == 9 ||
           (t >= 16 && t <= 31) || (t >= 56 && t <= 58) || is_strip_chunk(t);
}
static bool chunk_has_normal(int t) {
    return t == 33 || (t >= 41 && t <= 47);
}
// Fallback words-per-vertex; the real stride is derived from the chunk itself.
static int fallback_stride(int t) {
    if (t == 33) return 8;
    if (t == 41) return 6;
    if (t >= 42 && t <= 47) return 7;
    if (t == 49 || t == 50) return 5;
    return 4;
}

// ------------------------------------------------------------ NinjaBlob
NinjaBlob::NinjaBlob(std::vector<uint8_t> d, uint32_t base, bool be)
    : data_(std::move(d)), base_(base), be_(be) {}

uint32_t NinjaBlob::u32(size_t o) const {
    if (o + 4 > data_.size()) return 0;
    const uint8_t* d = data_.data() + o;
    return be_ ? ((uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 |
                  (uint32_t)d[2] << 8 | d[3])
               : ((uint32_t)d[3] << 24 | (uint32_t)d[2] << 16 |
                  (uint32_t)d[1] << 8 | d[0]);
}
int32_t NinjaBlob::i32(size_t o) const { return (int32_t)u32(o); }
uint16_t NinjaBlob::u16(size_t o) const {
    if (o + 2 > data_.size()) return 0;
    const uint8_t* d = data_.data() + o;
    return be_ ? (uint16_t)((d[0] << 8) | d[1]) : (uint16_t)((d[1] << 8) | d[0]);
}
int16_t NinjaBlob::i16(size_t o) const { return (int16_t)u16(o); }
float NinjaBlob::f32(size_t o) const {
    uint32_t v = u32(o);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// ------------------------------------------------------------ matrices
static void mat_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}
static void mat_mul(const float* a, const float* b, float* r) {
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(r, t, sizeof(t));
}
// Local transform = Scale * Rotation * Translation (row-vector convention).
static void mat_from_srt(const float pos[3], const int32_t rot[3],
                         const float scl[3], bool zyx, float* m) {
    float rx = rot[0] * kBams, ry = rot[1] * kBams, rz = rot[2] * kBams;
    float cx = cosf(rx), sx = sinf(rx);
    float cy = cosf(ry), sy = sinf(ry);
    float cz = cosf(rz), sz = sinf(rz);
    float m11, m12, m13, m21, m22, m23, m31, m32, m33;
    if (zyx) {
        // SA2's "ZYX" flag actually evaluates Z * X * Y.
        m11 = (sy * sx * sz) + (cy * cz); m12 = cx * sz; m13 = (cy * sx * sz) - (sy * cz);
        m21 = (sy * sx * cz) - (cy * sz); m22 = cx * cz; m23 = (cy * sx * cz) + (sy * sz);
        m31 = sy * cx;                    m32 = -sx;     m33 = cy * cx;
    } else {
        m11 = cz * cy;                    m12 = sz * cy;                    m13 = -sy;
        m21 = (cz * sy * sx) - (sz * cx); m22 = (sz * sy * sx) + (cz * cx); m23 = cy * sx;
        m31 = (cz * sy * cx) + (sz * sx); m32 = (sz * sy * cx) - (cz * sx); m33 = cy * cx;
    }
    m[0] = scl[0] * m11; m[1] = scl[0] * m12; m[2] = scl[0] * m13; m[3] = 0;
    m[4] = scl[1] * m21; m[5] = scl[1] * m22; m[6] = scl[1] * m23; m[7] = 0;
    m[8] = scl[2] * m31; m[9] = scl[2] * m32; m[10] = scl[2] * m33; m[11] = 0;
    m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2]; m[15] = 1;
}
static void xform_point(const float* m, const float* p, float* o) {
    o[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
    o[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
    o[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
}
static void xform_dir(const float* m, const float* p, float* o) {
    o[0] = p[0] * m[0] + p[1] * m[4] + p[2] * m[8];
    o[1] = p[0] * m[1] + p[1] * m[5] + p[2] * m[9];
    o[2] = p[0] * m[2] + p[1] * m[6] + p[2] * m[10];
}

// ------------------------------------------------------------ tree walk
static bool finite_ok(float v) { return std::isfinite(v) && fabsf(v) < 1e6f; }

std::vector<Node> NinjaBlob::read_tree(uint32_t root) const {
    std::vector<Node> out;
    std::set<uint32_t> seen;
    // explicit stack: (ptr, parent index, depth)
    struct Frame { uint32_t p; int parent; int depth; };
    std::vector<Frame> stack;
    stack.push_back({root, -1, 0});
    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();
        if (!ok(f.p) || f.depth > 96 || out.size() > 8192) continue;
        if (seen.count(f.p)) continue;
        size_t o = off(f.p);
        if (o + kObjSize > data_.size()) continue;

        float pos[3] = {f32(o + 8), f32(o + 12), f32(o + 16)};
        float scl[3] = {f32(o + 0x20), f32(o + 0x24), f32(o + 0x28)};
        bool sane = true;
        for (int i = 0; i < 3; i++)
            if (!finite_ok(pos[i]) || !finite_ok(scl[i])) sane = false;
        if (!sane) continue;   // a garbage node would poison every descendant

        seen.insert(f.p);
        Node nd;
        nd.ptr = f.p;
        nd.flags = u32(o);
        nd.attach_ptr = u32(o + 4);
        memcpy(nd.pos, pos, sizeof(pos));
        nd.rot[0] = i32(o + 0x14); nd.rot[1] = i32(o + 0x18); nd.rot[2] = i32(o + 0x1C);
        memcpy(nd.scale, scl, sizeof(scl));
        nd.parent = f.parent;
        nd.index = (int)out.size();
        out.push_back(nd);
        int me = nd.index;

        uint32_t child = u32(o + 0x2C);
        uint32_t sibling = u32(o + 0x30);
        // push sibling first so the child subtree is emitted depth-first
        if (ok(sibling)) stack.push_back({sibling, f.parent, f.depth});
        if (ok(child)) stack.push_back({child, me, f.depth + 1});
    }
    // world matrices (parents always precede children in this ordering)
    for (auto& nd : out) {
        float local[16];
        mat_from_srt(nd.pos, nd.rot, nd.scale, (nd.flags & 0x20) != 0, local);
        if (nd.parent < 0) memcpy(nd.world, local, sizeof(local));
        else mat_mul(local, out[nd.parent].world, nd.world);
    }
    return out;
}

bool NinjaBlob::valid_attach(uint32_t ptr) const {
    if (!ok(ptr)) return false;
    size_t a = off(ptr);
    if (a + kAttachSize > data_.size()) return false;
    uint32_t vl = u32(a), pl = u32(a + 4);
    if (vl == 0 && pl == 0) return false;
    if (vl) {
        if (!ok(vl)) return false;
        int h = (int)(u32(off(vl)) & 0xFF);
        if (!is_vertex_chunk(h) && h != 255) return false;
    }
    if (pl) {
        if (!ok(pl)) return false;
        int h = (int)(u16(off(pl)) & 0xFF);
        if (!is_poly_chunk(h)) return false;
    }
    float r = f32(a + 20);
    if (!std::isfinite(r) || r < 0.0f || r > 1e7f) return false;
    return true;
}

// ------------------------------------------------------------ chunk parsing
struct CVert {
    float pos[3];
    float nrm[3];
    bool has_nrm = false;
    uint32_t diffuse = 0xFFFFFFFFu;
};

struct CStrip {
    std::vector<uint16_t> idx;
    std::vector<float> uv;     // 2 per index, empty when the chunk has no UVs
    bool reversed = false;
};
struct CPoly {
    std::vector<CStrip> strips;
    int texture_id = -1;
    uint8_t flags = 0;
    uint32_t diffuse = 0xFFFFFFFFu;
    bool use_alpha = false;
};

// Parse a vertex chunk stream into cache-index -> vertex.
static void parse_vertices(const NinjaBlob& b, size_t off,
                           std::unordered_map<int, CVert>& out) {
    size_t n = b.size();
    for (int guard = 0; guard < 4096; guard++) {
        if (off + 4 > n) return;
        uint32_t w = b.u32(off);
        int head = (int)(w & 0xFF);
        int size = (int)(w >> 16);
        if (head == 255) return;
        if (head == 0) { off += 4; continue; }
        if (!is_vertex_chunk(head)) return;
        if (off + 8 > n) return;
        uint32_t w2 = b.u32(off + 4);
        int index_off = (int)(w2 & 0xFFFF);
        int nb = (int)(w2 >> 16);
        size_t body = off + 8;

        // `size` counts u32 words after the 4-byte header; the first of those is
        // the index header, so the payload is (size - 1) words. Deriving the
        // stride from the chunk is self-validating and beats a hardcoded table.
        int stride = (nb > 0 && size >= 1) ? (size - 1) / nb : fallback_stride(head);
        if (stride >= 3 && nb > 0) {
            size_t payload_end = body + (size_t)(size - 1) * 4;
            for (int i = 0; i < nb; i++) {
                size_t vo = body + (size_t)i * stride * 4;
                if (vo + 12 > n || vo + 12 > payload_end) break;
                CVert v;
                v.pos[0] = b.f32(vo);
                v.pos[1] = b.f32(vo + 4);
                v.pos[2] = b.f32(vo + 8);
                if (chunk_has_normal(head) && stride >= 6) {
                    size_t no = vo + (head == 33 ? 16 : 12);
                    if (no + 12 <= n) {
                        v.nrm[0] = b.f32(no);
                        v.nrm[1] = b.f32(no + 4);
                        v.nrm[2] = b.f32(no + 8);
                        v.has_nrm = true;
                    }
                    if (head == 42 && stride >= 7) v.diffuse = b.u32(vo + 24);
                } else if (head == 35 && stride >= 4) {
                    v.diffuse = b.u32(vo + 12);
                }
                out[index_off + i] = v;
            }
        }
        off += 4 + (size_t)size * 4;
    }
}

static void parse_polys(const NinjaBlob& b, size_t off, std::vector<CPoly>& out) {
    size_t n = b.size();
    int cur_tex = -1;
    uint32_t diffuse = 0xFFFFFFFFu;
    bool use_alpha = false;
    for (int guard = 0; guard < 8192; guard++) {
        if (off + 2 > n) return;
        uint16_t w = b.u16(off);
        int head = (int)(w & 0xFF);
        int flag = (int)(w >> 8);
        if (head == 255) return;
        if (head == 0) { off += 2; continue; }
        if (head >= 1 && head <= 5) { off += 2; continue; }
        if (head == 8 || head == 9) {
            cur_tex = (int)(b.u16(off + 2) & 0x1FFF);
            off += 4;
            continue;
        }
        if (head >= 16 && head <= 31) {
            int size = (int)b.u16(off + 2);
            size_t p = off + 4;
            int t = head - 16;
            // ARGB colours, stored as u32 but swapped as u16 pairs
            auto argb = [&](size_t q) -> uint32_t {
                uint32_t hi = b.u16(q), lo = b.u16(q + 2);
                return (lo << 16) | hi;
            };
            if (t & 1) { diffuse = argb(p); p += 4; }
            if (t & 2) { p += 4; }       // ambient
            if (t & 4) { p += 4; }       // specular
            use_alpha = (flag & 0x08) != 0;
            off += 4 + (size_t)size * 2;
            continue;
        }
        if (head >= 56 && head <= 58) {
            int size = (int)b.u16(off + 2);
            off += 4 + (size_t)size * 2;
            continue;
        }
        if (is_strip_chunk(head)) {
            int size = (int)b.u16(off + 2);
            size_t end = off + 4 + (size_t)size * 2;
            size_t limit = std::min(end, n);
            CPoly poly;
            poly.texture_id = cur_tex;
            poly.flags = (uint8_t)flag;
            poly.diffuse = diffuse;
            poly.use_alpha = use_alpha;
            uint16_t hdr = b.u16(off + 4);
            int nb_strip = hdr & 0x3FFF;
            int user_flags = (hdr >> 14) & 3;
            size_t p = off + 6;
            bool has_uv = (head == 65 || head == 66 || head == 68 || head == 69 ||
                           head == 71 || head == 72 || head == 74 || head == 75);
            bool uv_hi = (head == 66 || head == 69 || head == 72 || head == 75);
            bool has_nrm = (head >= 67 && head <= 69);
            bool has_col = (head >= 70 && head <= 72);
            for (int s = 0; s < nb_strip; s++) {
                if (p + 2 > limit) break;
                int16_t ln = b.i16(p); p += 2;
                CStrip st;
                st.reversed = ln < 0;
                int cnt = ln < 0 ? -ln : ln;
                for (int k = 0; k < cnt; k++) {
                    if (p + 2 > limit) break;
                    st.idx.push_back(b.u16(p)); p += 2;
                    if (has_uv) {
                        if (p + 4 > limit) break;
                        float d = uv_hi ? 1024.0f : 256.0f;
                        st.uv.push_back(b.i16(p) / d);
                        st.uv.push_back(b.i16(p + 2) / d);
                        p += 4;
                    }
                    if (has_nrm) p += 12;
                    if (has_col) p += 4;
                    if (user_flags) p += (size_t)user_flags * 2;
                }
                if (!st.idx.empty()) poly.strips.push_back(std::move(st));
            }
            if (!poly.strips.empty()) out.push_back(std::move(poly));
            off = end;
            continue;
        }
        int size = (int)b.u16(off + 2);
        off += 4 + (size_t)size * 2;
    }
}

// ------------------------------------------------------------ build
bool NinjaBlob::build_model(uint32_t root, Model& out) const {
    out.nodes = read_tree(root);
    out.parts.clear();
    if (out.nodes.empty()) return false;

    // Chunk models share ONE vertex cache across the whole tree: a node's poly
    // chunks may index vertices uploaded by an ancestor. Walk once, in order.
    std::unordered_map<int, CVert> cache;
    std::unordered_map<int, int> cache_node;   // cache index -> node index

    for (const auto& nd : out.nodes) {
        if (!valid_attach(nd.attach_ptr)) continue;
        size_t a = off(nd.attach_ptr);
        uint32_t vl = u32(a), pl = u32(a + 4);

        if (ok(vl)) {
            std::unordered_map<int, CVert> local;
            parse_vertices(*this, off(vl), local);
            for (auto& kv : local) {
                CVert v = kv.second;
                float p[3];
                xform_point(nd.world, v.pos, p);
                memcpy(v.pos, p, sizeof(p));
                if (v.has_nrm) {
                    float d[3];
                    xform_dir(nd.world, v.nrm, d);
                    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (len > 1e-8f) { d[0] /= len; d[1] /= len; d[2] /= len; }
                    memcpy(v.nrm, d, sizeof(d));
                }
                cache[kv.first] = v;
                cache_node[kv.first] = nd.index;
            }
        }
        if (!ok(pl)) continue;

        std::vector<CPoly> polys;
        parse_polys(*this, off(pl), polys);
        for (auto& poly : polys) {
            MeshPart part;
            part.texture_id = poly.texture_id;
            part.node_index = nd.index;
            part.double_sided = (poly.flags & 0x80) != 0;
            part.use_alpha = poly.use_alpha;
            part.diffuse = poly.diffuse;
            std::map<std::pair<int, uint64_t>, uint32_t> remap;

            for (auto& st : poly.strips) {
                size_t cnt = st.idx.size();
                bool has_uv = st.uv.size() >= cnt * 2;
                for (size_t k = 0; k + 2 < cnt; k++) {
                    int tri[3] = {st.idx[k], st.idx[k + 1], st.idx[k + 2]};
                    int tuv[3] = {(int)k, (int)k + 1, (int)k + 2};
                    bool flip = (k % 2 == 1);
                    if (st.reversed) flip = !flip;
                    if (flip) {
                        std::swap(tri[0], tri[1]);
                        std::swap(tuv[0], tuv[1]);
                    }
                    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
                        continue;
                    uint32_t vi[3];
                    bool good = true;
                    for (int c = 0; c < 3; c++) {
                        auto it = cache.find(tri[c]);
                        if (it == cache.end()) { good = false; break; }
                        float u = 0, v = 0;
                        if (has_uv) { u = st.uv[tuv[c] * 2]; v = st.uv[tuv[c] * 2 + 1]; }
                        uint64_t uvkey = ((uint64_t)(int32_t)(u * 4096.0f) << 32) ^
                                         (uint32_t)(int32_t)(v * 4096.0f);
                        auto key = std::make_pair(tri[c], uvkey);
                        auto rit = remap.find(key);
                        if (rit != remap.end()) { vi[c] = rit->second; continue; }
                        uint32_t ni = (uint32_t)(part.positions.size() / 3);
                        remap[key] = ni;
                        const CVert& cv = it->second;
                        part.positions.insert(part.positions.end(),
                                              {cv.pos[0], cv.pos[1], cv.pos[2]});
                        if (cv.has_nrm)
                            part.normals.insert(part.normals.end(),
                                                {cv.nrm[0], cv.nrm[1], cv.nrm[2]});
                        else
                            part.normals.insert(part.normals.end(), {0.0f, 1.0f, 0.0f});
                        part.uvs.push_back(u);
                        part.uvs.push_back(v);
                        part.colors.push_back(cv.diffuse);
                        auto cn = cache_node.find(tri[c]);
                        part.vertex_node.push_back(cn == cache_node.end() ? nd.index
                                                                         : cn->second);
                        vi[c] = ni;
                    }
                    if (!good) continue;
                    part.indices.insert(part.indices.end(), {vi[0], vi[1], vi[2]});
                }
            }
            if (!part.indices.empty()) out.parts.push_back(std::move(part));
        }
    }
    return !out.parts.empty();
}

std::vector<uint32_t> NinjaBlob::find_model_roots() const {
    std::vector<uint32_t> roots;
    size_t n = data_.size();
    if (n < kObjSize) return roots;
    std::set<uint32_t> cand;
    for (size_t o = 0; o + 4 <= n; o += 4) {
        uint32_t v = u32(o);
        if (!ok(v)) continue;
        size_t t = off(v);
        if (t + kObjSize > n) continue;
        if (u32(t + 0x20) != kOne || u32(t + 0x24) != kOne || u32(t + 0x28) != kOne)
            continue;
        if (!valid_attach(u32(t + 4))) continue;
        cand.insert(v);
    }
    // keep only roots that are not reachable as a descendant of another candidate
    std::set<uint32_t> covered;
    for (uint32_t p : cand) {
        if (covered.count(p)) continue;
        auto tree = read_tree(p);
        if (tree.size() <= 1) continue;
        for (size_t i = 1; i < tree.size(); i++) covered.insert(tree[i].ptr);
    }
    for (uint32_t p : cand)
        if (!covered.count(p)) roots.push_back(p);
    std::sort(roots.begin(), roots.end());
    return roots;
}

// ------------------------------------------------------------ motions
bool NinjaBlob::read_motion(uint32_t ptr, int node_count, Motion& out) const {
    if (!ok(ptr) || node_count <= 0 || node_count > 4096) return false;
    size_t o = off(ptr);
    if (o + 16 > data_.size()) return false;
    uint32_t mdata = u32(o);
    int32_t frames = i32(o + 4);
    uint16_t type = u16(o + 8);
    uint16_t inp_fn = u16(o + 10);
    if (!ok(mdata) || frames <= 0 || frames > 100000) return false;

    // one pointer per set channel bit, then one count per set channel bit
    static const uint16_t kOrder[] = {0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x40, 0x80,
                                      0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000,
                                      0x4000, 0x8000};
    std::vector<uint16_t> chans;
    for (uint16_t f : kOrder) if (type & f) chans.push_back(f);
    if (chans.empty()) return false;

    out.frame_count = frames;
    out.node_count = node_count;
    out.type = type;
    out.channels.clear();

    size_t stride = chans.size() * 8;
    size_t mbase = off(mdata);
    for (int i = 0; i < node_count; i++) {
        size_t base = mbase + (size_t)i * stride;
        if (base + stride > data_.size()) break;
        MotionChannel mc;
        for (size_t c = 0; c < chans.size(); c++) {
            uint32_t p = u32(base + c * 4);
            int32_t cnt = i32(base + chans.size() * 4 + c * 4);
            if (!ok(p) || cnt <= 0 || cnt > 65535) continue;
            size_t ko = off(p);
            uint16_t ch = chans[c];
            if (ch != 0x1 && ch != 0x2 && ch != 0x4) continue;
            if (ko + (size_t)cnt * 16 > data_.size()) continue;
            std::vector<Key3>* dst = (ch == 0x1) ? &mc.pos
                                   : (ch == 0x2) ? &mc.rot : &mc.scale;
            for (int k = 0; k < cnt; k++) {
                size_t q = ko + (size_t)k * 16;
                Key3 key;
                key.frame = i32(q);
                if (ch == 0x2) {   // rotation keys are BAMS int32
                    key.v[0] = i32(q + 4) * kBams;
                    key.v[1] = i32(q + 8) * kBams;
                    key.v[2] = i32(q + 12) * kBams;
                } else {
                    key.v[0] = f32(q + 4);
                    key.v[1] = f32(q + 8);
                    key.v[2] = f32(q + 12);
                }
                dst->push_back(key);
            }
        }
        if (!mc.pos.empty() || !mc.rot.empty() || !mc.scale.empty())
            out.channels[i] = std::move(mc);
    }
    return !out.channels.empty();
}

// ------------------------------------------------------------ containers
static uint32_t be32(const uint8_t* d, size_t o) {
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
           ((uint32_t)d[o + 2] << 8) | d[o + 3];
}
static uint16_t be16(const uint8_t* d, size_t o) {
    return (uint16_t)((d[o] << 8) | d[o + 1]);
}

uint32_t detect_event_base(const uint8_t* d, size_t n) {
    // Score each known load address by how many big-endian pointer-sized words
    // land inside the file when rebased against it.
    static const uint32_t kCandidates[] = {kEventBase, kEventBaseDcGc, 0x0C600000u};
    uint32_t best = kEventBase;
    size_t best_hits = 0;
    for (uint32_t b : kCandidates) {
        size_t hits = 0;
        for (size_t o = 0; o + 4 <= n; o += 4) {
            uint32_t v = be32(d, o);
            if (v > b && v < b + (uint32_t)n) hits++;
        }
        if (hits > best_hits) { best_hits = hits; best = b; }
    }
    return best;
}

std::vector<std::pair<uint32_t, uint32_t>> read_mdl_table(const uint8_t* d, size_t n) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (size_t o = 0; o + 8 <= n; o += 8) {
        uint32_t idx = be32(d, o);
        uint32_t ptr = be32(d, o + 4);
        if (idx == 0xFFFFFFFFu) break;
        if (ptr == 0 || ptr >= n) continue;
        out.emplace_back(idx, ptr);
        if (out.size() > 4096) break;
    }
    return out;
}

std::vector<MtnEntry> read_mtn_table(const uint8_t* d, size_t n) {
    std::vector<MtnEntry> out;
    for (size_t o = 0; o + 8 <= n; o += 8) {
        int16_t idx = (int16_t)be16(d, o);
        uint16_t cnt = be16(d, o + 2);
        uint32_t ptr = be32(d, o + 4);
        if (idx == -1) break;
        if (ptr == 0 || ptr >= n) continue;
        out.push_back({idx, cnt, ptr});
        if (out.size() > 4096) break;
    }
    return out;
}

// ------------------------------------------------------------ Model helpers
void Model::bounds(float lo[3], float hi[3]) const {
    lo[0] = lo[1] = lo[2] = 1e30f;
    hi[0] = hi[1] = hi[2] = -1e30f;
    bool any = false;
    for (const auto& p : parts) {
        for (size_t i = 0; i + 2 < p.positions.size(); i += 3) {
            any = true;
            for (int k = 0; k < 3; k++) {
                float v = p.positions[i + k];
                if (v < lo[k]) lo[k] = v;
                if (v > hi[k]) hi[k] = v;
            }
        }
    }
    if (!any) { lo[0] = lo[1] = lo[2] = -1; hi[0] = hi[1] = hi[2] = 1; }
}
size_t Model::triangle_count() const {
    size_t t = 0;
    for (const auto& p : parts) t += p.indices.size() / 3;
    return t;
}
size_t Model::vertex_count() const {
    size_t t = 0;
    for (const auto& p : parts) t += p.positions.size() / 3;
    return t;
}

}  // namespace sa2
