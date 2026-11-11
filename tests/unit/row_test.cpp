// Host test for Row.hpp and RowsFile.hpp: a monitor's row of desktops -- the
// implicit row (slot == id, unbounded), an Omarchy block (base+1..base+10) and
// its overflow (an 11th desktop that can never be another block's), and the
// parse of Omarchy's monitor-workspaces / -merged files.
#include "Row.hpp"
#include "RowsFile.hpp"

#include <cstdio>

using namespace Hyprdrive;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

int main() {
    // ---- the implicit row: the global number line from 1
    {
        const Row R{};
        CHECK(!R.bounded());
        CHECK(R.first() == 1);
        CHECK(R.slot(1) == 1 && R.slot(37) == 37 && R.slot(1'000'001) == 1'000'001);
        CHECK(!R.contains(0) && !R.contains(-5));
        CHECK(R.idOf(5) == 5 && R.idOf(0) == 0);
        CHECK(R.rank(0) == 0 && R.rank(-1) == -1); // shift(0, -1) stays refusable
        CHECK(R.inBlock(99) && !R.inOverflow(99));
    }
    // ---- a block: block 2 = 21..30, then its overflow
    {
        const Row R = Row::block(2, 10);
        CHECK(R.bounded() && R.base == 20 && R.first() == 21);
        CHECK(R.slot(21) == 1 && R.slot(30) == 10);
        CHECK(!R.contains(20) && !R.contains(31) && !R.contains(1) && !R.contains(11));
        CHECK(R.idOf(1) == 21 && R.idOf(10) == 30);
        // the 11th desktop is NOT 31 (block 3's first): it is overflow
        CHECK(R.idOf(11) == 1'002'001 && R.idOf(12) == 1'002'002);
        CHECK(R.slot(1'002'001) == 11 && R.inOverflow(1'002'001) && !R.inBlock(1'002'001));
        CHECK(R.idOf(R.lastSlot()) == 1'003'000 && R.idOf(R.lastSlot() + 1) == 0);
        CHECK(R.idOf(0) == 20 && !R.contains(R.idOf(0))); // below the row: outside it
        for (Row::Id s = 1; s <= R.lastSlot(); ++s)
            CHECK(R.slot(R.idOf(s)) == s);
    }
    // ---- rows never overlap: blocks 0..99, blocks and overflows
    {
        int overlaps = 0;
        for (Row::Id a = 0; a < 20; ++a)
            for (Row::Id b = 0; b < 20; ++b) {
                if (a == b)
                    continue;
                const Row A = Row::block(a, 10), B = Row::block(b, 10);
                for (Row::Id s = 1; s <= A.lastSlot(); s += 7)
                    overlaps += B.contains(A.idOf(s));
            }
        CHECK(overlaps == 0);
        // an overflow id is in no block Omarchy can own: block_of_id is huge
        CHECK((Row::block(0, 10).idOf(11) - 1) / 10 >= 100'000);
        CHECK((Row::block(99, 10).idOf(Row::block(99, 10).lastSlot()) - 1) / 10 < 1'000'000'000);
    }
    // ---- sameRow
    {
        CHECK(sameRow(Row{}, Row{}));
        CHECK(sameRow(Row::block(1, 10), Row::block(1, 10, true))); // provisional is the same row
        CHECK(!sameRow(Row{}, Row::block(0, 10)));                   // block 0 is NOT the implicit row
        CHECK(!sameRow(Row::block(1, 10), Row::block(2, 10)));
        CHECK(Row::block(1, 10, true).provisional);
    }
    // ---- monitor-workspaces: connector -> block, the bar's rule
    {
        const auto M = RowsFile::blocks("0\teDP-1\tSharp Corporation 0x14F9\n1\t\tLG Electronics LG ULTRAGEAR+\n2\tDP-1\tSamsung LC34G55T #1\n");
        CHECK(M && M->size() == 2);
        CHECK(M && M->at("eDP-1") == 0 && M->at("DP-1") == 2);
        CHECK(M && !M->contains(""));
        CHECK(RowsFile::blocks("") && RowsFile::blocks("")->empty());
        CHECK(RowsFile::blocks("\n\n0\tHEADLESS-1\tHEADLESS-1\r\n") && RowsFile::blocks("0\tHEADLESS-1\tx\r\n")->at("HEADLESS-1") == 0);
        CHECK(!RowsFile::blocks("x\tDP-1\tfoo\n"));     // not ours: refused whole
        CHECK(!RowsFile::blocks("2\n"));                 // no connector field
        CHECK(!RowsFile::blocks("0\tA\ta\n-1\tB\tb\n")); // one bad line refuses the file
        CHECK(!RowsFile::blocks("100\tA\ta\n"));         // no such block
    }
    // ---- monitor-workspaces-merged: the guests' ids
    {
        const auto G = RowsFile::merged("21\t1\tSharp Corporation 0x14F9\n22\t2\tSharp Corporation 0x14F9\n");
        CHECK(G && G->size() == 2 && G->contains(21) && G->contains(22));
        CHECK(RowsFile::merged("") && RowsFile::merged("")->empty());
        CHECK(!RowsFile::merged("abc\t1\tx\n"));
        CHECK(!RowsFile::merged("0\t1\tx\n"));
    }
    // ---- Omarchy's slotOf (a visitor's label)
    {
        CHECK(RowsFile::slotOf(1, 10) == 1 && RowsFile::slotOf(10, 10) == 10 && RowsFile::slotOf(11, 10) == 1 && RowsFile::slotOf(24, 10) == 4);
        CHECK(RowsFile::slotOf(7, 0) == 7);
    }
    std::printf("row_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
