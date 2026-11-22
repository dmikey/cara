// SPDX-License-Identifier: GPL-2.0-or-later
// Use the validated folder and substream metadata of our pinned libarchive
// version.
#include "archive_read_support_format_7zip.c"
#include "cara_7zip_layout.h"

int cara_7zip_lzma2_layout(struct archive *reader,
                           struct cara_lzma2_layout *out) {
  struct archive_read *a = (struct archive_read *)reader;
  if (!a->format || archive_format(reader) != ARCHIVE_FORMAT_7ZIP)
    return 0;
  struct _7zip *zip = a->format->data;
  const struct _7zip_entry *e = zip->entry;
  if (!e || e->folderIndex >= zip->si.ci.numFolders ||
      e->ssIndex >= zip->si.ss.unpack_streams)
    return 0;
  struct _7z_folder *f = &zip->si.ci.folders[e->folderIndex];
  if (f->numCoders != 1 || f->numPackedStreams != 1 || f->numBindPairs ||
      f->packIndex >= zip->si.pi.numPackStreams ||
      f->coders[0].codec != _7Z_LZMA2 || f->coders[0].numInStreams != 1 ||
      f->coders[0].numOutStreams != 1 || f->coders[0].propertiesSize != 1 ||
      f->coders[0].properties[0] > 40)
    return 0;
  size_t first = 0;
  for (size_t i = 0; i < e->folderIndex; ++i)
    if (archive_ckd_add_size(&first, first,
                             zip->si.ci.folders[i].numUnpackStreams))
      return 0;
  if (first > e->ssIndex || e->ssIndex - first >= f->numUnpackStreams)
    return 0;
  int64_t member = 0;
  for (size_t i = first; i < e->ssIndex; ++i)
    if (zip->si.ss.unpackSizes[i] < 0 ||
        archive_ckd_add_i64(&member, member, zip->si.ss.unpackSizes[i]))
      return 0;
  int64_t start, size = folder_uncompressed_size(f);
  if (archive_ckd_add_i64(&start, zip->seek_base,
                          zip->si.pi.positions[f->packIndex]) ||
      start < 0 || size <= 0 || zip->entry_bytes_remaining <= 0 ||
      member > size - zip->entry_bytes_remaining ||
      zip->si.pi.sizes[f->packIndex] <= 0)
    return 0;
  out->pack_start = (uint64_t)start;
  out->pack_size = (uint64_t)zip->si.pi.sizes[f->packIndex];
  out->folder_size = (uint64_t)size;
  out->member_offset = (uint64_t)member;
  out->member_size = (uint64_t)zip->entry_bytes_remaining;
  out->properties = f->coders[0].properties[0];
  out->has_crc = !!(e->flg & CRC32_IS_SET);
  out->member_crc = out->has_crc ? zip->si.ss.digests[e->ssIndex] : 0;
  return 1;
}
