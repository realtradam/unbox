# test-determinism
Headless integration tests that drive a real Wayland client MUST advance it by
CONDITION-BASED cooperative event-loop pumping (round-trip / dispatch until the
expected state is observed, with a bounded spin) — never a free-running client
thread + sleep/time budget. The latter starves under load and flakes: assertions
read sentinels or the case silently skips, so the doctest assertion COUNT drifts.
A correct test has an identical assertion count on every run.
