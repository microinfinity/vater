#pragma once
/// @file pane_tree.hpp
/// @brief L2 Multiplexer — Binary Split Pane Tree computed via `nisaba-layout` Flexbox engine.

#include "mux/session.hpp"
#include "nisaba/math/rect.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace vater::mux {

enum class SplitAxis : uint8_t {
    Horizontal, // Left | Right (FlexDirection::Row)
    Vertical,   // Top / Bottom (FlexDirection::Column)
};

enum class FocusDir : uint8_t {
    Left,
    Right,
    Up,
    Down,
};

struct PaneLayoutRect {
    uint32_t pane_id{0};
    nisaba::Rect bounds{};
    Session* session{nullptr};
    bool focused{false};
};

struct DividerRect {
    uint32_t split_id{0};
    nisaba::Rect bounds{};
    nisaba::Rect container_bounds{};
    SplitAxis axis{SplitAxis::Horizontal};
};

/// Binary split tree using `nisaba::layout::Node` flexbox solver for exact pixel-grid layout.
class PaneTree {
public:
    PaneTree() = default;
    ~PaneTree() = default;

    /// Initializes the tree with a single root pane.
    void init_root(std::unique_ptr<Session> session);

    /// Splits `target_pane_id` along `axis` with `new_session` at `ratio` (0.1..0.9).
    bool split_pane(uint32_t target_pane_id, SplitAxis axis,
                    std::unique_ptr<Session> new_session, float ratio = 0.5f);

    /// Closes `pane_id`. Returns false if it was the last remaining pane (caller should close tab).
    bool close_pane(uint32_t pane_id);

    /// Adjusts split ratio of the parent split containing `pane_id`.
    bool adjust_ratio(uint32_t pane_id, float delta);

    /// Sets split ratio directly by `split_id` (used for interactive divider mouse dragging).
    bool set_split_ratio(uint32_t split_id, float ratio);

    /// Computes layout of all panes inside `viewport` using `nisaba::layout` Flexbox solver.
    void compute_layout(const nisaba::Rect& viewport,
                        std::vector<PaneLayoutRect>& out_panes,
                        std::vector<DividerRect>& out_dividers,
                        float divider_px = 2.0f) const;

    /// Finds pane at screen coordinate (x, y) using last computed layout.
    [[nodiscard]] uint32_t pane_at(float x, float y, const std::vector<PaneLayoutRect>& layout) const noexcept;

    /// Directional focus navigation between panes based on layout geometry.
    [[nodiscard]] uint32_t find_neighbor(uint32_t from_pane, FocusDir dir,
                                         const std::vector<PaneLayoutRect>& layout) const noexcept;

    void set_focused_pane(uint32_t pane_id) noexcept { focused_pane_id_ = pane_id; }
    [[nodiscard]] uint32_t focused_pane_id() const noexcept { return focused_pane_id_; }
    [[nodiscard]] Session* focused_session() noexcept { return find_session(focused_pane_id_); }
    [[nodiscard]] const Session* focused_session() const noexcept { return find_session(focused_pane_id_); }

    void set_zoomed(bool z) noexcept { zoomed_ = z; }
    [[nodiscard]] bool is_zoomed() const noexcept { return zoomed_; }

    [[nodiscard]] Session* find_session(uint32_t pane_id) noexcept;
    [[nodiscard]] const Session* find_session(uint32_t pane_id) const noexcept;

    void collect_sessions(std::vector<Session*>& out) noexcept;
    [[nodiscard]] size_t pane_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return root_ == nullptr; }

private:
    struct TreeNode {
        bool is_leaf{true};
        uint32_t split_id{0};
        SplitAxis axis{SplitAxis::Horizontal};
        float ratio{0.5f};
        std::unique_ptr<Session> session;
        std::unique_ptr<TreeNode> first;
        std::unique_ptr<TreeNode> second;
    };

    static TreeNode* find_leaf(TreeNode* node, uint32_t pane_id) noexcept;
    static const TreeNode* find_leaf(const TreeNode* node, uint32_t pane_id) noexcept;
    static uint32_t first_leaf_id(const TreeNode* node) noexcept;
    static size_t count_leaves(const TreeNode* node) noexcept;
    static void collect_leaves(TreeNode* node, std::vector<Session*>& out) noexcept;
    static bool close_recursive(std::unique_ptr<TreeNode>& slot, uint32_t pane_id);
    static bool adjust_ratio_recursive(TreeNode* node, uint32_t pane_id, float delta);
    static bool set_split_ratio_recursive(TreeNode* node, uint32_t split_id, float ratio);

    std::unique_ptr<TreeNode> root_;
    uint32_t focused_pane_id_{0};
    uint32_t next_split_id_{1};
    bool zoomed_{false};
};

} // namespace vater::mux

