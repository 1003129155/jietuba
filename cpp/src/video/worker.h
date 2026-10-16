#pragma once
#include <string>
#include <vector>

// The MP4 recording worker process: only the native recorder runs here, frames stay
// native. Commands arrive as JSON lines on stdin, events leave as JSON lines on stdout.
namespace jietuba::video {
// args are the UTF-8 arguments after --video-recorder-worker; returns the exit code.
int worker_main(const std::vector<std::string> &args);
}
