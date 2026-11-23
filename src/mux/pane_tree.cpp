/// @file pane_tree.cpp
/// @brief L2 Multiplexer — Binary Split Pane Tree computed via `nisaba-layout` Flexbox engine.

#include "mux/pane_tree.hpp"

#include "nisaba/layout/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vater::mux {

namespace {

using nisaba::layout::Direction;
using nisaba::layout::FlexDirection;
using nisaba::layout::FloatOptional;
using nisaba::layout::Node;
using nisaba::layout::StyleSizeLength;

struct DividerEntry {
    uint32_t split_id{0};
    Node* div_node{nullptr};
    Node* parent_node{nullptr};
    SplitAxis axis{SplitAxis::Horizontal};
};

struct LayoutBuildCtx {
    std::vector<std::unique_ptr<Node>> owned_nodes;
    std::vector<std::pair<Node*, Session*>> leaf_map;
    std::vector<DividerEntry> divider_map;
};

} // namespace

void PaneTree::init_root(std::unique_ptr<Session> session) {
    root_ = std::make_unique<TreeNode>();
    root_->is_leaf = true;
    focused_pane_id_ = session ? session->id() : 0;
    root_->session = std::move(session);
    zoomed_ = false;
}

PaneTree::TreeNode* PaneTree::find_leaf(TreeNode* node, uint32_t pane_id) noexcept {
    if (!node) return nullptr;
    if (node->is_leaf) {
        return (node->session && node->session->id() == pane_id) ? node : nullptr;
    }
    if (auto* l = find_leaf(node->first.get(), pane_id)) return l;
    return find_leaf(node->second.get(), pane_id);
}

const PaneTree::TreeNode* PaneTree::find_leaf(const TreeNode* node, uint32_t pane_id) noexcept {
    if (!node) return nullptr;
    if (node->is_leaf) {
        return (node->session && node->session->id() == pane_id) ? node : nullptr;
    }
    if (auto* l = find_leaf(node->first.get(), pane_id)) return l;
    return find_leaf(node->second.get(), pane_id);
}

uint32_t PaneTree::first_leaf_id(const TreeNode* node) noexcept {
    if (!node) return 0;
    if (node->is_leaf) return node->session ? node->session->id() : 0;
    if (uint32_t id = first_leaf_id(node->first.get())) return id;
    return first_leaf_id(node->second.get());
}

size_t PaneTree::count_leaves(const TreeNode* node) noexcept {
    if (!node) return 0;
    if (node->is_leaf) return 1;
    return count_leaves(node->first.get()) + count_leaves(node->second.get());
}

void PaneTree::collect_leaves(TreeNode* node, std::vector<Session*>& out) noexcept {
    if (!node) return;
    if (node->is_leaf) {
        if (node->session) out.push_back(node->session.get());
        return;
    }
    collect_leaves(node->first.get(), out);
    collect_leaves(node->second.get(), out);
}

bool PaneTree::split_pane(uint32_t target_pane_id, SplitAxis axis,
                          std::unique_ptr<Session> new_session, float ratio) {
    if (!root_ || !new_session) return false;
    TreeNode* leaf = find_leaf(root_.get(), target_pane_id);
    if (!leaf) return false;

    const uint32_t new_id = new_session->id();
    auto old_child = std::make_unique<TreeNode>();
    old_child->is_leaf = true;
    old_child->session = std::move(leaf->session);

    auto new_child = std::make_unique<TreeNode>();
    new_child->is_leaf = true;
    new_child->session = std::move(new_session);

    leaf->is_leaf = false;
    leaf->split_id = next_split_id_++;
    leaf->axis = axis;
    leaf->ratio = std::clamp(ratio, 0.1f, 0.9f);
    leaf->first = std::move(old_child);
    leaf->second = std::move(new_child);

    focused_pane_id_ = new_id;
    zoomed_ = false;
    return true;
}

bool PaneTree::close_recursive(std::unique_ptr<TreeNode>& slot, uint32_t pane_id) {
    if (!slot || slot->is_leaf) return false;

    if (slot->first && slot->first->is_leaf && slot->first->session &&
        slot->first->session->id() == pane_id) {
        slot = std::move(slot->second);
        return true;
    }
    if (slot->second && slot->second->is_leaf && slot->second->session &&
        slot->second->session->id() == pane_id) {
        slot = std::move(slot->first);
        return true;
    }
    return close_recursive(slot->first, pane_id) || close_recursive(slot->second, pane_id);
}

bool PaneTree::close_pane(uint32_t pane_id) {
    if (!root_) return false;
    if (root_->is_leaf) {
        if (root_->session && root_->session->id() == pane_id) {
            root_.reset();
            focused_pane_id_ = 0;
            return false; // Last pane closed
        }
        return true;
    }
    if (!close_recursive(root_, pane_id)) return true;
    if (!find_leaf(root_.get(), focused_pane_id_)) {
        focused_pane_id_ = first_leaf_id(root_.get());
    }
    zoomed_ = false;
    return root_ != nullptr;
}

bool PaneTree::adjust_ratio_recursive(TreeNode* node, uint32_t pane_id, float delta) {
    if (!node || node->is_leaf) return false;
    if ((node->first && node->first->is_leaf && node->first->session && node->first->session->id() == pane_id) ||
        (node->second && node->second->is_leaf && node->second->session && node->second->session->id() == pane_id)) {
        node->ratio = std::clamp(node->ratio + delta, 0.15f, 0.85f);
        return true;
    }
    return adjust_ratio_recursive(node->first.get(), pane_id, delta) ||
           adjust_ratio_recursive(node->second.get(), pane_id, delta);
}

bool PaneTree::adjust_ratio(uint32_t pane_id, float delta) {
    return adjust_ratio_recursive(root_.get(), pane_id, delta);
}

bool PaneTree::set_split_ratio_recursive(TreeNode* node, uint32_t split_id, float ratio) {
    if (!node || node->is_leaf) return false;
    if (node->split_id == split_id) {
        node->ratio = std::clamp(ratio, 0.12f, 0.88f);
        return true;
    }
    return set_split_ratio_recursive(node->first.get(), split_id, ratio) ||
           set_split_ratio_recursive(node->second.get(), split_id, ratio);
}

bool PaneTree::set_split_ratio(uint32_t split_id, float ratio) {
    return set_split_ratio_recursive(root_.get(), split_id, ratio);
}

void PaneTree::compute_layout(const nisaba::Rect& viewport,
                              std::vector<PaneLayoutRect>& out_panes,
                              std::vector<DividerRect>& out_dividers,
                              float divider_px) const {
    out_panes.clear();
    out_dividers.clear();
    if (!root_) return;

    if (zoomed_) {
        if (const TreeNode* f = find_leaf(root_.get(), focused_pane_id_)) {
            out_panes.push_back(PaneLayoutRect{
                f->session->id(), viewport, f->session.get(), true
            });
            return;
        }
    }

    // Build a nisaba-layout Flexbox hierarchy directly mirroring the binary split tree
    LayoutBuildCtx ctx;

    std::function<Node*(const TreeNode*)> build_node = [&](const TreeNode* tn) -> Node* {
        auto n = std::make_unique<Node>();
        Node* raw = n.get();
        ctx.owned_nodes.push_back(std::move(n));

        if (tn->is_leaf) {
            ctx.leaf_map.emplace_back(raw, tn->session.get());
            return raw;
        }

        raw->style().setFlexDirection(
            tn->axis == SplitAxis::Horizontal ? FlexDirection::Row : FlexDirection::Column);

        Node* c1 = build_node(tn->first.get());
        c1->style().setFlex(FloatOptional{tn->ratio});

        auto div_node = std::make_unique<Node>();
        Node* div_raw = div_node.get();
        ctx.owned_nodes.push_back(std::move(div_node));
        if (tn->axis == SplitAxis::Horizontal) {
            div_raw->style().setDimension(nisaba::layout::Dimension::Width, StyleSizeLength::points(divider_px));
        } else {
            div_raw->style().setDimension(nisaba::layout::Dimension::Height, StyleSizeLength::points(divider_px));
        }
        ctx.divider_map.push_back(DividerEntry{tn->split_id, div_raw, raw, tn->axis});

        Node* c2 = build_node(tn->second.get());
        c2->style().setFlex(FloatOptional{1.0f - tn->ratio});

        raw->insertChild(c1, 0);
        raw->insertChild(div_raw, 1);
        raw->insertChild(c2, 2);
        return raw;
    };

    Node* root_layout = build_node(root_.get());
    root_layout->style().setDimension(nisaba::layout::Dimension::Width, StyleSizeLength::points(viewport.width()));
    root_layout->style().setDimension(nisaba::layout::Dimension::Height, StyleSizeLength::points(viewport.height()));
    root_layout->calculateLayout(viewport.width(), viewport.height(), Direction::LTR);

    // Helper: accumulate absolute coordinates relative to viewport origin
    auto abs_rect = [&](Node* n) -> nisaba::Rect {
        float x = viewport.left();
        float y = viewport.top();
        const float w = std::max(1.0f, n->getLayout().width());
        const float h = std::max(1.0f, n->getLayout().height());
        for (Node* cur = n; cur != nullptr; cur = cur->getParent()) {
            x += cur->getLayout().left();
            y += cur->getLayout().top();
        }
        return nisaba::Rect::from_xywh(x, y, w, h).value_or(viewport);
    };

    for (auto& [node, sess] : ctx.leaf_map) {
        if (!sess) continue;
        out_panes.push_back(PaneLayoutRect{
            sess->id(),
            abs_rect(node),
            sess,
            sess->id() == focused_pane_id_,
        });
    }

    for (const auto& de : ctx.divider_map) {
        out_dividers.push_back(DividerRect{
            de.split_id,
            abs_rect(de.div_node),
            abs_rect(de.parent_node),
            de.axis,
        });
    }
}

uint32_t PaneTree::pane_at(float x, float y, const std::vector<PaneLayoutRect>& layout) const noexcept {
    for (const auto& p : layout) {
        if (x >= p.bounds.left() && x <= p.bounds.right() &&
            y >= p.bounds.top() && y <= p.bounds.bottom()) {
            return p.pane_id;
        }
    }
    return 0;
}

uint32_t PaneTree::find_neighbor(uint32_t from_pane, FocusDir dir,
                                 const std::vector<PaneLayoutRect>& layout) const noexcept {
    const PaneLayoutRect* src = nullptr;
    for (const auto& p : layout) {
        if (p.pane_id == from_pane) { src = &p; break; }
    }
    if (!src) return from_pane;

    const float scx = (src->bounds.left() + src->bounds.right()) * 0.5f;
    const float scy = (src->bounds.top() + src->bounds.bottom()) * 0.5f;

    uint32_t best = from_pane;
    float best_score = std::numeric_limits<float>::max();

    for (const auto& p : layout) {
        if (p.pane_id == from_pane) continue;
        const float cx = (p.bounds.left() + p.bounds.right()) * 0.5f;
        const float cy = (p.bounds.top() + p.bounds.bottom()) * 0.5f;
        const float dx = cx - scx;
        const float dy = cy - scy;

        bool valid = false;
        float primary = 0.0f;
        float secondary = 0.0f;
        switch (dir) {
            case FocusDir::Left:  valid = (dx < -4.0f); primary = -dx; secondary = std::abs(dy); break;
            case FocusDir::Right: valid = (dx >  4.0f); primary =  dx; secondary = std::abs(dy); break;
            case FocusDir::Up:    valid = (dy < -4.0f); primary = -dy; secondary = std::abs(dx); break;
            case FocusDir::Down:  valid = (dy >  4.0f); primary =  dy; secondary = std::abs(dx); break;
        }
        if (valid) {
            const float score = primary + secondary * 2.0f;
            if (score < best_score) {
                best_score = score;
                best = p.pane_id;
            }
        }
    }
    return best;
}

Session* PaneTree::find_session(uint32_t pane_id) noexcept {
    if (auto* leaf = find_leaf(root_.get(), pane_id)) return leaf->session.get();
    return nullptr;
}

const Session* PaneTree::find_session(uint32_t pane_id) const noexcept {
    if (auto* leaf = find_leaf(root_.get(), pane_id)) return leaf->session.get();
    return nullptr;
}

void PaneTree::collect_sessions(std::vector<Session*>& out) noexcept {
    out.clear();
    collect_leaves(root_.get(), out);
}

size_t PaneTree::pane_count() const noexcept {
    return count_leaves(root_.get());
}

} // namespace vater::mux
