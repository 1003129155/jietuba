#include "core/bootstrap.h"
#include "video/worker.h"
#include <QString>
#include <algorithm>
#include <string>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace {
// The UTF-8 command line; argv is in the ANSI code page and would mangle non-ASCII output paths.
std::vector<std::string> utf8_arguments() {
    std::vector<std::string> args;
    int count = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) return args;
    for (int i = 0; i < count; ++i) args.push_back(QString::fromWCharArray(wide[i]).toStdString());
    LocalFree(wide);
    return args;
}
}

int main(int argc, char **argv) {
    // The MP4 recording worker branches off before Qt starts.
    if (argc > 1 && std::string(argv[1]) == "--video-recorder-worker") {
        const auto args = utf8_arguments();
        return jietuba::video::worker_main(std::vector<std::string>(args.begin() + std::min<size_t>(2, args.size()), args.end()));
    }
    return jietuba::run(argc, argv);
}
