# sanitizer-noise
build-asan must stay green WITHOUT blanket-disabling checks (never detect_leaks=0).
Third-party process-lifetime noise (Mesa/EGL/DRM driver globals; a vendored RmlUi
vptr downcast) goes in suppressions/ matched to those frames ONLY. A leak or UB
whose stack has an unbox:: frame is OURS — fix it, never suppress it.
