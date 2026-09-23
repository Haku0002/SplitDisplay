#include "layout.h"

#include <algorithm>
#include <cwchar>

namespace
{
int Extent(const RECT& r, LayoutNode::Kind kind)
{
    return kind == LayoutNode::Rows ? r.bottom - r.top : r.right - r.left;
}

// Splits `extent` in proportion to `sizes`; the last part takes the rounding remainder.
std::vector<int> Distribute(const std::vector<int>& sizes, int extent)
{
    long long sum = 0;
    for (int s : sizes) sum += std::max(s, 1);
    std::vector<int> out(sizes.size());
    int used = 0;
    for (size_t i = 0; i < sizes.size(); i++)
    {
        out[i] = i + 1 == sizes.size() ? extent - used : (int)((long long)std::max(sizes[i], 1) * extent / sum);
        used += out[i];
    }
    return out;
}

RECT KidRect(const RECT& r, LayoutNode::Kind kind, int offset, int size)
{
    RECT k = r;
    if (kind == LayoutNode::Rows)
    {
        k.top = r.top + offset;
        k.bottom = k.top + size;
    }
    else
    {
        k.left = r.left + offset;
        k.right = k.left + size;
    }
    return k;
}

// Smallest extent a node can take along the axis of `kind` without squeezing any region below
// kMinRegionSize.
int MinExtent(const LayoutNode& n, LayoutNode::Kind kind)
{
    if (n.kind == LayoutNode::Leaf) return kMinRegionSize;
    int v = 0;
    for (auto& k : n.kids)
        v = n.kind == kind ? v + MinExtent(k, kind) : std::max(v, MinExtent(k, kind));
    return v;
}

template <class Fn>
void Walk(const LayoutNode& n, const RECT& r, NodePath& path, Fn&& fn)
{
    fn(n, r, path);
    if (n.kind == LayoutNode::Leaf) return;
    auto parts = Distribute(n.sizes, Extent(r, n.kind));
    int offset = 0;
    for (size_t i = 0; i < n.kids.size(); i++)
    {
        path.push_back((int)i);
        Walk(n.kids[i], KidRect(r, n.kind, offset, parts[i]), path, fn);
        path.pop_back();
        offset += parts[i];
    }
}

LayoutNode* Find(LayoutNode& root, const NodePath& path)
{
    LayoutNode* n = &root;
    for (int i : path)
    {
        if (n->kind == LayoutNode::Leaf || i < 0 || i >= (int)n->kids.size()) return nullptr;
        n = &n->kids[i];
    }
    return n;
}

bool FindRect(const LayoutNode& root, const NodePath& target, int w, int h, RECT& out)
{
    bool found = false;
    NodePath path;
    Walk(root, RECT{ 0, 0, w, h }, path, [&](const LayoutNode&, const RECT& r, const NodePath& p) {
        if (p == target)
        {
            out = r;
            found = true;
        }
    });
    return found;
}

void Serialize(const LayoutNode& n, std::wstring& s)
{
    if (n.kind == LayoutNode::Leaf)
    {
        s += L'L';
        return;
    }
    s += n.kind == LayoutNode::Rows ? L"R(" : L"C(";
    for (size_t i = 0; i < n.kids.size(); i++)
    {
        if (i) s += L',';
        s += std::to_wstring(n.sizes[i]) + L':';
        Serialize(n.kids[i], s);
    }
    s += L')';
}

bool Parse(const wchar_t*& p, LayoutNode& n, int depth)
{
    if (depth > 16) return false;
    while (*p == L' ') p++;
    if (*p == L'L')
    {
        p++;
        n = LayoutNode{};
        return true;
    }
    if (*p != L'R' && *p != L'C') return false;
    n.kind = *p == L'R' ? LayoutNode::Rows : LayoutNode::Cols;
    p++;
    if (*p++ != L'(') return false;
    for (;;)
    {
        wchar_t* end = nullptr;
        long size = wcstol(p, &end, 10);
        if (end == p || size <= 0 || *end != L':') return false;
        p = end + 1;
        LayoutNode kid;
        if (!Parse(p, kid, depth + 1)) return false;
        n.sizes.push_back((int)size);
        n.kids.push_back(std::move(kid));
        if (*p == L',')
        {
            p++;
            continue;
        }
        if (*p == L')')
        {
            p++;
            break;
        }
        return false;
    }
    return n.kids.size() >= 2;
}

void Normalize(LayoutNode& n, const RECT& r)
{
    if (n.kind == LayoutNode::Leaf) return;
    auto parts = Distribute(n.sizes, Extent(r, n.kind));
    int offset = 0;
    for (size_t i = 0; i < n.kids.size(); i++)
    {
        n.sizes[i] = parts[i];
        Normalize(n.kids[i], KidRect(r, n.kind, offset, parts[i]));
        offset += parts[i];
    }
}

LayoutNode Cut(LayoutNode::Kind kind, std::vector<int> sizes, std::vector<LayoutNode> kids = {})
{
    LayoutNode n;
    n.kind = kind;
    n.sizes = std::move(sizes);
    n.kids = kids.empty() ? std::vector<LayoutNode>(n.sizes.size()) : std::move(kids);
    return n;
}

std::vector<int> Equal(int extent, int parts)
{
    std::vector<int> v(parts, extent / parts);
    v.back() += extent % parts;
    return v;
}
} // namespace

std::vector<LayoutRegion> ResolveLayout(const LayoutNode& root, int width, int height)
{
    std::vector<LayoutRegion> out;
    NodePath path;
    Walk(root, RECT{ 0, 0, width, height }, path, [&](const LayoutNode& n, const RECT& r, const NodePath& p) {
        if (n.kind == LayoutNode::Leaf) out.push_back({ r, p });
    });
    return out;
}

std::vector<LayoutSplitter> ResolveSplitters(const LayoutNode& root, int width, int height)
{
    std::vector<LayoutSplitter> out;
    NodePath path;
    Walk(root, RECT{ 0, 0, width, height }, path, [&](const LayoutNode& n, const RECT& r, const NodePath& p) {
        if (n.kind == LayoutNode::Leaf) return;
        auto parts = Distribute(n.sizes, Extent(r, n.kind));
        int offset = 0;
        for (size_t i = 0; i + 1 < parts.size(); i++)
        {
            offset += parts[i];
            LayoutSplitter s;
            s.parent = p;
            s.index = (int)i;
            s.vertical = n.kind == LayoutNode::Cols;
            int base = s.vertical ? r.left : r.top;
            s.pos = base + offset;
            s.lo = s.pos - parts[i] + MinExtent(n.kids[i], n.kind);
            s.hi = s.pos + parts[i + 1] - MinExtent(n.kids[i + 1], n.kind);
            s.line = s.vertical ? RECT{ s.pos, r.top, s.pos, r.bottom } : RECT{ r.left, s.pos, r.right, s.pos };
            out.push_back(s);
        }
    });
    return out;
}

int CountRegions(const LayoutNode& root)
{
    if (root.kind == LayoutNode::Leaf) return 1;
    int n = 0;
    for (auto& k : root.kids) n += CountRegions(k);
    return n;
}

std::wstring SerializeLayout(const LayoutNode& root)
{
    std::wstring s;
    Serialize(root, s);
    return s;
}

bool ParseLayout(const std::wstring& text, LayoutNode& out)
{
    const wchar_t* p = text.c_str();
    LayoutNode n;
    if (!Parse(p, n, 0)) return false;
    while (*p == L' ') p++;
    if (*p) return false;
    if (CountRegions(n) > kMaxRegions) return false;
    out = std::move(n);
    return true;
}

void NormalizeLayout(LayoutNode& root, int width, int height)
{
    Normalize(root, RECT{ 0, 0, width, height });
}

bool SplitEqual(LayoutNode& root, const NodePath& path, LayoutNode::Kind kind, int parts, int width, int height)
{
    if (kind == LayoutNode::Leaf || parts < 2) return false;
    if (CountRegions(root) - 1 + parts > kMaxRegions) return false;
    LayoutNode* n = Find(root, path);
    RECT r;
    if (!n || n->kind != LayoutNode::Leaf || !FindRect(root, path, width, height, r)) return false;
    int extent = Extent(r, kind);
    if (extent / parts < kMinRegionSize) return false;
    *n = Cut(kind, Equal(extent, parts));
    return true;
}

bool MergeParent(LayoutNode& root, const NodePath& path)
{
    if (path.empty()) return false;
    NodePath parentPath(path.begin(), path.end() - 1);
    LayoutNode* parent = Find(root, parentPath);
    if (!parent) return false;
    *parent = LayoutNode{};
    return true;
}

bool MoveSplitter(LayoutNode& root, const LayoutSplitter& s, int pos, int width, int height)
{
    NormalizeLayout(root, width, height);
    LayoutNode* n = Find(root, s.parent);
    if (!n || n->kind == LayoutNode::Leaf || s.index + 1 >= (int)n->sizes.size()) return false;
    pos = std::clamp(pos, s.lo, s.hi);
    int delta = pos - s.pos;
    n->sizes[s.index] += delta;
    n->sizes[s.index + 1] -= delta;
    NormalizeLayout(root, width, height);
    return true;
}

LayoutNode DefaultLayout(int width, int height)
{
    return height >= width ? Cut(LayoutNode::Rows, Equal(height, 2)) : Cut(LayoutNode::Cols, Equal(width, 2));
}

const std::vector<LayoutPreset>& LayoutPresets()
{
    static const std::vector<LayoutPreset> presets = {
        { L"Top / bottom", [](int, int h) { return Cut(LayoutNode::Rows, Equal(h, 2)); } },
        { L"Left / right", [](int w, int) { return Cut(LayoutNode::Cols, Equal(w, 2)); } },
        { L"2 x 2 grid",
            [](int w, int h) {
                auto half = Equal(h, 2);
                return Cut(LayoutNode::Rows, half, { Cut(LayoutNode::Cols, Equal(w, 2)), Cut(LayoutNode::Cols, Equal(w, 2)) });
            } },
        { L"Top + bottom halved",
            [](int w, int h) {
                return Cut(LayoutNode::Rows, Equal(h, 2), { LayoutNode{}, Cut(LayoutNode::Cols, Equal(w, 2)) });
            } },
        { L"Three rows", [](int, int h) { return Cut(LayoutNode::Rows, Equal(h, 3)); } },
        { L"Three columns", [](int w, int) { return Cut(LayoutNode::Cols, Equal(w, 3)); } },
    };
    return presets;
}
