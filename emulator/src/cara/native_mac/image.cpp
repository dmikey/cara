// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// SPDX-License-Identifier: GPL-2.0-or-later
// Native developer host uses installed libpng; old lodepng dependency is absent.
#include <png.h>
#include "cara/lodepng_image_interface.h"
#include "common/logging/log.h"
bool LodePNGImageInterface::DecodePNG(std::vector<u8>& dst, u32& width, u32& height,
                                      const std::string& path) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str()))
        return false;
    image.format = PNG_FORMAT_RGBA;
    dst.resize(PNG_IMAGE_SIZE(image));
    const bool okay = png_image_finish_read(&image, nullptr, dst.data(), 0, nullptr) != 0;
    if (okay) {
        width = image.width;
        height = image.height;
    } else {
        LOG_ERROR(Frontend, "PNG decode failed: {}", image.message);
        dst.clear();
    }
    png_image_free(&image);
    return okay;
}
bool LodePNGImageInterface::EncodePNG(const std::string& path, const std::vector<u8>& src,
                                      u32 width, u32 height) {
    if (!width || !height || u64(width) * height > src.size() / 4)
        return false;
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    const bool okay = png_image_write_to_file(&image, path.c_str(), 0, src.data(), 0, nullptr) != 0;
    if (!okay)
        LOG_ERROR(Frontend, "PNG encode failed: {}", image.message);
    png_image_free(&image);
    return okay;
}
