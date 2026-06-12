# listener-lifetime
Never a bare `wl_listener` or raw callback across a unit boundary. Subscribe
through the bus and keep the returned RAII subscription handle as a member —
destruction unsubscribes. Raw pointers received in a hook are borrows: valid
ONLY during the call, never stored. Use-after-free across contracts is this
project's #1 expected bug class; the type system is the defense.
