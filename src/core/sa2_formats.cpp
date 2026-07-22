// PRS decompression, PAK archives and GVR/GVM/DDS/PNG texture decoding.
#include "sa2core.h"

#include <cstring>
#include <cmath>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace sa2 {

const char* version() { return "1.0.0"; }

// ============================================================== PRS
bool prs_looks_valid(const uint8_t* d, size_t n) {
    // The first token is always a literal (bit 0 set) and the stream is
    // terminated by a zero 16-bit long-copy descriptor.
    if (n < 4) return false;
    if ((d[0] & 1) == 0) return false;
    return d[n - 1] == 0 && d[n - 2] == 0;
}

std::vector<uint8_t> prs_decompress(const uint8_t* src, size_t n) {
    std::vector<uint8_t> dst;
    if (!src || n == 0) return dst;
    dst.reserve(n * 2);

    size_t sp = 0;
    int bitpos = 0;
    uint8_t ctrl = 0;

    auto getbit = [&]() -> int {
        if (bitpos == 0) {
            if (sp >= n) return -1;
            ctrl = src[sp++];
            bitpos = 8;
        }
        int f = ctrl & 1;
        ctrl >>= 1;
        bitpos--;
        return f;
    };

    for (;;) {
        int f = getbit();
        if (f < 0) break;
        if (f == 1) {                       // literal
            if (sp >= n) break;
            dst.push_back(src[sp++]);
            continue;
        }
        int len, off;
        f = getbit();
        if (f < 0) break;
        if (f == 1) {                       // long copy
            if (sp + 1 >= n) break;
            int w = src[sp] | (src[sp + 1] << 8);
            sp += 2;
            if (w == 0) break;              // end of stream
            len = w & 7;
            off = (w >> 3) - 8192;          // sign-extended 13-bit
            if (len == 0) {
                if (sp >= n) break;
                len = src[sp++] + 1;
            } else {
                len += 2;
            }
        } else {                            // short copy
            int b1 = getbit(), b0 = getbit();
            if (b1 < 0 || b0 < 0) break;
            len = ((b1 << 1) | b0) + 2;
            if (sp >= n) break;
            off = (int)src[sp++] - 256;     // sign-extended 8-bit
        }
        long long start = (long long)dst.size() + off;
        if (start < 0) break;               // corrupt back-reference
        for (int i = 0; i < len; i++) {
            dst.push_back(dst[(size_t)start + i]);   // may overlap: byte at a time
        }
    }
    return dst;
}

// ============================================================== PAK
static uint32_t rd_u32le(const uint8_t* d, size_t o) {
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
           ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
}

bool PakArchive::parse(const uint8_t* d, size_t n) {
    entries.clear();
    if (n < 0x3D) return false;
    if (!(d[0] == 0x01 && d[1] == 'p' && d[2] == 'a' && d[3] == 'k')) return false;

    uint32_t count = rd_u32le(d, 0x39);
    if (count > 100000) return false;

    size_t pos = 0x3D;
    std::vector<PakEntry> tmp;
    tmp.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 4 > n) return false;
        uint32_t ln = rd_u32le(d, pos); pos += 4;
        if (ln > n || pos + ln > n) return false;
        PakEntry e;
        e.long_path.assign((const char*)d + pos, ln); pos += ln;
        if (pos + 4 > n) return false;
        uint32_t sn = rd_u32le(d, pos); pos += 4;
        if (sn > n || pos + sn > n) return false;
        e.name.assign((const char*)d + pos, sn); pos += sn;
        if (pos + 8 > n) return false;
        e.length = rd_u32le(d, pos); pos += 8;   // length is stored twice
        tmp.push_back(std::move(e));
    }
    uint32_t off = (uint32_t)pos;
    for (auto& e : tmp) {
        e.offset = off;
        if ((size_t)off + e.length > n) return false;
        off += e.length;
    }
    entries = std::move(tmp);
    return true;
}

// ============================================================== GVR
// GameCube textures are stored in fixed-size tiles, row-major within a tile and
// tiles row-major across the image. Tile size depends on bit depth.
static uint16_t rd_u16be(const uint8_t* d, size_t o) {
    return (uint16_t)((d[o] << 8) | d[o + 1]);
}

static void rgb565_to_rgba(uint16_t v, uint8_t* o) {
    uint8_t r = (uint8_t)((v >> 11) & 0x1F);
    uint8_t g = (uint8_t)((v >> 5) & 0x3F);
    uint8_t b = (uint8_t)(v & 0x1F);
    o[0] = (uint8_t)((r << 3) | (r >> 2));
    o[1] = (uint8_t)((g << 2) | (g >> 4));
    o[2] = (uint8_t)((b << 3) | (b >> 2));
    o[3] = 255;
}

static void rgb5a3_to_rgba(const uint8_t* s, uint8_t* o) {
    if (s[0] & 0x80) {                 // opaque RGB555
        uint8_t r = (uint8_t)((s[0] & 0x7C) << 1);
        uint8_t g = (uint8_t)((s[0] << 6) | ((s[1] & 0xE0) >> 2));
        uint8_t b = (uint8_t)(s[1] << 3);
        o[0] = (uint8_t)(r | (r >> 5));
        o[1] = (uint8_t)(g | (g >> 5));
        o[2] = (uint8_t)(b | (b >> 5));
        o[3] = 255;
    } else {                           // ARGB3444
        uint8_t r = (uint8_t)(s[0] & 0x0F);
        uint8_t g = (uint8_t)(s[1] & 0xF0);
        uint8_t b = (uint8_t)(s[1] & 0x0F);
        uint8_t a = (uint8_t)((s[0] & 0x70) << 1);
        o[0] = (uint8_t)(r | (r << 4));
        o[1] = (uint8_t)(g | (g >> 4));
        o[2] = (uint8_t)(b | (b << 4));
        o[3] = (uint8_t)(a | (a >> 3) | (a >> 6));
    }
}

// GameCube CMPR: 8x8 macroblocks of four 4x4 DXT1 sub-blocks, big-endian
// endpoints, selector bits read MSB-first.
static void decode_cmpr(const uint8_t* src, size_t srcn, int w, int h,
                        std::vector<uint8_t>& out) {
    out.assign((size_t)w * h * 4, 0);
    size_t si = 0;
    int bw = std::min(w, 8), bh = std::min(h, 8);
    int tile = std::min(w, 4);
    for (int yb = 0; yb < h; yb += bh) {
        for (int xb = 0; xb < w; xb += bw) {
            for (int y = 0; y < bh; y += tile) {
                for (int x = 0; x < bw; x += tile) {
                    if (si + 8 > srcn) return;
                    uint16_t c0 = rd_u16be(src, si);
                    uint16_t c1 = rd_u16be(src, si + 2);
                    uint8_t pal[16];
                    rgb565_to_rgba(c0, pal);
                    rgb565_to_rgba(c1, pal + 4);
                    if (c0 > c1) {
                        for (int k = 0; k < 3; k++) {
                            pal[8 + k] = (uint8_t)((pal[k] * 2 + pal[4 + k]) / 3);
                            pal[12 + k] = (uint8_t)((pal[4 + k] * 2 + pal[k]) / 3);
                        }
                        pal[11] = 255; pal[15] = 255;
                    } else {
                        for (int k = 0; k < 3; k++)
                            pal[8 + k] = (uint8_t)((pal[k] + pal[4 + k]) / 2);
                        pal[11] = 255;
                        pal[12] = pal[13] = pal[14] = pal[15] = 0;
                    }
                    si += 4;
                    for (int y2 = 0; y2 < tile; y2++) {
                        uint8_t bits = src[si + y2];
                        for (int x2 = 0; x2 < tile; x2++) {
                            int ci = ((bits >> (6 - x2 * 2)) & 3) * 4;
                            int px = xb + x + x2, py = yb + y + y2;
                            if (px >= w || py >= h) continue;
                            uint8_t* d = &out[((size_t)py * w + px) * 4];
                            d[0] = pal[ci]; d[1] = pal[ci + 1];
                            d[2] = pal[ci + 2]; d[3] = pal[ci + 3];
                        }
                    }
                    si += 4;
                }
            }
        }
    }
}

static void decode_tiled(const uint8_t* src, size_t srcn, int w, int h,
                         int fmt, const uint8_t* clut, int clut_fmt,
                         std::vector<uint8_t>& out) {
    out.assign((size_t)w * h * 4, 0);
    int bw, bh, bpp;   // block dims and bits per pixel
    switch (fmt) {
        case 0x00: bw = 8; bh = 8; bpp = 4; break;   // I4
        case 0x08: bw = 8; bh = 8; bpp = 4; break;   // Index4
        case 0x01: bw = 8; bh = 4; bpp = 8; break;   // I8
        case 0x02: bw = 8; bh = 4; bpp = 8; break;   // IA4
        case 0x09: bw = 8; bh = 4; bpp = 8; break;   // Index8
        case 0x03: bw = 4; bh = 4; bpp = 16; break;  // IA8
        case 0x04: bw = 4; bh = 4; bpp = 16; break;  // RGB565
        case 0x05: bw = 4; bh = 4; bpp = 16; break;  // RGB5A3
        case 0x06: bw = 4; bh = 4; bpp = 32; break;  // ARGB8888
        default: return;
    }
    bw = std::min(bw, w); bh = std::min(bh, h);
    size_t si = 0;
    auto pal_lookup = [&](int idx, uint8_t* o) {
        if (!clut) { o[0] = o[1] = o[2] = 0; o[3] = 255; return; }
        const uint8_t* p = clut + (size_t)idx * 2;
        if (clut_fmt == 1) rgb565_to_rgba(rd_u16be(p, 0), o);
        else if (clut_fmt == 2) rgb5a3_to_rgba(p, o);
        else { // IntensityA8
            o[0] = o[1] = o[2] = p[1]; o[3] = p[0];
        }
    };
    for (int yb = 0; yb < h; yb += bh) {
        for (int xb = 0; xb < w; xb += bw) {
            if (fmt == 0x06) {   // ARGB8888: AR plane then GB plane per block
                for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                    if (si + 2 > srcn) return;
                    int px = xb + x, py = yb + y;
                    if (px < w && py < h) {
                        uint8_t* d = &out[((size_t)py * w + px) * 4];
                        d[3] = src[si]; d[0] = src[si + 1];
                    }
                    si += 2;
                }
                for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                    if (si + 2 > srcn) return;
                    int px = xb + x, py = yb + y;
                    if (px < w && py < h) {
                        uint8_t* d = &out[((size_t)py * w + px) * 4];
                        d[1] = src[si]; d[2] = src[si + 1];
                    }
                    si += 2;
                }
                continue;
            }
            for (int y = 0; y < bh; y++) {
                for (int x = 0; x < bw; x += (bpp == 4 ? 2 : 1)) {
                    if (si >= srcn) return;
                    int px = xb + x, py = yb + y;
                    uint8_t rgba[8]{};
                    int n_out = 1;
                    if (bpp == 4) {
                        n_out = 2;
                        uint8_t b = src[si];
                        int v0 = b >> 4, v1 = b & 0xF;
                        if (fmt == 0x00) {
                            rgba[0] = rgba[1] = rgba[2] = (uint8_t)(v0 * 17); rgba[3] = 255;
                            rgba[4] = rgba[5] = rgba[6] = (uint8_t)(v1 * 17); rgba[7] = 255;
                        } else {
                            pal_lookup(v0, rgba);
                            pal_lookup(v1, rgba + 4);
                        }
                        si += 1;
                    } else if (bpp == 8) {
                        uint8_t b = src[si];
                        if (fmt == 0x01) { rgba[0] = rgba[1] = rgba[2] = b; rgba[3] = 255; }
                        else if (fmt == 0x02) {
                            uint8_t i = (uint8_t)((b & 0x0F) * 17);
                            uint8_t a = (uint8_t)((b >> 4) * 17);
                            rgba[0] = rgba[1] = rgba[2] = i; rgba[3] = a;
                        } else pal_lookup(b, rgba);
                        si += 1;
                    } else {  // 16bpp
                        if (si + 2 > srcn) return;
                        if (fmt == 0x03) {
                            rgba[0] = rgba[1] = rgba[2] = src[si + 1]; rgba[3] = src[si];
                        } else if (fmt == 0x04) rgb565_to_rgba(rd_u16be(src, si), rgba);
                        else rgb5a3_to_rgba(src + si, rgba);
                        si += 2;
                    }
                    for (int k = 0; k < n_out; k++) {
                        int qx = px + k;
                        if (qx < w && py < h)
                            memcpy(&out[((size_t)py * w + qx) * 4], rgba + k * 4, 4);
                    }
                }
            }
        }
    }
}

static int gvr_level_size(int w, int h, int fmt) {
    int raw;
    switch (fmt) {
        case 0x00: case 0x08: case 0x0E: raw = w * h / 2; break;
        case 0x01: case 0x02: case 0x09: raw = w * h; break;
        case 0x03: case 0x04: case 0x05: raw = w * h * 2; break;
        case 0x06: raw = std::max(w * h * 4, 64); break;
        default: raw = w * h * 2; break;
    }
    return std::max(32, raw);
}

bool gvr_decode(const uint8_t* d, size_t n, Image& out) {
    if (!d || n < 0x10) return false;
    size_t o = 0;
    if (n >= 8 && (memcmp(d, "GBIX", 4) == 0 || memcmp(d, "GCIX", 4) == 0)) {
        uint32_t cl = rd_u32le(d, 4);
        o = 8 + cl;
    }
    if (o + 0x10 > n || memcmp(d + o, "GVRT", 4) != 0) return false;
    uint32_t dlen = rd_u32le(d, o + 4);
    uint8_t pal_and_flags = d[o + 0x0A];
    uint8_t fmt = d[o + 0x0B];
    int pal_fmt = (pal_and_flags >> 4) & 0xF;
    int flags = pal_and_flags & 0xF;
    int w = rd_u16be(d, o + 0x0C);
    int h = rd_u16be(d, o + 0x0E);
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return false;

    size_t body = o + 0x10;
    size_t avail = (dlen + 8 <= n - o) ? (size_t)dlen - 8 : (n - body);
    if (body >= n) return false;
    avail = std::min(avail, n - body);

    const uint8_t* clut = nullptr;
    if ((fmt == 0x08 || fmt == 0x09) && (flags & 0x8)) {
        clut = d + body;
        size_t clut_bytes = (fmt == 0x08 ? 16u : 256u) * 2u;
        if (body + clut_bytes > n) return false;
        body += clut_bytes;
        avail = (avail > clut_bytes) ? avail - clut_bytes : 0;
    }

    out.width = w;
    out.height = h;
    if (fmt == 0x0E) decode_cmpr(d + body, avail, w, h, out.rgba);
    else decode_tiled(d + body, avail, w, h, fmt, clut, pal_fmt, out.rgba);
    return out.valid();
}

bool gvm_extract(const uint8_t* d, size_t n, std::vector<Image>& out) {
    if (!d || n < 12 || memcmp(d, "GVMH", 4) != 0) return false;
    uint32_t hdr = rd_u32le(d, 4);
    uint16_t flags = rd_u16be(d, 8);
    uint16_t count = rd_u16be(d, 10);
    if (count > 20000) return false;

    bool has_names = (flags & 0x8) != 0;
    bool has_fmt = (flags & 0x4) != 0;
    bool has_dims = (flags & 0x2) != 0;
    bool has_gbix = (flags & 0x1) != 0;

    std::vector<std::string> names;
    size_t p = 12;
    for (uint16_t i = 0; i < count; i++) {
        if (p + 2 > n) break;
        p += 2;                       // index
        std::string nm;
        if (has_names) {
            if (p + 28 > n) break;
            size_t len = 0;
            while (len < 28 && d[p + len]) len++;
            nm.assign((const char*)d + p, len);
            // names are space padded
            while (!nm.empty() && nm.back() == ' ') nm.pop_back();
            p += 28;
        }
        if (has_fmt) p += 2;
        if (has_dims) p += 2;
        if (has_gbix) p += 4;
        names.push_back(nm);
    }

    size_t o = (size_t)hdr + 8;
    for (uint16_t i = 0; i < count && o + 8 <= n; i++) {
        size_t start = o;
        if (memcmp(d + start, "GBIX", 4) == 0 || memcmp(d + start, "GCIX", 4) == 0) {
            uint32_t cl = rd_u32le(d, start + 4);
            start += 8 + cl;
        }
        if (start + 8 > n || memcmp(d + start, "GVRT", 4) != 0) break;
        uint32_t dlen = rd_u32le(d, start + 4);
        Image img;
        if (gvr_decode(d + o, n - o, img)) {
            img.name = (i < names.size() && !names[i].empty())
                           ? names[i] : ("texture_" + std::to_string(i));
            out.push_back(std::move(img));
        }
        o = start + 8 + dlen;
    }
    return !out.empty();
}

// ============================================================== DDS
static void dxt_block(const uint8_t* s, uint8_t pal[16], bool dxt1) {
    uint16_t c0 = (uint16_t)(s[0] | (s[1] << 8));
    uint16_t c1 = (uint16_t)(s[2] | (s[3] << 8));
    auto exp565 = [](uint16_t v, uint8_t* o) {
        uint8_t r = (uint8_t)((v >> 11) & 0x1F), g = (uint8_t)((v >> 5) & 0x3F),
                b = (uint8_t)(v & 0x1F);
        o[0] = (uint8_t)((r << 3) | (r >> 2));
        o[1] = (uint8_t)((g << 2) | (g >> 4));
        o[2] = (uint8_t)((b << 3) | (b >> 2));
        o[3] = 255;
    };
    exp565(c0, pal); exp565(c1, pal + 4);
    if (!dxt1 || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            pal[8 + k] = (uint8_t)((pal[k] * 2 + pal[4 + k]) / 3);
            pal[12 + k] = (uint8_t)((pal[4 + k] * 2 + pal[k]) / 3);
        }
        pal[11] = pal[15] = 255;
    } else {
        for (int k = 0; k < 3; k++)
            pal[8 + k] = (uint8_t)((pal[k] + pal[4 + k]) / 2);
        pal[11] = 255;
        pal[12] = pal[13] = pal[14] = pal[15] = 0;
    }
}

bool dds_decode(const uint8_t* d, size_t n, Image& out) {
    if (!d || n < 128 || memcmp(d, "DDS ", 4) != 0) return false;
    uint32_t h = rd_u32le(d, 12), w = rd_u32le(d, 16);
    uint32_t pf_flags = rd_u32le(d, 80);
    uint32_t fourcc = rd_u32le(d, 84);
    uint32_t rgb_bits = rd_u32le(d, 88);
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return false;
    out.width = (int)w; out.height = (int)h;
    out.rgba.assign((size_t)w * h * 4, 255);
    const uint8_t* s = d + 128;
    size_t avail = n - 128;

    bool compressed = (pf_flags & 0x4) != 0;
    if (compressed) {
        int bpb = (fourcc == 0x31545844u /*DXT1*/) ? 8 : 16;   // else DXT3/DXT5
        bool dxt1 = (bpb == 8);
        bool dxt5 = (fourcc == 0x35545844u);
        bool dxt3 = (fourcc == 0x33545844u);
        size_t si = 0;
        for (uint32_t by = 0; by < h; by += 4) {
            for (uint32_t bx = 0; bx < w; bx += 4) {
                if (si + bpb > avail) return true;
                const uint8_t* blk = s + si;
                uint8_t alpha[16];
                for (int i = 0; i < 16; i++) alpha[i] = 255;
                if (dxt3) {
                    for (int i = 0; i < 16; i++) {
                        uint8_t v = blk[i / 2];
                        uint8_t a4 = (i & 1) ? (v >> 4) : (v & 0xF);
                        alpha[i] = (uint8_t)(a4 * 17);
                    }
                    blk += 8;
                } else if (dxt5) {
                    uint8_t a0 = blk[0], a1 = blk[1];
                    uint8_t at[8];
                    at[0] = a0; at[1] = a1;
                    if (a0 > a1) for (int k = 1; k < 7; k++)
                        at[k + 1] = (uint8_t)(((7 - k) * a0 + k * a1) / 7);
                    else {
                        for (int k = 1; k < 5; k++)
                            at[k + 1] = (uint8_t)(((5 - k) * a0 + k * a1) / 5);
                        at[6] = 0; at[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int k = 0; k < 6; k++) bits |= (uint64_t)blk[2 + k] << (8 * k);
                    for (int i = 0; i < 16; i++) alpha[i] = at[(bits >> (3 * i)) & 7];
                    blk += 8;
                }
                uint8_t pal[16];
                dxt_block(blk, pal, dxt1);
                uint32_t bitsC = rd_u32le(blk, 4);
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
                    uint32_t px = bx + x, py = by + y;
                    if (px >= w || py >= h) continue;
                    int ci = ((bitsC >> (2 * (y * 4 + x))) & 3) * 4;
                    uint8_t* o = &out.rgba[((size_t)py * w + px) * 4];
                    o[0] = pal[ci]; o[1] = pal[ci + 1]; o[2] = pal[ci + 2];
                    o[3] = (dxt1) ? pal[ci + 3] : alpha[y * 4 + x];
                }
                si += bpb;
            }
        }
        return true;
    }
    // uncompressed
    int bytes = (int)(rgb_bits / 8);
    if (bytes != 3 && bytes != 4) return false;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        size_t si = ((size_t)y * w + x) * bytes;
        if (si + bytes > avail) return true;
        uint8_t* o = &out.rgba[((size_t)y * w + x) * 4];
        o[0] = s[si + 2]; o[1] = s[si + 1]; o[2] = s[si];
        o[3] = (bytes == 4) ? s[si + 3] : 255;
    }
    return true;
}

bool png_decode(const uint8_t* d, size_t n, Image& out) {
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(d, (int)n, &w, &h, &comp, 4);
    if (!px) return false;
    out.width = w; out.height = h;
    out.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

bool write_png(const std::string& path, const Image& img) {
    if (!img.valid()) return false;
    return stbi_write_png(path.c_str(), img.width, img.height, 4,
                          img.rgba.data(), img.width * 4) != 0;
}

}  // namespace sa2
