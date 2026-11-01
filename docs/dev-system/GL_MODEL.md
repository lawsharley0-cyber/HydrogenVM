# OpenGL ES Context, Thread, and FBO Model

## 1. Overview and Architecture

HydrogenVM supports OpenGL ES 1.1 (fixed-function + OES extensions) and OpenGL ES 2.0 (programmable shaders, programs, uniforms, vertex attributes, VBOs, FBOs, and textures) across two distinct backends:
1. **Software Rasterizer (`Runtime::UIKit`)**: A CPU-based software rasterizer and state tracker compositing directly into `CAEAGLLayer::LayerData::externalContents`. Used in headless CLI runs, automated CI test suites, and environments without host GPU access.
2. **Host GPU Passthrough (`HostGL`)**: A high-performance bridge dispatching guest OpenGL ES commands to host graphics APIs (Metal via host bridge on Apple devices; host desktop OpenGL/GLES on Linux/macOS desktops).

Both backends adhere to the core Objective-C EAGL interfaces: `EAGLContext`, `EAGLSharegroup`, and `CAEAGLLayer`.

---

## 2. Thread Affinity and Context Binding

### Invariant Rules
1. **Single Context per Thread**:
   At most one `EAGLContext` may be active on any host/guest thread at any moment. Calling `+[EAGLContext setCurrentContext:ctx]` makes `ctx` active for the calling thread and unbinds any previously active context on that thread. Passing `nil` (0) unbinds the current context.

2. **Software Backend Exclusivity**:
   In the software rasterizer, an `EAGLContext` instance maintains an exclusive single-thread affinity lock (`activeThread_`).
   - If thread $A$ has bound context $C$ via `setCurrentContext:`, and thread $B$ calls `[EAGLContext setCurrentContext:C]`, the software backend must reject the bind, log `[warn] EAGLContext setCurrentContext: fails because ctx is in use on thread A`, and return `NO` (0).
   - Thread $A$ must explicitly call `+[EAGLContext setCurrentContext:nil]` or exit before context $C$ can be bound by thread $B$.

3. **Host GPU Transparent Shared Binding**:
   Certain multi-threaded titles (e.g. Clumsy Ninja) expect shared background resource upload or render threads. Under `HostGL`:
   - When a second thread attempts to bind a context belonging to an active sharegroup, the host platform layer transparently provisions a shared host context bound to that thread.
   - The guest-facing call returns `YES` (1) so the engine does not report a false bind failure.

4. **Thread Teardown**:
   When a guest thread terminates, any active `EAGLContext` reference in thread-local storage (`currentContext_`) must be detached without destroying the underlying context or mutating other threads' state.

---

## 3. Sharegroup and Resource Sharing

### Shared Resources
Per OpenGL ES 2.0 section 2.5, objects shared within an `EAGLSharegroup` include:
- **Textures (`glBindTexture`)**
- **Buffer Objects (`glBindBuffer`, VBOs, IBOs)**
- **Shaders and Programs (`glCreateShader`, `glCreateProgram`)**
- **Renderbuffers (`glBindRenderbuffer`)**

### Context-Private Objects (Not Shared)
- **Framebuffer Objects (`glBindFramebuffer`, FBOs)**: FBO container state is strictly context-private. Binding an FBO on context $C_1$ does not affect context $C_2$. However, the texture or renderbuffer attachments bound *to* that FBO are shared resources.
- **Vertex Array Objects (VAOs)**: Container state remains local to the binding context.
- **Current Pipeline & Transform State**: Active program, viewport, scissor, blend modes, depth/stencil tests, and bound texture units are thread/context local.

---

## 4. Framebuffer Objects (FBO) and CAEAGLLayer Lifecycle

### Default FBO vs Custom FBOs
- **FBO 0**: Represents the screen or default layer surface.
- **Custom FBOs ($> 0$)**: Allocated via `glGenFramebuffers`.

### Surface Attachment Flow
1. **Creation**:
   The app initializes a `CAEAGLLayer` on `UIView`, allocates an `EAGLContext`, sets it current, generates a renderbuffer (`glGenRenderbuffers`), and binds it (`glBindRenderbuffer(GL_RENDERBUFFER, rbo)`).
2. **Storage Allocation**:
   The app calls `-[EAGLContext renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer]`.
   - The runtime extracts dimensions and pixel format (`kEAGLColorFormatRGBA8` / `kEAGLColorFormatRGB565`) from the layer.
   - In software mode, memory is reserved in `LayerData::externalContents` matching layer width $\times$ height $\times$ bpp.
3. **FBO Attachment**:
   The app calls `glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo)`.
4. **Completeness Enforcement**:
   Calls to `glCheckFramebufferStatus(GL_FRAMEBUFFER)` must evaluate attachments:
   - Return `GL_FRAMEBUFFER_COMPLETE` (`0x8CD5`) if valid color, depth, or stencil attachment with non-zero dimensions exists.
   - Return `GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT` (`0x8CD7`) if no image is attached.
   - Return `GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS` (`0x8CD9`) if attached renderbuffers/textures differ in width/height.
   - Return `GL_FRAMEBUFFER_UNSUPPORTED` (`0x8CDD`) if internal formats violate device configuration.

### Presentation (`presentRenderbuffer:`)
When `-[EAGLContext presentRenderbuffer:GL_RENDERBUFFER]` is called:
- The completed color renderbuffer contents are committed to the compositor.
- Frame presented counters and histograms increment monotonically.
- Dirty rectangles are flushed to the host screen / virtual display.

---

## 5. Relaunch and Process Lifecycle Safety

1. **Guest Arena & Relaunch Boundaries**:
   During batch testing or relaunch on saved state:
   - All `HostGL` context handles and texture allocations must be purged.
   - Thread-local context pointers (`currentContext_`) must be cleared prior to releasing the guest process.
   - No dangling host GL context or stale TLS pointers may persist across `Process` instantiations.

2. **Telemetry Synchronization**:
   - `textureBytes`, VBO allocations, and frame counters must update under the global telemetry lock or using atomic primitives to prevent deadlocks and data races between host render threads and guest engine threads.

---

## Known deviations (software backend)

These differ from the invariants above today. They are listed so nobody relies on the stated rule where the
code does not enforce it yet.

- **Unbound threads still see a context.** After `+[EAGLContext setCurrentContext:nil]`, `ctx_current`/`currentCtx` in
  `OpenGLES.cpp` fall back to the last active context, so a thread that unbound can still issue GL calls.
- **Framebuffer objects are sharegroup-wide.** `SharegroupData::framebuffers` holds the container for every context in
  a sharegroup, so two shared contexts that bind the same framebuffer name modify the same attachments. In OpenGL ES
  framebuffer objects are not shared between contexts.
- **Completeness is tested on the software backend only.** `hvm_tests fulltest` and Test Lab `TL-R` run without
  `HostGL`; the host GPU passthrough relies on the host driver's own `glCheckFramebufferStatus`.

Completeness itself is enforced as documented: no attachment is `INCOMPLETE_MISSING_ATTACHMENT`, an attachment whose
object is missing or has no storage is `INCOMPLETE_ATTACHMENT` (0x8CD6), attachments of different sizes are
`INCOMPLETE_DIMENSIONS` (0x8CD9), detaching with object name 0 clears the attachment point, and
`glCheckFramebufferStatus` answers for the read or draw binding named by its target.
