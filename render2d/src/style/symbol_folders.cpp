// SPDX-License-Identifier: AGPL-3.0-or-later
#include "style/symbol_folders.hpp"

#include <filesystem>
#include <mutex>

namespace r2d {

namespace {

namespace fs = std::filesystem;

std::mutex               g_mutex;
std::vector<std::string> g_folders;

// `path` under `folder`: the relative path ("nature/park.svg"), else "".
std::string under(const fs::path& path, const std::string& folder) {
    if (folder.empty()) return {};
    const fs::path rel = path.lexically_normal().lexically_relative(fs::path(folder).lexically_normal());
    if (rel.empty() || rel.is_absolute() || *rel.begin() == ".." || *rel.begin() == ".") return {};
    return rel.generic_string();
}

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

} // namespace

void setSymbolFolders(std::vector<std::string> folders) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_folders.clear();
    for (std::string& f : folders)
        if (!f.empty()) g_folders.push_back(std::move(f));
}

std::vector<std::string> symbolFolders() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_folders;
}

std::string findSymbol(const std::string& url, const std::string& styleDir) {
    if (url.empty()) return {};
    const fs::path u(url);
    if (u.is_absolute()) return isFile(u) ? u.lexically_normal().string() : std::string();
    const fs::path local = styleDir.empty() ? u : fs::path(styleDir) / u;   // the style's folder first
    if (isFile(local)) return local.lexically_normal().string();
    for (const std::string& folder : symbolFolders()) {
        const fs::path p = fs::path(folder) / u;
        if (isFile(p)) return p.lexically_normal().string();
    }
    return {};
}

std::string symbolUrl(const std::string& path, const std::string& styleDir) {
    const fs::path p(path);
    if (std::string rel = under(p, styleDir); !rel.empty()) return rel;
    for (const std::string& folder : symbolFolders())
        if (std::string rel = under(p, folder); !rel.empty()) return rel;
    return p.generic_string();
}

std::vector<std::string> splitFolderList(const std::string& text) {
#ifdef _WIN32
    const char sep = ';';   // "C:/..." holds a colon
#else
    const char sep = ':';
#endif
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == sep) { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

} // namespace r2d
