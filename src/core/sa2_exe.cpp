// sonic2app.exe stores its enemy / object / NPC models as little-endian Ninja
// object trees compiled into the executable's .data section. This builds a flat
// virtual image from the PE (each section placed at its RVA) so the ordinary
// NinjaBlob(image, image_base, /*be=*/false) can resolve the absolute virtual
// pointers and find_model_roots / build_model can extract them.
#include "sa2core.h"

#include <algorithm>
#include <cstring>

namespace sa2 {

static uint32_t le32(const uint8_t* d, size_t o) {
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16) |
           ((uint32_t)d[o + 3] << 24);
}
static uint16_t le16(const uint8_t* d, size_t o) {
    return (uint16_t)((uint32_t)d[o] | ((uint32_t)d[o + 1] << 8));
}

bool load_pe_image(const std::string& path, std::vector<uint8_t>& image,
                   uint32_t& base) {
    auto d = read_file(path);
    size_t n = d.size();
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
    uint32_t e_lfanew = le32(d.data(), 0x3C);
    if (e_lfanew + 24 > n || memcmp(d.data() + e_lfanew, "PE\0\0", 4) != 0)
        return false;
    size_t coff = e_lfanew + 4;
    uint16_t nsec = le16(d.data(), coff + 2);
    uint16_t opt_size = le16(d.data(), coff + 16);
    size_t opt = coff + 20;
    if (opt + 32 > n) return false;
    uint16_t magic = le16(d.data(), opt);
    if (magic == 0x10b) base = le32(d.data(), opt + 28);        // PE32
    else if (magic == 0x20b) base = (uint32_t)le32(d.data(), opt + 24);  // PE32+
    else return false;
    size_t sect = opt + opt_size;
    if (sect + (size_t)nsec * 40 > n) return false;

    // first pass: total virtual size
    uint32_t top = 0;
    for (int i = 0; i < nsec; i++) {
        size_t o = sect + (size_t)i * 40;
        uint32_t vsize = le32(d.data(), o + 8), vaddr = le32(d.data(), o + 12);
        uint32_t rsize = le32(d.data(), o + 16);
        uint32_t span = vaddr + (vsize > rsize ? vsize : rsize);
        if (span > top) top = span;
    }
    if (top == 0 || top > 0x08000000u) return false;   // sanity: < 128 MB
    image.assign(top, 0);
    for (int i = 0; i < nsec; i++) {
        size_t o = sect + (size_t)i * 40;
        uint32_t vaddr = le32(d.data(), o + 12);
        uint32_t rsize = le32(d.data(), o + 16), roff = le32(d.data(), o + 20);
        if ((size_t)roff + rsize > n) rsize = (uint32_t)(n - roff);
        if ((size_t)vaddr + rsize <= image.size())
            memcpy(image.data() + vaddr, d.data() + roff, rsize);
    }
    return true;
}

// Extract every embedded model root worth showing (>= min_tris triangles),
// most-detailed first. Returns the roots; the caller builds them with the same
// NinjaBlob. Kept here so the flat image is built once.
std::vector<uint32_t> find_exe_models(const std::vector<uint8_t>& image,
                                      uint32_t base, int min_tris,
                                      std::vector<int>* tri_counts) {
    NinjaBlob blob(image, base, false);
    auto roots = blob.find_model_roots();
    std::vector<std::pair<uint32_t, int>> kept;
    for (uint32_t r : roots) {
        Model m;
        if (!blob.build_model(r, m)) continue;
        int t = (int)m.triangle_count();
        if (t >= min_tris) kept.push_back({r, t});
    }
    std::sort(kept.begin(), kept.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<uint32_t> out;
    for (auto& kv : kept) {
        out.push_back(kv.first);
        if (tri_counts) tri_counts->push_back(kv.second);
    }
    return out;
}

}  // namespace sa2
