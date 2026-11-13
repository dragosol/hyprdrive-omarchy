#pragma once
// The Wine prefixes of tests/fixtures/identity/ have a dosdevices/c: link to
// ../drive_c. The repository holds no symbolic links (the Omarchy marketplace
// refuses a plugin folder that has one), so the prefixes that have the link
// are listed in tests/fixtures/identity/dosdevices.list and a test makes the
// links in its private copy of a fixture.
#include <filesystem>
#include <fstream>
#include <string>

inline void fixtureLinks(const std::string& name, const std::string& copyRoot) {
    std::ifstream in("tests/fixtures/identity/dosdevices.list");
    std::string   line;
    while (std::getline(in, line)) {
        if (line.rfind(name + "/", 0) != 0)
            continue;
        const std::filesystem::path DIR = std::filesystem::path(copyRoot) / line.substr(name.size() + 1) / "dosdevices";
        std::error_code             ec;
        std::filesystem::create_directories(DIR, ec);
        std::filesystem::create_symlink("../drive_c", DIR / "c:", ec);
    }
}
