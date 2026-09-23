// Unit tests for the layout tree (no display access). Run: build\Release\layout_test.exe
#include <cstdio>
#include <string>

#include "layout.h"

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(cond))                                                         \
        {                                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static bool Eq(const RECT& r, LONG l, LONG t, LONG ri, LONG b)
{
    return r.left == l && r.top == t && r.right == ri && r.bottom == b;
}

int main()
{
    const int W = 2560, H = 2880;

    // Default: two halves along the long side.
    {
        auto n = DefaultLayout(W, H);
        auto regs = ResolveLayout(n, W, H);
        CHECK(regs.size() == 2);
        CHECK(Eq(regs[0].rc, 0, 0, 2560, 1440));
        CHECK(Eq(regs[1].rc, 0, 1440, 2560, 2880));
        CHECK(SerializeLayout(n) == L"R(1440:L,1440:L)");
        auto wide = ResolveLayout(DefaultLayout(3440, 1440), 3440, 1440);
        CHECK(wide.size() == 2 && Eq(wide[0].rc, 0, 0, 1720, 1440));
    }

    // Parse / serialize round trip, nested cuts.
    {
        LayoutNode n;
        CHECK(ParseLayout(L"R(1440:L,1440:C(1280:L,1280:L))", n));
        CHECK(SerializeLayout(n) == L"R(1440:L,1440:C(1280:L,1280:L))");
        auto regs = ResolveLayout(n, W, H);
        CHECK(regs.size() == 3);
        CHECK(Eq(regs[2].rc, 1280, 1440, 2560, 2880));
        CHECK(regs[2].path == NodePath({ 1, 1 }));
    }

    // Rejects malformed or oversized layouts.
    {
        LayoutNode n;
        CHECK(!ParseLayout(L"", n));
        CHECK(!ParseLayout(L"R(1440:L)", n));          // a cut needs 2+ parts
        CHECK(!ParseLayout(L"R(1440:L,1440:L", n));    // unbalanced
        CHECK(!ParseLayout(L"R(0:L,1440:L)", n));      // zero size
        CHECK(!ParseLayout(L"X(1:L,1:L)", n));
        CHECK(!ParseLayout(L"C(1:L,1:L,1:L,1:L,1:L,1:L,1:L,1:L,1:L)", n)); // 9 > kMaxRegions
        CHECK(ParseLayout(L"L", n) && CountRegions(n) == 1);
    }

    // Proportional when the panel resolution differs from the design resolution.
    {
        LayoutNode n;
        CHECK(ParseLayout(L"R(1440:L,1440:L)", n));
        auto regs = ResolveLayout(n, 1920, 2160);
        CHECK(Eq(regs[0].rc, 0, 0, 1920, 1080) && Eq(regs[1].rc, 0, 1080, 1920, 2160));
        // Odd extents: parts always cover the whole panel.
        auto odd = ResolveLayout(n, 2560, 2881);
        CHECK(odd[1].rc.bottom == 2881);
    }

    // Cake cutting: split the bottom half into 3 equal columns, then a column into 2 rows.
    {
        auto n = DefaultLayout(W, H);
        CHECK(SplitEqual(n, { 1 }, LayoutNode::Cols, 3, W, H));
        auto regs = ResolveLayout(n, W, H);
        CHECK(regs.size() == 4);
        CHECK(regs[1].rc.right - regs[1].rc.left == 853);
        CHECK(regs[3].rc.right == 2560);
        CHECK(SplitEqual(n, { 1, 2 }, LayoutNode::Rows, 2, W, H));
        CHECK(CountRegions(n) == 5);
        // Too small: 1440 / 5 = 288 < kMinRegionSize.
        CHECK(!SplitEqual(n, { 0 }, LayoutNode::Rows, 5, W, H));
        // Only leaves can be cut.
        CHECK(!SplitEqual(n, { 1 }, LayoutNode::Rows, 2, W, H));
        // Undo the last cut.
        CHECK(MergeParent(n, { 1, 2, 0 }));
        CHECK(CountRegions(n) == 4);
        CHECK(!MergeParent(n, {}));
    }

    // Region limit.
    {
        LayoutNode n;
        CHECK(ParseLayout(L"C(640:L,640:L,640:L,640:L)", n));
        CHECK(SplitEqual(n, { 0 }, LayoutNode::Rows, 2, W, H));
        CHECK(SplitEqual(n, { 1 }, LayoutNode::Rows, 2, W, H));
        CHECK(SplitEqual(n, { 2 }, LayoutNode::Rows, 2, W, H));
        CHECK(CountRegions(n) == 7);
        CHECK(!SplitEqual(n, { 3 }, LayoutNode::Rows, 3, W, H)); // would make 9
        CHECK(SplitEqual(n, { 3 }, LayoutNode::Rows, 2, W, H));  // exactly 8
        CHECK(CountRegions(n) == kMaxRegions);
    }

    // Manual: move a splitter, clamped so no region gets smaller than kMinRegionSize.
    {
        auto n = DefaultLayout(W, H);
        auto sp = ResolveSplitters(n, W, H);
        CHECK(sp.size() == 1 && !sp[0].vertical && sp[0].pos == 1440);
        CHECK(sp[0].lo == kMinRegionSize && sp[0].hi == H - kMinRegionSize);
        CHECK(MoveSplitter(n, sp[0], 1600, W, H));
        auto regs = ResolveLayout(n, W, H);
        CHECK(Eq(regs[0].rc, 0, 0, 2560, 1600) && Eq(regs[1].rc, 0, 1600, 2560, 2880));
        CHECK(SerializeLayout(n) == L"R(1600:L,1280:L)");
        sp = ResolveSplitters(n, W, H);
        CHECK(MoveSplitter(n, sp[0], 10, W, H)); // clamped
        regs = ResolveLayout(n, W, H);
        CHECK(regs[0].rc.bottom == kMinRegionSize);
    }

    // A nested same-axis cut limits how far the parent line can move.
    {
        LayoutNode n;
        CHECK(ParseLayout(L"R(1440:L,1440:R(720:L,720:L))", n));
        auto sp = ResolveSplitters(n, W, H);
        CHECK(sp.size() == 2);
        CHECK(sp[0].hi == H - 2 * kMinRegionSize); // bottom holds two rows
    }

    // Presets resolve to full coverage.
    for (auto& p : LayoutPresets())
    {
        auto n = p.make(W, H);
        long long area = 0;
        for (auto& r : ResolveLayout(n, W, H)) area += (long long)(r.rc.right - r.rc.left) * (r.rc.bottom - r.rc.top);
        CHECK(area == (long long)W * H);
        LayoutNode back;
        CHECK(ParseLayout(SerializeLayout(n), back) && SerializeLayout(back) == SerializeLayout(n));
    }

    std::printf(g_failures ? "%d FAILED\n" : "all layout tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
