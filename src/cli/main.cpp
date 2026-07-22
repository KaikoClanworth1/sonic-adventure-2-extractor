// sa2cli - command line harness and batch regression driver.
#include "sa2core.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <map>
#include <algorithm>
#include <filesystem>
#include <chrono>

namespace fs = std::filesystem;
using namespace sa2;

static void usage() {
    printf(
        "sa2cli %s - Sonic Adventure 2 asset tool\n\n"
        "  sa2cli list      <game>            list and classify every asset\n"
        "  sa2cli info      <file>            identify one file\n"
        "  sa2cli extract   <file> <outdir>   extract textures / PAK contents\n"
        "  sa2cli model     <file>            summarise the models in a file\n"
        "  sa2cli fbx       <file> <out.fbx> [n]  export model n (default 0,\n"
        "                                         -1 = every model merged)\n"
        "  sa2cli regress   <game>            batch-test every parser on every file\n"
        "  sa2cli search    <game> <query>    search asset names\n",
        version());
}

static std::string human(uint64_t n) {
    char buf[64];
    if (n > (1u << 30)) snprintf(buf, sizeof buf, "%.1f GB", n / 1073741824.0);
    else if (n > (1u << 20)) snprintf(buf, sizeof buf, "%.1f MB", n / 1048576.0);
    else if (n > 1024) snprintf(buf, sizeof buf, "%.1f KB", n / 1024.0);
    else snprintf(buf, sizeof buf, "%llu B", (unsigned long long)n);
    return buf;
}

static int cmd_list(const char* game) {
    GameIndex idx;
    if (!idx.scan(game)) { printf("could not scan '%s'\n", game); return 1; }
    std::map<std::string, int> bykind;
    std::map<std::string, uint64_t> bysize;
    for (auto& e : idx.entries()) {
        bykind[kind_name(e.kind)]++;
        bysize[kind_name(e.kind)] += e.size;
    }
    printf("Indexed %zu files under %s\n\n", idx.entries().size(), idx.root().c_str());
    printf("  %-22s %8s  %12s\n", "KIND", "COUNT", "SIZE");
    for (auto& kv : bykind)
        printf("  %-22s %8d  %12s\n", kv.first.c_str(), kv.second,
               human(bysize[kv.first]).c_str());
    return 0;
}

static int cmd_search(const char* game, const char* q) {
    GameIndex idx;
    if (!idx.scan(game)) { printf("could not scan '%s'\n", game); return 1; }
    auto hits = idx.search(q, 200);
    printf("%zu matches for \"%s\"\n", hits.size(), q);
    for (int i : hits) {
        const auto& e = idx.entries()[i];
        printf("  %-58s %-20s %10s\n", e.rel_path.c_str(), kind_name(e.kind),
               human(e.size).c_str());
    }
    return 0;
}

static int cmd_info(const char* path) {
    auto raw = read_file(path);
    if (raw.empty()) { printf("cannot read %s\n", path); return 1; }
    printf("file      : %s\n", path);
    printf("size      : %s\n", human(raw.size()).c_str());
    bool prs = prs_looks_valid(raw.data(), raw.size());
    printf("PRS       : %s\n", prs ? "yes" : "no");
    auto data = load_file(path);
    if (prs) printf("unpacked  : %s\n", human(data.size()).c_str());
    if (data.size() >= 4) {
        printf("magic     : %02x %02x %02x %02x  '%c%c%c%c'\n",
               data[0], data[1], data[2], data[3],
               isprint(data[0]) ? data[0] : '.', isprint(data[1]) ? data[1] : '.',
               isprint(data[2]) ? data[2] : '.', isprint(data[3]) ? data[3] : '.');
    }
    std::vector<Image> imgs;
    if (data.size() >= 4 && memcmp(data.data(), "GVMH", 4) == 0) {
        gvm_extract(data.data(), data.size(), imgs);
        printf("type      : GVM texture archive, %zu textures\n", imgs.size());
        for (size_t i = 0; i < imgs.size() && i < 10; i++)
            printf("   [%2zu] %-24s %dx%d\n", i, imgs[i].name.c_str(),
                   imgs[i].width, imgs[i].height);
    }
    PakArchive pak;
    if (pak.parse(data.data(), data.size())) {
        printf("type      : PAK archive, %zu entries\n", pak.entries.size());
        for (size_t i = 0; i < pak.entries.size() && i < 10; i++)
            printf("   [%2zu] %-40s %s\n", i, pak.entries[i].name.c_str(),
                   human(pak.entries[i].length).c_str());
    }
    auto mdl = read_mdl_table(data.data(), data.size());
    if (mdl.size() > 2) printf("type      : MDL container, %zu models\n", mdl.size());
    auto mtn = read_mtn_table(data.data(), data.size());
    if (mtn.size() > 2) printf("type      : MTN container, %zu motions\n", mtn.size());
    return 0;
}

static int cmd_extract(const char* path, const char* outdir) {
    std::error_code ec;
    fs::create_directories(outdir, ec);
    std::vector<Image> imgs;
    if (load_textures(path, imgs)) {
        int n = 0;
        for (auto& img : imgs) {
            std::string name = img.name.empty()
                                   ? ("tex_" + std::to_string(n)) : img.name;
            std::string out = std::string(outdir) + "/" + name + ".png";
            if (write_png(out, img)) n++;
        }
        printf("wrote %d PNG(s) to %s\n", n, outdir);
        return 0;
    }
    auto data = load_file(path);
    PakArchive pak;
    if (pak.parse(data.data(), data.size())) {
        int n = 0;
        for (auto& e : pak.entries) {
            std::string out = std::string(outdir) + "/" +
                              fs::path(e.name).filename().string();
            FILE* f = nullptr;
#ifdef _WIN32
            fopen_s(&f, out.c_str(), "wb");
#else
            f = fopen(out.c_str(), "wb");
#endif
            if (!f) continue;
            fwrite(data.data() + e.offset, 1, e.length, f);
            fclose(f);
            n++;
        }
        printf("wrote %d file(s) to %s\n", n, outdir);
        return 0;
    }
    printf("nothing extractable in %s\n", path);
    return 1;
}

// Classify a single path the same way GameIndex would, without a full scan.
static AssetEntry entry_for(const char* path) {
    AssetEntry e;
    e.path = path;
    e.rel_path = path;
    e.name = fs::path(path).filename().string();
    std::string nl = e.name;
    for (auto& c : nl) c = (char)tolower((unsigned char)c);
    std::string rl = e.rel_path;
    for (auto& c : rl) c = (char)tolower((unsigned char)c);
    auto ends = [&](const char* s) {
        size_t n = strlen(s);
        return nl.size() >= n && nl.compare(nl.size() - n, n, s) == 0;
    };
    if (ends("mdl.prs")) e.kind = AssetKind::CharacterModel;
    else if (ends("mtn.prs")) e.kind = AssetKind::CharacterMotion;
    else if (ends(".pak")) e.kind = AssetKind::PakArchive;
    else if (ends(".gvr") || ends(".dds") || ends(".png")) e.kind = AssetKind::Texture;
    else if (rl.find("event") != std::string::npos && nl.size() > 5 &&
             nl[0] == 'e' && isdigit((unsigned char)nl[1]) &&
             nl.find("texture") == std::string::npos &&
             nl.find("texlist") == std::string::npos)
        e.kind = AssetKind::EventScene;
    else e.kind = AssetKind::TextureArchive;
    e.compressed = ends(".prs");
    return e;
}

static int cmd_model(const char* path) {
    GameIndex idx;
    LoadedAsset la;
    std::string err;
    if (!load_asset(entry_for(path), idx, la, &err)) {
        printf("failed: %s\n", err.c_str());
        return 1;
    }
    printf("%s\n", path);
    printf("  models   : %zu\n", la.models.size());
    printf("  motions  : %zu\n", la.motions.size());
    printf("  textures : %zu\n", la.textures.size());
    size_t tris = 0, verts = 0, nodes = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (auto& m : la.models) {
        tris += m.triangle_count();
        verts += m.vertex_count();
        nodes += m.nodes.size();
        float a[3], b[3];
        m.bounds(a, b);
        for (int k = 0; k < 3; k++) {
            lo[k] = std::min(lo[k], a[k]);
            hi[k] = std::max(hi[k], b[k]);
        }
    }
    printf("  nodes    : %zu\n", nodes);
    printf("  vertices : %zu\n", verts);
    printf("  triangles: %zu\n", tris);
    if (tris)
        printf("  bounds   : (%.2f %.2f %.2f) .. (%.2f %.2f %.2f)\n",
               lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    for (size_t i = 0; i < la.models.size() && i < 8; i++)
        printf("    [%zu] %-14s nodes=%-4zu tris=%zu\n", i, la.models[i].name.c_str(),
               la.models[i].nodes.size(), la.models[i].triangle_count());
    return 0;
}

static int cmd_fbx(const char* path, const char* out, int model_index) {
    GameIndex idx;
    LoadedAsset la;
    std::string err;
    if (!load_asset(entry_for(path), idx, la, &err)) {
        printf("failed: %s\n", err.c_str());
        return 1;
    }
    if (la.models.empty()) { printf("no models in %s\n", path); return 1; }

    // A container holds many independent models, all rooted at the origin, so
    // merging them all stacks them on top of each other. Pick one unless the
    // caller explicitly asked for everything.
    std::vector<Model> subset;
    if (model_index >= 0) {
        if (model_index >= (int)la.models.size()) {
            printf("model index %d out of range (%zu models)\n", model_index,
                   la.models.size());
            return 1;
        }
        subset.push_back(la.models[model_index]);
    } else {
        subset = la.models;
    }

    Model merged;
    merged.name = fs::path(path).stem().string();
    int node_base = 0;
    for (auto& m : subset) {
        for (auto n : m.nodes) {
            if (n.parent >= 0) n.parent += node_base;
            n.index += node_base;
            merged.nodes.push_back(n);
        }
        for (auto p : m.parts) {
            p.node_index += node_base;
            for (auto& vn : p.vertex_node) vn += node_base;
            merged.parts.push_back(std::move(p));
        }
        node_base = (int)merged.nodes.size();
        if (merged.parts.size() > 6000) break;
    }
    FbxExportOptions opts;
    std::string e2;
    if (!export_fbx(out, merged, la.textures, la.motions, opts, &e2)) {
        printf("FBX export failed: %s\n", e2.c_str());
        return 1;
    }
    printf("wrote %s\n", out);
    printf("  models=%zu/%zu meshes=%zu nodes=%zu tris=%zu textures=%zu anims=%zu\n",
           subset.size(), la.models.size(), merged.parts.size(), merged.nodes.size(),
           merged.triangle_count(), la.textures.size(), la.motions.size());
    return 0;
}

static int cmd_regress(const char* game) {
    GameIndex idx;
    if (!idx.scan(game)) { printf("could not scan '%s'\n", game); return 1; }
    auto t0 = std::chrono::steady_clock::now();

    int prs_ok = 0, prs_fail = 0;
    int pak_ok = 0, pak_fail = 0;
    int gvm_ok = 0, gvm_fail = 0;
    long long tex_total = 0;
    int mdl_ok = 0, mdl_fail = 0, mtn_ok = 0, mtn_fail = 0, evt_ok = 0, evt_fail = 0;
    long long tris = 0, verts = 0, motions = 0, keys = 0;
    int suspect = 0;

    for (const auto& e : idx.entries()) {
        if (e.compressed) {
            auto raw = read_file(e.path);
            auto dec = prs_decompress(raw.data(), raw.size());
            if (dec.empty()) { prs_fail++; continue; }
            prs_ok++;
        }
        if (e.kind == AssetKind::PakArchive) {
            auto raw = read_file(e.path);
            PakArchive p;
            if (p.parse(raw.data(), raw.size())) pak_ok++; else pak_fail++;
            continue;
        }
        if (e.kind == AssetKind::Audio || e.kind == AssetKind::Video) continue;

        auto data = load_file(e.path);
        if (data.empty()) continue;

        if (data.size() >= 4 && memcmp(data.data(), "GVMH", 4) == 0) {
            std::vector<Image> imgs;
            if (gvm_extract(data.data(), data.size(), imgs)) {
                gvm_ok++;
                tex_total += (long long)imgs.size();
            } else gvm_fail++;
            continue;
        }
        if (e.kind == AssetKind::CharacterModel) {
            auto table = read_mdl_table(data.data(), data.size());
            NinjaBlob blob(data, 0, true);
            int built = 0;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (auto& kv : table) {
                Model m;
                if (!blob.build_model(kv.second, m)) continue;
                built++;
                tris += (long long)m.triangle_count();
                verts += (long long)m.vertex_count();
                float a[3], b[3];
                m.bounds(a, b);
                for (int k = 0; k < 3; k++) {
                    lo[k] = std::min(lo[k], a[k]);
                    hi[k] = std::max(hi[k], b[k]);
                }
            }
            float span = 0;
            for (int k = 0; k < 3; k++) span = std::max(span, hi[k] - lo[k]);
            if (built && (span > 5000.0f || !(span > 0))) {
                suspect++;
                printf("  [suspect] %s span=%.1f\n", e.name.c_str(), span);
            }
            if (built) mdl_ok++; else mdl_fail++;
            continue;
        }
        if (e.kind == AssetKind::CharacterMotion) {
            NinjaBlob blob(data, 0, true);
            int got = 0;
            for (auto& me : read_mtn_table(data.data(), data.size())) {
                Motion mo;
                if (!blob.read_motion(me.ptr, me.node_count, mo)) continue;
                got++;
                for (auto& kv : mo.channels)
                    keys += (long long)(kv.second.pos.size() + kv.second.rot.size() +
                                        kv.second.scale.size());
            }
            motions += got;
            if (got) mtn_ok++; else mtn_fail++;
            continue;
        }
        if (e.kind == AssetKind::EventScene) {
            NinjaBlob blob(data, detect_event_base(data.data(), data.size()), true);
            auto roots = blob.find_model_roots();
            int built = 0;
            for (uint32_t r : roots) {
                Model m;
                if (!blob.build_model(r, m)) continue;
                built++;
                tris += (long long)m.triangle_count();
                verts += (long long)m.vertex_count();
            }
            if (built) evt_ok++; else evt_fail++;
            continue;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    printf("\n=================== SA2 BATCH REGRESSION ===================\n");
    printf("  files indexed      : %zu\n", idx.entries().size());
    printf("  PRS decompressed   : %d ok, %d failed\n", prs_ok, prs_fail);
    printf("  PAK archives       : %d ok, %d failed\n", pak_ok, pak_fail);
    printf("  GVM archives       : %d ok, %d failed (%lld textures)\n",
           gvm_ok, gvm_fail, tex_total);
    printf("  character models   : %d ok, %d failed\n", mdl_ok, mdl_fail);
    printf("  character motions  : %d ok, %d failed (%lld motions, %lld keys)\n",
           mtn_ok, mtn_fail, motions, keys);
    printf("  event scenes       : %d ok, %d failed\n", evt_ok, evt_fail);
    printf("  geometry           : %lld vertices, %lld triangles\n", verts, tris);
    printf("  suspect bounds     : %d\n", suspect);
    printf("  elapsed            : %.1f s\n", secs);
    int fails = prs_fail + pak_fail + gvm_fail + mdl_fail + mtn_fail + evt_fail + suspect;
    printf("  RESULT             : %s\n", fails == 0 ? "ALL PASS" : "FAILURES PRESENT");
    printf("============================================================\n");
    return fails == 0 ? 0 : 2;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "list" && argc >= 3)     return cmd_list(argv[2]);
    if (cmd == "info" && argc >= 3)     return cmd_info(argv[2]);
    if (cmd == "extract" && argc >= 4)  return cmd_extract(argv[2], argv[3]);
    if (cmd == "model" && argc >= 3)    return cmd_model(argv[2]);
    if (cmd == "fbx" && argc >= 4)
        return cmd_fbx(argv[2], argv[3], argc >= 5 ? atoi(argv[4]) : 0);
    if (cmd == "regress" && argc >= 3)  return cmd_regress(argv[2]);
    if (cmd == "search" && argc >= 4)   return cmd_search(argv[2], argv[3]);
    usage();
    return 1;
}
