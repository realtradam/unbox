#include <unbox/ext-wallpaper/ext_wallpaper.hpp>

#include "config.hpp"

#include <unbox/kernel/extension.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/ui.hpp>
#include <unbox/kernel/wlr.hpp>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

// ext-wallpaper glue (standard tier, GLOSSARY: "wallpaper").
// Composites a config-driven background image + solid colour at
// SceneLayer::background, below every application window.
//
// The design is deliberately simple — it is a LEAF (no hooks, no services):
//
//   activate()
//     -> discover + load config
//     -> create_surface() with an inline-baked RML document
//     -> watch the config file (hold FileWatch as a member)
//     -> subscribe to on_output_added / on_output_removed (RAII Subscriptions)
//
// DEFAULT IMAGE FALLBACK: when cfg_.path is empty, the glue resolves the
// bundled default image via resolve_asset_root() (reads $UNBOX_ASSET_DIR,
// falls back to UNBOX_ASSET_DIR_DEFAULT compile-time define, then ".") and
// config::default_image_path(root). If the file does not exist at that path
// (e.g. running uninstalled with no UNBOX_ASSET_DIR), the surface shows only
// the solid cfg_.color — one warning is logged and we never crash.
//
// Hot-reload on config change: DROP the old surface and CREATE a new one with
// freshly-baked values. This avoids the data-binding/decorator-path flakiness
// described in the brief; the surface is always baked with the current values.
// The drop+create path is also taken on the first output-added event if the
// surface could not be created (no output at activate time is a valid state;
// the output-added event drives the first sizing).
//
// If create_surface() returns null (no GL backend, e.g. headless pixman),
// we degrade gracefully: log a message and carry on without a surface. NEVER
// throw out of activate() for this reason — the brief is explicit.
//
// Input transparent: the surface is created with input_transparent=true so it
// NEVER steals clicks from windows or other surfaces above it.
//
// Lifetime discipline (listener-lifetime.md): RAII members released in
// reverse declaration order: subscriptions first (they may fire their last
// event into the surface), then surface (dropped while host_ borrow is still
// valid), then everything else. No manual teardown.

namespace unbox::ext_wallpaper {
namespace {

using kernel::Host;

// ---- config helpers (effects: file I/O lives here; pure parse in config.cpp) -

auto read_file(const std::string& path, std::string& out) -> bool {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

auto discover_config_path(const std::optional<std::string>& explicit_path)
    -> std::optional<std::string> {
    if (explicit_path) {
        return explicit_path;
    }
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
        return std::string(xdg) + "/unbox/unbox.toml";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::string(home) + "/.config/unbox/unbox.toml";
    }
    return std::nullopt;
}

// Load the wallpaper config from the effective path. Logs every warning.
// Called both at activate() and on hot-reload. Never throws.
auto load_config(const std::optional<std::string>& effective_path) -> config::WallpaperConfig {
    if (!effective_path) {
        wlr_log(WLR_INFO, "ext-wallpaper: no config path; using defaults");
        return config::WallpaperConfig{};
    }
    std::string text;
    if (!read_file(*effective_path, text)) {
        wlr_log(WLR_INFO, "ext-wallpaper: no config at '%s'; using defaults",
                effective_path->c_str());
        return config::WallpaperConfig{};
    }
    config::LoadResult loaded = config::load_from_string(text);
    for (const std::string& w : loaded.warnings) {
        wlr_log(WLR_ERROR, "ext-wallpaper: %s", w.c_str());
    }
    return loaded.cfg;
}

// ---- default asset path resolution (glue — effectful) ------------------------
//
// Mirror how the kernel substrate resolves a relative rml_path (ui.hpp):
//   1. $UNBOX_ASSET_DIR env var (set by the dev launch / test harness)
//   2. UNBOX_ASSET_DIR_DEFAULT compile-time macro (root meson.build -D define)
//   3. "." (process working directory — last resort)
// The pure path-join (config::default_image_path) is in config.cpp and is
// doctested; the env read stays here in the glue.

auto resolve_asset_root() -> std::string {
    if (const char* env = std::getenv("UNBOX_ASSET_DIR");
        env != nullptr && env[0] != '\0') {
        return std::string(env);
    }
#ifdef UNBOX_ASSET_DIR_DEFAULT
    return std::string(UNBOX_ASSET_DIR_DEFAULT);
#else
    return std::string(".");
#endif
}

// Cheap existence check: try opening the file for reading.
auto file_exists(const std::string& path) -> bool {
    std::ifstream f(path);
    return f.good();
}

// ---- inline RML builder -------------------------------------------------------
//
// Build a self-contained inline RML document that paints `color` as the body
// background and, when `image_path` is non-empty, applies the image as an
// image decorator using the given RmlUi fit keyword.
//
// We bake the values directly into the document text so we never need to fight
// data-binding / decorator-path binding (which the brief notes is unproven for
// decorators). On every config change we drop the old surface and create a new
// one with a freshly-baked document.
//
// RCSS image-decorator syntax (RmlUi docs):
//   decorator: image(<path> <fit> <alignment>) [, …];
// where fit is one of: cover | contain | fill | scale-none | …
// We supply center-center alignment in all cases (ignored by cover/contain but
// required by scale-none to centre the image).

auto build_rml(const std::string& image_path,
               const std::string& fit_keyword,
               const std::string& color) -> std::string {
    std::string doc;
    doc.reserve(512);
    doc += "<rml><head><style>\n";
    doc += "body {\n";
    doc += "    width: 100%;\n";
    doc += "    height: 100%;\n";
    doc += "    margin: 0;\n";
    doc += "    padding: 0;\n";
    doc += "    background-color: " + color + ";\n";
    if (!image_path.empty()) {
        // decorator: image('<path>' <fit> center center)
        // Fit keyword mapping (config.hpp rmlui_fit_keyword):
        //   cover   -> cover
        //   contain -> contain
        //   stretch -> fill
        //   center  -> scale-none
        doc += "    decorator: image('" + image_path + "' " + fit_keyword + " center center);\n";
    }
    doc += "}\n";
    doc += "</style></head><body></body></rml>\n";
    return doc;
}

// ---- WallpaperExtension -------------------------------------------------------

class WallpaperExtension final : public kernel::Extension {
public:
    explicit WallpaperExtension(std::optional<std::string> config_path)
        : config_path_(std::move(config_path)),
          effective_path_(discover_config_path(config_path_)),
          cfg_(load_config(effective_path_)) {}

    [[nodiscard]] auto manifest() const -> const kernel::Manifest& override { return manifest_; }

    void activate(Host& host) override {
        host_ = &host;

        // Subscribe to output events (RAII — released before surface_ at teardown).
        // The RAII ordering is: sub_ members declared after surface_, so they are
        // destroyed FIRST. We declare them after surface_ (see member declarations).
        // On add: size the surface (or create it if it was null due to no-GL on
        // first attempt). On remove: nothing — the surface stays (the next add will
        // resize it). This is the Wave-1 / primary-output posture; multi-output is
        // a documented gap.
        output_added_sub_ = host.subscribe(
            host.on_output_added(),
            [this](const kernel::OutputEvent& ev) { on_output_added(ev); });
        output_removed_sub_ = host.subscribe(
            host.on_output_removed(),
            [this](const kernel::OutputEvent& /*ev*/) {
                // Primary-output only (Wave 1). If the primary is removed we have
                // no output to show on; keep the surface but don't crash.
                wlr_log(WLR_INFO, "ext-wallpaper: output removed (multi-output not yet supported)");
            });

        // Arm hot-reload watch on the effective config path. Holds a FileWatch
        // member so it lives exactly as long as this extension.
        if (effective_path_) {
            config_watch_ = host.watch_file(*effective_path_, [this] { reload(); });
        }

        // Create the initial wallpaper surface sized to the primary output (if
        // one already exists). If not, on_output_added will create it.
        create_or_replace_surface(primary_output_box());
    }

private:
    // ---- output sizing --------------------------------------------------------

    void on_output_added(const kernel::OutputEvent& /*ev*/) {
        // Re-query the primary output box (the new output might BE the primary).
        const wlr_box box = primary_output_box();
        if (box.width <= 0 || box.height <= 0) {
            return;
        }
        if (surface_ == nullptr) {
            // Either no GL backend materialised yet, or we never had an output.
            // Try again now.
            create_or_replace_surface(box);
        } else {
            surface_->set_position(box.x, box.y);
            surface_->set_size(box.width, box.height);
        }
    }

    [[nodiscard]] auto primary_output_box() const -> wlr_box {
        wlr_box box{};
        if (host_ == nullptr) {
            return box;
        }
        wlr_output_layout* ol = host_->output_layout();
        if (ol == nullptr) {
            return box;
        }
        wlr_output_layout_output* lo = nullptr;
        // Wave-1: size to the first (primary) output only.
        wl_list_for_each(lo, &ol->outputs, link) {
            wlr_box b{};
            wlr_output_layout_get_box(ol, lo->output, &b);
            if (!wlr_box_empty(&b)) {
                box = b;
            }
            break; // primary only
        }
        return box;
    }

    // ---- surface management ---------------------------------------------------

    // Build a surface from the current cfg_ and the given output geometry.
    // If create_surface() returns null (no GL path / headless) we log and
    // return without crashing. On a config hot-reload we always drop the old
    // surface first (drop-and-recreate approach).
    void create_or_replace_surface(const wlr_box& box) {
        if (host_ == nullptr) {
            return;
        }

        // Drop old surface first (may happen on hot-reload or first-output-added).
        surface_.reset();

        // Resolve the effective image path: use cfg_.path when non-empty;
        // otherwise fall back to the bundled default. The pure path-join lives
        // in config::default_image_path; the env read + existence check is here
        // (glue — effectful). If the default file is also absent, degrade to
        // colour-only (one warning; no crash).
        std::string effective_image = cfg_.path;
        if (effective_image.empty()) {
            const std::string default_path =
                config::default_image_path(resolve_asset_root());
            if (file_exists(default_path)) {
                effective_image = default_path;
            } else {
                wlr_log(WLR_INFO,
                        "ext-wallpaper: no image configured and default not found at '%s'; "
                        "showing solid colour only",
                        default_path.c_str());
            }
        }

        const std::string fit_kw{config::rmlui_fit_keyword(cfg_.fit)};
        const std::string doc = build_rml(effective_image, fit_kw, cfg_.color);

        kernel::UiSurfaceSpec spec;
        spec.rml_inline = doc;
        spec.model = "ui";
        spec.x = box.x;
        spec.y = box.y;
        // Non-positive dimensions are rejected by the substrate; clamp to >= 1
        // (size_to_ via on_output_added will resize to the real box).
        spec.width = std::max(1, box.width);
        spec.height = std::max(1, box.height);
        spec.layer = kernel::SceneLayer::background;
        spec.visible = true;
        // REQUIRED for a wallpaper: must NEVER capture pointer/touch input.
        spec.input_transparent = true;

        surface_ = host_->ui().create_surface(spec);
        if (surface_ == nullptr) {
            wlr_log(WLR_INFO,
                    "ext-wallpaper: ui substrate unavailable (no GL path?); "
                    "running without wallpaper surface");
        }
    }

    // ---- hot-reload -----------------------------------------------------------

    void reload() {
        if (!effective_path_) {
            return;
        }
        cfg_ = load_config(effective_path_);
        wlr_log(WLR_INFO, "ext-wallpaper: config reloaded from '%s'",
                effective_path_->c_str());
        // Drop + recreate the surface with baked values (robust path; avoids
        // decorator data-binding flakiness noted in the brief).
        const wlr_box box = primary_output_box();
        create_or_replace_surface(box);
        // Resize to the actual output in case we got a valid box.
        if (surface_ != nullptr && box.width > 0 && box.height > 0) {
            surface_->set_position(box.x, box.y);
            surface_->set_size(box.width, box.height);
        }
    }

    // ---- data members ---------------------------------------------------------

    const kernel::Manifest manifest_{
        .id = "wallpaper",
        .tier = kernel::Tier::standard,
        .depends_on = {},
    };

    Host* host_ = nullptr;

    // Config state. cfg_ is the live config, swapped on hot-reload.
    std::optional<std::string> config_path_;
    std::optional<std::string> effective_path_;
    config::WallpaperConfig cfg_;

    // The wallpaper ui surface. Null on no-GL backend. Owned by this extension;
    // destroyed BEFORE the subscriptions (sub_ members declared after it), which
    // is exactly what we want: subscriptions release first so no event fires into
    // a dead surface. Wait — actually in C++ members are destroyed in REVERSE
    // declaration order (last declared = first destroyed), so we must declare
    // subscriptions AFTER the surface so they are destroyed FIRST. That's the
    // order below.
    std::unique_ptr<kernel::UiSurface> surface_;

    // RAII handles — declared AFTER surface_ so they are DESTROYED FIRST at
    // teardown. This ensures no late event fires into a dead surface or reads
    // cfg_ after the extension's data is gone. (Reverse-declaration destruction,
    // extension-agent.md.)
    kernel::FileWatch config_watch_;
    kernel::Subscription output_added_sub_;
    kernel::Subscription output_removed_sub_;
};

} // namespace

auto create(std::optional<std::string> config_path) -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<WallpaperExtension>(std::move(config_path));
}

} // namespace unbox::ext_wallpaper
