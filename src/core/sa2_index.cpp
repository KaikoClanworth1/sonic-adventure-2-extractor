// Game folder scanning, asset classification and high-level loading.
#include "sa2core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace sa2 {

const char* kind_name(AssetKind k) {
    switch (k) {
        case AssetKind::TextureArchive:  return "Texture archive";
        case AssetKind::CharacterModel:  return "Character model";
        case AssetKind::CharacterMotion: return "Character motions";
        case AssetKind::EventScene:      return "Cutscene models";
        case AssetKind::EventTexture:    return "Cutscene textures";
        case AssetKind::EventMotion:     return "Cutscene motions";
        case AssetKind::PakArchive:      return "PAK archive";
        case AssetKind::Texture:         return "Texture";
        case AssetKind::Audio:           return "Audio";
        case AssetKind::Video:           return "Video";
        case AssetKind::Message:         return "Text/messages";
        case AssetKind::Level:           return "Level module";
        default:                         return "Data";
    }
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static bool ends_with(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = nullptr;
#ifdef _WIN32
    fopen_s(&f, path.c_str(), "rb");
#else
    f = fopen(path.c_str(), "rb");
#endif
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize((size_t)n);
        size_t got = fread(out.data(), 1, (size_t)n, f);
        out.resize(got);
    }
    fclose(f);
    return out;
}

std::vector<uint8_t> load_file(const std::string& path) {
    auto raw = read_file(path);
    if (raw.empty()) return raw;
    if (ends_with(lower(path), ".prs") && prs_looks_valid(raw.data(), raw.size())) {
        auto dec = prs_decompress(raw.data(), raw.size());
        if (!dec.empty()) return dec;
    }
    return raw;
}

static AssetKind classify(const std::string& name_l, const std::string& rel_l) {
    if (ends_with(name_l, ".pak")) return AssetKind::PakArchive;
    if (ends_with(name_l, ".gvr") || ends_with(name_l, ".dds") ||
        ends_with(name_l, ".png") || ends_with(name_l, ".gvp") ||
        ends_with(name_l, ".plt"))
        return AssetKind::Texture;
    if (ends_with(name_l, ".adx") || ends_with(name_l, ".csb") ||
        ends_with(name_l, ".afs") || ends_with(name_l, ".mid") ||
        ends_with(name_l, ".mlt"))
        return AssetKind::Audio;
    if (ends_with(name_l, ".sfd") || ends_with(name_l, ".m1v"))
        return AssetKind::Video;
    if (ends_with(name_l, ".rel")) return AssetKind::Level;

    bool in_event = rel_l.find("event") != std::string::npos;
    if (ends_with(name_l, ".prs")) {
        if (ends_with(name_l, "mdl.prs")) return AssetKind::CharacterModel;
        if (ends_with(name_l, "mtn.prs")) return AssetKind::CharacterMotion;
        if (name_l.find("texture") != std::string::npos && in_event)
            return AssetKind::EventTexture;
        if (name_l.find("texlist") != std::string::npos)
            return AssetKind::Unknown;
        if (name_l.find("tex") != std::string::npos ||
            name_l.rfind("landtx", 0) == 0 || name_l.rfind("objtex", 0) == 0)
            return AssetKind::TextureArchive;
        // A cutscene scene file is exactly "e<digits>.prs"; the "_0".."_5"/"_j"
        // siblings are per-language subtitle streams and hold no geometry.
        if (in_event && name_l.size() >= 6 && name_l[0] == 'e') {
            size_t stem = name_l.size() - 4;   // strip ".prs"
            bool all_digits = stem > 1;
            for (size_t i = 1; i < stem; i++)
                if (!isdigit((unsigned char)name_l[i])) { all_digits = false; break; }
            if (all_digits) return AssetKind::EventScene;
            return AssetKind::Message;
        }
        if (rel_l.find("message") != std::string::npos)
            return AssetKind::Message;
        return AssetKind::TextureArchive;   // most loose .prs decompress to GVM
    }
    if (ends_with(name_l, ".bin")) {
        if (name_l.find("motion") != std::string::npos)
            return AssetKind::EventMotion;
        return AssetKind::Unknown;
    }
    return AssetKind::Unknown;
}

bool GameIndex::scan(const std::string& folder) {
    entries_.clear();
    root_.clear();
    std::error_code ec;
    if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) return false;

    // accept either the game root or the resource/gd_PC folder
    fs::path base(folder);
    fs::path gd = base / "resource" / "gd_PC";
    if (fs::exists(gd, ec) && fs::is_directory(gd, ec)) base = base;
    root_ = base.string();

    for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        AssetEntry e;
        e.path = it->path().string();
        std::error_code ec2;
        e.rel_path = fs::relative(it->path(), base, ec2).string();
        if (ec2) e.rel_path = e.path;
        e.name = it->path().filename().string();
        e.size = (uint64_t)it->file_size(ec2);
        std::string nl = lower(e.name), rl = lower(e.rel_path);
        e.kind = classify(nl, rl);
        e.compressed = ends_with(nl, ".prs");
        entries_.push_back(std::move(e));
        if (entries_.size() > 200000) break;
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const AssetEntry& a, const AssetEntry& b) {
                  return a.rel_path < b.rel_path;
              });
    return !entries_.empty();
}

std::vector<int> GameIndex::search(const std::string& query, int limit) const {
    std::vector<int> out;
    if (query.empty()) {
        for (int i = 0; i < (int)entries_.size() && (int)out.size() < limit; i++)
            out.push_back(i);
        return out;
    }
    std::string q = lower(query);
    for (int i = 0; i < (int)entries_.size(); i++) {
        if (lower(entries_[i].rel_path).find(q) != std::string::npos) {
            out.push_back(i);
            if ((int)out.size() >= limit) break;
        }
    }
    return out;
}

// ---------------------------------------------------------------- textures
bool load_textures(const std::string& path, std::vector<Image>& out) {
    auto data = load_file(path);
    if (data.empty()) return false;
    const uint8_t* d = data.data();
    size_t n = data.size();

    if (n >= 4 && memcmp(d, "GVMH", 4) == 0) return gvm_extract(d, n, out);
    if (n >= 4 && (memcmp(d, "GBIX", 4) == 0 || memcmp(d, "GCIX", 4) == 0 ||
                   memcmp(d, "GVRT", 4) == 0)) {
        Image img;
        if (gvr_decode(d, n, img)) {
            img.name = fs::path(path).stem().string();
            out.push_back(std::move(img));
            return true;
        }
        return false;
    }
    if (n >= 4 && memcmp(d, "DDS ", 4) == 0) {
        Image img;
        if (dds_decode(d, n, img)) {
            img.name = fs::path(path).stem().string();
            out.push_back(std::move(img));
            return true;
        }
        return false;
    }
    if (n >= 4 && d[0] == 0x01 && d[1] == 'p' && d[2] == 'a' && d[3] == 'k') {
        PakArchive pak;
        if (!pak.parse(d, n)) return false;
        for (const auto& e : pak.entries) {
            if (e.offset + e.length > n) continue;
            const uint8_t* p = d + e.offset;
            Image img;
            bool ok = false;
            if (e.length >= 4 && memcmp(p, "DDS ", 4) == 0)
                ok = dds_decode(p, e.length, img);
            else if (e.length >= 8 && p[0] == 0x89 && p[1] == 'P')
                ok = png_decode(p, e.length, img);
            if (ok) {
                img.name = fs::path(e.name).stem().string();
                out.push_back(std::move(img));
            }
        }
        return !out.empty();
    }
    return false;
}

// ---------------------------------------------------------------- assets
// Given "sonicmdl.prs", find "sonicmtn.prs"; given "e0000.prs", find
// "e0000texture.prs" / "e0000motion.bin".
static std::string sibling(const std::string& path, const std::string& from,
                           const std::string& to) {
    std::string p = path;
    size_t at = p.rfind(from);
    if (at == std::string::npos) return "";
    p.replace(at, from.size(), to);
    std::error_code ec;
    return fs::exists(p, ec) ? p : std::string();
}

bool load_asset(const AssetEntry& e, const GameIndex& idx, LoadedAsset& out,
                std::string* error) {
    out.models.clear();
    out.motions.clear();
    out.textures.clear();
    out.source = e.rel_path;

    auto fail = [&](const char* m) {
        if (error) *error = m;
        return false;
    };

    if (e.kind == AssetKind::Texture || e.kind == AssetKind::TextureArchive ||
        e.kind == AssetKind::PakArchive || e.kind == AssetKind::EventTexture) {
        if (!load_textures(e.path, out.textures)) return fail("no textures found");
        return true;
    }

    auto data = load_file(e.path);
    if (data.empty()) return fail("could not read file");

    if (e.kind == AssetKind::CharacterModel) {
        auto table = read_mdl_table(data.data(), data.size());
        if (table.empty()) return fail("no model table");
        NinjaBlob blob(data, 0, true);
        for (auto& kv : table) {
            Model m;
            if (blob.build_model(kv.second, m)) {
                m.name = "model_" + std::to_string(kv.first);
                out.models.push_back(std::move(m));
            }
        }
        // matching motions: sonicmdl.prs -> sonicmtn.prs
        std::string mtn = sibling(e.path, "mdl.prs", "mtn.prs");
        if (!mtn.empty()) {
            auto md = load_file(mtn);
            if (!md.empty()) {
                NinjaBlob mb(md, 0, true);
                for (auto& me : read_mtn_table(md.data(), md.size())) {
                    Motion mo;
                    if (mb.read_motion(me.ptr, me.node_count, mo)) {
                        mo.name = "anim_" + std::to_string(me.index);
                        out.motions.push_back(std::move(mo));
                    }
                }
            }
        }
        // textures: sonicmdl.prs -> sonictex.prs
        std::string tex = sibling(e.path, "mdl.prs", "tex.prs");
        if (!tex.empty()) load_textures(tex, out.textures);
        return !out.models.empty() ? true : fail("no models decoded");
    }

    if (e.kind == AssetKind::CharacterMotion) {
        NinjaBlob mb(data, 0, true);
        for (auto& me : read_mtn_table(data.data(), data.size())) {
            Motion mo;
            if (mb.read_motion(me.ptr, me.node_count, mo)) {
                mo.name = "anim_" + std::to_string(me.index);
                out.motions.push_back(std::move(mo));
            }
        }
        return !out.motions.empty() ? true : fail("no motions decoded");
    }

    if (e.kind == AssetKind::EventScene) {
        NinjaBlob blob(data, detect_event_base(data.data(), data.size()), true);
        auto roots = blob.find_model_roots();
        int n = 0;
        for (uint32_t r : roots) {
            Model m;
            if (blob.build_model(r, m)) {
                m.name = "obj_" + std::to_string(n++);
                out.models.push_back(std::move(m));
            }
            if (out.models.size() > 4000) break;
        }
        // e0000.prs -> e0000texture.prs
        std::string tex = sibling(e.path, ".prs", "texture.prs");
        if (!tex.empty()) load_textures(tex, out.textures);
        return !out.models.empty() ? true : fail("no models found in event");
    }

    // Unknown: try every interpretation we have.
    if (data.size() >= 4 && memcmp(data.data(), "GVMH", 4) == 0)
        return gvm_extract(data.data(), data.size(), out.textures)
                   ? true : fail("bad GVM");
    return fail("unrecognised file type");
}

}  // namespace sa2
