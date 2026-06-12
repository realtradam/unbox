# no-translation-layers
GLES 3.2 is NATIVE here (Mesa crocus on HD 4400 — hardware-verified, see
notes/plan.md §1). NO ANGLE, no Vulkan (hasvk deprecated, not installed),
no GL-version downgrades "for speed" — it's the same driver either way.
Compositing: wlr GLES2 renderer. RMLUi: sibling GLES 3.2 context on the
shared EGLDisplay. Settled decision — do not relitigate.
