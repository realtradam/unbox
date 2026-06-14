#include "frame_driver.hpp"

namespace unbox::kernel {

FrameDriver::FrameDriver(std::function<void(ExtensionId)> disable) : disable_(std::move(disable)) {}

auto FrameDriver::add(std::function<void(double)> on_frame, ExtensionId who) -> FrameRequest {
    const Token token = ++next_token_;
    entries_.emplace(token, Entry{std::move(on_frame), who});
    return FrameRequest(this, token);
}

void FrameDriver::remove_frame_request(Token token) noexcept { entries_.erase(token); }

void FrameDriver::drain(double dt_seconds) {
    if (entries_.empty()) {
        return;
    }
    // Snapshot the live tokens so a callback that adds/removes requests (its own
    // or another's) mid-drain cannot invalidate iteration: a token added during
    // the drain is not in this snapshot (fires next frame), a token removed
    // during the drain is skipped via the re-lookup below (does not fire again).
    std::vector<Token> to_fire;
    to_fire.reserve(entries_.size());
    for (const auto& [token, e] : entries_) {
        to_fire.push_back(token);
    }
    for (const Token token : to_fire) {
        auto it = entries_.find(token);
        if (it == entries_.end()) {
            continue; // removed by an earlier callback this drain
        }
        // Copy what we need before invoking: the callback may remove this entry.
        std::function<void(double)> cb = it->second.on_frame;
        const ExtensionId who = it->second.who;
        if (!cb) {
            continue;
        }
        try {
            cb(dt_seconds);
        } catch (...) {
            // Same isolation boundary as a throwing hook/getter/file-watch:
            // disable the owning extension, never take down the loop/session.
            if (disable_) {
                disable_(who);
            }
        }
    }
}

} // namespace unbox::kernel
