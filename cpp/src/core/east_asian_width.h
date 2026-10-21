#pragma once

namespace jietuba {
// Whether the code point's East Asian Width is W or F (wide or fullwidth).
bool is_east_asian_wide(char32_t code_point);
}
