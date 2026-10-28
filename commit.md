This patch resolves a memory leak observed in HostGL-accelerated guests, specifically when navigating screen transitions (such as battles) where `EAGLSharegroup` instances are created and torn down dynamically.

Previously, `EAGLSharegroup` destruction was unmapped and its underlying resources managed by implicit worker contexts bypassed native tracking, meaning contexts were left lingering in `st.workers` and `st.shareRoot` referencing OpenGL data long after the guest object expired.

This implements:
- Correct mapping of `EAGLSharegroup::dealloc` back to `HostGL::sharegroupDestroy`.
- A resource telemetry tracking structure mapping creation bounds directly per sharegroup (tracking Texture dimensions, sizes, and buffers limits).
- Safe concurrent worker removal and TLS cleanup ensuring resources are strictly bounded and deterministically swept.
