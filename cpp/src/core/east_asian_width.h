#pragma once

namespace jietuba {
// Whether the code point's East Asian Width is W or F (wide or fullwidth), exactly as
// the reference interpreter's unicodedata reports it.
bool is_east_asian_wide(char32_t code_point);
}
