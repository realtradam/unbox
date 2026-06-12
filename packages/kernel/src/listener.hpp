#pragma once

// The RAII wl_listener wrapper is now a PUBLIC contract type (slice 4):
// extensions do their own wlroots glue and must never hold a bare wl_listener.
// The kernel's own glue uses the very same type. This shim keeps the existing
// `#include "listener.hpp"` sites in src/ pointing at the public definition.
#include <unbox/kernel/listener.hpp>
