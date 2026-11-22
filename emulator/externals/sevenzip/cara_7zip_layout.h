// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct archive;
struct cara_lzma2_layout {
  uint64_t pack_start, pack_size, folder_size, member_offset, member_size;
  uint32_t member_crc;
  uint8_t has_crc, properties;
};
// Only a single raw LZMA2 coder is eligible. Filtered, encrypted and other
// folders return 0.
int cara_7zip_lzma2_layout(struct archive *reader,
                           struct cara_lzma2_layout *layout);
#ifdef __cplusplus
}
#endif
