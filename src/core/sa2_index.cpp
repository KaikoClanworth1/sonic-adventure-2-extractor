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
        case AssetKind::Stage:           return "Stage geometry";
        case AssetKind::SetPlacement:    return "Object placement";
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
    // Stage geometry lives in stgXXD.rel; boss arenas in Boss_*D.rel. Other RELs
    // (Chao, advertise, title...) are modules with no landtable.
    if (ends_with(name_l, ".rel")) {
        if (name_l.rfind("stg", 0) == 0 || name_l.find("boss") != std::string::npos)
            return AssetKind::Stage;
        return AssetKind::Level;
    }
    if (name_l.rfind("set", 0) == 0 && ends_with(name_l, ".bin"))
        return AssetKind::SetPlacement;
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

// Is this file a particle / effect asset? (effect texture sheets, fire, splash)
static bool is_particle(const std::string& name_l) {
    static const char* kw[] = {"efftex", "_eff", "eveff", "screeneffect",
                               "_fire", "fire_", "backfire", "splash", "kemuri",
                               "bomtex", "blast", "explo", "hanabi"};
    for (const char* k : kw)
        if (name_l.find(k) != std::string::npos) return true;
    return false;
}

// Pull the two-digit stage number out of "stg13d.rel" / "set0013_s.bin".
static int stage_number(const std::string& name_l) {
    size_t i = 0;
    while (i < name_l.size() && !std::isdigit((unsigned char)name_l[i])) i++;
    if (i >= name_l.size()) return -1;
    int n = 0, digits = 0;
    while (i < name_l.size() && std::isdigit((unsigned char)name_l[i]) && digits < 4) {
        n = n * 10 + (name_l[i] - '0');
        i++; digits++;
    }
    return digits ? n : -1;
}

// Friendly boss-arena names, keyed by a substring of the REL file name.
static std::string boss_name(const std::string& nl) {
    struct Row { const char* key; const char* name; };
    static const Row rows[] = {
        {"bigfoot", "Boss: Big Foot"}, {"hotshot", "Boss: Hot Shot"},
        {"bigbogy", "Boss: King Boom Boo"}, {"fdog", "Boss: Flying Dog"},
        {"golem", "Boss: Egg Golem"}, {"last1", "Boss: Biolizard"},
        {"last2", "Boss: Finalhazard"},
    };
    for (const auto& r : rows)
        if (nl.find(r.key) != std::string::npos) return r.name;
    return "";
}

// Assign a user-facing section + friendly display name to an entry.
static void assign_display(AssetEntry& e, const NameTable& names) {
    std::string nl = lower(e.name);
    e.subtitle = e.name;
    switch (e.kind) {
        case AssetKind::Stage: {
            e.section = Section::Maps;
            int num = stage_number(nl);
            auto it = names.stages.find(num);
            if (nl.find("boss") != std::string::npos) {
                std::string b = boss_name(nl);
                e.display_name = b.empty() ? ("Boss " + std::to_string(num)) : b;
            } else if (it != names.stages.end()) {
                e.display_name = it->second;
            } else {
                e.display_name = "Stage " + std::to_string(num);
            }
            break;
        }
        case AssetKind::CharacterModel:
            e.section = Section::Characters;
            e.display_name = friendly_character_name(e.name);
            break;
        case AssetKind::CharacterMotion:
            e.section = Section::Characters;
            e.display_name = friendly_character_name(e.name) + " (animations)";
            break;
        case AssetKind::SetPlacement: {
            e.section = Section::Objects;
            int num = stage_number(nl);
            auto it = names.stages.find(num);
            std::string base = (it != names.stages.end()) ? it->second
                                                          : ("Stage " + std::to_string(num));
            e.display_name = base + " - object layout";
            break;
        }
        case AssetKind::EventScene:
            e.section = Section::Objects;
            e.display_name = "Cutscene " + e.name.substr(0, e.name.find('.'));
            break;
        case AssetKind::Audio:
        case AssetKind::Video:
            e.section = Section::Audio;
            e.display_name = e.name;
            break;
        case AssetKind::TextureArchive:
        case AssetKind::Texture:
        case AssetKind::PakArchive:
        case AssetKind::EventTexture:
            e.section = is_particle(nl) ? Section::Particles : Section::Objects;
            e.display_name = e.name;
            break;
        case AssetKind::Message:
            e.section = Section::Other;
            e.display_name = e.name;
            break;
        default:
            e.section = is_particle(nl) ? Section::Particles : Section::Other;
            e.display_name = e.name;
            break;
    }
    if (e.display_name.empty()) e.display_name = e.name;
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

    // friendly names come from the executable, so load it once up front
    names_ = load_name_table(root_);

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
        assign_display(e, names_);
        entries_.push_back(std::move(e));
        if (entries_.size() > 200000) break;
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const AssetEntry& a, const AssetEntry& b) {
                  // within a section, order by display name then path
                  if (a.section != b.section)
                      return (int)a.section < (int)b.section;
                  if (a.display_name != b.display_name)
                      return a.display_name < b.display_name;
                  return a.rel_path < b.rel_path;
              });
    return !entries_.empty();
}

std::vector<int> GameIndex::in_section(Section s, const std::string& query,
                                       int limit) const {
    std::vector<int> out;
    std::string q = lower(query);
    for (int i = 0; i < (int)entries_.size(); i++) {
        if (entries_[i].section != s) continue;
        if (!q.empty()) {
            std::string dn = lower(entries_[i].display_name);
            std::string rn = lower(entries_[i].rel_path);
            if (dn.find(q) == std::string::npos && rn.find(q) == std::string::npos)
                continue;
        }
        out.push_back(i);
        if ((int)out.size() >= limit) break;
    }
    return out;
}

int GameIndex::section_count(Section s) const {
    int n = 0;
    for (const auto& e : entries_) if (e.section == s) n++;
    return n;
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

// Read a PAK's <name>.inf index to recover the texlist order. Each record is
// 0x3C bytes and begins with a NUL-padded texture name.
static std::vector<std::string> read_inf_order(const uint8_t* d, size_t n) {
    std::vector<std::string> order;
    for (size_t o = 0; o + 0x3C <= n; o += 0x3C) {
        size_t len = 0;
        while (len < 0x1C && d[o + len]) len++;
        order.emplace_back((const char*)d + o, len);
    }
    return order;
}

// Prefer the PC DDS replacement archive (PRS/<base>.pak) over the GVR/GVM when
// it exists. Same spatial resolution, but it is the texture the PC game ships
// (DXT5, better alpha) and is loaded in texlist order so texture ids still map.
// Returns true if a PAK was found and used.
static bool load_hires_pak(const std::string& base, const std::string& game_root,
                           std::vector<Image>& out) {
    if (base.empty() || game_root.empty()) return false;
    std::string pak = game_root + "/resource/gd_PC/PRS/" + base + ".pak";
    std::error_code ec;
    if (!fs::exists(pak, ec)) return false;
    auto data = read_file(pak);
    PakArchive arc;
    if (!arc.parse(data.data(), data.size())) return false;

    // find the .inf to get texture order; fall back to entry order
    std::vector<std::string> order;
    std::map<std::string, const PakEntry*> by_name;
    for (const auto& e : arc.entries) {
        std::string leaf = fs::path(e.name).stem().string();
        std::string el = lower(fs::path(e.name).extension().string());
        if (el == ".inf") {
            order = read_inf_order(data.data() + e.offset, e.length);
        } else if (el == ".dds" || el == ".png") {
            by_name[lower(leaf)] = &e;
        }
    }
    auto decode = [&](const PakEntry* e) {
        const uint8_t* p = data.data() + e->offset;
        Image img;
        bool ok = false;
        if (e->length >= 4 && memcmp(p, "DDS ", 4) == 0) ok = dds_decode(p, e->length, img);
        else if (e->length >= 8 && p[0] == 0x89 && p[1] == 'P') ok = png_decode(p, e->length, img);
        if (ok) { img.name = fs::path(e->name).stem().string(); out.push_back(std::move(img)); }
    };
    if (!order.empty()) {
        for (const auto& nm : order) {
            auto it = by_name.find(lower(nm));
            if (it != by_name.end()) decode(it->second);
        }
    }
    if (out.empty()) {   // no usable .inf: fall back to entry order
        for (const auto& e : arc.entries) {
            std::string el = lower(fs::path(e.name).extension().string());
            if (el == ".dds" || el == ".png") decode(&e);
        }
    }
    return !out.empty();
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
        // build every model, remembering its root and animated-node count
        struct Built { Model m; uint32_t root; int anim; };
        std::vector<Built> built;
        for (auto& kv : table) {
            Model m;
            if (blob.build_model(kv.second, m)) {
                m.name = "model_" + std::to_string(kv.first);
                built.push_back({std::move(m), kv.second, blob.count_animated(kv.second)});
            }
        }
        if (built.empty()) return fail("no models decoded");
        // The MDL table's first file entry is often a partial sub-model. Put the
        // fullest animatable model (matches a motion's node count, most tris)
        // first so the viewer shows a complete, poseable character by default.
        std::string mtn = sibling(e.path, "mdl.prs", "mtn.prs");
        int want_nodes = -1;
        if (!mtn.empty()) {
            auto md = load_file(mtn);
            if (!md.empty()) {
                NinjaBlob mb(md, 0, true);
                auto mtab = read_mtn_table(md.data(), md.size());
                if (!mtab.empty()) want_nodes = mtab[0].node_count;
                for (auto& me : mtab) {
                    Motion mo;
                    if (mb.read_motion(me.ptr, me.node_count, mo)) {
                        mo.name = "anim_" + std::to_string(me.index);
                        out.motions.push_back(std::move(mo));
                    }
                }
            }
        }
        std::stable_sort(built.begin(), built.end(),
            [&](const Built& a, const Built& b) {
                bool am = (a.anim == want_nodes), bm = (b.anim == want_nodes);
                if (am != bm) return am;                     // animatable first
                return a.m.triangle_count() > b.m.triangle_count();  // then detail
            });
        out.anim_data = data;
        for (auto& bd : built) {
            out.model_roots.push_back(bd.root);
            out.model_anim_count.push_back(bd.anim);
            out.models.push_back(std::move(bd.m));
        }
        // textures: sonicmdl.prs -> sonictex.prs
        std::string tex = sibling(e.path, "mdl.prs", "tex.prs");
        if (!tex.empty()) load_textures(tex, out.textures);
        return true;
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

    if (e.kind == AssetKind::Stage) {
        std::vector<uint8_t> img;
        if (!rel_relocate(data.data(), data.size(), img))
            return fail("not a REL module");
        NinjaBlob blob(std::move(img), 0, true);
        auto lts = find_landtables(blob);
        if (lts.empty()) return fail("no landtable in this REL");
        // Build every landtable as its own model: the main table plus the
        // animated-scenery auxiliaries (_uv scroll, _ani, _x). This is the
        // extractable part of "stage animations" - the animated geometry itself;
        // the motion binding is compiled into the game and not data-driven.
        std::string main_tex = lts[0].texture_name;
        for (size_t i = 0; i < lts.size(); i++) {
            Model m;
            if (!build_landtable(blob, lts[i], m)) continue;
            m.name = lts[i].texture_name.empty()
                         ? ("landtable_" + std::to_string(i))
                         : lts[i].texture_name;
            out.models.push_back(std::move(m));
        }
        if (out.models.empty()) return fail("landtable produced no geometry");
        lts[0].texture_name = main_tex;
        // Textures: prefer the PC DDS PAK (highest-quality shipped), else the
        // GVR archive. landtx13 -> PRS/landtx13.pak or LANDTX13.PRS.
        if (!lts[0].texture_name.empty() && !idx.root().empty()) {
            if (!load_hires_pak(lts[0].texture_name, idx.root(), out.textures)) {
                std::string tp = idx.root() + "/resource/gd_PC/" +
                                 lts[0].texture_name + ".prs";
                std::error_code ec;
                if (!fs::exists(tp, ec)) {
                    for (const auto& ae : idx.entries()) {
                        std::string nl = lower(ae.name);
                        if (nl == lower(lts[0].texture_name) + ".prs") { tp = ae.path; break; }
                    }
                }
                load_textures(tp, out.textures);
            }
        }
        return true;
    }

    if (e.kind == AssetKind::SetPlacement) {
        // No standalone object models ship in gd_PC (the id->model table lives in
        // the community objdefs / the exe), so visualise the layout as a marker
        // cube at every placed object's position. This makes the "object layout"
        // entries load, and is the geometry a stage overlay / object list builds on.
        std::vector<SetObject> objs;
        if (!parse_set_file(data.data(), data.size(), objs) || objs.empty())
            return fail("no objects in SET file");
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const auto& o : objs)
            for (int k = 0; k < 3; k++) {
                lo[k] = std::min(lo[k], o.pos[k]);
                hi[k] = std::max(hi[k], o.pos[k]);
            }
        float ext = std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]),
                             std::max(hi[2] - lo[2], 1.0f));
        float s = ext * 0.004f;   // marker half-size, relative to the layout extent
        static const float cv[8][3] = {
            {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
            {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
        static const int cf[12][3] = {
            {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
            {2, 3, 7}, {2, 7, 6}, {1, 2, 6}, {1, 6, 5}, {0, 4, 7}, {0, 7, 3}};
        Model m;
        m.name = "objects";
        MeshPart part;
        part.texture_id = -1;
        part.double_sided = true;
        part.diffuse = 0xFFFFCC22;   // amber
        for (const auto& o : objs) {
            uint32_t base = (uint32_t)(part.positions.size() / 3);
            for (int v = 0; v < 8; v++) {
                part.positions.insert(part.positions.end(),
                    {o.pos[0] + cv[v][0] * s, o.pos[1] + cv[v][1] * s,
                     o.pos[2] + cv[v][2] * s});
                part.normals.insert(part.normals.end(), {0.0f, 1.0f, 0.0f});
                part.uvs.insert(part.uvs.end(), {0.0f, 0.0f});
                part.colors.push_back(part.diffuse);
                part.vertex_node.push_back(0);
            }
            for (int f = 0; f < 12; f++)
                for (int k = 0; k < 3; k++)
                    part.indices.push_back(base + cf[f][k]);
        }
        m.parts.push_back(std::move(part));
        out.models.push_back(std::move(m));
        return true;
    }

    // Unknown: try every interpretation we have.
    if (data.size() >= 4 && memcmp(data.data(), "GVMH", 4) == 0)
        return gvm_extract(data.data(), data.size(), out.textures)
                   ? true : fail("bad GVM");
    return fail("unrecognised file type");
}

}  // namespace sa2
