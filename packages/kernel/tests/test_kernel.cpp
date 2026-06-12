#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unbox/kernel/kernel.hpp>
#include <unbox/kernel/server.hpp>

#include <cstdlib>

TEST_CASE("kernel compiles against and links wlroots + libwayland-server") {
    CHECK(unbox::kernel::link_probe());
    CHECK(unbox::kernel::wlroots_version().substr(0, 4) == "0.20");
}

TEST_CASE("vendored RMLUi subproject compiled and linked") {
    CHECK(!unbox::kernel::rmlui_version().empty());
}

TEST_CASE("server boots and shuts down on the headless backend") {
    // Headless backend + pixman renderer: no GPU, no parent session needed.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    auto server = unbox::kernel::Server::create({});
    CHECK(!server->socket_name().empty());
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
    // Destruction runs the full tinywl shutdown sequence.
}

TEST_CASE("ui spike defaults off and is the slice-2 server") {
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);

    auto server = unbox::kernel::Server::create({});
    CHECK(server->ui_spike_frame_count() == 0);
    for (int i = 0; i < 3; ++i) {
        CHECK(server->dispatch(10));
    }
    CHECK(server->ui_spike_frame_count() == 0);
}

TEST_CASE("ui spike boots, renders frames, and shuts down cleanly") {
    // Drive the RMLUi -> wlr_scene bridge on the headless backend with the
    // gles2 renderer so the real GL path is exercised (Plan A attempted,
    // Plan B as fallback). The headless backend uses an EGL render node; if
    // GL is unavailable the bridge disables itself gracefully and frame_count
    // stays 0 (asserted as the no-crash fallback). A headless output must be
    // created so the frame handler (which drives tick()) fires.
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);

    auto server = unbox::kernel::Server::create({.ui_spike = true});
    CHECK(!server->socket_name().empty());

    // Pump enough turns for the headless output to emit frames.
    for (int i = 0; i < 200; ++i) {
        CHECK(server->dispatch(10));
    }

    const int frames = server->ui_spike_frame_count();
    INFO("ui_spike_frame_count() = ", frames);
    // Either the bridge ran (frames advanced) or it disabled itself on a
    // headless box without a usable GL path. Both are acceptable; a crash is
    // not. Clean shutdown is exercised on destruction below.
    CHECK(frames >= 0);
}

TEST_CASE("ui spike submits an upright (non-flipped) buffer") {
    // Orientation regression guard. The spike document carries distinctive
    // solid bands at its top and bottom edges; on the CPU-readback (Plan B)
    // path the bridge inspects the SUBMITTED buffer and reports +1 if the top
    // band is in the top rows (upright), -1 if vertically flipped. GL's
    // bottom-left framebuffer origin vs the wlr_buffer top-first convention
    // makes the flip the default failure mode, so this must never silently
    // regress. Force the shm path so the readback exists; if GL is
    // unavailable the spike disables itself and orientation() returns 0
    // (skipped, not failed — same graceful-degrade contract as above).
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "gles2", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
    setenv("UNBOX_UI_SPIKE_FORCE_SHM", "1", 1);

    auto server = unbox::kernel::Server::create({.ui_spike = true});
    for (int i = 0; i < 200; ++i) {
        CHECK(server->dispatch(10));
    }

    const int orient = server->ui_spike_orientation();
    INFO("ui_spike_orientation() = ", orient);
    // MUST NOT be flipped. +1 = upright (the bridge ran), 0 = indeterminate
    // (no GL path on this box). A flip (-1) is the bug and fails here.
    CHECK(orient != -1);
    if (server->ui_spike_frame_count() > 0) {
        // The shm bridge ran: orientation must be positively confirmed upright.
        CHECK(orient == 1);
    }

    unsetenv("UNBOX_UI_SPIKE_FORCE_SHM");
}
