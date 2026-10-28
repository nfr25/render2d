// SPDX-License-Identifier: AGPL-3.0-or-later
// Reading viewer2d's viewer.yaml (notions §57): the keys render2d uses, the
// ones it ignores, relative paths, and the errors that stop it.
// Moved from editor2d's ConfigTest with the reader.

#include <filesystem>
#include <fstream>
#include <string>

#include "check.hpp"
#include "map/config.hpp"

using namespace r2d;

namespace {

namespace fs = std::filesystem;

std::string write(const std::string& name, const std::string& text) {
    const fs::path dir = fs::temp_directory_path() / "r2d_config_test";
    const fs::path p = dir / name;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
    return p.generic_string();
}

// The shape of viewer2d's file, with its keys for viewer2d only.
void testViewerYaml() {
    const std::string path = write("viewer.yaml", R"(
render2d: D:/work/render2d/render2d/build-release/render2d.exe
layers:
  - source: /data/parcelles.fgb
    style:  /styles/parcelles.yaml
  - source: /data/batiments.fgb
    visible: false
    title: Bâtiments
  - source: /data/adresses.gpkg::adresses
    style:  styles/adresses-numeros.yaml
start:
  bbox: [148000, 168000, 156000, 175000]
max_parallel: 2
tiles: C:/data/cache
metatile: 4
)");
    const ConfigResult r = loadConfig(path);
    CHECK(r.ok);
    CHECK(r.warnings.empty());   // viewer2d's keys are not mistakes
    if (!r.ok) { std::printf("      %s\n", r.error.c_str()); return; }
    const auto& layers = r.config.layers;
    CHECK(layers.size() == 3);
    if (layers.size() != 3) return;
    CHECK(layers[0].source == "/data/parcelles.fgb" && layers[0].style == "/styles/parcelles.yaml");   // kept
    CHECK(!layers[1].style.has_value());                          // default style
    CHECK(!layers[1].visible && layers[1].title == "Bâtiments");  // the editor2d project's keys
    CHECK(layers[0].visible && layers[0].title.empty());
    CHECK(layers[2].source == "/data/adresses.gpkg::adresses");   // file::layer kept

    // §57: relative to the FILE's folder, not to the current folder.
    const std::string expected = (fs::path(path).parent_path() / "styles/adresses-numeros.yaml").generic_string();
    CHECK(layers[2].style == expected);

    CHECK(r.config.startBbox.has_value());
    if (r.config.startBbox) {
        const Envelope& b = *r.config.startBbox;
        CHECK(b.xmin == 148000 && b.ymin == 168000 && b.xmax == 156000 && b.ymax == 175000);
    }
}

// §57, the worked example: "../" goes up one folder.
void testDotDot() {
    const std::string path = write("sub/viewer.yaml", "layers: [{source: ../data/a.fgb, style: ../styles/a.yaml}]\n");
    const ConfigResult r = loadConfig(path);
    CHECK(r.ok && r.config.layers.size() == 1);
    if (!r.ok || r.config.layers.empty()) return;
    const fs::path dir = fs::path(path).parent_path().parent_path();   // .../r2d_config_test
    CHECK(r.config.layers[0].source == (dir / "data/a.fgb").generic_string());
    CHECK(r.config.layers[0].style == (dir / "styles/a.yaml").generic_string());
    CHECK(!r.config.startBbox.has_value());   // optional for render2d
}

void testWarnings() {
    const ConfigResult r = loadConfig(write("typo.yaml", "layer: x\nlayers: [{source: a.fgb, stile: b.yaml}]\n"));
    CHECK(r.ok);
    CHECK(r.warnings.size() == 2);   // 'layer' and 'stile': typos, said
}

void testErrors() {
    CHECK(!loadConfig("/no/such/viewer.yaml").ok);                                   // missing file
    CHECK(!loadConfig(write("bad.yaml", "layers: [ {source: a\n")).ok);              // invalid YAML
    CHECK(!loadConfig(write("nolayer.yaml", "start: {bbox: [0, 0, 1, 1]}\n")).ok);   // no layer
    CHECK(!loadConfig(write("flat.yaml", "layers: [{source: a.fgb}]\nstart: {bbox: [5, 0, 1, 1]}\n")).ok);
    CHECK(!loadConfig(write("half.yaml", "layers: [{source: a.fgb}]\nstart: {zoom: 3}\n")).ok);
    CHECK(!loadConfig(write("vis.yaml", "layers: [{source: a.fgb, visible: maybe}]\n")).ok);
    // A missing data file is NOT an error here: Map::open warns, the others draw.
    CHECK(loadConfig(write("absent.yaml", "layers: [{source: /nowhere.fgb}]\n")).ok);
}

// §61: the worked example of relativeToFolder (Linux spelling of the same paths).
void testRelative() {
    CHECK(relativeToFolder("/cartes/data/batiments.fgb", "/cartes") == "data/batiments.fgb");
    CHECK(relativeToFolder("/cartes/data/batiments.fgb", "/cartes/") == "data/batiments.fgb");
    CHECK(relativeToFolder("/srv/gis/data/arbres.fgb", "/cartes") == "/srv/gis/data/arbres.fgb");
    CHECK(relativeToFolder("/cartes-old/a.fgb", "/cartes") == "/cartes-old/a.fgb");   // whole parts, not letters
    CHECK(relativeToFolder("/cartes/data/urbis.gpkg::Buildings", "/cartes") == "data/urbis.gpkg::Buildings");
    CHECK(relativeToFolder("PG:service=urbis", "/cartes") == "PG:service=urbis");
    CHECK(relativeToFolder("data/a.fgb", "/cartes") == "data/a.fgb");                 // already relative
    CHECK(relativeToFolder("/cartes/x/../data/a.fgb", "/cartes") == "data/a.fgb");    // normalised first
}

// §61: write, read again: the same map; viewer2d's keys kept.
void testRoundTrip() {
    const fs::path dir = fs::temp_directory_path() / "r2d_config_test" / "rt";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string path = (dir / "projet.yaml").generic_string();
    // A viewer2d file is there first: its keys must survive.
    std::ofstream(path) << "render2d: Z:/r2d/render2d.exe\n# a comment\nlayers: [{source: old.fgb}]\n"
                           "max_parallel: 2\ntiles: C:/cache\n";

    Config c;
    LayerSpec a((dir / "data/parcelles.fgb").generic_string(), (dir / "styles/parcelles.yaml").generic_string());
    LayerSpec b("/elsewhere/arbres.fgb");
    b.visible = false;
    b.title   = "Arbres d'alignement";   // a quote inside
    LayerSpec g((dir / "data/urbis.gpkg").generic_string() + "::Buildings");
    c.layers    = {a, b, g};
    c.startBbox = Envelope{148800, 169900.5, 149600, 170400};

    const std::string text = configText(c, path);
    CHECK(text.find("source: 'data/parcelles.fgb'") != std::string::npos);
    CHECK(text.find("style: 'styles/parcelles.yaml'") != std::string::npos);
    CHECK(text.find("title: 'Arbres d''alignement'") != std::string::npos);
    CHECK(text.find("bbox: [148800, 169900.5, 149600, 170400]") != std::string::npos);
    CHECK(text.find("old.fgb") == std::string::npos);   // our layers replace the old ones

    std::string error;
    CHECK(writeConfig(c, path, error));
    const ConfigResult r = loadConfig(path);
    CHECK(r.ok);
    CHECK(r.warnings.empty());   // no key of ours lost, no typo
    CHECK(r.config.layers.size() == 3);
    if (r.config.layers.size() == 3) {
        CHECK(r.config.layers[0].source == a.source);   // full again
        CHECK(r.config.layers[0].style == a.style);
        CHECK(r.config.layers[1].source == "/elsewhere/arbres.fgb");
        CHECK(!r.config.layers[1].visible);
        CHECK(r.config.layers[1].title == "Arbres d'alignement");
        CHECK(!r.config.layers[1].style.has_value());
        CHECK(r.config.layers[2].source == g.source);
        CHECK(r.config.layers[0].visible && r.config.layers[0].title.empty());
    }
    CHECK(r.config.startBbox && r.config.startBbox->ymin == 169900.5);

    // viewer2d's keys, still there.
    std::ifstream in(path);
    const std::string back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(back.find("render2d: Z:/r2d/render2d.exe") != std::string::npos);
    CHECK(back.find("max_parallel: 2") != std::string::npos);
    CHECK(back.find("tiles: C:/cache") != std::string::npos);

    // Written twice: the same text (editor2d compares texts to know "changed").
    CHECK(configText(c, path) == back);
}

} // namespace

int main() {
    testViewerYaml();
    testDotDot();
    testWarnings();
    testErrors();
    testRelative();
    testRoundTrip();
    return check::summary();
}
