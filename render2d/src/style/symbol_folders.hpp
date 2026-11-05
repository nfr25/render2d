// SPDX-License-Identifier: AGPL-3.0-or-later
// Symbol libraries (notions §76): folders where an image a style names
// without a folder ("park.svg", "nature/park.svg") is looked for, after the
// style's own folder. As QGIS's "SVG paths": the symbols are part of the
// data of a map; a map sent elsewhere is packaged with them.
#pragma once

#include <string>
#include <vector>

namespace r2d {

// The libraries, in search order (render2d: --symbols, R2D_SYMBOLS, then
// symbols/ beside the executable; carto2D: the user's, then its own). Set
// once at start; read by every thread.
void                     setSymbolFolders(std::vector<std::string> folders);
std::vector<std::string> symbolFolders();

// The file a style's url names. An absolute url: itself. A relative one:
// in the style's folder first (`styleDir`; "" = the current folder), then in
// each library. "" when found nowhere.
std::string findSymbol(const std::string& url, const std::string& styleDir);

// The url a style in `styleDir` writes for the file `path`: relative to the
// style when under its folder ("icons/tree.svg"), else relative to the first
// library holding it ("nature/park.svg"), else the whole path.
std::string symbolUrl(const std::string& path, const std::string& styleDir);

// "C:/a;D:/b" (Windows) or "/a:/b" (elsewhere), as PATH: the folders of R2D_SYMBOLS.
std::vector<std::string> splitFolderList(const std::string& text);

} // namespace r2d
