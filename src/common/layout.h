#pragma once
// Split layout: a cake-cutting tree over the physical panel.
//
// Every node is either a leaf (one virtual monitor) or a cut into 2..N parts, stacked as rows
// (top to bottom) or columns (left to right). Sizes are pixels at the design resolution; when the
// panel resolution differs they are applied proportionally.
//
// Text form (config.ini "layout"):  L | R(size:node,size:node,...) | C(size:node,...)
//   top/bottom halves of 2560x2880:  R(1440:L,1440:L)
//   top half + bottom split in two:  R(1440:L,1440:C(1280:L,1280:L))

#include <Windows.h>

#include <string>
#include <vector>

constexpr int kMaxRegions = 8;
constexpr int kMinRegionSize = 320; // px, smallest virtual monitor edge

struct LayoutNode
{
    enum Kind
    {
        Leaf,
        Rows,
        Cols
    } kind = Leaf;
    std::vector<int> sizes;
    std::vector<LayoutNode> kids;
};

using NodePath = std::vector<int>; // child indices from the root

struct LayoutRegion
{
    RECT rc;
    NodePath path;
};

struct LayoutSplitter
{
    NodePath parent; // the cut node
    int index;       // boundary between kids[index] and kids[index + 1]
    bool vertical;   // true: a vertical line (between columns)
    int pos;         // absolute x (vertical) or y (horizontal) in panel pixels
    int lo, hi;      // allowed range for pos
    RECT line;       // the line's extent, for drawing/hit testing
};

// Leaves in depth-first order (= virtual monitor order), in panel pixels.
std::vector<LayoutRegion> ResolveLayout(const LayoutNode& root, int width, int height);
std::vector<LayoutSplitter> ResolveSplitters(const LayoutNode& root, int width, int height);
int CountRegions(const LayoutNode& root);

std::wstring SerializeLayout(const LayoutNode& root);
bool ParseLayout(const std::wstring& text, LayoutNode& out);

// Rewrites every size as exact pixels for this resolution.
void NormalizeLayout(LayoutNode& root, int width, int height);

// Cake cutting: turns the leaf at `path` into `parts` equal rows or columns.
bool SplitEqual(LayoutNode& root, const NodePath& path, LayoutNode::Kind kind, int parts, int width, int height);
// Undoes the cut that produced the leaf at `path` (its parent becomes one region again).
bool MergeParent(LayoutNode& root, const NodePath& path);
// Manual: moves a splitter to an absolute pixel position (clamped).
bool MoveSplitter(LayoutNode& root, const LayoutSplitter& s, int pos, int width, int height);

struct LayoutPreset
{
    const wchar_t* name;
    LayoutNode (*make)(int width, int height);
};
const std::vector<LayoutPreset>& LayoutPresets();

// Two halves along the panel's long side.
LayoutNode DefaultLayout(int width, int height);
