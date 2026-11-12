// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/configuration_store.h"
#import "ios/skin_layout.h"
#include <cmath>

static NSError* ConfigurationError(NSString* text) { return [NSError errorWithDomain:@"CaraConfiguration" code:1 userInfo:@{NSLocalizedDescriptionKey:text}]; }
static NSArray* PortableKeys() { return @[@"controllerBindings",@"keyboardBindings",@"portraitStatsVisible",@"landscapeStatsVisible",@"portraitSkinOpacity",@"landscapeSkinOpacity",@"portraitHaptics",@"landscapeHaptics"]; }
static BOOL ValidPreferences(NSDictionary* values) {
    if (![values isKindOfClass:NSDictionary.class] || values.count>PortableKeys().count) return NO;
    NSSet* actions=[NSSet setWithArray:@[@"a",@"b",@"x",@"y",@"l",@"r",@"zl",@"zr",@"start",@"select",@"up",@"down",@"left",@"right",@"circleUp",@"circleDown",@"circleLeft",@"circleRight",@"cstickUp",@"cstickDown",@"cstickLeft",@"cstickRight",@"menu",@"quickSave",@"quickLoad",@"fastForward"]];
    for (NSString* key in values) {
        if (![PortableKeys() containsObject:key]) return NO;
        id value=values[key];
        if ([key hasSuffix:@"Bindings"]) {
            if (![value isKindOfClass:NSDictionary.class] || [value count]>96) return NO;
            for (id input in value) if (![input isKindOfClass:NSString.class] || [input length]>32 || ![actions containsObject:value[input]]) return NO;
        } else {
            if (![value isKindOfClass:NSNumber.class]) return NO;
            const double number=[value doubleValue];
            if (!std::isfinite(number)) return NO;
            if ([key hasSuffix:@"Opacity"]) { if (number<.2 || number>1) return NO; }
            else if (number!=0 && number!=1) return NO;
        }
    }
    return YES;
}
@implementation CaraConfigurationStore {
    NSURL* documents;
    NSURL* root;
    NSUserDefaults* preferences;
}
- (instancetype)initWithDocuments:(NSURL*)url defaults:(NSUserDefaults*)defaults {
    if ((self=[super init])) { documents=url; root=[url URLByAppendingPathComponent:@"cara/settings" isDirectory:YES]; preferences=defaults; }
    return self;
}
- (BOOL)exportPreferencesChanged:(BOOL*)changed error:(NSError**)error {
    if (changed) *changed=NO;
    if (![NSFileManager.defaultManager createDirectoryAtURL:[root URLByAppendingPathComponent:@"skins"] withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    NSMutableDictionary* values=[NSMutableDictionary new];
    for (NSString* key in PortableKeys()) { id value=[preferences objectForKey:key]; if (value) values[key]=value; }
    if (!ValidPreferences(values)) { if (error) *error=ConfigurationError(@"Some controller settings are invalid. Reset their mappings and try again."); return NO; }
    NSURL* currentFile=[root URLByAppendingPathComponent:@"controller-preferences.json"];
    const BOOL customSkin=[preferences stringForKey:@"controllerSkinPortrait"].length || [preferences stringForKey:@"controllerSkinLandscape"].length;
    if (!values.count && !customSkin && ![NSFileManager.defaultManager fileExistsAtPath:currentFile.path]) return YES;
    NSMutableDictionary* skins=[NSMutableDictionary new];
    for (NSString* orientation in @[@"Portrait",@"Landscape"]) {
        NSString* filename=[preferences stringForKey:[@"controllerSkin" stringByAppendingString:orientation]];
        if (!filename.length) { skins[orientation]=@"builtin"; continue; }
        NSURL* source=[documents URLByAppendingPathComponent:filename];
        if (![source.URLByResolvingSymlinksInPath.URLByStandardizingPath.path hasPrefix:[documents.URLByResolvingSymlinksInPath.URLByStandardizingPath.path stringByAppendingString:@"/"]]) { if (error) *error=ConfigurationError(@"A skin must be stored inside Cara before it can sync."); return NO; }
        NSNumber* size=nil; [source getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
        if (!size || size.unsignedLongLongValue>32ull*1024*1024) { if (error) *error=ConfigurationError(@"A controller skin is missing or too large to sync."); return NO; }
        NSData* bytes=[NSData dataWithContentsOfURL:source options:0 error:error]; if (!bytes) return NO;
        NSURL* destination=[root URLByAppendingPathComponent:[NSString stringWithFormat:@"skins/%@.deltaskin",orientation.lowercaseString]];
        NSData* previous=[NSData dataWithContentsOfURL:destination];
        if (![bytes isEqual:previous]) { if (![bytes writeToURL:destination options:NSDataWritingAtomic error:error]) return NO; if (changed) *changed=YES; }
        skins[orientation]=@"custom";
    }
    NSDictionary* payload=@{@"version":@1,@"preferences":values,@"skins":skins};
    NSURL* file=[root URLByAppendingPathComponent:@"controller-preferences.json"];
    NSData* bytes=[NSJSONSerialization dataWithJSONObject:payload options:NSJSONWritingSortedKeys error:error]; if (!bytes) return NO;
    if (![bytes isEqual:[NSData dataWithContentsOfURL:file]]) { if (![bytes writeToURL:file options:NSDataWritingAtomic error:error]) return NO; if (changed) *changed=YES; }
    return YES;
}
- (BOOL)applyPreferences:(NSError**)error {
    NSURL* file=[root URLByAppendingPathComponent:@"controller-preferences.json"];
    if (![NSFileManager.defaultManager fileExistsAtPath:file.path]) return YES;
    NSNumber* size=nil; [file getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    if (!size || size.unsignedLongLongValue>2ull*1024*1024) { if (error) *error=ConfigurationError(@"Synced controller settings are too large. Local preferences were kept."); return NO; }
    NSData* bytes=[NSData dataWithContentsOfURL:file options:0 error:error];
    NSDictionary* payload=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:error]:nil;
    if (![payload isKindOfClass:NSDictionary.class] || ![payload[@"version"] isEqual:@1] || !ValidPreferences(payload[@"preferences"]) || ![payload[@"skins"] isKindOfClass:NSDictionary.class]) { if (error) *error=ConfigurationError(@"Synced controller settings are invalid. Local preferences were kept."); return NO; }
    NSMutableDictionary* selected=[NSMutableDictionary new];
    for (NSString* orientation in @[@"Portrait",@"Landscape"]) {
        NSString* choice=payload[@"skins"][orientation];
        if (![@[@"builtin",@"custom"] containsObject:choice]) { if (error) *error=ConfigurationError(@"A synced skin choice is invalid. Local preferences were kept."); return NO; }
        if ([choice isEqual:@"custom"]) {
            NSString* relative=[NSString stringWithFormat:@"cara/settings/skins/%@.deltaskin",orientation.lowercaseString];
            CaraSkin* skin=[CaraSkin loadURL:[documents URLByAppendingPathComponent:relative] error:error];
            if (![skin strictRepresentationForPad:YES landscape:[orientation isEqual:@"Landscape"]] && ![skin strictRepresentationForPad:NO landscape:[orientation isEqual:@"Landscape"]]) { if (error && !*error) *error=ConfigurationError(@"A synced skin has no matching orientation. Local preferences were kept."); return NO; }
            selected[orientation]=relative;
        }
    }
    // Validate the entire document and both archives before changing any key.
    for (NSString* key in PortableKeys()) {
        id value=payload[@"preferences"][key];
        if (value) [preferences setObject:value forKey:key]; else [preferences removeObjectForKey:key];
    }
    for (NSString* orientation in @[@"Portrait",@"Landscape"]) {
        NSString* key=[@"controllerSkin" stringByAppendingString:orientation];
        if (selected[orientation]) [preferences setObject:selected[orientation] forKey:key]; else [preferences removeObjectForKey:key];
    }
    [preferences setBool:YES forKey:@"orientationSkinPreferencesMigrated"];
    return YES;
}
@end
