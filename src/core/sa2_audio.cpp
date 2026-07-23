// CRI ADX decoder (SA2 PC keeps its music as resource/gd_PC/ADX/*.adx) and a
// 16-bit PCM WAV writer. Standard 2-coefficient ADX ADPCM: each channel block
// is a big-endian u16 scale followed by 4-bit signed nibbles, and the predictor
// coefficients come from the header's highpass cutoff and sample rate.
#include "sa2core.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace sa2 {

static uint16_t be16(const uint8_t* d, size_t o) {
    return (uint16_t)((d[o] << 8) | d[o + 1]);
}
static uint32_t be32(const uint8_t* d, size_t o) {
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
           ((uint32_t)d[o + 2] << 8) | d[o + 3];
}

bool decode_adx(const uint8_t* d, size_t n, AudioClip& out) {
    if (n < 20 || be16(d, 0) != 0x8000) return false;
    uint32_t co = be16(d, 2);
    int enc = d[4], bs = d[5], bd = d[6], ch = d[7];
    uint32_t sr = be32(d, 8), total = be32(d, 12), hp = be16(d, 16);
    if ((enc != 2 && enc != 3 && enc != 4) || ch < 1 || ch > 2 || bs < 3 ||
        bd != 4 || sr == 0 || total == 0)
        return false;
    size_t start = (size_t)co + 4;
    if (start >= n) return false;

    // standard ADX predictor coefficients (fixed point, 1/4096)
    double sqrt2 = std::sqrt(2.0);
    double z = std::cos(2.0 * 3.14159265358979323846 * (double)hp / (double)sr);
    double a = sqrt2 - z, b = sqrt2 - 1.0;
    double c = (a - std::sqrt((a + b) * (a - b))) / b;
    int32_t coef1 = (int32_t)std::lround(c * 2.0 * 4096.0);
    int32_t coef2 = (int32_t)std::lround(-(c * c) * 4096.0);

    int spb = (bs - 2) * 8 / bd;   // samples per block (32 for 18-byte/4-bit)
    out.sample_rate = (int)sr;
    out.channels = ch;
    out.pcm.assign((size_t)total * ch, 0);

    int32_t h1[2] = {0, 0}, h2[2] = {0, 0};
    size_t pos = start;
    uint32_t produced = 0;
    while (pos + (size_t)bs * ch <= n && produced < total) {
        int frames = spb;
        if (produced + frames > total) frames = (int)(total - produced);
        for (int ci = 0; ci < ch; ci++) {
            int32_t scale = be16(d, pos);
            const uint8_t* blk = d + pos + 2;
            int32_t p1 = h1[ci], p2 = h2[ci];
            for (int i = 0; i < frames; i++) {
                uint8_t byte = blk[i >> 1];
                int nib = (i & 1) == 0 ? (byte >> 4) : (byte & 0xF);
                if (nib >= 8) nib -= 16;
                int32_t pred = (coef1 * p1 + coef2 * p2) >> 12;
                int32_t s = nib * scale + pred;
                if (s > 32767) s = 32767;
                else if (s < -32768) s = -32768;
                out.pcm[(size_t)(produced + i) * ch + ci] = (int16_t)s;
                p2 = p1; p1 = s;
            }
            // advance histories over any samples in this block beyond `frames`
            for (int i = frames; i < spb; i++) {
                uint8_t byte = blk[i >> 1];
                int nib = (i & 1) == 0 ? (byte >> 4) : (byte & 0xF);
                if (nib >= 8) nib -= 16;
                int32_t pred = (coef1 * p1 + coef2 * p2) >> 12;
                int32_t s = nib * scale + pred;
                if (s > 32767) s = 32767;
                else if (s < -32768) s = -32768;
                p2 = p1; p1 = s;
            }
            h1[ci] = p1; h2[ci] = p2;
            pos += bs;
        }
        produced += spb;
    }
    if (produced < total) out.pcm.resize((size_t)produced * ch);
    return !out.pcm.empty();
}

bool write_wav(const std::string& path, const AudioClip& clip) {
    if (!clip.valid()) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    uint32_t data_bytes = (uint32_t)(clip.pcm.size() * 2);
    uint32_t byte_rate = (uint32_t)clip.sample_rate * clip.channels * 2;
    uint16_t block_align = (uint16_t)(clip.channels * 2);
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + data_bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16((uint16_t)clip.channels);
    w32((uint32_t)clip.sample_rate); w32(byte_rate); w16(block_align); w16(16);
    fwrite("data", 1, 4, f); w32(data_bytes);
    fwrite(clip.pcm.data(), 1, data_bytes, f);
    fclose(f);
    return true;
}

}  // namespace sa2
