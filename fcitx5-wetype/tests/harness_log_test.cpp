#include "harness_log.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void writeSized(const std::string &path, off_t bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::string chunk(1024, 'x');
    for (off_t written = 0; written < bytes; written += 1024) {
        const off_t remaining = bytes - written;
        out.write(chunk.data(), remaining < 1024 ? static_cast<std::streamsize>(remaining) : 1024);
    }
}

off_t sizeOf(const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? st.st_size : -1;
}

}  // namespace

int main() {
    try {
        // $WETYPE_HARNESS_LOG wins over everything else.
        if (wetype::harnessLogPath("/custom/harness.log", "/state", "/home/kai") !=
            "/custom/harness.log") {
            throw std::runtime_error("override path not honoured");
        }
        // Otherwise: XDG state dir, never /tmp.
        if (wetype::harnessLogPath(nullptr, "/state", "/home/kai") !=
            "/state/wetype-ime/harness.log") {
            throw std::runtime_error("XDG_STATE_HOME not honoured");
        }
        if (wetype::harnessLogPath("", nullptr, "/home/kai") !=
            "/home/kai/.local/state/wetype-ime/harness.log") {
            throw std::runtime_error("HOME fallback not honoured");
        }
        if (wetype::harnessLogPath("", "", "") != "/tmp/.local/state/wetype-ime/harness.log") {
            throw std::runtime_error("homeless fallback not honoured");
        }

        // Rotation: one generation, and only once past the ceiling.
        char tmpl[] = "/tmp/wetype-logtest-XXXXXX";
        const char *dir = ::mkdtemp(tmpl);
        if (!dir) throw std::runtime_error("mkdtemp failed");
        const std::string path = std::string(dir) + "/harness.log";

        if (wetype::capHarnessLog(path, 4096)) {
            throw std::runtime_error("rotated a file that does not exist");
        }
        writeSized(path, 4096);
        if (wetype::capHarnessLog(path, 4096)) {
            throw std::runtime_error("rotated exactly at the ceiling");
        }
        if (sizeOf(path) != 4096) {
            throw std::runtime_error("touched a file under the ceiling");
        }
        writeSized(path, 8192);
        if (!wetype::capHarnessLog(path, 4096)) {
            throw std::runtime_error("did not rotate past the ceiling");
        }
        if (sizeOf(path + ".1") != 8192 || sizeOf(path) != -1) {
            throw std::runtime_error("rotation did not move the file to .1");
        }
        // A second rotation replaces the previous generation instead of piling up.
        writeSized(path, 5000);
        if (!wetype::capHarnessLog(path, 4096) || sizeOf(path + ".1") != 5000) {
            throw std::runtime_error("second rotation did not replace the old generation");
        }

        ::unlink(path.c_str());
        ::unlink((path + ".1").c_str());
        ::rmdir(dir);

        std::cout << "PASS harness_log: override, XDG state fallback, single-generation rotation\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
