/// @file preview_panel.cpp
/// @brief L5 UI (M5) — Integrated Side-Panel Preview for Markdown, PDF, and Images on Nisaba.

#include "ui/preview_panel.hpp"

#include "nisaba/canvas/paint.hpp"
#include "nisaba/image/image_io.hpp"

#include <algorithm>
#include <cctype>

namespace vater::ui {

namespace {

std::string lowercase_ext(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string basename_str(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

constexpr std::string_view kVaterHelpMarkdown = R"md(
# Vater — Sovereign Terminal

Built on **Nisaba** (C++20, zero third-party dependencies).

## Shortcuts

- `Ctrl+Shift+T` — New Tab
- `Ctrl+Shift+D` — Split Horizontal
- `Ctrl+Shift+E` — Split Vertical
- `Ctrl+Shift+W` — Close Pane / Tab
- `Ctrl+Shift+Z` — Zoom Active Pane
- `Ctrl+Shift+F` — Search Scrollback (Thompson NFA Regex)
- `Ctrl+Shift+P` — Command Palette
- `Ctrl+Shift+B` — Toggle Arabic BiDi Mode
- `Ctrl+Shift+L` — Toggle OpenType Ligatures
- `Ctrl+Shift+G` — Toggle Retro CRT Shader
- `Ctrl+Shift+M` — Toggle Preview Panel
- `Ctrl+Alt+Up/Down` — Jump Between Prompts (OSC 133)

## Features

1. **VT500 + xterm + Kitty** keyboard & inline graphics + Sixel
2. **Arabic Implicit BiDi** + Contextual Shaping
3. **vaterd** persistent sessions (`vater --attach`)
4. **Quake Mode** drop-down overlay (`vater --quake`)
)md";

} // namespace

bool PreviewPanel::open_file(const std::string& path) {
    const std::string ext = lowercase_ext(path);
    scroll_y_ = 0.0f;
    pdf_page_ = 0;

    if (ext == ".md" || ext == ".markdown" || ext == ".txt") {
        md_doc_ = nisaba::markdown::MarkdownDocument::from_file(path);
        if (!md_doc_.is_valid()) return false;
        mode_ = Mode::Markdown;
        title_ = basename_str(path);
        open_ = true;
        return true;
    }

    if (ext == ".pdf") {
        auto reader = std::make_unique<nisaba::pdf::PdfReader>();
        if (!reader->open_from_file(path) || reader->page_count() == 0) return false;
        pdf_reader_ = std::move(reader);
        mode_ = Mode::Pdf;
        title_ = basename_str(path);
        open_ = true;
        return true;
    }

    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" ||
        ext == ".qoi" || ext == ".bmp" || ext == ".ppm") {
        auto img = nisaba::image::load_image_file(path);
        if (!img.value.has_value()) return false;
        image_pixmap_ = std::move(*img.value);
        mode_ = Mode::Image;
        title_ = basename_str(path);
        open_ = true;
        return true;
    }

    return false;
}

void PreviewPanel::open_markdown_text(const std::string& title, std::string_view md_text) {
    md_doc_ = nisaba::markdown::MarkdownDocument::from_string(md_text);
    mode_ = Mode::Markdown;
    title_ = title;
    scroll_y_ = 0.0f;
    open_ = true;
}

void PreviewPanel::close() {
    open_ = false;
}

void PreviewPanel::toggle_help() {
    if (open_) {
        close();
    } else {
        open_markdown_text("Vater Guide", kVaterHelpMarkdown);
    }
}

void PreviewPanel::scroll(float dy) noexcept {
    scroll_y_ = std::max(0.0f, scroll_y_ - dy * 36.0f);
}

void PreviewPanel::next_pdf_page() noexcept {
    if (pdf_reader_ && pdf_page_ + 1 < pdf_reader_->page_count()) ++pdf_page_;
}

void PreviewPanel::prev_pdf_page() noexcept {
    if (pdf_page_ > 0) --pdf_page_;
}

void PreviewPanel::render(nisaba::gpu::GpuCanvas& canvas,
                          const nisaba::Rect& bounds,
                          const obsidian::ui::AppTheme& theme,
                          const obsidian::ui::UnitScale& scale) {
    if (!open_) return;

    const float u = scale.u;
    const float hdr_h = 34.0f * u;

    canvas.save();
    canvas.clip_rect(bounds);

    // Panel background & left separator
    const nisaba::Color panel_bg = obsidian::ui::detail::with_alpha(theme.secondBackground, 0.94f);
    canvas.fill_rect(bounds, nisaba::Paint(panel_bg));
    if (auto sep = nisaba::Rect::from_xywh(bounds.left(), bounds.top(), 1.0f, bounds.height())) {
        canvas.fill_rect(*sep, nisaba::Paint(theme.separator));
    }

    // Panel header
    std::string header = "Preview: " + title_;
    if (mode_ == Mode::Pdf && pdf_reader_) {
        header += "  [" + std::to_string(pdf_page_ + 1) + "/" + std::to_string(pdf_reader_->page_count()) + "]";
    }
    canvas.draw_text(bounds.left() + 14.0f * u, bounds.top() + 22.0f * u, header,
                     nisaba::Paint(theme.text), 12.5f * u, "sans");
    if (auto hsep = nisaba::Rect::from_xywh(bounds.left() + 10.0f * u, bounds.top() + hdr_h, bounds.width() - 20.0f * u, 1.0f)) {
        canvas.fill_rect(*hsep, nisaba::Paint(theme.headerSeparator));
    }

    auto body = nisaba::Rect::from_xywh(
        bounds.left() + 14.0f * u,
        bounds.top() + hdr_h + 10.0f * u,
        std::max(40.0f, bounds.width() - 28.0f * u),
        std::max(40.0f, bounds.height() - hdr_h - 20.0f * u));

    if (body) {
        canvas.clip_rect(*body);
        if (mode_ == Mode::Markdown && md_doc_.is_valid() && canvas.font_system()) {
            nisaba::markdown::MarkdownStyle style;
            style.text_color = theme.text;
            style.heading_color = theme.text;
            style.link_color = theme.highlight;
            style.code_block_bg = obsidian::ui::detail::with_alpha(theme.background, 0.92f);
            style.code_text_color = theme.text;
            style.code_span_bg = theme.mutedPlate;
            style.code_span_text = theme.highlight;
            style.base_font_size = 13.5f * u;
            md_doc_.layout(body->width(), style, *canvas.font_system(), md_glyph_cache_);
            nisaba::markdown::MarkdownRenderer::render(
                static_cast<nisaba::ICanvas&>(canvas),
                md_doc_, body->left(), body->top(), body->width(),
                style, *canvas.font_system(), md_glyph_cache_, scroll_y_);
        } else if (mode_ == Mode::Pdf && pdf_reader_ && pdf_page_ < pdf_reader_->page_count()) {
            const auto pbox = pdf_reader_->page_box(pdf_page_);
            const float fit_scale = (pbox.width() > 1.0f) ? (body->width() / pbox.width()) : 1.0f;
            canvas.save();
            canvas.translate(body->left(), body->top() - scroll_y_);
            pdf_reader_->render_page(pdf_page_, static_cast<nisaba::ICanvas&>(canvas),
                                     fit_scale, canvas.font_system());
            canvas.restore();
        } else if (mode_ == Mode::Image && image_pixmap_.width() > 0 && image_pixmap_.height() > 0) {
            const float iw = static_cast<float>(image_pixmap_.width());
            const float ih = static_cast<float>(image_pixmap_.height());
            const float s = std::min(body->width() / iw, body->height() / ih);
            const float dw = iw * s;
            const float dh = ih * s;
            const float dx = body->left() + (body->width() - dw) * 0.5f;
            const float dy = body->top() + (body->height() - dh) * 0.5f;
            auto src = nisaba::Rect::from_xywh(0.0f, 0.0f, iw, ih);
            auto dst = nisaba::Rect::from_xywh(dx, dy, dw, dh);
            if (src && dst) {
                canvas.draw_pixmap_rect(image_pixmap_.as_ref(), *src, *dst);
            }
        }
    }

    canvas.restore();
}

} // namespace vater::ui

