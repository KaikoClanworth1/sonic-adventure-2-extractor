// sa2viewer - Dear ImGui + GLFW + OpenGL browser and model viewer for
// Sonic Adventure 2 (PC).
//
// Our own drawing uses fixed-function OpenGL 1.1 client arrays, all of which
// are exported directly from opengl32.dll, so no GL loader is needed here;
// ImGui's GL3 backend brings its own.
//
// Debug hooks (so the app can be verified without stealing focus):
//   SA2VIEWER_SCREENSHOT=<path.png>  dump the framebuffer to PNG
//   SA2VIEWER_FRAMES=<n>             render n frames, dump, then exit
//   SA2VIEWER_GAME=<folder>          override the configured game folder
//   SA2VIEWER_OPEN=<substring>       auto-select and load a matching asset
#include <windows.h>
#include <shlobj.h>

#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "sa2core.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sa2;

// ------------------------------------------------------------------ config
struct Config {
    std::string game_folder;
    float ui_scale = 1.0f;

    static std::string path() {
        char buf[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
            std::string dir = std::string(buf) + "\\sa2viewer";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir + "\\config.ini";
        }
        return "sa2viewer.ini";
    }
    void load() {
        FILE* f = nullptr;
        fopen_s(&f, path().c_str(), "rb");
        if (!f) return;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            auto eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq), v = s.substr(eq + 1);
            if (k == "game_folder") game_folder = v;
            else if (k == "ui_scale") ui_scale = (float)atof(v.c_str());
        }
        fclose(f);
        if (!(ui_scale >= 0.5f && ui_scale <= 4.0f)) ui_scale = 1.0f;
    }
    void save() const {
        FILE* f = nullptr;
        fopen_s(&f, path().c_str(), "wb");
        if (!f) return;
        fprintf(f, "game_folder=%s\n", game_folder.c_str());
        fprintf(f, "ui_scale=%.3f\n", ui_scale);
        fclose(f);
    }
};

static std::string pick_folder() {
    BROWSEINFOA bi{};
    char display[MAX_PATH]{};
    bi.pszDisplayName = display;
    bi.lpszTitle = "Select the Sonic Adventure 2 game folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return "";
    char out[MAX_PATH]{};
    std::string result;
    if (SHGetPathFromIDListA(pidl, out)) result = out;
    CoTaskMemFree(pidl);
    return result;
}

// ------------------------------------------------------------------ scene
struct GLTexture {
    GLuint id = 0;
    int w = 0, h = 0;
    std::string name;
};

struct DrawPart {
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned int> idx;
    int tex = -1;
    bool double_sided = false;
    bool blend = false;
    float color[4]{1, 1, 1, 1};
};

struct Scene {
    std::vector<DrawPart> parts;
    std::vector<GLTexture> textures;
    float center[3]{0, 0, 0};
    float radius = 1.0f;
    size_t tris = 0, verts = 0;
    int nodes = 0;
    std::string title;
    std::vector<std::string> anim_names;

    void reset() {
        for (auto& t : textures) if (t.id) glDeleteTextures(1, &t.id);
        textures.clear();
        parts.clear();
        tris = verts = 0;
        nodes = 0;
        anim_names.clear();
    }
};

static GLuint upload_texture(const Image& img) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    return id;
}

// model_sel < 0 shows every model in the file; otherwise just that one.
static void scene_from_asset(const LoadedAsset& la, Scene& sc,
                             const std::string& title, int model_sel) {
    sc.reset();
    sc.title = title;
    for (const auto& img : la.textures) {
        if (!img.valid()) continue;
        GLTexture t;
        t.id = upload_texture(img);
        t.w = img.width;
        t.h = img.height;
        t.name = img.name;
        sc.textures.push_back(t);
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (size_t mi = 0; mi < la.models.size(); mi++) {
        if (model_sel >= 0 && (int)mi != model_sel) continue;
        const auto& m = la.models[mi];
        sc.nodes += (int)m.nodes.size();
        for (const auto& p : m.parts) {
            DrawPart dp;
            dp.pos = p.positions;
            dp.nrm = p.normals;
            dp.uv = p.uvs;
            dp.idx.assign(p.indices.begin(), p.indices.end());
            dp.tex = p.texture_id;
            dp.double_sided = p.double_sided;
            dp.blend = p.use_alpha;
            uint32_t d = p.diffuse;
            dp.color[0] = ((d >> 16) & 0xFF) / 255.0f;
            dp.color[1] = ((d >> 8) & 0xFF) / 255.0f;
            dp.color[2] = (d & 0xFF) / 255.0f;
            dp.color[3] = ((d >> 24) & 0xFF) / 255.0f;
            if (dp.color[3] <= 0.01f) dp.color[3] = 1.0f;
            sc.tris += dp.idx.size() / 3;
            sc.verts += dp.pos.size() / 3;
            for (size_t i = 0; i + 2 < dp.pos.size(); i += 3) {
                any = true;
                for (int k = 0; k < 3; k++) {
                    lo[k] = std::min(lo[k], dp.pos[i + k]);
                    hi[k] = std::max(hi[k], dp.pos[i + k]);
                }
            }
            sc.parts.push_back(std::move(dp));
        }
        if (sc.parts.size() > 20000) break;
    }
    if (!any) { lo[0] = lo[1] = lo[2] = -1; hi[0] = hi[1] = hi[2] = 1; }
    float r = 0;
    for (int k = 0; k < 3; k++) {
        sc.center[k] = (lo[k] + hi[k]) * 0.5f;
        r = std::max(r, (hi[k] - lo[k]) * 0.5f);
    }
    sc.radius = r > 1e-5f ? r : 1.0f;
    for (const auto& mo : la.motions)
        sc.anim_names.push_back(mo.name + "  (" + std::to_string(mo.frame_count) +
                                " frames)");
}

// Rebuild only the drawable geometry from one (posed) model, keeping the
// uploaded GL textures and the camera framing. Used every animation frame.
static void scene_geometry_from_model(const Model& m, Scene& sc,
                                      bool update_bounds) {
    sc.parts.clear();
    sc.tris = sc.verts = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool any = false;
    for (const auto& p : m.parts) {
        DrawPart dp;
        dp.pos = p.positions;
        dp.nrm = p.normals;
        dp.uv = p.uvs;
        dp.idx.assign(p.indices.begin(), p.indices.end());
        dp.tex = p.texture_id;
        dp.double_sided = p.double_sided;
        dp.blend = p.use_alpha;
        uint32_t d = p.diffuse;
        dp.color[0] = ((d >> 16) & 0xFF) / 255.0f;
        dp.color[1] = ((d >> 8) & 0xFF) / 255.0f;
        dp.color[2] = (d & 0xFF) / 255.0f;
        dp.color[3] = ((d >> 24) & 0xFF) / 255.0f;
        if (dp.color[3] <= 0.01f) dp.color[3] = 1.0f;
        sc.tris += dp.idx.size() / 3;
        sc.verts += dp.pos.size() / 3;
        if (update_bounds)
            for (size_t i = 0; i + 2 < dp.pos.size(); i += 3) {
                any = true;
                for (int k = 0; k < 3; k++) {
                    lo[k] = std::min(lo[k], dp.pos[i + k]);
                    hi[k] = std::max(hi[k], dp.pos[i + k]);
                }
            }
        sc.parts.push_back(std::move(dp));
    }
    if (update_bounds && any) {
        float r = 0;
        for (int k = 0; k < 3; k++) {
            sc.center[k] = (lo[k] + hi[k]) * 0.5f;
            r = std::max(r, (hi[k] - lo[k]) * 0.5f);
        }
        sc.radius = r > 1e-5f ? r : 1.0f;
    }
}

// ------------------------------------------------------------------ camera
struct Camera {
    float yaw = 0.7f, pitch = 0.35f, dist = 2.2f;
};

static void set_perspective(float fovy, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy * 0.5f);
    float m[16] = {f / aspect, 0, 0, 0,
                   0, f, 0, 0,
                   0, 0, (zf + zn) / (zn - zf), -1,
                   0, 0, (2 * zf * zn) / (zn - zf), 0};
    glLoadMatrixf(m);
}

static void set_lookat(const float eye[3], const float ctr[3]) {
    float up[3] = {0, 1, 0};
    float f[3] = {ctr[0] - eye[0], ctr[1] - eye[1], ctr[2] - eye[2]};
    float fl = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-8f) fl = 1;
    for (int i = 0; i < 3; i++) f[i] /= fl;
    float s[3] = {f[1] * up[2] - f[2] * up[1],
                  f[2] * up[0] - f[0] * up[2],
                  f[0] * up[1] - f[1] * up[0]};
    float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-8f) sl = 1;
    for (int i = 0; i < 3; i++) s[i] /= sl;
    float u[3] = {s[1] * f[2] - s[2] * f[1],
                  s[2] * f[0] - s[0] * f[2],
                  s[0] * f[1] - s[1] * f[0]};
    float m[16] = {s[0], u[0], -f[0], 0,
                   s[1], u[1], -f[1], 0,
                   s[2], u[2], -f[2], 0,
                   0, 0, 0, 1};
    glLoadMatrixf(m);
    glTranslatef(-eye[0], -eye[1], -eye[2]);
}

static void draw_scene(const Scene& sc, const Camera& cam, int w, int h,
                       bool wireframe, bool lighting, bool textured,
                       bool force_two_sided) {
    glViewport(0, 0, w, h);
    glClearColor(0.10f, 0.11f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (sc.parts.empty()) return;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glMatrixMode(GL_PROJECTION);
    float aspect = h > 0 ? (float)w / (float)h : 1.0f;
    set_perspective(0.9f, aspect, sc.radius * 0.01f + 0.001f, sc.radius * 60.0f + 20.0f);

    glMatrixMode(GL_MODELVIEW);
    float d = cam.dist * sc.radius;
    float eye[3] = {sc.center[0] + cosf(cam.pitch) * sinf(cam.yaw) * d,
                    sc.center[1] + sinf(cam.pitch) * d,
                    sc.center[2] + cosf(cam.pitch) * cosf(cam.yaw) * d};
    set_lookat(eye, sc.center);

    if (lighting) {
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        GLfloat lp[4] = {0.4f, 1.0f, 0.7f, 0.0f};
        GLfloat ld[4] = {0.9f, 0.9f, 0.9f, 1.0f};
        GLfloat la[4] = {0.5f, 0.5f, 0.55f, 1.0f};
        glLightfv(GL_LIGHT0, GL_POSITION, lp);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, ld);
        glLightfv(GL_LIGHT0, GL_AMBIENT, la);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        glEnable(GL_NORMALIZE);
    } else {
        glDisable(GL_LIGHTING);
    }
    glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glAlphaFunc(GL_GREATER, 0.03f);
    glEnable(GL_ALPHA_TEST);

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);

    for (int pass = 0; pass < 2; pass++) {
        for (const auto& p : sc.parts) {
            bool blend = p.blend || p.color[3] < 0.99f;
            if ((pass == 0) == blend) continue;
            if (p.pos.empty() || p.idx.empty()) continue;
            if (textured && p.tex >= 0 && p.tex < (int)sc.textures.size()) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, sc.textures[p.tex].id);
            } else {
                glDisable(GL_TEXTURE_2D);
            }
            if (p.double_sided || force_two_sided) glDisable(GL_CULL_FACE);
            else { glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
            glColor4fv(p.color);
            glVertexPointer(3, GL_FLOAT, 0, p.pos.data());
            bool has_n = (p.nrm.size() == p.pos.size());
            if (has_n) {
                glEnableClientState(GL_NORMAL_ARRAY);
                glNormalPointer(GL_FLOAT, 0, p.nrm.data());
            } else {
                glDisableClientState(GL_NORMAL_ARRAY);
            }
            bool has_uv = (p.uv.size() / 2 == p.pos.size() / 3);
            if (has_uv) {
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, 0, p.uv.data());
            } else {
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            }
            glDrawElements(GL_TRIANGLES, (GLsizei)p.idx.size(), GL_UNSIGNED_INT,
                           p.idx.data());
        }
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

static bool dump_framebuffer(const std::string& path, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    std::vector<unsigned char> px((size_t)w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<unsigned char> flip((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4],
               (size_t)w * 4);
    return stbi_write_png(path.c_str(), w, h, 4, flip.data(), w * 4) != 0;
}

// The viewer is a WIN32-subsystem binary, so there is no console to print to.
// Route diagnostics to a log file next to the exe and to the debugger.
static void log_line(const std::string& s) {
    OutputDebugStringA((s + "\n").c_str());
    FILE* f = nullptr;
    fopen_s(&f, "sa2viewer.log", "ab");
    if (f) { fprintf(f, "%s\n", s.c_str()); fclose(f); }
}

static std::string get_env(const char* key) {
    char buf[1024];
    size_t n = 0;
    if (getenv_s(&n, buf, sizeof buf, key) == 0 && n > 1) return std::string(buf);
    return std::string();
}

// Pick a sensible motion to auto-play on load: the applicable motion with the
// most frames. The longest motion is a smooth loop rather than a 2-4 frame
// flicker, and for the playable characters it is the in-place idle/wait cycle,
// which is what makes an otherwise raw-bind-pose character read correctly.
static int pick_default_motion(const std::vector<int>& applicable,
                               const std::vector<Motion>& motions) {
    int best = -1;
    for (int mi : applicable) {
        if (mi < 0 || mi >= (int)motions.size()) continue;
        if (best < 0 || motions[mi].frame_count > motions[best].frame_count)
            best = mi;
    }
    return best;
}

static Model merge_models(const std::vector<Model>& models, const std::string& name) {
    Model merged;
    merged.name = name;
    int base = 0;
    for (const auto& m : models) {
        for (auto n : m.nodes) {
            if (n.parent >= 0) n.parent += base;
            n.index += base;
            merged.nodes.push_back(n);
        }
        for (auto p : m.parts) {
            p.node_index += base;
            for (auto& vn : p.vertex_node) vn += base;
            merged.parts.push_back(p);
        }
        base = (int)merged.nodes.size();
        if (merged.parts.size() > 8000) break;
    }
    return merged;
}

// ------------------------------------------------------------------ main
int run_app() {
    Config cfg;
    cfg.load();
    std::string envGame = get_env("SA2VIEWER_GAME");
    if (!envGame.empty()) cfg.game_folder = envGame;

    std::string shot_path = get_env("SA2VIEWER_SCREENSHOT");
    std::string open_match = get_env("SA2VIEWER_OPEN");
    std::string frames_s = get_env("SA2VIEWER_FRAMES");
    int shot_frames = frames_s.empty() ? 0 : atoi(frames_s.c_str());

    if (!glfwInit()) { log_line("glfwInit failed"); return 1; }
    // No version hints: the default Windows context is a compatibility profile,
    // which serves both our fixed-function drawing and ImGui's GLSL 130 shaders.
    glfwWindowHint(GLFW_VISIBLE, shot_frames > 0 ? GLFW_FALSE : GLFW_TRUE);
    GLFWwindow* win = glfwCreateWindow(1600, 950,
                                       "SA2 Extractor & Model Viewer", nullptr, nullptr);
    if (!win) { log_line("glfwCreateWindow failed"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(shot_frames > 0 ? 0 : 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    GameIndex index;
    // Auto-detect a Steam install on first run so most users never see setup.
    if (cfg.game_folder.empty()) {
        std::string found = autodetect_game_folder();
        if (!found.empty()) cfg.game_folder = found;
    }
    if (!cfg.game_folder.empty()) index.scan(cfg.game_folder);

    Scene scene;
    Camera cam;
    LoadedAsset current;
    char search_buf[256]{};
    int cur_section = (int)Section::Maps;
    std::vector<int> section_items;
    bool refresh_list = true;    // recompute section_items next frame
    std::string status = index.entries().empty()
                             ? "Select your Sonic Adventure 2 folder to begin."
                             : ("Loaded " + std::to_string(index.entries().size()) +
                                " files.");
    int selected = -1, pending = -1;
    int force_section = -1;      // when >=0, force the tab bar to this section
    int model_sel = -1;          // -1 = every model in the file
    bool rebuild_scene = false;
    // animation playback
    int anim_sel = -1;           // index into current.motions, -1 = bind pose
    float anim_frame = 0.0f;
    bool anim_playing = false;
    double anim_last_time = 0.0;
    std::vector<int> anim_applicable;   // motions valid for the current model
    bool anim_dirty = false;     // re-pose next frame
    bool wireframe = false, lighting = true, textured = true, show_textures = false;
    // SA2 stage walls are single-sided; a free-orbit camera sees straight through
    // their backs, which reads as "missing meshes". Draw everything two-sided by
    // default so nothing disappears when you rotate around a map.
    bool show_backfaces = true;
    bool show_settings = false;
    bool show_setup = index.entries().empty();
    char path_buf[512]{};
    strncpy_s(path_buf, cfg.game_folder.c_str(), sizeof(path_buf) - 1);
    float applied_scale = -1.0f;
    int frame = 0;

    // Rescan the game folder in path_buf and refresh UI state.
    auto do_rescan = [&](const std::string& folder) {
        cfg.game_folder = folder;
        index.scan(folder);
        cfg.save();
        selected = -1;
        refresh_list = true;
        show_setup = index.entries().empty();
        status = index.entries().empty()
                     ? "No game files found in that folder."
                     : ("Loaded " + std::to_string(index.entries().size()) + " files.");
    };

    if (!open_match.empty()) {
        // Prefer an exact filename match: "sonicmdl.prs" must not select
        // "metalsonicmdl.prs", which contains it as a substring.
        for (int i = 0; i < (int)index.entries().size() && selected < 0; i++)
            if (index.entries()[i].name == open_match) selected = pending = i;
        for (int i = 0; i < (int)index.entries().size() && selected < 0; i++)
            if (index.entries()[i].rel_path.find(open_match) != std::string::npos)
                selected = pending = i;
        if (selected >= 0) {
            cur_section = (int)index.entries()[selected].section;
            force_section = cur_section;
            refresh_list = true;
        }
        if (const char* ms = getenv("SA2VIEWER_MODEL")) model_sel = atoi(ms);
    }

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        if (fabsf(applied_scale - cfg.ui_scale) > 0.001f) {
            ImGuiStyle& st = ImGui::GetStyle();
            st = ImGuiStyle();
            ImGui::StyleColorsDark();
            st.ScaleAllSizes(cfg.ui_scale);
            io.FontGlobalScale = cfg.ui_scale;
            applied_scale = cfg.ui_scale;
        }

        if (pending >= 0 && pending < (int)index.entries().size()) {
            const auto& e = index.entries()[pending];
            LoadedAsset la;
            std::string err;
            if (load_asset(e, index, la, &err)) {
                current = std::move(la);
                // a single-model file needs no selector; multi-model files
                // default to the first so parts do not stack at the origin
                if (model_sel >= (int)current.models.size()) model_sel = -1;
                if (model_sel < 0 && current.models.size() > 1) model_sel = 0;
                scene_from_asset(current, scene, e.rel_path, model_sel);
                cam = Camera();
                // set up animation: which motions apply to this model?
                anim_applicable = current.motions_for(model_sel < 0 ? 0 : model_sel);
                // Auto-play a default motion (the idle) so the character animates
                // on load instead of showing SA2's collapsed raw bind pose, which
                // is what makes an un-posed character look like its body is
                // missing.
                anim_sel = pick_default_motion(anim_applicable, current.motions);
                anim_frame = 0.0f;
                anim_playing = anim_sel >= 0;
                anim_last_time = 0.0;
                anim_dirty = anim_sel >= 0;
                // debug hooks: SA2VIEWER_ANIM = index into applicable list,
                // SA2VIEWER_ANIMFRAME = frame number (holds a fixed frame so a
                // screenshot is deterministic)
                std::string ea = get_env("SA2VIEWER_ANIM");
                if (!ea.empty() && !anim_applicable.empty()) {
                    int ai = atoi(ea.c_str());
                    if (ai >= 0 && ai < (int)anim_applicable.size()) {
                        anim_sel = anim_applicable[ai];
                        std::string ef = get_env("SA2VIEWER_ANIMFRAME");
                        anim_frame = ef.empty() ? 0.0f : (float)atof(ef.c_str());
                        anim_playing = false;
                        anim_dirty = true;
                    }
                }
                // A playing character swings limbs through a wide arc; pull the
                // camera back a little so the whole body stays framed rather than
                // cropping in tight on the default distance.
                if (!anim_applicable.empty()) cam.dist = 2.9f;
                // diagnostic overrides for headless screenshots:
                // SA2VIEWER_CAM="yaw,pitch,dist", SA2VIEWER_BACKFACES=0|1
                std::string ecam = get_env("SA2VIEWER_CAM");
                if (!ecam.empty()) {
                    float y = cam.yaw, p = cam.pitch, d = cam.dist;
                    sscanf(ecam.c_str(), "%f,%f,%f", &y, &p, &d);
                    cam.yaw = y; cam.pitch = p; cam.dist = d;
                }
                std::string ebf = get_env("SA2VIEWER_BACKFACES");
                if (!ebf.empty()) show_backfaces = atoi(ebf.c_str()) != 0;
                char buf[256];
                snprintf(buf, sizeof buf,
                         "%s: %d tris, %d textures, %d anims",
                         e.display_name.c_str(), (int)scene.tris,
                         (int)scene.textures.size(), (int)anim_applicable.size());
                status = buf;
            } else {
                status = "Could not open " + e.display_name + " - " + err;
            }
            pending = -1;
        }
        if (rebuild_scene) {
            scene_from_asset(current, scene, scene.title, model_sel);
            cam = Camera();
            anim_applicable = current.motions_for(model_sel < 0 ? 0 : model_sel);
            anim_sel = pick_default_motion(anim_applicable, current.motions);
            anim_frame = 0.0f;
            anim_playing = anim_sel >= 0;
            anim_last_time = 0.0;
            anim_dirty = anim_sel >= 0;
            if (!anim_applicable.empty()) cam.dist = 2.9f;
            rebuild_scene = false;
        }

        // advance playback, then re-pose the mesh if the animation frame changed
        if (anim_playing && anim_sel >= 0 && anim_sel < (int)current.motions.size()) {
            double now = glfwGetTime();
            double dt = (anim_last_time > 0.0) ? (now - anim_last_time) : 0.0;
            anim_last_time = now;
            int fc = current.motions[anim_sel].frame_count;
            if (fc > 1) {
                anim_frame += (float)(dt * 30.0);   // SA2 motions run at ~30 fps
                while (anim_frame >= (float)fc) anim_frame -= (float)fc;
            }
            anim_dirty = true;
        } else {
            anim_last_time = 0.0;
        }
        if (anim_dirty) {
            int mi = (model_sel < 0) ? 0 : model_sel;
            if (mi >= 0 && mi < (int)current.models.size()) {
                if (anim_sel >= 0 && anim_sel < (int)current.motions.size() &&
                    !current.anim_data.empty() && mi < (int)current.model_roots.size()) {
                    NinjaBlob blob(current.anim_data, 0, true);
                    Model posed;
                    if (blob.build_model_posed(current.model_roots[mi],
                                               &current.motions[anim_sel], anim_frame,
                                               posed))
                        scene_geometry_from_model(posed, scene, false);
                } else {
                    scene_geometry_from_model(current.models[mi], scene, false);
                }
            }
            anim_dirty = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int dw = 0, dh = 0;
        glfwGetFramebufferSize(win, &dw, &dh);
        float panel_w = std::min(460.0f * cfg.ui_scale, dw * 0.42f);
        float right_w = std::min(340.0f * cfg.ui_scale, dw * 0.3f);

        if (refresh_list) {
            section_items = index.in_section((Section)cur_section, search_buf, 20000);
            refresh_list = false;
        }

        // -------- left: browser with section tabs
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(panel_w, (float)dh));
        ImGui::Begin("Library", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

        ImGui::TextUnformatted("Sonic Adventure 2");
        ImGui::SameLine(panel_w - 90 * cfg.ui_scale);
        if (ImGui::Button("Settings")) show_settings = true;

        // search within the current section
        ImGui::PushItemWidth(-1);
        if (ImGui::InputTextWithHint("##search", "Search this section...", search_buf,
                                     sizeof search_buf))
            refresh_list = true;
        ImGui::PopItemWidth();

        // section tabs
        if (ImGui::BeginTabBar("sections", ImGuiTabBarFlags_FittingPolicyScroll)) {
            static const Section kTabs[] = {Section::Maps, Section::Characters,
                                            Section::Objects, Section::Particles,
                                            Section::Audio, Section::Other};
            int clicked = -1;
            for (Section s : kTabs) {
                int n = index.section_count(s);
                char label[64];
                snprintf(label, sizeof label, "%s (%d)", section_name(s), n);
                ImGuiTabItemFlags fl = 0;
                if (force_section == (int)s) fl |= ImGuiTabItemFlags_SetSelected;
                if (ImGui::BeginTabItem(label, nullptr, fl)) {
                    clicked = (int)s;
                    ImGui::EndTabItem();
                }
            }
            // A forced switch wins over the tab bar's default-first-tab behaviour
            // on the frame it happens; otherwise follow the user's click.
            if (force_section >= 0) {
                cur_section = force_section;
                force_section = -1;
                refresh_list = true;
            } else if (clicked >= 0 && clicked != cur_section) {
                cur_section = clicked;
                refresh_list = true;
            }
            ImGui::EndTabBar();
        }

        ImGui::BeginChild("list", ImVec2(0, -40 * cfg.ui_scale));
        if (section_items.empty()) {
            ImGui::TextDisabled(index.entries().empty()
                                    ? "No game loaded."
                                    : "Nothing here matches your search.");
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)section_items.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                int ei = section_items[i];
                const auto& e = index.entries()[ei];
                ImGui::PushID(ei);
                bool sel = (ei == selected);
                if (ImGui::Selectable(e.display_name.c_str(), sel,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected = ei;
                    pending = ei;
                    model_sel = -1;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s  -  %llu KB", e.subtitle.c_str(),
                                      kind_name(e.kind),
                                      (unsigned long long)(e.size / 1024));
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::Separator();
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();

        // -------- settings modal
        if (show_settings) {
            ImGui::OpenPopup("Settings");
            show_settings = false;
        }
        // Pin the width every frame (height auto-fits content). Combining
        // AlwaysAutoResize with the full-width (-1) InputText below made the
        // popup converge on the button-row width and visibly shrink each frame.
        ImGui::SetNextWindowSize(ImVec2(560 * cfg.ui_scale, 0));
        if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoResize)) {
            ImGui::TextUnformatted("Game folder");
            ImGui::TextDisabled("The folder containing sonic2app.exe and resource\\gd_PC");
            ImGui::PushItemWidth(-1);
            ImGui::InputText("##path", path_buf, sizeof path_buf);
            ImGui::PopItemWidth();
            if (ImGui::Button("Browse...")) {
                std::string p = pick_folder();
                if (!p.empty()) strncpy_s(path_buf, p.c_str(), sizeof(path_buf) - 1);
            }
            ImGui::SameLine();
            if (ImGui::Button("Auto-detect")) {
                std::string f = autodetect_game_folder();
                if (!f.empty()) strncpy_s(path_buf, f.c_str(), sizeof(path_buf) - 1);
                else status = "No Steam install of SA2 found automatically.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply")) do_rescan(path_buf);

            ImGui::Separator();
            ImGui::TextUnformatted("Display");
            ImGui::PushItemWidth(220 * cfg.ui_scale);
            if (ImGui::SliderFloat("UI scale", &cfg.ui_scale, 0.75f, 3.0f, "%.2fx"))
                cfg.save();
            ImGui::PopItemWidth();
            ImGui::TextDisabled("Tip: raise this on a 4K monitor.");

            ImGui::Separator();
            if (ImGui::Button("Close", ImVec2(120 * cfg.ui_scale, 0)))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // -------- first-run setup (centered) when no game is loaded
        if (show_setup && index.entries().empty()) {
            ImGui::SetNextWindowPos(ImVec2(dw * 0.5f, dh * 0.5f), ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(620 * cfg.ui_scale, 0));
            ImGui::Begin("Welcome", nullptr,
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
            ImGui::TextUnformatted("Welcome to the Sonic Adventure 2 Extractor & Model Viewer");
            ImGui::Spacing();
            ImGui::TextWrapped("Point the app at your Sonic Adventure 2 install to begin. "
                               "That is the folder holding sonic2app.exe and the "
                               "resource\\gd_PC folder - usually:");
            ImGui::TextDisabled("  C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sonic Adventure 2");
            ImGui::Spacing();
            ImGui::PushItemWidth(-1);
            ImGui::InputTextWithHint("##setuppath", "Paste your game folder here...",
                                     path_buf, sizeof path_buf);
            ImGui::PopItemWidth();
            ImGui::Spacing();
            if (ImGui::Button("Auto-detect Steam install", ImVec2(230 * cfg.ui_scale, 0))) {
                std::string f = autodetect_game_folder();
                if (!f.empty()) { strncpy_s(path_buf, f.c_str(), sizeof(path_buf) - 1); do_rescan(f); }
                else status = "Could not find SA2 automatically - browse to it instead.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...", ImVec2(120 * cfg.ui_scale, 0))) {
                std::string p = pick_folder();
                if (!p.empty()) { strncpy_s(path_buf, p.c_str(), sizeof(path_buf) - 1); do_rescan(p); }
            }
            ImGui::SameLine();
            if (ImGui::Button("Use this path", ImVec2(140 * cfg.ui_scale, 0)))
                do_rescan(path_buf);
            if (!status.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", status.c_str());
            }
            ImGui::End();
        }

        // -------- right: inspector
        ImGui::SetNextWindowPos(ImVec2((float)dw - right_w, 0));
        ImGui::SetNextWindowSize(ImVec2(right_w, (float)dh));
        ImGui::Begin("Inspector", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        ImGui::TextUnformatted("View");
        ImGui::Checkbox("Wireframe", &wireframe);
        ImGui::Checkbox("Lighting", &lighting);
        ImGui::Checkbox("Textures", &textured);
        ImGui::Checkbox("Double-sided", &show_backfaces);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw single-sided stage walls from both sides so\n"
                              "they don't vanish when you orbit behind them.");
        ImGui::Separator();
        if (current.models.size() > 1) {
            ImGui::Separator();
            ImGui::Text("Models in file (%d)", (int)current.models.size());
            if (ImGui::RadioButton("All (stacked)", model_sel == -1)) {
                model_sel = -1;
                rebuild_scene = true;
            }
            ImGui::BeginChild("modellist", ImVec2(0, 150 * cfg.ui_scale));
            for (int i = 0; i < (int)current.models.size(); i++) {
                char lbl[128];
                snprintf(lbl, sizeof lbl, "%s  (%d tris)",
                         current.models[i].name.c_str(),
                         (int)current.models[i].triangle_count());
                if (ImGui::RadioButton(lbl, model_sel == i)) {
                    model_sel = i;
                    rebuild_scene = true;
                }
            }
            ImGui::EndChild();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Loaded asset");
        ImGui::BulletText("meshes    %d", (int)scene.parts.size());
        ImGui::BulletText("nodes     %d", scene.nodes);
        ImGui::BulletText("vertices  %d", (int)scene.verts);
        ImGui::BulletText("triangles %d", (int)scene.tris);
        ImGui::BulletText("textures  %d", (int)scene.textures.size());
        ImGui::BulletText("anims     %d", (int)current.motions.size());
        ImGui::Separator();
        if (ImGui::Button("Export FBX") && !current.models.empty()) {
            std::error_code ec;
            fs::create_directories("exports", ec);
            std::string stem = fs::path(scene.title).stem().string();
            if (stem.empty()) stem = "model";
            // export exactly what is on screen
            std::vector<Model> subset;
            if (model_sel >= 0 && model_sel < (int)current.models.size()) {
                subset.push_back(current.models[model_sel]);
                stem += "_" + current.models[model_sel].name;
            } else {
                subset = current.models;
            }
            std::string out = "exports/" + stem + ".fbx";
            Model merged = merge_models(subset, stem);
            FbxExportOptions opts;
            std::string err;
            status = export_fbx(out, merged, current.textures, current.motions, opts,
                                &err)
                         ? ("Exported " + out)
                         : ("FBX export failed: " + err);
        }
        ImGui::SameLine();
        if (ImGui::Button("Export PNGs") && !current.textures.empty()) {
            std::error_code ec;
            fs::create_directories("exports/textures", ec);
            int n = 0;
            for (auto& img : current.textures) {
                std::string nm =
                    img.name.empty() ? ("tex_" + std::to_string(n)) : img.name;
                if (write_png("exports/textures/" + nm + ".png", img)) n++;
            }
            status = "Wrote " + std::to_string(n) + " PNGs to exports/textures";
        }
        ImGui::Separator();
        ImGui::Checkbox("Show textures", &show_textures);
        if (show_textures && !scene.textures.empty()) {
            ImGui::BeginChild("texlist", ImVec2(0, 260 * cfg.ui_scale));
            for (size_t i = 0; i < scene.textures.size(); i++) {
                ImGui::Text("%2d %s (%dx%d)", (int)i, scene.textures[i].name.c_str(),
                            scene.textures[i].w, scene.textures[i].h);
                ImGui::Image((ImTextureID)(intptr_t)scene.textures[i].id,
                             ImVec2(76 * cfg.ui_scale, 76 * cfg.ui_scale));
            }
            ImGui::EndChild();
        }
        if (!anim_applicable.empty()) {
            ImGui::Separator();
            ImGui::Text("Animation (%d)", (int)anim_applicable.size());
            // play / pause / stop
            if (ImGui::Button(anim_playing ? "Pause" : "Play")) {
                if (anim_sel < 0 && !anim_applicable.empty()) {
                    anim_sel = anim_applicable[0];
                    anim_dirty = true;
                }
                anim_playing = !anim_playing;
                anim_last_time = 0.0;
            }
            ImGui::SameLine();
            if (ImGui::Button("Bind pose")) {
                anim_sel = -1; anim_playing = false; anim_frame = 0.0f;
                anim_dirty = true;
            }
            // frame scrubber
            if (anim_sel >= 0 && anim_sel < (int)current.motions.size()) {
                int fc = current.motions[anim_sel].frame_count;
                if (ImGui::SliderFloat("Frame", &anim_frame, 0.0f,
                                       (float)std::max(1, fc - 1), "%.0f")) {
                    anim_playing = false;
                    anim_dirty = true;
                }
            }
            ImGui::BeginChild("anims", ImVec2(0, 180 * cfg.ui_scale));
            for (int ai : anim_applicable) {
                const auto& mo = current.motions[ai];
                char lbl[96];
                snprintf(lbl, sizeof lbl, "%s (%d f)", mo.name.c_str(), mo.frame_count);
                if (ImGui::Selectable(lbl, ai == anim_sel)) {
                    anim_sel = ai;
                    anim_frame = 0.0f;
                    anim_playing = true;
                    anim_last_time = 0.0;
                    anim_dirty = true;
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();

        // -------- viewport hit area (transparent, behind the panels)
        ImGui::SetNextWindowPos(ImVec2(panel_w, 0));
        ImGui::SetNextWindowSize(ImVec2(std::max(1.0f, dw - panel_w - right_w),
                                        (float)dh));
        ImGui::Begin("Viewport", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoBackground |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        bool hovered = ImGui::IsWindowHovered();
        if (scene.parts.empty())
            ImGui::TextUnformatted("Select a model, texture archive or cutscene "
                                   "on the left.");
        ImGui::End();

        if (hovered && !io.WantTextInput) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                cam.yaw -= d.x * 0.008f;
                cam.pitch = std::max(-1.5f, std::min(1.5f, cam.pitch + d.y * 0.008f));
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            }
            if (io.MouseWheel != 0.0f)
                cam.dist = std::max(0.15f, cam.dist * (1.0f - io.MouseWheel * 0.12f));
        }

        ImGui::Render();
        draw_scene(scene, cam, dw, dh, wireframe, lighting, textured, show_backfaces);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        frame++;
        if (shot_frames > 0 && frame >= shot_frames) {
            glFinish();
            if (!shot_path.empty()) {
                bool ok = dump_framebuffer(shot_path, dw, dh);
                log_line("screenshot " + shot_path + (ok ? " OK" : " FAILED") +
                         " (" + std::to_string(dw) + "x" + std::to_string(dh) +
                         ") status: " + status);
            }
            glfwSwapBuffers(win);
            break;
        }
        glfwSwapBuffers(win);
    }

    cfg.save();
    scene.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return run_app(); }
int main(int, char**) { return run_app(); }
