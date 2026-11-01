# Clash Royale Cache Eviction Investigation Report

## Issue Summary
Clash Royale logs `Clearing font cache` and `Stage::render again because font texture was full`, and exhibits repeated loading of `.sc` files. The investigation was directed towards missing font/text capabilities, GL limits, and asset loading correctness in HydrogenVM.

## Investigation Steps

### 1. GL Limits Reporting
Checked `src/runtime/uikit/OpenGLES.cpp` and `src/runtime/gl/HostGL.cpp`. `GL_MAX_TEXTURE_SIZE` is capped at 4096 (what iOS devices of this era report). This limit should be sufficient for texture atlases and doesn't appear misreported.

### 2. Font & Text Compatibility
The provided reports indicated missing CoreText and CoreGraphics APIs, most notably `CTLineGetOffsetForStringIndex`, `CTFontDescriptorCreateCopyWithAttributes`, and `CTFontDescriptorCreateWithAttributes`.
When a game relies on `CTLineGetOffsetForStringIndex` to measure glyph layouts, a missing implementation defaults to returning 0 or an invalid state, which may make a game's font layout engine treat its metrics as invalid and rebuild its font atlases (this is a hypothesis; it was not confirmed against Clash Royale itself). The FontDescriptor routines still ignore their `attrs` dictionary: attribute parsing and merging remain missing.

### 3. Asset Loading Correctness
The `url_getResourceValue` and file IO routines inside `Bundle.cpp` handle `NSURLIsDirectoryKey`, `NSURLFileSizeKey`, etc. properly by querying the host filesystem using `std::filesystem`. No incorrect filesystem behavior was observed.

## Resolution
Implemented `CTLineGetOffsetForStringIndex` in `src/runtime/uikit/CoreGraphics.cpp` returning a 32-bit `float` (the guest `CGFloat`, returned in r0; an earlier revision returned a `double`, which the guest read as the wrong value), by iterating over `d->advances` up to the requested string index, resolving the offset calculation and taking care of ranges. The secondary offset pointer is only written when all four bytes are guest-writable. The float return itself has no host test yet, because building a CTLine needs a real TrueType font fixture.
Also implemented `CTFontDescriptorCreateCopyWithAttributes` and `CTFontDescriptorCreateWithAttributes` with warning logs noting that full dictionary parsing is missing, returning baseline parsed font objects instead of fully merged descriptors.

## Note on Font-Cache Rebuild Fix Claim
No claims can be made to state that the cache rebuilds have been fixed because Clash Royale itself was skipped during test lab replays (due to missing device fixtures). The resolution currently focuses on restoring missing Core Text and CG capability according to best-guess indicators rather than confirming a strict fix inside the Clash runtime engine, which requires an actual device and IPA.
