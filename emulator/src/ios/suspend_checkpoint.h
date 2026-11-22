// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Foundation/Foundation.h>
#include <array>
#include <cstdint>
#include <cstring>

static NSString* const CaraSuspendSelection = @"suspended-session";

// Local recovery stays outside the manual-slot and cloud-sync directories.
inline NSURL* CaraSuspendCheckpointURL(NSURL* directory, NSData* gameHash,
                                       bool renderTiming, bool transferTiming) {
    if (![gameHash isKindOfClass:NSData.class] || gameHash.length != 32) return nil;
    const auto* hash = static_cast<const std::uint8_t*>(gameHash.bytes);
    std::array<char, 65> hex{};
    constexpr char digits[] = "0123456789abcdef";
    bool identified = false;
    for (unsigned i = 0; i < 32; ++i) {
        hex[2*i] = digits[hash[i] >> 4];
        hex[2*i+1] = digits[hash[i] & 15];
        identified |= hash[i] != 0;
    }
    if (!identified) return nil;
    return [directory URLByAppendingPathComponent:
        [NSString stringWithFormat:@"%s-%u%u.carastate", hex.data(),
                                    unsigned(renderTiming), unsigned(transferTiming)]];
}

// The header is only a library availability check. The core validates the
// entire archive, including title/ROM identity and checksum, before restoring.
inline NSDate* CaraCompatibleSuspendCheckpoint(NSURL* url, NSData* gameHash,
                                                 NSString* engine, bool new3DS,
                                                 unsigned backend) {
    if (!url || ![gameHash isKindOfClass:NSData.class] || gameHash.length != 32) return nil;
    NSDictionary* attributes = [url resourceValuesForKeys:
        @[NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey, NSURLContentModificationDateKey] error:nil];
    if (![attributes[NSURLIsRegularFileKey] boolValue] ||
        [attributes[NSURLIsSymbolicLinkKey] boolValue]) return nil;
    NSFileHandle* file = [NSFileHandle fileHandleForReadingFromURL:url error:nil];
    NSData* header = nil;
    std::uint64_t fileSize{};
    @try {
        header = [file readDataOfLength:256];
        fileSize = [file seekToEndOfFile];
    }
    @catch (NSException* exception) { return nil; }
    @finally { [file closeFile]; }
    if (header.length != 256) return nil;
    const auto* bytes = static_cast<const std::uint8_t*>(header.bytes);
    std::uint32_t version{}, headerSize{};
    std::uint64_t compressed{};
    std::memcpy(&version, bytes + 8, sizeof(version));
    std::memcpy(&headerSize, bytes + 12, sizeof(headerSize));
    std::memcpy(&compressed, bytes + 32, sizeof(compressed));
    if (std::memcmp(bytes, "CARAST01", 8) != 0 || version != 1 || headerSize != header.length ||
        fileSize <= headerSize || compressed != fileSize - headerSize) return nil;
    NSString* fingerprint = [[NSString alloc] initWithBytes:bytes + 48 length:64
                                                   encoding:NSASCIIStringEncoding];
    std::uint32_t mode{}, savedBackend{};
    std::memcpy(&mode, bytes + 144, sizeof(mode));
    std::memcpy(&savedBackend, bytes + 180, sizeof(savedBackend));
    return [fingerprint isEqualToString:engine] && mode == unsigned(new3DS) &&
        savedBackend == backend && std::memcmp(bytes + 148, gameHash.bytes, 32) == 0
        ? attributes[NSURLContentModificationDateKey] : nil;
}
