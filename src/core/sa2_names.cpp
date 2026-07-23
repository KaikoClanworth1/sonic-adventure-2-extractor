// Friendly names and section classification.
//
// Stage names are pulled straight from sonic2app.exe: the compiled source paths
// (`..\src\stgNN_codename`) give a reliable stage-number -> codename map, and
// the in-game English stage-name strings give clean display names. We match the
// codename against those strings, falling back to a prettified codename.
#include "sa2core.h"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <set>
#include <filesystem>

namespace fs = std::filesystem;

namespace sa2 {

const char* section_name(Section s) {
    switch (s) {
        case Section::Maps:       return "Maps";
        case Section::Characters: return "Characters";
        case Section::Objects:    return "Objects";
        case Section::Particles:  return "Particles";
        case Section::Audio:      return "Audio";
        case Section::Enemies:    return "Enemies";
        default:                  return "Other";
    }
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string normalize(const std::string& s) {
    std::string o;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) o += (char)std::tolower((unsigned char)c);
    return o;
}

// Split camelCase / digit boundaries and title-case, e.g. "CrazyGadget" ->
// "Crazy Gadget", "cityescape" -> "Cityescape".
static std::string prettify(const std::string& code) {
    std::string s;
    for (size_t i = 0; i < code.size(); i++) {
        char c = code[i];
        if (i > 0) {
            char p = code[i - 1];
            bool camel = std::islower((unsigned char)p) && std::isupper((unsigned char)c);
            bool digit = std::isalpha((unsigned char)p) && std::isdigit((unsigned char)c);
            if (camel || digit) s += ' ';
        }
        s += c;
    }
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

// Rival-battle codenames like "sonicvsshadow" -> "Sonic vs Shadow". Returns ""
// when there is no "vs" to split on.
static std::string prettify_versus(const std::string& code) {
    std::string lc;
    for (char c : code) lc += (char)std::tolower((unsigned char)c);
    size_t v = lc.find("vs");
    if (v == std::string::npos || v == 0 || v + 2 >= lc.size()) return "";
    struct Ab { const char* k; const char* v; };
    static const Ab abbr[] = {{"ew", "Eggwalker"}, {"tw", "Cyclone"}};
    auto expand = [&](const std::string& part) -> std::string {
        for (const auto& a : abbr) if (part == a.k) return a.v;
        std::string s = part;
        if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
        return s;
    };
    std::string a = expand(lc.substr(0, v));
    std::string b = expand(lc.substr(v + 2));
    return a + " vs " + b;
}

// ---------------------------------------------------------------- stage names
NameTable load_name_table(const std::string& game_root) {
    NameTable t;
    if (game_root.empty()) return t;
    std::string exe = game_root + "/sonic2app.exe";
    std::error_code ec;
    if (!fs::exists(exe, ec)) return t;
    auto data = read_file(exe);
    if (data.empty()) return t;
    const std::string blob((const char*)data.data(), data.size());

    // 1) stage number -> codename, from "src\stgNN_codename"
    std::map<int, std::string> codenames;
    const std::string marker = "src\\stg";
    size_t pos = 0;
    while ((pos = blob.find(marker, pos)) != std::string::npos) {
        size_t p = pos + marker.size();
        if (p + 3 <= blob.size() && std::isdigit((unsigned char)blob[p]) &&
            std::isdigit((unsigned char)blob[p + 1]) && blob[p + 2] == '_') {
            int num = (blob[p] - '0') * 10 + (blob[p + 1] - '0');
            size_t q = p + 3;
            std::string code;
            while (q < blob.size() && (std::isalnum((unsigned char)blob[q])))
                code += blob[q++];
            if (!code.empty() && !codenames.count(num)) codenames[num] = code;
        }
        pos += marker.size();
    }

    // 2) clean in-game stage names: the "<Name> BGM." list and the bare-name
    //    block are proper nouns; collect any Title-Case ASCII run near them.
    std::set<std::string> clean;
    auto add_clean = [&](const std::string& s) {
        if (s.size() >= 3 && s.size() <= 28 &&
            std::isupper((unsigned char)s[0])) clean.insert(s);
    };
    // "<Name> BGM."
    pos = 0;
    while ((pos = blob.find(" BGM.", pos)) != std::string::npos) {
        size_t start = pos;
        while (start > 0) {
            char c = blob[start - 1];
            if (c == 0 || (unsigned char)c < 0x20) break;
            start--;
        }
        add_clean(blob.substr(start, pos - start));
        pos += 5;
    }

    // match each codename to a clean name; else prettify
    static const char* kChars[] = {"Sonic", "Shadow", "Tails", "Eggman",
                                    "Knuckles", "Rouge"};
    std::map<std::string, std::string> clean_by_norm;
    for (const auto& c : clean) clean_by_norm[normalize(c)] = c;

    for (auto& kv : codenames) {
        std::string code = kv.second;
        std::string charname, base = code;
        for (const char* ch : kChars) {
            size_t cl = strlen(ch);
            if (code.size() > cl && code.compare(code.size() - cl, cl, ch) == 0) {
                charname = ch;
                base = code.substr(0, code.size() - cl);
                break;
            }
        }
        std::string n = normalize(base);
        std::string name;
        std::string versus = prettify_versus(base);
        auto it = clean_by_norm.find(n);
        if (!versus.empty()) {
            name = versus;                // rival battles: "Sonic vs Shadow"
        } else if (it != clean_by_norm.end()) {
            name = it->second;
        } else {
            // shortest clean name whose normalized form contains the codename
            std::string best;
            for (auto& c : clean_by_norm) {
                if (n.size() >= 4 && c.first.find(n) != std::string::npos) {
                    if (best.empty() || c.second.size() < best.size()) best = c.second;
                }
            }
            name = best.empty() ? prettify(base) : best;
        }
        if (!charname.empty()) name += " (" + charname + ")";
        t.stages[kv.first] = name;
    }
    t.loaded = !t.stages.empty();
    return t;
}

// ---------------------------------------------------------------- characters
std::string friendly_character_name(const std::string& file_name) {
    std::string n = lower(file_name);
    // strip trailing "mdl.prs" / "mtn.prs" / ".prs"
    for (const char* suf : {"mdl.prs", "mtn.prs", ".prs"}) {
        size_t sl = strlen(suf);
        if (n.size() > sl && n.compare(n.size() - sl, sl, suf) == 0) {
            n = n.substr(0, n.size() - sl);
            break;
        }
    }
    struct Row { const char* prefix; const char* name; };
    // Order matters: longer / more specific prefixes first.
    static const Row rows[] = {
        {"ssonic", "Super Sonic"}, {"sshadow", "Super Shadow"},
        {"metalsonic", "Metal Sonic"}, {"sonic1", "Sonic"}, {"sonic", "Sonic"},
        {"shadow1", "Shadow"}, {"shadow", "Shadow"}, {"terios", "Shadow (Terios)"},
        {"bknuck", "Knuckles (boss)"}, {"knuck", "Knuckles"},
        {"brouge", "Rouge (boss)"}, {"rouge", "Rouge"},
        {"miles", "Tails"}, {"twalk1", "Cyclone (Tails)"}, {"twalk", "Cyclone (Tails)"},
        {"ewalk1", "Eggwalker"}, {"ewalk2", "Eggwalker"}, {"ewalk", "Eggwalker"},
        {"cwalk", "Walker"}, {"dwalk", "Walker"},
        {"egg", "Dr. Eggman"}, {"amy", "Amy"}, {"chaos0", "Chaos"},
        {"tical", "Tikal"},
    };
    for (const auto& r : rows) {
        size_t pl = strlen(r.prefix);
        if (n.compare(0, pl, r.prefix) == 0) return r.name;
    }
    return prettify(n);
}

// ---------------------------------------------------------------- autodetect
std::string autodetect_game_folder() {
    static const char* kSuffix =
        "/steamapps/common/Sonic Adventure 2";
    std::vector<std::string> roots;
#ifdef _WIN32
    for (char drive = 'C'; drive <= 'H'; drive++) {
        std::string d(1, drive);
        roots.push_back(d + ":/Program Files (x86)/Steam");
        roots.push_back(d + ":/Program Files/Steam");
        roots.push_back(d + ":/Steam");
        roots.push_back(d + ":/SteamLibrary");
        roots.push_back(d + ":/Games/Steam");
    }
#endif
    std::error_code ec;
    for (const auto& r : roots) {
        std::string cand = r + kSuffix;
        if (fs::exists(cand + "/sonic2app.exe", ec)) return cand;
        if (fs::exists(cand + "/resource/gd_PC", ec)) return cand;
    }
    return "";
}

}  // namespace sa2
