# Deferred Finding: stb_image.h RGB-to-Y Precision

## Issue Description
In `src/thirdparty/stb/stb_image.h`, there are two related TODO comments concerning the loading of 8-bit images when 16-bit Grayscale (Y) or Grayscale+Alpha (YA) is requested (`req_comp = 1` or `2`):
```c
   // @TODO: move stbi__convert_format16 to here
   // @TODO: special case RGB-to-Y (and RGBA-to-YA) for 8-bit-to-16-bit case to keep more precision
```

Currently, if an 8-bit RGB image (such as a PNG) is loaded via `stbi_load_16` with `req_comp=1`, the format-specific loader (e.g. `stbi__png_load`) performs the RGB-to-Y color conversion down to 1-channel *in 8-bit math* before returning. Then, `stbi__load_and_postprocess_16bit` scales that 8-bit Y channel to 16-bit.

This causes precision loss because the fractional bits from the RGB-to-Y matrix multiplication are truncated during the 8-bit `stbi__compute_y` step. If the 8-bit RGB channels were first scaled to 16-bit RGB, and *then* converted to 16-bit Y (using `stbi__convert_format16`), those fractional bits would be retained in the final 16-bit output.

## Synthetic Measurement
A simple C program demonstrates the difference for an RGB pixel `(100, 150, 200)`:
* **Method 1 (Current STB Image method):** 8-bit Y -> 16-bit Y
  `y_8 = (100 * 77 + 150 * 150 + 200 * 29) >> 8 = 140`
  `y_16 = (140 << 8) | 140 = 35980`
* **Method 2 (High Precision method):** 16-bit RGB -> 16-bit Y
  `r_16 = 25700, g_16 = 38550, b_16 = 51400`
  `y_16 = (25700 * 77 + 38550 * 150 + 51400 * 29) >> 8 = 36140`
* **Difference:** 160 units in the 16-bit color space.

## Reason for Deferral
While fixing the conversion order improves precision, an upstream-compatible patch would require one of the following:
1. **Passing `req_comp = 0` to `stbi__load_main`:** This forces all loaders to return native components, so we can scale to 16-bit first and then convert formats centrally. However, this breaks optimizations in loaders like JPEG, which normally skip decoding the Cb and Cr channels when `req_comp = 1`.
2. **Modifying Internal Loaders:** We could pass the `bpc` argument to loaders like `stbi__png_load`, allowing them to internally perform the 16-bit scaling before the color conversion (`stbi__convert_format_to_16`). This requires signature changes across many internal loader functions (BMP, PNG, TGA, PIC, GIF, PNM) and modifying vendored third-party code.

Because there is no known HydrogenVM game regression tied to this precision difference, and an ad-hoc fix risks altering JPEG streaming state and performance, this fix is deferred.

## Recommendation
Future work should prioritize raising this upstream in the `stb_image` repository. If a HydrogenVM game asset is later discovered to be visually corrupted by this precision loss, an upstream-first investigation with a targeted test fixture should be pursued before performing local speculative rewrites.
## Compilation Notes
During the initial attempt to implement the `bpc` plumbing across all loaders, a compilation failure occurred `ninja: build stopped: subcommand failed.`. The first actual error hidden in the output logs was `src/thirdparty/stb/stb_image.h:1317:19: error: ‘stbi__convert_format16’ was not declared in this scope`, due to `stbi__convert_format16` being used before its definition when trying to insert the new logic. This underscores the complexity of safely modifying large vendored headers.
