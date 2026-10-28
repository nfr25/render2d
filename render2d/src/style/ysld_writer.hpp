// SPDX-License-Identifier: AGPL-3.0-or-later
// Style -> YSLD text: the reader's twin (notions §59). Writes what the reader
// reads, in the model's order, and only what differs from the defaults: the
// file stays short. Comments and layout of a hand-written file are not kept.
#pragma once

#include <string>

#include "style/style.hpp"

namespace r2d {

// `baseDir`: the folder the text will be saved in; image paths under it are
// written relative to it, as the reader resolves them (§42). Empty: as they are.
std::string writeYsld(const Style& style, const std::string& baseDir = "");

} // namespace r2d
