// MountSort (dock:mount_sort): the smart order a drive list is shown in.
#include "../MountSort.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace ScrollOverview::MountSort;

static int checks = 0, failed = 0;

static void expectOrder(std::vector<std::string> want, const char* what) {
    // Every permutation's sort must land on `want`: start from the reverse and a rotation.
    for (int variant = 0; variant < 3; ++variant) {
        auto in = want;
        if (variant == 1)
            std::reverse(in.begin(), in.end());
        if (variant == 2 && in.size() > 1)
            std::rotate(in.begin(), in.begin() + 1, in.end());
        std::stable_sort(in.begin(), in.end(), smartLess);
        ++checks;
        if (in != want) {
            ++failed;
            std::printf("FAIL %s (variant %d):", what, variant);
            for (const auto& s : in)
                std::printf(" [%s]", s.c_str());
            std::printf("\n");
        }
    }
}

static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    // The report that started it (2026-10-08).
    expectOrder({"KleOne", "KleTwo", "KleThree", "KleFour"}, "camelCase English number words");
    expectOrder({"KleUn", "KleDeux", "KleTrois", "KleQuatre"}, "camelCase French number words");
    expectOrder({"Disque premier", "Disque deuxi\xc3\xa8me", "Disque troisi\xc3\xa8me"}, "French ordinals");
    expectOrder({"Backup 2", "Backup 10", "Backup 100"}, "digit runs as numbers");
    expectOrder({"Backup I", "Backup II", "Backup 3", "Backup IV"}, "standalone Roman numerals mix with digits");
    expectOrder({"archive", "Media", "photos", "Zeta"}, "words without case");
    expectOrder({"Data", "Data 2", "Data two b"}, "shorter first when equal so far");
    expectOrder({"disk 1", "disk one"}, "a tie on value falls back to the raw name");
    expectOrder({"Drive9", "Drive Ten", "DriveEleven"}, "words and digits on one scale");

    // Things that must NOT read as numbers.
    expect(tokens("DiskMIX").back().number == false, "a camelCase hump is never a Roman numeral");
    expect(tokens("Phone")[0].number == false, "a word containing 'one' is not one");
    expect(tokens("IIII")[0].number == false, "non-canonical Roman is a word");
    expect(tokens("mix")[0].number == false, "lower-case roman letters are a word");
    expect(tokens("VOLUME")[0].number == false, "an upper-case word is not Roman");
    expect(roman("XIV") == 14 && roman("MCMXC") == 1990, "Roman values");

    // The setting.
    expect(orderFromName("name") == EOrder::Name && orderFromName("smart") == EOrder::Smart && orderFromName("mounted") == EOrder::Mounted &&
               orderFromName("bogus") == EOrder::Mounted,
           "orderFromName");

    std::printf("mount_sort_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
