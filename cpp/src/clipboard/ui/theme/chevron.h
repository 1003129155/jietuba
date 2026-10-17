#pragma once
#include <QString>

namespace jietuba::clipboard {
// A style sheet "image" takes a file and no colour, so a small down chevron is drawn per colour and
// size into the cache folder and reused. Empty when the folder cannot be written; then no arrow is drawn.
QString chevron_image(const QString &color, int width, int height);
}
