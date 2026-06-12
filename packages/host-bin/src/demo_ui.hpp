#pragma once

#include <unbox/kernel/extension.hpp>
#include <unbox/kernel/host.hpp>
#include <unbox/kernel/ui.hpp>

#include <memory>
#include <string>

// Slice-5 acceptance demo (orchestrator-owned, TEMPORARY until slice 6's
// real UI extensions): a ui surface built purely on the public substrate
// contract. Proves the extension-facing path end-to-end and gives the
// hands-on test target: the SAME button must work by mouse and finger,
// with touch-mode visibly scaling it (dp units).
//
// Dies with slice 6 (taskbar/launcher become the real consumers).

namespace unbox::host_bin {

class DemoUi final : public kernel::Extension {
public:
    [[nodiscard]] auto manifest() const -> const kernel::Manifest& override {
        static const kernel::Manifest m{
            .id = "ui-demo",
            .tier = kernel::Tier::standard,
            .depends_on = {},
        };
        return m;
    }

    void activate(kernel::Host& host) override {
        ui_ = &host.ui();
        kernel::UiSurfaceSpec spec;
        spec.rml_inline = kDemoRml;
        spec.x = 48;
        spec.y = 48;
        spec.width = 420;
        spec.height = 260;
        surface_ = ui_->create_surface(spec);
        if (!surface_) {
            return; // degrade gracefully (no GL path) per the contract
        }
        surface_->bind_int("clicks", [this] { return clicks_; });
        surface_->bind_string("mode", [this] {
            return ui_->touch_mode() ? std::string{"finger (touch-mode ON)"}
                                     : std::string{"pointer"};
        });
        surface_->bind_event("bump", [this] {
            ++clicks_;
            surface_->dirty("clicks");
            surface_->dirty("mode");
        });
        // Live mode label + sizing idiom: text is px (stable), only the
        // button is dp — the surface needs no resize at ratio 1.25.
        surface_->on_touch_mode_changed([this](bool) { surface_->dirty("mode"); });
    }

private:
    // dp units everywhere: touch-mode (dp-ratio) must visibly scale this
    // document without any change here.
    static constexpr const char* kDemoRml = R"(<rml>
<head>
<style>
body {
    width: 100%; height: 100%;
    background-color: #1e2230;
    color: #e8eaf2;
    font-family: Noto Sans;
    font-size: 18px;   /* text in px: stable under touch-mode */
    padding: 16px;
}
h1 { font-size: 22px; color: #9ecbff; display: block; margin-bottom: 8px; }
p  { display: block; margin: 6px 0; }
button {
    display: block;
    width: 160dp;      /* hit target in dp: grows in touch-mode */
    padding: 18dp;
    margin: 14px 0;
    text-align: center;
    background-color: #3a4670;
    border-radius: 6px;
}
button:hover  { background-color: #4d5c91; }
button:active { background-color: #7e93e0; }
</style>
</head>
<body data-model="ui">
<h1>unbox ui demo</h1>
<button data-event-click="bump">press me</button>
<p>clicks: {{ clicks }}</p>
<p>last input: {{ mode }}</p>
</body>
</rml>)";

    kernel::UiSubstrate* ui_ = nullptr;            // borrow, session lifetime
    std::unique_ptr<kernel::UiSurface> surface_{}; // dies with the extension
    int clicks_ = 0;
};

[[nodiscard]] inline auto create_demo_ui() -> std::unique_ptr<kernel::Extension> {
    return std::make_unique<DemoUi>();
}

} // namespace unbox::host_bin
