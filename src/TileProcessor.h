#pragma once
#include "PixelContract.h"
#include <vector>

namespace Gaslightr {
// Linear sRGB/D65, straight-alpha RGBA only. Borrowed input remains valid for
// the whole synchronous call. No Photoshop or JavaScript APIs run here.
std::vector<float> ProcessTile(std::span<const float> rgba, const TileRequest& request);
}
