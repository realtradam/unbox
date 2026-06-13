# spawn-env
Any process spawned to run a Wayland client (launcher, terminal, …) MUST get the
compositor's own WAYLAND_DISPLAY. The kernel setenv()s it at startup; don't build
a child env that drops it, and don't trust the inherited parent env (it points at
the session that launched unbox). Scar: spawned fuzzel hit parent labwc → "no monitors".
