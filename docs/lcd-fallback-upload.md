# LCD fallback texture upload

The OpenGL renderer's `LoadFBToScreenInfo` first asks `AccelerateDisplay` for an existing GPU surface. Only a false result reaches the CPU-RAM texture upload corrected here. Successful accelerated display does not stage pixels or query/change unpack state. No guest instruction, event, clock or timing policy changes.

The GLES fallback previously inferred three or four bytes from `GL_RGB`/`GL_RGBA`, despite RGB565, RGB5A1 and RGBA4 using two-byte packed types. It also walked the source as tightly packed while retaining the guest row length for the tightly packed scratch upload. RGBA8 used a BGR-style swap that retained the wrong alpha position: guest memory is ABGR, so the GLES unsigned-byte upload requires full reversal to RGBA. RGB8 memory is BGR.

`gl_lcd_upload.h` stages each visible row using the guest byte stride. RGBA8 becomes RGBA, RGB8 becomes RGB, and the three packed16 formats preserve each little-endian guest word in host-native order for their matching GL packed type. Padding is skipped. LCD memory remains sideways; existing `DrawScreens` texture coordinates perform the rotation for both top and bottom displays.

The upload uses row length0 and alignment1 for tight GLES scratch rows. The desktop direct-memory upload retains the guest pixel row length and also uses alignment1. Both fallback paths clear unpack skips and unbind any unpack buffer while using client pointers, then restore the prior unpack buffer, row length, alignment, skips and texture binding. The old requirement that pixel stride be a multiple of4 is unnecessary with explicit alignment1. The existing requirement that byte stride be divisible by guest bytes-per-pixel remains before acceleration.

The helper rejects null source, unsupported format, zero dimensions, undersized stride/span, and size arithmetic overflow without changing the output on validation failure. This does not establish physical mapping safety for arbitrary guest framebuffer addresses or dimensions; the existing mapped-memory/allocated-texture contract remains. It does not change framebuffer selection, filling, flushing, stereo selection or rotation.

## Native byte oracle

From the repository root, choose a private output path:

```sh
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Iemulator/src tests/lcd_upload_self_test.cpp -o /tmp/cara-lcd-upload-self-test
/tmp/cara-lcd-upload-self-test
```

The arithmetic-only test passes 6,417,668 byte/field assertions under ASan and UBSan. It covers all65,536 codes in each packed16 format; distinguishable RGBA8 alpha/channel bytes; RGB8; tight and poisoned padded rows; odd upload widths not aligned to4; short spans and overflow rejection; and both400×240 top and320×240 bottom presentations using the established sideways texture-coordinate mapping. Source bytes must remain unchanged. No ROM, proprietary assets or user data are used.

The complete renderer translation unit also passes a private macOS syntax check using current native build dependencies. Existing unrelated ARM-interface switch and legacy source-encoding warnings remain. No GL context, actual texture readback, game capture, device run, speed result or Sonic causality is established by these checks. A selected-texture observer/game run is a separate proof step.

The layout rules follow the [OpenGL ES3 specification](https://registry.khronos.org/OpenGL/specs/es/3.0/es_spec_3.0.pdf), sections on pixel storage and packed pixel types: the packed unsigned-short types use one16-bit element per pixel; row length and alignment determine unpack row stepping. The reference guest channel/bit layouts are also defined in the existing `common/color.h` decoders.
