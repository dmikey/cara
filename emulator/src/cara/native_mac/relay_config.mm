// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#import <Foundation/Foundation.h>
#include "cara/native_mac/relay_config.h"
namespace NativeMac {
bool ParseRelayConfig(std::string_view input, Network::RelayClient::Config& config) {
    @autoreleasepool {
        if (input.empty() || input.size() > 16384) return false;
        NSData* data=[NSData dataWithBytes:input.data() length:input.size()];
        id value=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if (![value isKindOfClass:NSDictionary.class]) return false;
        NSDictionary* object=value;
        for (id key in object) if (![@[@"endpoint", @"room", @"password"] containsObject:key]) return false;
        NSString* endpoint=object[@"endpoint"] ?: @"wss://carap2p.therad.ninja/relay";
        NSString* room=object[@"room"], *password=object[@"password"];
        if (![endpoint isKindOfClass:NSString.class] || ![room isKindOfClass:NSString.class] || ![password isKindOfClass:NSString.class]) return false;
        NSURLComponents* url=[NSURLComponents componentsWithString:endpoint];
        if (![url.scheme.lowercaseString isEqual:@"wss"] || !url.host.length || !url.URL ||
            url.user!=nil || url.password!=nil || url.query!=nil || url.fragment!=nil || ![url.path isEqual:@"/relay"]) return false;
        const auto room_size=[room lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        const auto password_size=[password lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
        if (!room_size || room_size>64 || !password_size || password_size>512) return false;
        // UTF8String would truncate embedded NUL; reject it before conversion.
        const unichar zero=0;
        NSString* nul=[NSString stringWithCharacters:&zero length:1];
        for (NSString* field in @[endpoint,room,password]) if ([field rangeOfString:nul].location!=NSNotFound) return false;
        config.endpoint=endpoint.UTF8String;config.room=room.UTF8String;config.password=password.UTF8String;
        return true;
    }
}
}
