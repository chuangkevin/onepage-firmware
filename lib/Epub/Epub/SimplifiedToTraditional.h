#pragma once

#include <string>

// Simplified -> Traditional conversion for EPUB text.
//
// Backed by OpenCC's dictionaries (Apache-2.0), compiled into S2TTables.h by
// scripts/gen_s2t.py. Phrases are consulted before single characters, longest
// first, because a character-wise pass gets ambiguous words wrong (里 -> 裏 would
// turn 公里 into 公裏; the phrase table keeps the correct reading).
namespace SimplifiedToTraditional {

// Returns `in` with every simplified character replaced by its traditional form.
// Text that is already traditional passes through unchanged.
std::string convert(const std::string& in);

}  // namespace SimplifiedToTraditional
