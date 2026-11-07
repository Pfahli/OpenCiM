#pragma once
// Saving the map state (docs/save-format/README.md §1, §3-4): gsGrid.saveToFile and gsGridTimeline.saveToFile.
#include <cstdint>
#include <functional>
#include <vector>

#include "script/vm.h"

namespace regs::engine {

class GridObj;

// Writes one top-level section of map.grid (a whole chunk: tag, size, body) from live state into out and returns
// true, or returns false to keep the section as loaded. Called for every section of the loaded file in its order, then
// once for each of the simulation's tags 0x07040000..0x07070000 the file lacked (to append them).
using GridSectionWriter = std::function<bool(script::VM&, GridObj*, uint32_t tag, std::vector<uint8_t>& out)>;
void setGridSectionWriter(GridSectionWriter writer);

// a whole map.grid of a GridData: header, terrain and network only (the editor's templates)
struct GridData;
std::vector<uint8_t> gridFileOf(const GridData& g);

void registerSaveNatives(script::VM& vm);

}  // namespace regs::engine
