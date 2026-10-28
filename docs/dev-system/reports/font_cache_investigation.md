# Clash Royale Cache Eviction Investigation Report

## Issue Summary
Clash Royale logs `Clearing font cache` and `Stage::render again because font texture was full`, and exhibits repeated loading of `.sc` files. The investigation was directed towards missing font/text capabilities, GL limits, and asset loading correctness in HydrogenVM.

## Investigation Steps

### 1. GL Limits Reporting
Checked `src/runtime/uikit/OpenGLES.cpp` and `src/runtime/gl/HostGL.cpp`. `GL_MAX_TEXTURE_SIZE` is capped at 4096 (what iOS devices of this era report). This limit should be sufficient for texture atlases.

### 2. Font & Text Compatibility (Hypothesis — Not Reproduced)
The provided reports indicated missing CoreText and CoreGraphics APIs, most notably `CTLineGetOffsetForStringIndex`, `CTFontDescriptorCreateCopyWithAttributes`, and `CTFontDescriptorCreateWithAttributes`.
These missing APIs may contribute to layout errors, but the provided reports do not establish that they cause font-atlas eviction or repeated `.sc` loading. A sustained Clash Royale reproduction with before/after telemetry is required to test that hypothesis.

### 3. Asset Loading Correctness
The `url_getResourceValue` and file IO routines inside `Bundle.cpp` handle `NSURLIsDirectoryKey`, `NSURLFileSizeKey`, etc. properly by querying the host filesystem using `std::filesystem`. No incorrect filesystem behavior was observed.

## Resolution
Added a bounded `CTLineGetOffsetForStringIndex` implementation for the runtime's simple left-to-right per-character advance model, with ARMv7 `CGFloat` ABI coverage. The descriptor attribute constructors are presently incomplete stubs: they do not preserve and merge arbitrary CFDictionary attributes, and must not be represented as fully working APIs. Complex shaping, bidi boundaries, and ligatures require further independent validation.

## Telemetry
No gameplay telemetry has been collected to measure changes in atlas reconstruction, memory use, or `.sc` loading. No arbitrary filesystem caches were added, and no GL limits were arbitrarily expanded without basis.

## Validation
Guest regressions and the independently built Test Lab must be run against a clean head for review. Earlier compatibility reports include baseline or dirty-worktree runs and must not be used as proof that Clash Royale's font cache is fixed.
