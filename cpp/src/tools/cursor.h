#pragma once
#include "tools/base.h"

namespace jietuba {
// Placeholder for "no drawing tool"; SmartEditController handles selecting and editing items.
class CursorTool : public Tool {
public:
    QString id() const override { return "cursor"; }
};
}
