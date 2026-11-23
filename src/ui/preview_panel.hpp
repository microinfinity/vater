#pragma once
/// @file preview_panel.hpp
/// @brief L5 UI (M5) — Integrated Side-Panel Preview for Markdown (`nisaba::markdown`),
/// PDF (`nisaba::pdf`), and Images (`nisaba::image`).

#include "nisaba/canvas/pixmap.hpp"
#include "nisaba/gpu/gpu_canvas.hpp"
#include "nisaba/markdown/markdown.hpp"
#include "nisaba/math/rect.hpp"
#include "nisaba/pdf/pdf.hpp"
#include "theme.hpp"

#include <memory>
#include <string>

namespace vater::ui {

class PreviewPanel {
public:
    enum class Mode { None, Markdown, Pdf, Image };

    PreviewPanel() = default;

    /// Opens a `.md`, `.pdf`, or image file (`.png`, `.jpg`, `.webp`, `.qoi`, `.bmp`) for side-panel preview.
    bool open_file(const std::string& path);

    /// Opens inline Markdown text (e.g. Vater built-in help / shortcut cheatsheet).
    void open_markdown_text(const std::string& title, std::string_view md_text);

    void close();
    void toggle_help();

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    void scroll(float dy) noexcept;
    void next_pdf_page() noexcept;
    void prev_pdf_page() noexcept;

    void render(nisaba::gpu::GpuCanvas& canvas,
                const nisaba::Rect& bounds,
                const obsidian::ui::AppTheme& theme,
                const obsidian::ui::UnitScale& scale);

private:
    bool open_{false};
    Mode mode_{Mode::None};
    std::string title_;
    float scroll_y_{0.0f};

    nisaba::markdown::MarkdownDocument md_doc_;
    std::unique_ptr<nisaba::pdf::PdfReader> pdf_reader_;
    size_t pdf_page_{0};
    nisaba::Pixmap image_pixmap_;
    nisaba::text::GlyphCache md_glyph_cache_{2048};
};

} // namespace vater::ui

