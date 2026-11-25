/*
 * Copyright (C) 2026 The Thimsy OS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// nano-buttonmap SCENE.json FONT ARTDIR WIDTH HEIGHT OUT.rgba [--fallback FONT]...
// The button map panel as Nano draws it (NanoButtonMapArt), as raw RGBA: for the golden test and for
// looking at the layout at any screen size. The scene's "page" (1-based) or "lines" scroll the body
// the way L1/R1 or the D-pad would. --dump prints the layout instead.
//
// --map EMULATOR:SYSTEM[:CORE] --root DIR --data DIR [--i18n LANG.json] [--gammapad on|swap]: the
// panel as Nano builds it on the device instead of the scene's rows: the emulator's reader on a
// device-like tree (DIR), the pad table and RetroArch tables in --data (ps3xmb/buttonmap), the
// hotkeys, and the text through a translation file. The scene still gives the palette and the page.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "NanoButtonMap.h"
#include "NanoButtonMapArt.h"
#include "NanoButtonMapNav.h"
#include "NanoDeviceKeys.h"
#include "NanoI18n.h"
#include "NanoJson.h"

using namespace android;

static bool readFile(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out->size() < (8u << 20)) out->append(buf, n);
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: nano-buttonmap SCENE.json FONT ARTDIR WIDTH HEIGHT OUT.rgba [--fallback FONT]... [--dump]\n");
        return 2;
    }
    std::string sceneText, art;
    if (!readFile(argv[1], &sceneText)) { perror(argv[1]); return 1; }
    bmart::Scene scene;
    if (!bmart::sceneFromJson(sceneText, &scene)) { fprintf(stderr, "nano-buttonmap: %s is not a scene\n", argv[1]); return 1; }
    njson::Value v;
    njson::parse(sceneText, &v);
    bmart::Assets as;
    as.fontPath = argv[2];
    as.artDir = argv[3];
    bool dump = false;
    std::string map, root, data, i18n, gammapad;
    for (int i = 7; i < argc; i++) {
        if (!strcmp(argv[i], "--fallback") && i + 1 < argc) as.fallbackFonts.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--dump")) dump = true;
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) map = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!strcmp(argv[i], "--i18n") && i + 1 < argc) i18n = argv[++i];
        else if (!strcmp(argv[i], "--gammapad") && i + 1 < argc) gammapad = argv[++i];
    }
    bmap::Catalog cat;
    if (!map.empty() ? !bmap::loadCatalogFrom(as.artDir, data, &cat)
                     : (!readFile(as.artDir + "/controllers.json", &art) || !bmap::loadCatalog(art, "", "", &cat))) {
        fprintf(stderr, "nano-buttonmap: no controllers.json in %s\n", argv[3]);
        return 1;
    }
    as.catalog = &cat;
    if (!map.empty()) {
        // EMULATOR:SYSTEM[:CORE], as the context Nano resolves for a focused system or a game in front.
        bmap::Context ctx;
        const size_t a = map.find(':'), b = a == std::string::npos ? a : map.find(':', a + 1);
        ctx.emulator = map.substr(0, a);
        ctx.systemId = a == std::string::npos ? std::string() : map.substr(a + 1, b == std::string::npos ? b : b - a - 1);
        if (b != std::string::npos) ctx.core = map.substr(b + 1);
        ctx.kind = ctx.emulator == "browser" ? bmap::Context::kBrowser : bmap::Context::kGame;
        if (ctx.kind == bmap::Context::kBrowser) ctx.browserPkg = ctx.package = ctx.systemId;
        bmap::GamepadState gp;
        gp.running = !gammapad.empty();
        gp.swap = gammapad == "swap";
        const bmap::PadModel pad = bmap::padModelFor(bmap::DeviceId(), bmap::loadPadSpecsFrom(data), gp);
        bmap::ReaderEnv env;
        env.root = root;
        bmap::MapResult r = bmap::build(env, ctx, pad, cat);
        if (ctx.kind == bmap::Context::kBrowser) {
            const bmap::BrowserState bs = bmap::browserState(gp.running, true);
            r.browserGreyed = bs != bmap::BrowserState::kActive;
            r.browserHint = bmap::browserHint(bs);
        } else {
            auto rows = bmap::chordRows({{"save_state", "x"}, {"load_state", "x"}, {"fast_forward", "x"}, {"rewind", "x"}, {"quit", "x"}},
                                        env, pad, devkeys::functionPrompt(), devkeys::current().sharedBack);
            rows.insert(rows.end(), r.hotkeys.begin(), r.hotkeys.end());
            r.hotkeys = rows;
        }
        const bmart::Palette pal = scene.pal;
        scene = bmart::sceneFor(r, pad);
        scene.pal = pal;
        scene.table = r.drawing.empty() || v.getBool("table");
        if (!i18n.empty()) {
            std::string text;
            if (!readFile(i18n, &text)) { perror(i18n.c_str()); return 1; }
            std::string code = i18n.substr(i18n.find_last_of('/') + 1);
            code = code.substr(0, code.find('.'));
            i18nLoadFromText(code.c_str(), text);
            i18nLoad(code.c_str());
            bmart::translateScene(&scene, [](const char* k) { return trDyn(k); });
            scene.rtl = code == "ar" || code == "he";
        }
    }
    const int w = atoi(argv[4]), h = atoi(argv[5]);
    bmart::Layout L;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || !bmart::layout(scene, as, w, h, &L)) {
        fprintf(stderr, "nano-buttonmap: cannot lay out %dx%d with %s\n", w, h, argv[2]);
        return 1;
    }
    bmap::Nav nav;
    nav.setModel(L.nav());
    for (int p = 1; p < v.getInt("page", 1); p++) nav.pageDown();
    for (int i = 0; i < v.getInt("lines", 0); i++) nav.lineDown();
    if (dump) {
        printf("panel %d,%d %dx%d centred=%d body=%d px rows=%d labelled=%d strip=%d pages=%d content=%d view=%d\n",
               L.panel.x, L.panel.y, L.panel.w, L.panel.h, L.centred, L.bodyPx, L.rowH, L.labelled, L.stripRows,
               nav.pageCount(), L.contentH, L.body.h);
        return 0;
    }
    std::vector<uint8_t> rgba;
    if (!bmart::render(scene, as, L, nav.scroll(), &rgba)) { fprintf(stderr, "nano-buttonmap: cannot draw\n"); return 1; }
    FILE* f = fopen(argv[6], "wb");
    if (!f || fwrite(rgba.data(), 1, rgba.size(), f) != rgba.size()) { perror(argv[6]); return 1; }
    fclose(f);
    return 0;
}
