#pragma once

#include <sys/stat.h>
#include <sys/types.h>
#include <cstddef>
#include <string>

namespace wetype {

// The engine writes to its stderr on every start and on every request, and that
// stream is append-only. The project's own benchmark notes measure ~400 MB per
// run with logging on, so the path needs room to grow *and* a ceiling.
//
// The default used to be /tmp/wetype-harness.log. A tmpfs is small and is also
// where every tool's scratch files live: with a fcitx5 unit stuck in a 2 s
// restart loop (Restart=always, its bus name held by a second instance started
// from a terminal), each restart spawned a fresh engine that appended its ~30 KB
// startup banner to that one file. Measured: 405,986 restarts -> 11.7 GB, which
// exhausted the user's tmpfs quota and took down every tool that needs a temp
// file (bash, ripgrep, the browser).
inline constexpr off_t HARNESS_LOG_MAX_BYTES = 16 * 1024 * 1024;

// mkdir -p, best effort: an already existing directory is not an error, and a
// directory we cannot create surfaces later as a failed open() in the child.
inline void mkdirParents(const std::string &path) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0700);
    }
    ::mkdir(path.c_str(), 0700);
}

// $WETYPE_HARNESS_LOG wins. Otherwise follow the XDG base directory spec: a log
// is state, so it belongs in $XDG_STATE_HOME, not in /tmp.
inline std::string harnessLogPath(const char *overridePath, const char *xdgStateHome,
                                  const char *home) {
    if (overridePath && *overridePath) return overridePath;
    std::string dir = xdgStateHome && *xdgStateHome
                          ? std::string(xdgStateHome)
                          : std::string(home && *home ? home : "/tmp") + "/.local/state";
    dir += "/wetype-ime";
    return dir + "/harness.log";
}

// Rotate one generation (current -> .1, replacing the previous one) once the file
// passes the ceiling, so the footprint stays bounded at about twice the ceiling
// instead of growing without limit. Returns true when a rotation happened; a
// failure to rotate is not fatal, it only means the file keeps growing.
inline bool capHarnessLog(const std::string &path, off_t maxBytes = HARNESS_LOG_MAX_BYTES) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || st.st_size <= maxBytes) return false;
    return ::rename(path.c_str(), (path + ".1").c_str()) == 0;
}

}  // namespace wetype
