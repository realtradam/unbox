#pragma once

#include <unbox/kernel/extension.hpp>

#include <memory>

// Test-only probe surface (PRIVATE — src/, never part of the contract). The
// headless integration test needs to assert activate() actually ran on the
// concrete extension object; the public create() hides the type behind
// kernel::Extension. This factory hands back the same Extension plus a borrowed
// probe pointer the test can poll. Glue/shell test convenience only.

namespace unbox::ext_xdg_shell {

// A non-owning view onto the live extension for tests. Valid as long as the
// returned unique_ptr (and thus the extension) is alive.
class ActivationProbe {
public:
    virtual ~ActivationProbe() = default;
    // True once activate() completed (global created, hooks/subscriptions
    // wired, service registered).
    [[nodiscard]] virtual auto activated() const -> bool = 0;
};

struct ExtensionWithProbe {
    std::unique_ptr<unbox::kernel::Extension> extension; // install() this
    ActivationProbe* probe = nullptr;                    // borrow into the above
};

// Same extension as create(), but also yields a probe borrow.
[[nodiscard]] auto make_extension_with_probe() -> ExtensionWithProbe;

} // namespace unbox::ext_xdg_shell
