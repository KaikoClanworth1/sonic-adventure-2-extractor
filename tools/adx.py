"""CRI ADX (type 3/4) decoder -> PCM16, plus WAV writer. SA2 PC audio lives in
resource/gd_PC/ADX/*.adx. Standard 2-coefficient ADPCM; the predictor
coefficients come from the header's highpass cutoff and sample rate."""
import struct, math, sys, os


def decode_adx(data, max_samples=None):
    if struct.unpack_from(">H", data, 0)[0] != 0x8000:
        raise ValueError("not ADX")
    co = struct.unpack_from(">H", data, 2)[0]
    enc = data[4]; bs = data[5]; bd = data[6]; ch = data[7]
    sr = struct.unpack_from(">I", data, 8)[0]
    total = struct.unpack_from(">I", data, 12)[0]
    hp = struct.unpack_from(">H", data, 16)[0]
    if enc not in (2, 3, 4):
        raise ValueError(f"unsupported ADX encoding {enc}")
    start = co + 4

    # standard ADX predictor coefficients from the highpass cutoff
    sqrt2 = math.sqrt(2.0)
    z = math.cos(2.0 * math.pi * hp / sr)
    a = sqrt2 - z
    b = sqrt2 - 1.0
    c = (a - math.sqrt((a + b) * (a - b))) / b
    coef1 = int(c * 2.0 * 4096)
    coef2 = int(-(c * c) * 4096)

    spb = (bs - 2) * 8 // bd            # samples per block (32 for 18-byte/4-bit)
    hist = [[0, 0] for _ in range(ch)]
    out = [[] for _ in range(ch)]
    pos = start
    n = len(data)
    want = total if max_samples is None else min(total, max_samples)
    produced = 0
    while pos + bs * ch <= n and produced < want:
        for ci in range(ch):
            scale = struct.unpack_from(">H", data, pos)[0]
            blk = data[pos + 2:pos + bs]
            h1, h2 = hist[ci]
            oc = out[ci]
            for i in range(spb):
                byte = blk[i >> 1]
                nib = (byte >> 4) if (i & 1) == 0 else (byte & 0xF)
                if nib >= 8:
                    nib -= 16
                pred = (coef1 * h1 + coef2 * h2) >> 12
                s = nib * scale + pred
                if s > 32767: s = 32767
                elif s < -32768: s = -32768
                oc.append(s)
                h2 = h1; h1 = s
            hist[ci] = [h1, h2]
            pos += bs
        produced += spb

    # trim + interleave to int16
    for ci in range(ch):
        del out[ci][want:]
    frames = min(len(o) for o in out)
    inter = bytearray(frames * ch * 2)
    mv = memoryview(inter).cast("h")
    for ci in range(ch):
        oc = out[ci]
        for f in range(frames):
            mv[f * ch + ci] = oc[f]
    return sr, ch, bytes(inter), total


def write_wav(path, sr, ch, pcm):
    import wave
    w = wave.open(path, "wb")
    w.setnchannels(ch); w.setsampwidth(2); w.setframerate(sr)
    w.writeframes(pcm); w.close()


if __name__ == "__main__":
    src = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    data = open(src, "rb").read()
    sr = struct.unpack_from(">I", data, 8)[0]
    srate, ch, pcm, total = decode_adx(data, max_samples=int(sr * secs))
    dst = os.path.join(r"F:\ClaudeProjects\Sonic Adventure 2\build",
                       os.path.splitext(os.path.basename(src))[0] + ".wav")
    write_wav(dst, srate, ch, pcm)
    nf = len(pcm) // (ch * 2)
    # stats: clipping fraction + peak, to sanity-check the coefficients
    mv = memoryview(pcm).cast("h")
    clip = sum(1 for i in range(0, len(mv), max(1, len(mv) // 20000)) if abs(mv[i]) >= 32767)
    print(f"{os.path.basename(src)}: {srate} Hz x{ch}, total={total} "
          f"({total/srate:.1f}s), decoded {nf} frames -> {dst}")
    print(f"  clip-sample fraction (sampled): {clip/20000:.4f} (low = coeffs OK)")
