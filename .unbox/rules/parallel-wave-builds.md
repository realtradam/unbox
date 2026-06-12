# parallel-wave-builds
During a parallel wave the shared `build/` may transiently fail to
configure while sibling units land their meson.build/files. Build YOUR
targets, retry once after a pause, and NEVER report a sibling's
mid-flight compile/configure state as a blocker or change-request.
