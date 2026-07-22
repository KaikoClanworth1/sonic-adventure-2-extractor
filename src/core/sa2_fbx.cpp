// Binary FBX 7.4 (version 7400) exporter -- meshes, materials, textures,
// skeleton, skin clusters and one animation stack per motion.
//
// Everything is assembled into one in-memory buffer so that a node record's
// endOffset (which is an absolute file offset just past the record) can be
// backpatched after its children have been emitted; the buffer is flushed in a
// single write at the end.
//
// Conventions taken from sa2_model.cpp and verified against it:
//   * Node::world is row-major with the translation in elements 12..14 and
//     points transform as p' = p * M (row-vector).  FbxAMatrix uses exactly the
//     same storage order, so the 16 values go out untransposed.
//   * The local transform is S * R * T (row-vector), which is what FBX composes
//     from Lcl Scaling / Lcl Rotation / Lcl Translation.
//   * mat_from_srt() evaluates Rx*Ry*Rz (row-vector) normally and Rz*Rx*Ry when
//     NJD_EVAL_ZXY_ANG (flags & 0x20) is set.  Those are FBX rotation orders
//     eEulerXYZ (0) and eEulerZXY (4) respectively, emitted per node.
//   * Node::rot is BAMS (0x10000 == 360 deg); Motion rotation keys are already
//     radians (read_motion multiplies by 2*pi/65536).
//   * MeshPart UVs are raw Ninja UVs with a top-left origin, so V is flipped.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sa2core.h"

// ---------------------------------------------------------------------------
// write_png ownership.
//
// sa2core.h declares sa2::write_png() and this translation unit is able to
// provide it, but src/core/sa2_formats.cpp already defines it (together with
// STB_IMAGE_WRITE_IMPLEMENTATION).  Defining it here as well would produce
// duplicate symbols for write_png and for every stb_image_write entry point, so
// the implementation below is compiled out by default.  Flip this to 1 (and
// drop the copy in sa2_formats.cpp) to move ownership here.
// ---------------------------------------------------------------------------
#ifndef SA2_FBX_OWNS_WRITE_PNG
#define SA2_FBX_OWNS_WRITE_PNG 0
#endif

#if SA2_FBX_OWNS_WRITE_PNG
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace sa2 {
namespace {

// ============================================================ small helpers

constexpr int64_t kFbxTicksPerSecond = 46186158000LL;
constexpr double  kExportFps         = 30.0;
constexpr int64_t kTicksPerFrame     = kFbxTicksPerSecond / 30;   // exact
constexpr double  kBamsToDeg         = 360.0 / 65536.0;
constexpr double  kRadToDeg          = 57.295779513082320876798;
constexpr int32_t kKeyAttrLinear     = 24836;   // cubic|auto bits + linear interp
constexpr bool    kFlipV             = true;    // Ninja UVs are top-left origin

// Every value that reaches the file goes through this: FBX readers are not
// tolerant of NaN/Inf and a single bad float can wreck an import.
inline double san(double v) {
    if (!(v > -1e18 && v < 1e18)) return 0.0;   // false for NaN as well
    return v;
}
inline double sanf(float v) { return san((double)v); }

std::string sanitize_name(const std::string& in, const char* fallback) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        unsigned char u = (unsigned char)c;
        if (u < 32 || c == ':' || c == '"' || c == '|') continue;
        s.push_back(c);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) s.clear(); else s = s.substr(b);
    if (s.empty()) s = fallback;
    return s;
}

std::string sanitize_filename(const std::string& in, const char* fallback) {
    std::string s;
    for (char c : in) {
        unsigned char u = (unsigned char)c;
        if (u < 32) continue;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|')
            continue;
        s.push_back(c);
    }
    // drop an existing extension so we can append .png cleanly
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && s.size() - dot <= 5) s.resize(dot);
    if (s.empty()) s = fallback;
    return s;
}

class NameSet {
public:
    std::string unique(const std::string& want) {
        auto it = used_.find(want);
        if (it == used_.end()) { used_[want] = 1; return want; }
        for (int n = it->second; n < 100000; ++n) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "_%d", n);
            std::string cand = want + buf;
            if (used_.find(cand) == used_.end()) {
                it->second = n + 1;
                used_[cand] = 1;
                return cand;
            }
        }
        return want;
    }
private:
    std::map<std::string, int> used_;
};

// ------------------------------------------------------------------ matrices
// Row-major storage, row-vector convention (translation at 12..14) -- the same
// layout Node::world uses and the same 16-value order FBX expects.
struct Mat4 {
    double m[16];
};

Mat4 mat_identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0;
    return r;
}

Mat4 mat_from_world(const float w[16], double translation_scale) {
    Mat4 r{};
    for (int i = 0; i < 16; ++i) r.m[i] = sanf(w[i]);
    // A model loader that never filled the matrix leaves it all zero; a zero
    // matrix would make every cluster transform singular.
    bool zero = true;
    for (int i = 0; i < 16; ++i) if (r.m[i] != 0.0) { zero = false; break; }
    if (zero) return mat_identity();
    if (r.m[15] == 0.0) r.m[15] = 1.0;
    r.m[12] *= translation_scale;
    r.m[13] *= translation_scale;
    r.m[14] *= translation_scale;
    return r;
}

// Affine inverse: invert the 3x3 basis, then t' = -t * inv3.
bool mat_inverse_affine(const Mat4& in, Mat4& out) {
    const double* a = in.m;
    double c00 = a[5] * a[10] - a[6] * a[9];
    double c01 = a[6] * a[8]  - a[4] * a[10];
    double c02 = a[4] * a[9]  - a[5] * a[8];
    double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (!(std::fabs(det) > 1e-16)) return false;
    double inv = 1.0 / det;

    double b[9];
    b[0] = c00 * inv;
    b[3] = c01 * inv;
    b[6] = c02 * inv;
    b[1] = (a[2] * a[9]  - a[1] * a[10]) * inv;
    b[4] = (a[0] * a[10] - a[2] * a[8])  * inv;
    b[7] = (a[1] * a[8]  - a[0] * a[9])  * inv;
    b[2] = (a[1] * a[6]  - a[2] * a[5])  * inv;
    b[5] = (a[2] * a[4]  - a[0] * a[6])  * inv;
    b[8] = (a[0] * a[5]  - a[1] * a[4])  * inv;

    out = mat_identity();
    out.m[0] = b[0]; out.m[1] = b[1]; out.m[2]  = b[2];
    out.m[4] = b[3]; out.m[5] = b[4]; out.m[6]  = b[5];
    out.m[8] = b[6]; out.m[9] = b[7]; out.m[10] = b[8];
    out.m[12] = -(a[12] * b[0] + a[13] * b[3] + a[14] * b[6]);
    out.m[13] = -(a[12] * b[1] + a[13] * b[4] + a[14] * b[7]);
    out.m[14] = -(a[12] * b[2] + a[13] * b[5] + a[14] * b[8]);
    for (int i = 0; i < 16; ++i) out.m[i] = san(out.m[i]);
    return true;
}

std::vector<double> mat_values(const Mat4& m) {
    return std::vector<double>(m.m, m.m + 16);
}

// ============================================================ binary writer

class FbxWriter {
public:
    FbxWriter() { buf_.reserve(1u << 20); }

    // ---- node records -----------------------------------------------------
    void begin(const char* name) {
        if (!stack_.empty() && !stack_.back().has_children) {
            stack_.back().has_children = true;
            stack_.back().prop_end = buf_.size();
        }
        Rec r;
        r.start = buf_.size();
        put_u32(0);                       // endOffset      (backpatched)
        put_u32(0);                       // numProperties  (backpatched)
        put_u32(0);                       // propertyListLen(backpatched)
        size_t nl = std::strlen(name);
        if (nl > 255) nl = 255;
        buf_.push_back((uint8_t)nl);
        raw(name, nl);
        r.prop_start   = buf_.size();
        r.prop_end     = 0;
        r.nprops       = 0;
        r.has_children = false;
        stack_.push_back(r);
    }

    void end() {
        if (stack_.empty()) return;
        Rec r = stack_.back();
        stack_.pop_back();
        size_t prop_end = r.has_children ? r.prop_end : buf_.size();
        if (r.has_children)
            buf_.insert(buf_.end(), 13, 0u);      // nested-list terminator
        patch_u32(r.start + 0, (uint32_t)buf_.size());
        patch_u32(r.start + 4, r.nprops);
        patch_u32(r.start + 8, (uint32_t)(prop_end - r.prop_start));
    }

    // ---- scalar properties ------------------------------------------------
    void pY(int16_t v)  { tag('Y'); put_pod(v); }
    void pC(bool v)     { tag('C'); buf_.push_back(v ? 1u : 0u); }
    void pI(int32_t v)  { tag('I'); put_pod(v); }
    void pF(float v)    { tag('F'); put_pod(v); }
    void pD(double v)   { tag('D'); put_pod(san(v)); }
    void pL(int64_t v)  { tag('L'); put_pod(v); }
    void pS(const std::string& s) {
        tag('S');
        put_u32((uint32_t)s.size());
        raw(s.data(), s.size());
    }
    void pS(const char* s) { pS(std::string(s ? s : "")); }
    void pR(const void* data, size_t n) {
        tag('R');
        put_u32((uint32_t)n);
        raw(data, n);
    }

    // ---- array properties (encoding 0 == uncompressed) --------------------
    void pArr(const std::vector<double>& v)  { array('d', v); }
    void pArr(const std::vector<float>& v)   { array('f', v); }
    void pArr(const std::vector<int32_t>& v) { array('i', v); }
    void pArr(const std::vector<int64_t>& v) { array('l', v); }

    // ---- convenience node writers ----------------------------------------
    void nI(const char* name, int32_t v)            { begin(name); pI(v); end(); }
    void nL(const char* name, int64_t v)            { begin(name); pL(v); end(); }
    void nD(const char* name, double v)             { begin(name); pD(v); end(); }
    void nB(const char* name, bool v)               { begin(name); pC(v); end(); }
    void nS(const char* name, const std::string& v) { begin(name); pS(v); end(); }
    template <class T>
    void nArr(const char* name, const std::vector<T>& v) {
        begin(name); pArr(v); end();
    }

    // ---- Properties70 helpers --------------------------------------------
    void pBegin(const char* n, const char* t, const char* s, const char* f) {
        begin("P"); pS(n); pS(t); pS(s); pS(f);
    }
    void Pint(const char* n, const char* t, const char* s, const char* f, int32_t v) {
        pBegin(n, t, s, f); pI(v); end();
    }
    void Pbool(const char* n, bool v) {
        pBegin(n, "bool", "", ""); pI(v ? 1 : 0); end();
    }
    void Pdouble(const char* n, const char* t, const char* s, const char* f, double v) {
        pBegin(n, t, s, f); pD(v); end();
    }
    void Pstring(const char* n, const char* t, const char* s, const char* f,
                 const std::string& v) {
        pBegin(n, t, s, f); pS(v); end();
    }
    void Pvec3(const char* n, const char* t, const char* s, const char* f,
               double x, double y, double z) {
        pBegin(n, t, s, f); pD(x); pD(y); pD(z); end();
    }
    void Ptime(const char* n, int64_t v) {
        pBegin(n, "KTime", "Time", ""); pL(v); end();
    }
    void Pcompound(const char* n) { pBegin(n, "Compound", "", ""); end(); }

    // ---- output -----------------------------------------------------------
    const std::vector<uint8_t>& bytes() const { return buf_; }
    size_t size() const { return buf_.size(); }
    void raw(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        buf_.insert(buf_.end(), b, b + n);
    }
    void zeros(size_t n) { buf_.insert(buf_.end(), n, 0u); }
    void put_u32(uint32_t v) { put_pod(v); }

private:
    struct Rec {
        size_t   start;
        size_t   prop_start;
        size_t   prop_end;
        uint32_t nprops;
        bool     has_children;
    };

    void tag(char c) {
        buf_.push_back((uint8_t)c);
        if (!stack_.empty()) ++stack_.back().nprops;
    }
    template <class T>
    void put_pod(T v) { raw(&v, sizeof(T)); }        // x86/x64: little-endian
    void patch_u32(size_t at, uint32_t v) {
        if (at + 4 <= buf_.size()) std::memcpy(buf_.data() + at, &v, 4);
    }
    template <class T>
    void array(char code, const std::vector<T>& v) {
        tag(code);
        put_u32((uint32_t)v.size());                 // element count
        put_u32(0u);                                 // encoding: raw
        put_u32((uint32_t)(v.size() * sizeof(T)));   // byte length
        if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    }

    std::vector<uint8_t> buf_;
    std::vector<Rec>     stack_;
};

// FBX stores "Model::Bone01" as "Bone01\0\1Model" in binary files.
std::string fbx_name(const std::string& name, const char* cls) {
    std::string s = name;
    s.push_back('\0');
    s.push_back('\1');
    s += cls;
    return s;
}

// ============================================================ export plan
//
// The whole scene is planned (ids assigned, arrays built) before a single byte
// is written, because Definitions needs exact per-ObjectType counts.

struct MaterialOut {
    int64_t     id = 0;
    std::string name;
    int         texture = -1;          // index into `textures`
    double      r = 1, g = 1, b = 1;
    bool        alpha = false;
    bool        two_sided = false;
};

struct TextureOut {
    int64_t     tex_id = 0;
    int64_t     vid_id = 0;
    int         image = -1;
    std::string name;
    std::string abs_path;
    std::string rel_path;
};

struct ClusterOut {
    int64_t              id = 0;
    int                  node = 0;
    std::string          name;
    std::vector<int32_t> indexes;
    std::vector<double>  weights;
    Mat4                 transform{};        // inverse bind
    Mat4                 transform_link{};   // bone world
};

struct GeomOut {
    int64_t     geom_id  = 0;
    int64_t     model_id = 0;
    int64_t     skin_id  = 0;
    std::string name;
    int         material = -1;

    std::vector<double>  vertices;      // 3 * vertex count
    std::vector<int32_t> polygons;      // polygon-vertex indices, last ~xor'd
    std::vector<double>  normals;       // 3 * polygon vertex count
    std::vector<double>  uv;            // 2 * vertex count
    std::vector<int32_t> uv_index;      // 1 * polygon vertex count
    std::vector<double>  colors;        // 4 * vertex count
    std::vector<int32_t> color_index;
    std::vector<ClusterOut> clusters;
};

struct BoneOut {
    int64_t     model_id = 0;
    int64_t     attr_id  = 0;
    std::string name;
    double      t[3]{0, 0, 0};
    double      r[3]{0, 0, 0};
    double      s[3]{1, 1, 1};
    int         rotation_order = 0;
    int         parent = -1;
    Mat4        world{};
};

struct CurveOut {
    int64_t              id = 0;
    std::vector<int64_t> times;
    std::vector<float>   values;
};

struct CurveNodeOut {
    int64_t     id = 0;
    const char* target_prop = "Lcl Translation";
    int         bone = 0;
    double      def[3]{0, 0, 0};
    CurveOut    curve[3];
};

struct AnimOut {
    int64_t                   stack_id = 0;
    int64_t                   layer_id = 0;
    std::string               name;
    int64_t                   stop = 0;
    std::vector<CurveNodeOut> nodes;
};

struct Scene {
    int64_t root_id = 0;
    int64_t root_attr_id = 0;
    int64_t doc_id = 0;
    int64_t pose_id = 0;
    std::string root_name;
    std::vector<BoneOut>     bones;
    std::vector<GeomOut>     geoms;
    std::vector<MaterialOut> materials;
    std::vector<TextureOut>  texs;
    std::vector<AnimOut>     anims;
    bool has_pose = false;

    int32_t n_deformers = 0;
    int32_t n_curvenodes = 0;
    int32_t n_curves = 0;
};

// ------------------------------------------------------------ geometry build

void compute_face_normal(const std::vector<double>& v, int a, int b, int c,
                         double out[3]) {
    double ux = v[b * 3 + 0] - v[a * 3 + 0];
    double uy = v[b * 3 + 1] - v[a * 3 + 1];
    double uz = v[b * 3 + 2] - v[a * 3 + 2];
    double wx = v[c * 3 + 0] - v[a * 3 + 0];
    double wy = v[c * 3 + 1] - v[a * 3 + 1];
    double wz = v[c * 3 + 2] - v[a * 3 + 2];
    double nx = uy * wz - uz * wy;
    double ny = uz * wx - ux * wz;
    double nz = ux * wy - uy * wx;
    double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 1e-12)) { out[0] = 0; out[1] = 1; out[2] = 0; return; }
    out[0] = nx / len; out[1] = ny / len; out[2] = nz / len;
}

// Builds one Geometry from one MeshPart. Returns false when nothing usable is
// left after dropping out-of-range and degenerate triangles.
bool build_geometry(const MeshPart& part, const Model& model, double scale,
                    GeomOut& g) {
    const size_t src_verts = part.positions.size() / 3;
    if (src_verts < 3 || part.indices.size() < 3) return false;

    const bool have_normals = part.normals.size() >= src_verts * 3;
    const bool have_uv      = part.uvs.size()     >= src_verts * 2;
    const bool have_colors  = part.colors.size()  >= src_verts;
    const bool have_bind    = part.vertex_node.size() >= src_verts;

    // Keep only vertices that a surviving triangle actually references, so the
    // mesh never imports with loose vertices.
    std::vector<int32_t> remap(src_verts, -1);
    std::vector<uint32_t> src_of;
    src_of.reserve(src_verts);
    std::vector<int32_t> tris;
    tris.reserve(part.indices.size());

    for (size_t i = 0; i + 2 < part.indices.size(); i += 3) {
        uint32_t a = part.indices[i], b = part.indices[i + 1], c = part.indices[i + 2];
        if (a >= src_verts || b >= src_verts || c >= src_verts) continue;
        if (a == b || b == c || a == c) continue;
        const uint32_t tri[3] = {a, b, c};
        for (int k = 0; k < 3; ++k) {
            uint32_t v = tri[k];
            if (remap[v] < 0) {
                remap[v] = (int32_t)src_of.size();
                src_of.push_back(v);
            }
            tris.push_back(remap[v]);
        }
    }
    if (tris.size() < 3 || src_of.size() < 3) return false;

    const size_t nv = src_of.size();
    const size_t npv = tris.size();

    g.vertices.resize(nv * 3);
    for (size_t i = 0; i < nv; ++i) {
        const float* p = &part.positions[(size_t)src_of[i] * 3];
        g.vertices[i * 3 + 0] = san((double)p[0] * scale);
        g.vertices[i * 3 + 1] = san((double)p[1] * scale);
        g.vertices[i * 3 + 2] = san((double)p[2] * scale);
    }

    g.polygons.resize(npv);
    for (size_t i = 0; i + 2 < npv; i += 3) {
        g.polygons[i + 0] = tris[i + 0];
        g.polygons[i + 1] = tris[i + 1];
        g.polygons[i + 2] = ~tris[i + 2];    // last index of a polygon: XOR -1
    }

    // Normals: ByPolygonVertex / Direct.
    g.normals.resize(npv * 3);
    for (size_t i = 0; i + 2 < npv; i += 3) {
        double face[3];
        bool need_face = !have_normals;
        if (!need_face) {
            for (int k = 0; k < 3; ++k) {
                const float* n = &part.normals[(size_t)src_of[tris[i + k]] * 3];
                double len = std::sqrt(sanf(n[0]) * sanf(n[0]) +
                                       sanf(n[1]) * sanf(n[1]) +
                                       sanf(n[2]) * sanf(n[2]));
                if (!(len > 1e-8)) { need_face = true; break; }
            }
        }
        if (need_face)
            compute_face_normal(g.vertices, tris[i], tris[i + 1], tris[i + 2], face);
        for (int k = 0; k < 3; ++k) {
            double* dst = &g.normals[(i + k) * 3];
            if (need_face) {
                dst[0] = face[0]; dst[1] = face[1]; dst[2] = face[2];
            } else {
                const float* n = &part.normals[(size_t)src_of[tris[i + k]] * 3];
                double x = sanf(n[0]), y = sanf(n[1]), z = sanf(n[2]);
                double len = std::sqrt(x * x + y * y + z * z);
                if (len > 1e-8) { x /= len; y /= len; z /= len; }
                dst[0] = x; dst[1] = y; dst[2] = z;
            }
        }
    }

    // UVs: ByPolygonVertex / IndexToDirect over a per-vertex table.
    if (have_uv) {
        g.uv.resize(nv * 2);
        for (size_t i = 0; i < nv; ++i) {
            double u = sanf(part.uvs[(size_t)src_of[i] * 2 + 0]);
            double v = sanf(part.uvs[(size_t)src_of[i] * 2 + 1]);
            g.uv[i * 2 + 0] = u;
            g.uv[i * 2 + 1] = kFlipV ? 1.0 - v : v;
        }
        g.uv_index.assign(tris.begin(), tris.end());
    }

    // Vertex colours, only when something is actually tinted.
    if (have_colors) {
        bool any = false;
        for (size_t i = 0; i < nv && !any; ++i)
            if (part.colors[src_of[i]] != 0xFFFFFFFFu) any = true;
        if (any) {
            g.colors.resize(nv * 4);
            for (size_t i = 0; i < nv; ++i) {
                uint32_t argb = part.colors[src_of[i]];
                g.colors[i * 4 + 0] = ((argb >> 16) & 0xFF) / 255.0;
                g.colors[i * 4 + 1] = ((argb >> 8) & 0xFF) / 255.0;
                g.colors[i * 4 + 2] = (argb & 0xFF) / 255.0;
                g.colors[i * 4 + 3] = ((argb >> 24) & 0xFF) / 255.0;
            }
            g.color_index.assign(tris.begin(), tris.end());
        }
    }

    // Skin clusters: one per referenced node, every vertex fully weighted.
    if (!model.nodes.empty()) {
        std::map<int, std::vector<int32_t>> by_node;
        const int fallback = (part.node_index >= 0 &&
                              part.node_index < (int)model.nodes.size())
                                 ? part.node_index : 0;
        for (size_t i = 0; i < nv; ++i) {
            int n = fallback;
            if (have_bind) {
                int cand = part.vertex_node[src_of[i]];
                if (cand >= 0 && cand < (int)model.nodes.size()) n = cand;
            }
            by_node[n].push_back((int32_t)i);
        }
        g.clusters.reserve(by_node.size());
        for (auto& kv : by_node) {
            ClusterOut c;
            c.node = kv.first;
            c.indexes = std::move(kv.second);
            c.weights.assign(c.indexes.size(), 1.0);
            g.clusters.push_back(std::move(c));
        }
    }
    return true;
}

// ------------------------------------------------------------ animation build

void build_curve(const std::vector<Key3>& keys, int comp, double factor,
                 CurveOut& out, int64_t& stop) {
    out.times.clear();
    out.values.clear();
    out.times.reserve(keys.size());
    out.values.reserve(keys.size());
    int last_frame = -0x7FFFFFFF;
    for (const Key3& k : keys) {
        int f = k.frame;
        if (f < 0) f = 0;
        if (f > 1000000) continue;
        if (f <= last_frame && !out.times.empty()) continue;   // keep monotonic
        last_frame = f;
        int64_t t = (int64_t)f * kTicksPerFrame;
        out.times.push_back(t);
        out.values.push_back((float)san((double)k.v[comp] * factor));
        if (t > stop) stop = t;
    }
}

bool build_curvenode(const std::vector<Key3>& keys, const char* prop, int bone,
                     double factor, CurveNodeOut& cn, int64_t& stop) {
    if (keys.empty()) return false;
    cn.target_prop = prop;
    cn.bone = bone;
    for (int c = 0; c < 3; ++c) {
        build_curve(keys, c, factor, cn.curve[c], stop);
        cn.def[c] = cn.curve[c].values.empty() ? 0.0
                                               : (double)cn.curve[c].values[0];
    }
    return !cn.curve[0].times.empty();
}

// ============================================================ writing

void write_properties70_model(FbxWriter& w, const BoneOut& b) {
    w.begin("Properties70");
    w.Pint("RotationOrder", "enum", "", "", b.rotation_order);
    w.Pbool("RotationActive", true);
    w.Pint("InheritType", "enum", "", "", 0);       // eInheritRrSs: plain chain
    w.Pint("DefaultAttributeIndex", "int", "Integer", "", 0);
    w.Pvec3("Lcl Translation", "Lcl Translation", "", "A", b.t[0], b.t[1], b.t[2]);
    w.Pvec3("Lcl Rotation", "Lcl Rotation", "", "A", b.r[0], b.r[1], b.r[2]);
    w.Pvec3("Lcl Scaling", "Lcl Scaling", "", "A", b.s[0], b.s[1], b.s[2]);
    w.end();
}

void write_model_node(FbxWriter& w, int64_t id, const std::string& name,
                      const char* subclass, const BoneOut& trs) {
    w.begin("Model");
    w.pL(id);
    w.pS(fbx_name(name, "Model"));
    w.pS(subclass);
    w.nI("Version", 232);
    write_properties70_model(w, trs);
    w.nB("Shading", true);
    w.nS("Culling", "CullingOff");
    w.end();
}

void write_geometry(FbxWriter& w, const GeomOut& g) {
    w.begin("Geometry");
    w.pL(g.geom_id);
    w.pS(fbx_name(g.name, "Geometry"));
    w.pS("Mesh");

    w.begin("Properties70"); w.end();
    w.nI("GeometryVersion", 124);
    w.nArr("Vertices", g.vertices);
    w.nArr("PolygonVertexIndex", g.polygons);

    w.begin("LayerElementNormal");
    w.pI(0);
    w.nI("Version", 101);
    w.nS("Name", "");
    w.nS("MappingInformationType", "ByPolygonVertex");
    w.nS("ReferenceInformationType", "Direct");
    w.nArr("Normals", g.normals);
    w.end();

    if (!g.uv.empty()) {
        w.begin("LayerElementUV");
        w.pI(0);
        w.nI("Version", 101);
        w.nS("Name", "UVMap");
        w.nS("MappingInformationType", "ByPolygonVertex");
        w.nS("ReferenceInformationType", "IndexToDirect");
        w.nArr("UV", g.uv);
        w.nArr("UVIndex", g.uv_index);
        w.end();
    }

    if (!g.colors.empty()) {
        w.begin("LayerElementColor");
        w.pI(0);
        w.nI("Version", 101);
        w.nS("Name", "Col");
        w.nS("MappingInformationType", "ByPolygonVertex");
        w.nS("ReferenceInformationType", "IndexToDirect");
        w.nArr("Colors", g.colors);
        w.nArr("ColorIndex", g.color_index);
        w.end();
    }

    w.begin("LayerElementMaterial");
    w.pI(0);
    w.nI("Version", 101);
    w.nS("Name", "");
    w.nS("MappingInformationType", "AllSame");
    w.nS("ReferenceInformationType", "IndexToDirect");
    w.nArr("Materials", std::vector<int32_t>{0});
    w.end();

    w.begin("Layer");
    w.pI(0);
    w.nI("Version", 100);
    auto layer_elem = [&](const char* type) {
        w.begin("LayerElement");
        w.nS("Type", type);
        w.nI("TypedIndex", 0);
        w.end();
    };
    layer_elem("LayerElementNormal");
    if (!g.uv.empty())     layer_elem("LayerElementUV");
    if (!g.colors.empty()) layer_elem("LayerElementColor");
    layer_elem("LayerElementMaterial");
    w.end();

    w.end();
}

void write_definitions(FbxWriter& w, const Scene& sc) {
    const int32_t n_model = 1 + (int32_t)sc.bones.size() + (int32_t)sc.geoms.size();
    const int32_t n_attr  = 1 + (int32_t)sc.bones.size();
    const int32_t n_geom  = (int32_t)sc.geoms.size();
    const int32_t n_mat   = (int32_t)sc.materials.size();
    const int32_t n_tex   = (int32_t)sc.texs.size();
    const int32_t n_vid   = (int32_t)sc.texs.size();
    const int32_t n_def   = sc.n_deformers;
    const int32_t n_pose  = sc.has_pose ? 1 : 0;
    const int32_t n_stack = (int32_t)sc.anims.size();
    const int32_t n_layer = (int32_t)sc.anims.size();
    const int32_t n_cn    = sc.n_curvenodes;
    const int32_t n_cv    = sc.n_curves;

    const int32_t total = 1 + n_model + n_attr + n_geom + n_mat + n_tex + n_vid +
                          n_def + n_pose + n_stack + n_layer + n_cn + n_cv;

    w.begin("Definitions");
    w.nI("Version", 100);
    w.nI("Count", total);

    auto simple = [&](const char* type, int32_t count) {
        if (count <= 0) return;
        w.begin("ObjectType");
        w.pS(type);
        w.nI("Count", count);
        w.end();
    };

    simple("GlobalSettings", 1);

    if (n_attr > 0) simple("NodeAttribute", n_attr);

    if (n_model > 0) {
        w.begin("ObjectType");
        w.pS("Model");
        w.nI("Count", n_model);
        w.begin("PropertyTemplate");
        w.pS("FbxNode");
        w.begin("Properties70");
        w.Pint("RotationOrder", "enum", "", "", 0);
        w.Pbool("RotationActive", false);
        w.Pint("InheritType", "enum", "", "", 0);
        w.Pvec3("ScalingMax", "Vector3D", "Vector", "", 0, 0, 0);
        w.Pint("DefaultAttributeIndex", "int", "Integer", "", -1);
        w.Pvec3("Lcl Translation", "Lcl Translation", "", "A", 0, 0, 0);
        w.Pvec3("Lcl Rotation", "Lcl Rotation", "", "A", 0, 0, 0);
        w.Pvec3("Lcl Scaling", "Lcl Scaling", "", "A", 1, 1, 1);
        w.Pdouble("Visibility", "Visibility", "", "A", 1.0);
        w.end();
        w.end();
        w.end();
    }

    if (n_geom > 0) {
        w.begin("ObjectType");
        w.pS("Geometry");
        w.nI("Count", n_geom);
        w.begin("PropertyTemplate");
        w.pS("FbxMesh");
        w.begin("Properties70");
        w.Pvec3("Color", "ColorRGB", "Color", "", 0.8, 0.8, 0.8);
        w.Pbool("Primary Visibility", true);
        w.end();
        w.end();
        w.end();
    }

    if (n_mat > 0) {
        w.begin("ObjectType");
        w.pS("Material");
        w.nI("Count", n_mat);
        w.begin("PropertyTemplate");
        w.pS("FbxSurfacePhong");
        w.begin("Properties70");
        w.Pstring("ShadingModel", "KString", "", "", "Phong");
        w.Pvec3("DiffuseColor", "Color", "", "A", 0.8, 0.8, 0.8);
        w.Pvec3("SpecularColor", "Color", "", "A", 0.2, 0.2, 0.2);
        w.Pdouble("Shininess", "double", "Number", "", 20.0);
        w.Pdouble("Opacity", "double", "Number", "", 1.0);
        w.end();
        w.end();
        w.end();
    }

    if (n_tex > 0) {
        w.begin("ObjectType");
        w.pS("Texture");
        w.nI("Count", n_tex);
        w.begin("PropertyTemplate");
        w.pS("FbxFileTexture");
        w.begin("Properties70");
        w.Pstring("TextureTypeUse", "enum", "", "", "");
        w.Pdouble("Texture alpha", "Number", "", "A", 1.0);
        w.Pint("UseMaterial", "bool", "", "", 0);
        w.Pint("WrapModeU", "enum", "", "", 0);
        w.Pint("WrapModeV", "enum", "", "", 0);
        w.end();
        w.end();
        w.end();
    }

    simple("Video", n_vid);
    simple("Deformer", n_def);
    simple("Pose", n_pose);

    if (n_stack > 0) {
        w.begin("ObjectType");
        w.pS("AnimationStack");
        w.nI("Count", n_stack);
        w.begin("PropertyTemplate");
        w.pS("FbxAnimStack");
        w.begin("Properties70");
        w.Pstring("Description", "KString", "", "", "");
        w.Ptime("LocalStart", 0);
        w.Ptime("LocalStop", 0);
        w.Ptime("ReferenceStart", 0);
        w.Ptime("ReferenceStop", 0);
        w.end();
        w.end();
        w.end();
    }
    if (n_layer > 0) {
        w.begin("ObjectType");
        w.pS("AnimationLayer");
        w.nI("Count", n_layer);
        w.begin("PropertyTemplate");
        w.pS("FbxAnimLayer");
        w.begin("Properties70");
        w.Pdouble("Weight", "Number", "", "A", 100.0);
        w.end();
        w.end();
        w.end();
    }
    if (n_cn > 0) {
        w.begin("ObjectType");
        w.pS("AnimationCurveNode");
        w.nI("Count", n_cn);
        w.begin("PropertyTemplate");
        w.pS("FbxAnimCurveNode");
        w.begin("Properties70");
        w.Pcompound("d");
        w.end();
        w.end();
        w.end();
    }
    simple("AnimationCurve", n_cv);

    w.end();
}

// ============================================================ the exporter

class Exporter {
public:
    Exporter(const Model& model, const std::vector<Image>& textures,
             const std::vector<Motion>& motions, const FbxExportOptions& opts)
        : model_(model), images_(textures), motions_(motions), opts_(opts) {}

    bool run(const std::string& out_path, std::string* error);

private:
    int64_t uid() { return next_id_++; }
    void plan_textures(const std::filesystem::path& fbx_path);
    void plan_materials();
    void plan_geometry();
    void plan_skeleton();
    void plan_animation();
    void write(FbxWriter& w, const std::string& out_path);
    void write_objects(FbxWriter& w);
    void write_connections(FbxWriter& w);

    const Model&               model_;
    const std::vector<Image>&  images_;
    const std::vector<Motion>& motions_;
    const FbxExportOptions&    opts_;

    // (texture, diffuse, alpha, two-sided) -> index into sc_.materials
    typedef std::tuple<int, uint32_t, bool, bool> MatKey;

    Scene    sc_;
    int64_t  next_id_ = 1000000;
    double   scale_   = 1.0;
    std::map<int, int>    image_to_tex_;  // image index -> index into sc_.texs
    std::map<MatKey, int> mat_lookup_;
    NameSet  model_names_;
    std::string fbm_rel_;                 // "<base>.fbm"
};

void Exporter::plan_textures(const std::filesystem::path& fbx_path) {
    // Which images do the parts actually reference?
    std::set<int> used;
    for (const MeshPart& p : model_.parts)
        if (p.texture_id >= 0 && p.texture_id < (int)images_.size())
            used.insert(p.texture_id);

    std::string base = fbx_path.stem().string();
    if (base.empty()) base = "model";
    fbm_rel_ = base + ".fbm";
    std::filesystem::path dir = fbx_path.parent_path() / fbm_rel_;

    bool dir_ok = true;
    if (opts_.write_textures && (!used.empty() || !images_.empty())) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        dir_ok = !ec || std::filesystem::exists(dir, ec);
    }
    // FileName / Path are supposed to be absolute; RelativeFilename stays
    // relative to the .fbx so the pair survives being moved.
    std::filesystem::path dir_abs = dir;
    {
        std::error_code ec;
        std::filesystem::path a = std::filesystem::absolute(dir, ec);
        if (!ec && !a.empty()) dir_abs = a;
    }

    NameSet tex_names;
    // Write every supplied image, but only create Texture/Video objects for the
    // ones a material references.
    for (size_t i = 0; i < images_.size(); ++i) {
        const Image& img = images_[i];
        char fb[32];
        std::snprintf(fb, sizeof(fb), "tex_%03d", (int)i);
        std::string fname = tex_names.unique(sanitize_filename(img.name, fb));
        std::string file  = fname + ".png";

        if (opts_.write_textures && dir_ok && img.valid())
            write_png((dir / file).string(), img);

        if (!used.count((int)i)) continue;

        TextureOut t;
        t.image    = (int)i;
        t.name     = fname;
        t.abs_path = (dir_abs / file).string();
        t.rel_path = fbm_rel_ + "/" + file;
        t.tex_id   = uid();
        t.vid_id   = uid();
        image_to_tex_[(int)i] = (int)sc_.texs.size();
        sc_.texs.push_back(std::move(t));
    }
}

void Exporter::plan_materials() {
    // One material per distinct (texture, diffuse, alpha, two-sided) tuple.
    NameSet mat_names;
    for (const MeshPart& p : model_.parts) {
        int tex = (p.texture_id >= 0 && p.texture_id < (int)images_.size())
                      ? p.texture_id : -1;
        MatKey key(tex, p.diffuse, p.use_alpha, p.double_sided);
        if (mat_lookup_.count(key)) continue;
        MaterialOut m;
        m.id        = uid();
        m.texture   = tex;
        m.alpha     = p.use_alpha;
        m.two_sided = p.double_sided;
        m.r = ((p.diffuse >> 16) & 0xFF) / 255.0;
        m.g = ((p.diffuse >> 8) & 0xFF) / 255.0;
        m.b = (p.diffuse & 0xFF) / 255.0;
        char fb[48];
        if (tex >= 0 && tex < (int)images_.size() && !images_[tex].name.empty())
            std::snprintf(fb, sizeof(fb), "mat_%.32s", images_[tex].name.c_str());
        else
            std::snprintf(fb, sizeof(fb), "mat_%03d", (int)sc_.materials.size());
        m.name = mat_names.unique(sanitize_name(fb, "mat"));
        mat_lookup_[key] = (int)sc_.materials.size();
        sc_.materials.push_back(std::move(m));
    }
}

void Exporter::plan_geometry() {
    for (size_t i = 0; i < model_.parts.size(); ++i) {
        const MeshPart& p = model_.parts[i];
        GeomOut g;
        if (!build_geometry(p, model_, scale_, g)) continue;

        char fb[64];
        std::snprintf(fb, sizeof(fb), "mesh_%03d", (int)i);
        g.name = model_names_.unique(sanitize_name(fb, "mesh"));

        int tex = (p.texture_id >= 0 && p.texture_id < (int)images_.size())
                      ? p.texture_id : -1;
        auto it = mat_lookup_.find(MatKey(tex, p.diffuse, p.use_alpha,
                                          p.double_sided));
        g.material = (it == mat_lookup_.end()) ? -1 : it->second;

        g.geom_id  = uid();
        g.model_id = uid();
        if (!g.clusters.empty()) {
            g.skin_id = uid();
            sc_.n_deformers += 1 + (int32_t)g.clusters.size();
            for (ClusterOut& c : g.clusters) c.id = uid();
        }
        sc_.geoms.push_back(std::move(g));
    }
}

void Exporter::plan_skeleton() {
    const size_t n = model_.nodes.size();
    sc_.bones.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const Node& nd = model_.nodes[i];
        BoneOut& b = sc_.bones[i];
        char fb[32];
        std::snprintf(fb, sizeof(fb), "node_%03d", (int)i);
        b.name = model_names_.unique(sanitize_name(nd.name, fb));
        b.model_id = uid();
        b.attr_id  = uid();
        b.parent   = (nd.parent >= 0 && nd.parent < (int)n &&
                      nd.parent != (int)i) ? nd.parent : -1;
        for (int k = 0; k < 3; ++k) {
            b.t[k] = san((double)nd.pos[k] * scale_);
            b.r[k] = san((double)nd.rot[k] * kBamsToDeg);
            double s = sanf(nd.scale[k]);
            b.s[k] = (std::fabs(s) < 1e-9) ? 1.0 : s;
        }
        // NJD_EVAL_ZXY_ANG -> Rz*Rx*Ry (row-vector) == FBX eEulerZXY.
        b.rotation_order = (nd.flags & 0x20u) ? 4 : 0;
        b.world = mat_from_world(nd.world, scale_);
    }

    // Bind matrices for every cluster.
    for (GeomOut& g : sc_.geoms) {
        for (ClusterOut& c : g.clusters) {
            Mat4 world = mat_identity();
            if (c.node >= 0 && c.node < (int)sc_.bones.size())
                world = sc_.bones[c.node].world;
            c.transform_link = world;
            if (!mat_inverse_affine(world, c.transform))
                c.transform = mat_identity();
            c.name = (c.node >= 0 && c.node < (int)sc_.bones.size())
                         ? sc_.bones[c.node].name : std::string("root");
        }
        if (!g.clusters.empty()) sc_.has_pose = true;
    }
}

void Exporter::plan_animation() {
    NameSet take_names;
    for (size_t mi = 0; mi < motions_.size(); ++mi) {
        const Motion& mo = motions_[mi];
        if (mo.channels.empty()) continue;

        AnimOut a;
        char fb[64];
        std::snprintf(fb, sizeof(fb), "Take_%03d", (int)mi);
        a.name = take_names.unique(sanitize_name(mo.name, fb));
        a.stack_id = uid();
        a.layer_id = uid();
        a.stop = 0;

        for (const auto& kv : mo.channels) {
            int node = kv.first;
            if (node < 0 || node >= (int)sc_.bones.size()) continue;
            const MotionChannel& ch = kv.second;

            CurveNodeOut cn;
            if (build_curvenode(ch.pos, "Lcl Translation", node, scale_, cn, a.stop)) {
                cn.id = uid();
                a.nodes.push_back(cn);
            }
            CurveNodeOut cr;
            if (build_curvenode(ch.rot, "Lcl Rotation", node, kRadToDeg, cr, a.stop)) {
                cr.id = uid();
                a.nodes.push_back(cr);
            }
            CurveNodeOut cs;
            if (build_curvenode(ch.scale, "Lcl Scaling", node, 1.0, cs, a.stop)) {
                cs.id = uid();
                a.nodes.push_back(cs);
            }
        }
        if (a.nodes.empty()) continue;

        int64_t by_count = (int64_t)std::max(0, mo.frame_count - 1) * kTicksPerFrame;
        if (by_count > a.stop) a.stop = by_count;

        for (CurveNodeOut& cn : a.nodes) {
            sc_.n_curvenodes++;
            for (int c = 0; c < 3; ++c) {
                cn.curve[c].id = uid();
                sc_.n_curves++;
            }
        }
        sc_.anims.push_back(std::move(a));
    }
}

void Exporter::write_objects(FbxWriter& w) {
    w.begin("Objects");

    BoneOut identity;   // all-default TRS, used for the null and the meshes

    // -- root Null and its attribute ---------------------------------------
    w.begin("NodeAttribute");
    w.pL(sc_.root_attr_id);
    w.pS(fbx_name(sc_.root_name, "NodeAttribute"));
    w.pS("Null");
    w.begin("Properties70"); w.end();
    w.nS("TypeFlags", "Null");
    w.end();
    write_model_node(w, sc_.root_id, sc_.root_name, "Null", identity);

    // -- skeleton -----------------------------------------------------------
    for (const BoneOut& b : sc_.bones) {
        w.begin("NodeAttribute");
        w.pL(b.attr_id);
        w.pS(fbx_name(b.name, "NodeAttribute"));
        w.pS("LimbNode");
        w.begin("Properties70");
        w.Pdouble("Size", "double", "Number", "", 1.0);
        w.end();
        w.nS("TypeFlags", "Skeleton");
        w.end();
        write_model_node(w, b.model_id, b.name, "LimbNode", b);
    }

    // -- geometry and its models -------------------------------------------
    for (const GeomOut& g : sc_.geoms) {
        write_geometry(w, g);
        write_model_node(w, g.model_id, g.name, "Mesh", identity);
    }

    // -- materials ----------------------------------------------------------
    for (const MaterialOut& m : sc_.materials) {
        w.begin("Material");
        w.pL(m.id);
        w.pS(fbx_name(m.name, "Material"));
        w.pS("");
        w.nI("Version", 102);
        w.nS("ShadingModel", "phong");
        w.nI("MultiLayer", 0);
        w.begin("Properties70");
        w.Pstring("ShadingModel", "KString", "", "", "phong");
        w.Pvec3("AmbientColor", "Color", "", "A", 0.0, 0.0, 0.0);
        w.Pvec3("DiffuseColor", "Color", "", "A", m.r, m.g, m.b);
        w.Pdouble("DiffuseFactor", "Number", "", "A", 1.0);
        w.Pvec3("SpecularColor", "Color", "", "A", 0.0, 0.0, 0.0);
        w.Pdouble("SpecularFactor", "Number", "", "A", 0.0);
        w.Pdouble("Shininess", "Number", "", "A", 2.0);
        w.Pdouble("ShininessExponent", "Number", "", "A", 2.0);
        w.Pdouble("Opacity", "Number", "", "A", 1.0);
        w.Pdouble("TransparencyFactor", "Number", "", "A", 0.0);
        w.end();
        w.end();
    }

    // -- textures and video clips ------------------------------------------
    for (const TextureOut& t : sc_.texs) {
        w.begin("Video");
        w.pL(t.vid_id);
        w.pS(fbx_name(t.name, "Video"));
        w.pS("Clip");
        w.nS("Type", "Clip");
        w.begin("Properties70");
        w.Pstring("Path", "KString", "XRefUrl", "", t.abs_path);
        w.Pstring("RelPath", "KString", "XRefUrl", "", t.rel_path);
        w.end();
        w.nI("UseMipMap", 0);
        w.nS("Filename", t.abs_path);
        w.nS("RelativeFilename", t.rel_path);
        w.end();

        w.begin("Texture");
        w.pL(t.tex_id);
        w.pS(fbx_name(t.name, "Texture"));
        w.pS("");
        w.nS("Type", "TextureVideoClip");
        w.nI("Version", 202);
        w.nS("TextureName", fbx_name(t.name, "Texture"));
        w.begin("Properties70");
        w.Pstring("UVSet", "KString", "", "", "UVMap");
        w.Pint("UseMaterial", "bool", "", "", 1);
        w.Pint("WrapModeU", "enum", "", "", 0);
        w.Pint("WrapModeV", "enum", "", "", 0);
        w.end();
        w.nS("Media", fbx_name(t.name, "Video"));
        w.nS("FileName", t.abs_path);
        w.nS("RelativeFilename", t.rel_path);
        w.nArr("ModelUVTranslation", std::vector<double>{0.0, 0.0});
        w.nArr("ModelUVScaling", std::vector<double>{1.0, 1.0});
        w.nS("Texture_Alpha_Source", "None");
        w.nArr("Cropping", std::vector<int32_t>{0, 0, 0, 0});
        w.end();
    }

    // -- skins and clusters -------------------------------------------------
    for (const GeomOut& g : sc_.geoms) {
        if (g.clusters.empty()) continue;
        w.begin("Deformer");
        w.pL(g.skin_id);
        w.pS(fbx_name(g.name, "Deformer"));
        w.pS("Skin");
        w.nI("Version", 101);
        w.nD("Link_DeformAcuracy", 50.0);
        w.nS("SkinningType", "Linear");
        w.end();

        for (const ClusterOut& c : g.clusters) {
            w.begin("Deformer");
            w.pL(c.id);
            w.pS(fbx_name(c.name, "SubDeformer"));
            w.pS("Cluster");
            w.nI("Version", 100);
            w.begin("UserData"); w.pS(""); w.pS(""); w.end();
            w.nArr("Indexes", c.indexes);
            w.nArr("Weights", c.weights);
            w.nArr("Transform", mat_values(c.transform));
            w.nArr("TransformLink", mat_values(c.transform_link));
            w.end();
        }
    }

    // -- bind pose ----------------------------------------------------------
    if (sc_.has_pose) {
        int32_t count = (int32_t)sc_.bones.size();
        for (const GeomOut& g : sc_.geoms) if (!g.clusters.empty()) ++count;

        w.begin("Pose");
        w.pL(sc_.pose_id);
        w.pS(fbx_name("BindPose", "Pose"));
        w.pS("BindPose");
        w.nS("Type", "BindPose");
        w.nI("Version", 100);
        w.nI("NbPoseNodes", count);
        for (const GeomOut& g : sc_.geoms) {
            if (g.clusters.empty()) continue;
            w.begin("PoseNode");
            w.nL("Node", g.model_id);
            w.nArr("Matrix", mat_values(mat_identity()));
            w.end();
        }
        for (const BoneOut& b : sc_.bones) {
            w.begin("PoseNode");
            w.nL("Node", b.model_id);
            w.nArr("Matrix", mat_values(b.world));
            w.end();
        }
        w.end();
    }

    // -- animation ----------------------------------------------------------
    for (const AnimOut& a : sc_.anims) {
        w.begin("AnimationStack");
        w.pL(a.stack_id);
        w.pS(fbx_name(a.name, "AnimStack"));
        w.pS("");
        w.begin("Properties70");
        w.Pstring("Description", "KString", "", "", "");
        w.Ptime("LocalStart", 0);
        w.Ptime("LocalStop", a.stop);
        w.Ptime("ReferenceStart", 0);
        w.Ptime("ReferenceStop", a.stop);
        w.end();
        w.end();

        w.begin("AnimationLayer");
        w.pL(a.layer_id);
        w.pS(fbx_name(a.name, "AnimLayer"));
        w.pS("");
        w.end();

        static const char* kAxis[3] = {"X", "Y", "Z"};
        for (const CurveNodeOut& cn : a.nodes) {
            const char* label = "T";
            if (std::strcmp(cn.target_prop, "Lcl Rotation") == 0) label = "R";
            else if (std::strcmp(cn.target_prop, "Lcl Scaling") == 0) label = "S";

            w.begin("AnimationCurveNode");
            w.pL(cn.id);
            w.pS(fbx_name(label, "AnimCurveNode"));
            w.pS("");
            w.begin("Properties70");
            w.Pcompound("d");
            for (int c = 0; c < 3; ++c) {
                std::string nm = std::string("d|") + kAxis[c];
                w.Pdouble(nm.c_str(), "Number", "", "A", cn.def[c]);
            }
            w.end();
            w.end();

            for (int c = 0; c < 3; ++c) {
                const CurveOut& cv = cn.curve[c];
                w.begin("AnimationCurve");
                w.pL(cv.id);
                w.pS(fbx_name("", "AnimCurve"));
                w.pS("");
                w.nD("Default", cn.def[c]);
                w.nI("KeyVer", 4008);
                w.nArr("KeyTime", cv.times);
                w.nArr("KeyValueFloat", cv.values);
                w.nArr("KeyAttrFlags", std::vector<int32_t>{kKeyAttrLinear});
                w.nArr("KeyAttrDataFloat",
                       std::vector<float>{0.0f, 0.0f, 0.0f, 0.0f});
                w.nArr("KeyAttrRefCount",
                       std::vector<int32_t>{(int32_t)cv.times.size()});
                w.end();
            }
        }
    }

    w.end();   // Objects
}

void Exporter::write_connections(FbxWriter& w) {
    w.begin("Connections");

    auto oo = [&](int64_t child, int64_t parent) {
        w.begin("C"); w.pS("OO"); w.pL(child); w.pL(parent); w.end();
    };
    auto op = [&](int64_t child, int64_t parent, const char* prop) {
        w.begin("C"); w.pS("OP"); w.pL(child); w.pL(parent); w.pS(prop); w.end();
    };

    // Scene graph.
    oo(sc_.root_id, 0);
    oo(sc_.root_attr_id, sc_.root_id);
    for (const BoneOut& b : sc_.bones) {
        oo(b.model_id, b.parent < 0 ? sc_.root_id
                                    : sc_.bones[b.parent].model_id);
        oo(b.attr_id, b.model_id);
    }
    for (const GeomOut& g : sc_.geoms) {
        oo(g.model_id, sc_.root_id);
        oo(g.geom_id, g.model_id);
        if (g.material >= 0 && g.material < (int)sc_.materials.size())
            oo(sc_.materials[g.material].id, g.model_id);
    }

    // Materials -> textures -> videos.
    for (const MaterialOut& m : sc_.materials) {
        if (m.texture < 0) continue;
        auto it = image_to_tex_.find(m.texture);
        if (it == image_to_tex_.end()) continue;
        const TextureOut& t = sc_.texs[it->second];
        op(t.tex_id, m.id, "DiffuseColor");
    }
    for (const TextureOut& t : sc_.texs) oo(t.vid_id, t.tex_id);

    // Skins.
    for (const GeomOut& g : sc_.geoms) {
        if (g.clusters.empty()) continue;
        oo(g.skin_id, g.geom_id);
        for (const ClusterOut& c : g.clusters) {
            oo(c.id, g.skin_id);
            if (c.node >= 0 && c.node < (int)sc_.bones.size())
                oo(sc_.bones[c.node].model_id, c.id);
        }
    }

    // Animation.
    static const char* kAxis[3] = {"d|X", "d|Y", "d|Z"};
    for (const AnimOut& a : sc_.anims) {
        oo(a.layer_id, a.stack_id);
        for (const CurveNodeOut& cn : a.nodes) {
            oo(cn.id, a.layer_id);
            if (cn.bone >= 0 && cn.bone < (int)sc_.bones.size())
                op(cn.id, sc_.bones[cn.bone].model_id, cn.target_prop);
            for (int c = 0; c < 3; ++c) op(cn.curve[c].id, cn.id, kAxis[c]);
        }
    }

    w.end();
}

void Exporter::write(FbxWriter& w, const std::string& out_path) {
    // ---- header ----------------------------------------------------------
    static const char kMagic[] = "Kaydara FBX Binary  ";
    w.raw(kMagic, 20);
    w.zeros(1);
    const uint8_t tail[2] = {0x1A, 0x00};
    w.raw(tail, 2);
    w.put_u32(7400u);

    std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_MSC_VER)
    localtime_s(&lt, &now);
#else
    if (const std::tm* p = std::localtime(&now)) lt = *p;
#endif
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d:000",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    const std::string creator = std::string("SA2Extractor FBX exporter - ") +
                                sa2::version();

    // ---- FBXHeaderExtension ----------------------------------------------
    w.begin("FBXHeaderExtension");
    w.nI("FBXHeaderVersion", 1003);
    w.nI("FBXVersion", 7400);
    w.nI("EncryptionType", 0);
    w.begin("CreationTimeStamp");
    w.nI("Version", 1000);
    w.nI("Year", lt.tm_year + 1900);
    w.nI("Month", lt.tm_mon + 1);
    w.nI("Day", lt.tm_mday);
    w.nI("Hour", lt.tm_hour);
    w.nI("Minute", lt.tm_min);
    w.nI("Second", lt.tm_sec);
    w.nI("Millisecond", 0);
    w.end();
    w.nS("Creator", creator);
    w.begin("SceneInfo");
    w.pS(fbx_name("GlobalInfo", "SceneInfo"));
    w.pS("UserData");
    w.nS("Type", "UserData");
    w.nI("Version", 100);
    w.begin("MetaData");
    w.nI("Version", 100);
    w.nS("Title", "");
    w.nS("Subject", "");
    w.nS("Author", "SA2Extractor");
    w.nS("Keywords", "");
    w.nS("Revision", "");
    w.nS("Comment", "");
    w.end();
    w.begin("Properties70");
    w.Pstring("DocumentUrl", "KString", "Url", "", out_path);
    w.Pstring("SrcDocumentUrl", "KString", "Url", "", out_path);
    w.Pcompound("Original");
    w.Pstring("Original|ApplicationVendor", "KString", "", "", "SA2Extractor");
    w.Pstring("Original|ApplicationName", "KString", "", "", "SA2Extractor");
    w.Pstring("Original|ApplicationVersion", "KString", "", "", sa2::version());
    w.Pstring("Original|FileName", "KString", "", "", out_path);
    w.Pcompound("LastSaved");
    w.Pstring("LastSaved|ApplicationVendor", "KString", "", "", "SA2Extractor");
    w.Pstring("LastSaved|ApplicationName", "KString", "", "", "SA2Extractor");
    w.Pstring("LastSaved|ApplicationVersion", "KString", "", "", sa2::version());
    w.end();
    w.end();   // SceneInfo
    w.end();   // FBXHeaderExtension

    static const uint8_t kFileId[16] = {
        0x28, 0xb3, 0x2a, 0xeb, 0xb6, 0x24, 0xcc, 0xc2,
        0xbf, 0xc8, 0xb0, 0x2a, 0xa9, 0x2b, 0xfc, 0xf1};
    w.begin("FileId"); w.pR(kFileId, 16); w.end();
    w.nS("CreationTime", stamp);
    w.nS("Creator", creator);

    // ---- GlobalSettings ---------------------------------------------------
    int64_t time_stop = 0;
    for (const AnimOut& a : sc_.anims) time_stop = std::max(time_stop, a.stop);

    w.begin("GlobalSettings");
    w.nI("Version", 1000);
    w.begin("Properties70");
    if (opts_.y_up) {
        // Y up, Z front, X right -- the FBX default; geometry is not rotated.
        w.Pint("UpAxis", "int", "Integer", "", 1);
        w.Pint("UpAxisSign", "int", "Integer", "", 1);
        w.Pint("FrontAxis", "int", "Integer", "", 2);
        w.Pint("FrontAxisSign", "int", "Integer", "", 1);
        w.Pint("CoordAxis", "int", "Integer", "", 0);
        w.Pint("CoordAxisSign", "int", "Integer", "", 1);
        w.Pint("OriginalUpAxis", "int", "Integer", "", 1);
        w.Pint("OriginalUpAxisSign", "int", "Integer", "", 1);
    } else {
        // Z up, -Y front, X right. Also only a declaration: no rotation.
        w.Pint("UpAxis", "int", "Integer", "", 2);
        w.Pint("UpAxisSign", "int", "Integer", "", 1);
        w.Pint("FrontAxis", "int", "Integer", "", 1);
        w.Pint("FrontAxisSign", "int", "Integer", "", -1);
        w.Pint("CoordAxis", "int", "Integer", "", 0);
        w.Pint("CoordAxisSign", "int", "Integer", "", 1);
        w.Pint("OriginalUpAxis", "int", "Integer", "", 2);
        w.Pint("OriginalUpAxisSign", "int", "Integer", "", 1);
    }
    w.Pdouble("UnitScaleFactor", "double", "Number", "", 1.0);
    w.Pdouble("OriginalUnitScaleFactor", "double", "Number", "", 1.0);
    w.Pvec3("AmbientColor", "ColorRGB", "Color", "", 0.0, 0.0, 0.0);
    w.Pstring("DefaultCamera", "KString", "", "", "Producer Perspective");
    w.Pint("TimeMode", "enum", "", "", 6);          // eFrames30
    w.Pint("TimeProtocol", "enum", "", "", 2);
    w.Pint("SnapOnFrameMode", "enum", "", "", 0);
    w.Ptime("TimeSpanStart", 0);
    w.Ptime("TimeSpanStop", time_stop);
    w.Pdouble("CustomFrameRate", "double", "Number", "", kExportFps);
    w.end();
    w.end();

    // ---- Documents / References ------------------------------------------
    w.begin("Documents");
    w.nI("Count", 1);
    w.begin("Document");
    w.pL(sc_.doc_id);
    w.pS("");
    w.pS("Scene");
    w.begin("Properties70");
    w.pBegin("SourceObject", "object", "", ""); w.end();
    w.Pstring("ActiveAnimStackName", "KString", "", "",
              sc_.anims.empty() ? std::string() : sc_.anims.front().name);
    w.end();
    w.nL("RootNode", 0);
    w.end();
    w.end();

    w.begin("References"); w.end();

    write_definitions(w, sc_);
    write_objects(w);
    write_connections(w);

    // ---- Takes ------------------------------------------------------------
    w.begin("Takes");
    w.nS("Current", sc_.anims.empty() ? std::string() : sc_.anims.front().name);
    for (const AnimOut& a : sc_.anims) {
        w.begin("Take");
        w.pS(a.name);
        w.nS("FileName", a.name + ".tak");
        w.begin("LocalTime");     w.pL(0); w.pL(a.stop); w.end();
        w.begin("ReferenceTime"); w.pL(0); w.pL(a.stop); w.end();
        w.end();
    }
    w.end();

    // ---- top-level list terminator + footer -------------------------------
    w.zeros(13);

    static const uint8_t kFootId[16] = {
        0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66,
        0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
    static const uint8_t kFootMagic[16] = {
        0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e,
        0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
    w.raw(kFootId, 16);
    w.zeros(16 - (w.size() % 16));    // 1..16 bytes of padding
    w.zeros(4);
    w.put_u32(7400u);
    w.zeros(120);
    w.raw(kFootMagic, 16);
}

bool Exporter::run(const std::string& out_path, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (out_path.empty()) return fail("empty output path");

    scale_ = san((double)opts_.scale);
    if (!(std::fabs(scale_) > 1e-9)) scale_ = 1.0;

    std::filesystem::path fbx_path;
    try {
        fbx_path = std::filesystem::path(out_path);
        if (fbx_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(fbx_path.parent_path(), ec);
        }
    } catch (...) {
        return fail("invalid output path");
    }

    sc_.root_name = model_names_.unique(sanitize_name(model_.name, "SA2Model"));
    sc_.root_id      = uid();
    sc_.root_attr_id = uid();
    sc_.doc_id       = uid();
    sc_.pose_id      = uid();

    plan_textures(fbx_path);
    plan_materials();
    plan_geometry();
    plan_skeleton();
    plan_animation();

    FbxWriter w;
    write(w, out_path);

    std::ofstream f(fbx_path, std::ios::binary | std::ios::trunc);
    if (!f) return fail("cannot open output file for writing");
    const std::vector<uint8_t>& b = w.bytes();
    if (!b.empty())
        f.write(reinterpret_cast<const char*>(b.data()),
                (std::streamsize)b.size());
    f.flush();
    if (!f) return fail("write failed (disk full or permission denied?)");
    f.close();
    return true;
}

}  // namespace

// ============================================================ public API

bool export_fbx(const std::string& out_path,
                const Model& model,
                const std::vector<Image>& textures,
                const std::vector<Motion>& motions,
                const FbxExportOptions& opts,
                std::string* error) {
    if (error) error->clear();
    try {
        Exporter ex(model, textures, motions, opts);
        return ex.run(out_path, error);
    } catch (const std::exception& e) {
        if (error) *error = std::string("FBX export failed: ") + e.what();
        return false;
    } catch (...) {
        if (error) *error = "FBX export failed: unknown error";
        return false;
    }
}

#if SA2_FBX_OWNS_WRITE_PNG
bool write_png(const std::string& path, const Image& img) {
    if (path.empty() || !img.valid()) return false;
    if ((size_t)img.width * (size_t)img.height * 4u > img.rgba.size()) return false;
    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    } catch (...) {
        // a failed mkdir is not fatal; stb will simply fail to open the file
    }
    return stbi_write_png(path.c_str(), img.width, img.height, 4,
                          img.rgba.data(), img.width * 4) != 0;
}
#endif

}  // namespace sa2
