// SPDX-License-Identifier: GPL-2.0-or-later
// Runs only in the ROM-free UIKit fixture host.
#import "ios/action_layout.h"
#import "ios/input_hub.h"
#include <cmath>
#include <array>
// Drive the real editor state transition while keeping this fixture's Save out
// of the user's persistent Application Support directory. Registration itself
// is exercised below against a temporary directory.
@interface CaraActionLayoutEditor (RegressionAccess)
- (void)update:(NSDictionary*)changes;
- (void)save;
@end
@interface CaraActionTestEditor : CaraActionLayoutEditor
@end
@implementation CaraActionTestEditor
- (BOOL)registerDraft:(NSError**)error { return YES; }
@end
static void ActionCheck(BOOL ok,NSString* label) { if (!ok) @throw [NSException exceptionWithName:@"ActionLayoutRegression" reason:label userInfo:nil]; }
static void CheckActionLayout() {
    NSDictionary* starter=CaraNewActionLayout(); ActionCheck(CaraValidateActionLayout(starter,nil),@"starter validates");
    NSMutableDictionary* invalid=[starter mutableCopy]; invalid[@"version"]=@99; ActionCheck(!CaraValidateActionLayout(invalid,nil),@"future version rejected");
    invalid=[starter mutableCopy]; invalid[@"portrait"]=@[@{}]; ActionCheck(!CaraValidateActionLayout(invalid,nil),@"missing fields rejected");
    invalid=[starter mutableCopy]; invalid[@"portrait"]=@[@"malformed"]; ActionCheck(!CaraValidateActionLayout(invalid,nil),@"wrong types rejected");
    NSDictionary* source=starter[@"portrait"][0];
    invalid=[starter mutableCopy]; invalid[@"portrait"]=@[source,source]; ActionCheck(!CaraValidateActionLayout(invalid,nil),@"duplicate owners rejected");
    NSMutableDictionary* c=[source mutableCopy]; c[@"kind"]=@"button"; c[@"binding"]=@"a"; c[@"frame"]=@{@"x":@.1,@"y":@.1,@"width":@.2,@"height":@.2};
    invalid=[starter mutableCopy]; invalid[@"portrait"]=@[c]; ActionCheck(CaraValidateActionLayout(invalid,nil),@"explicit binding validates");
    NSMutableDictionary* malformed=[c mutableCopy]; malformed[@"frame"]=@{@"x":@0,@"y":@0,@"width":@{},@"height":@.2};
    NSMutableDictionary* badFrame=[invalid mutableCopy]; badFrame[@"portrait"]=@[malformed]; ActionCheck(!CaraValidateActionLayout(badFrame,nil),@"nested malformed frame rejected without throwing");
    malformed=[c mutableCopy]; malformed[@"hidden"]=@"yes"; badFrame[@"portrait"]=@[malformed]; ActionCheck(!CaraValidateActionLayout(badFrame,nil),@"wrong hidden flag rejected");
    malformed=[c mutableCopy]; malformed[@"travel"]=@0; badFrame[@"portrait"]=@[malformed]; ActionCheck(!CaraValidateActionLayout(badFrame,nil),@"invalid analog travel rejected");
    NSData* json=[NSJSONSerialization dataWithJSONObject:invalid options:0 error:nil]; ActionCheck(CaraValidateActionLayout([NSJSONSerialization JSONObjectWithData:json options:0 error:nil],nil),@"JSON round trip");
    CaraActionControls* view=[[CaraActionControls alloc] initWithFrame:CGRectMake(0,0,1000,1000)];
    CaraInputHub* hub=[[CaraInputHub alloc] initWithDefaults:NSUserDefaults.standardUserDefaults observesDevices:NO]; hub.active=YES;
    __block NSSet* buttons; __block CGPoint circle,stick;
    hub.inputChanged=^(NSSet* b,CGPoint left,CGPoint right) { buttons=b; circle=left; stick=right; };
    view.inputChanged=^(NSSet* b,CGPoint left,CGPoint right) { [hub updateSurface:@"actions" buttons:b circle:left cstick:right touch:CGPointMake(-1,-1)]; };
    NSValue* one=[NSValue valueWithPointer:(void*)1],*two=[NSValue valueWithPointer:(void*)2];
    view.controls=@[c]; [view updateContact:one atPoint:CGPointMake(200,200)]; ActionCheck([buttons containsObject:@"a"],@"bound action reaches existing hub");
    [view updateContact:two atPoint:CGPointMake(200,200)]; [view endContact:one]; ActionCheck(buttons.count==0,@"second contact cannot own held control"); [view updateContact:two atPoint:CGPointMake(210,200)]; ActionCheck(buttons.count==0,@"rejected contact stays rejected until lift"); [view endContact:two];
    [view updateContact:one atPoint:CGPointMake(200,200)]; [view updateContact:one atPoint:CGPointMake(900,900)]; ActionCheck(buttons.count==0,@"hold releases outside"); [view updateContact:one atPoint:CGPointMake(200,200)]; ActionCheck([buttons containsObject:@"a"],@"hold can return"); [view endContact:one];
    NSMutableDictionary* combo=[c mutableCopy]; combo[@"bindings"]=@[@"a",@"r"];
    NSMutableDictionary* comboLayout=[starter mutableCopy]; comboLayout[@"portrait"]=@[combo];
    ActionCheck(CaraValidateActionLayout(comboLayout,nil),@"simultaneous combo validates");
    NSMutableDictionary* badCombo=[combo mutableCopy]; badCombo[@"bindings"]=@[@"a",@"a"];
    NSMutableDictionary* rejectedLayout=[starter mutableCopy]; rejectedLayout[@"portrait"]=@[badCombo];
    ActionCheck(!CaraValidateActionLayout(rejectedLayout,nil),@"duplicate combo inputs rejected");
    badCombo=[combo mutableCopy]; badCombo[@"bindings"]=@[@"invalid"];
    rejectedLayout[@"portrait"]=@[badCombo]; ActionCheck(!CaraValidateActionLayout(rejectedLayout,nil),@"unknown combo input rejected");
    view.controls=@[combo]; [view updateContact:one atPoint:CGPointMake(200,200)];
    ActionCheck([buttons isEqualToSet:[NSSet setWithArray:@[@"a",@"r"]]],@"one glyph holds both inputs");
    [view endContact:one]; ActionCheck(buttons.count==0,@"lifting releases whole combo");
    combo[@"toggle"]=@YES; view.controls=@[combo]; [view updateContact:one atPoint:CGPointMake(200,200)]; [view endContact:one];
    ActionCheck(buttons.count==2,@"combo toggle latches both inputs"); [view reset]; ActionCheck(buttons.count==0,@"reset releases whole toggled combo");
    // Test the UIKit exporter through the real Delta reader, not JSON alone.
    NSData* package=CaraActionSkinData(comboLayout,nil); ActionCheck(package.length>0,@"enhanced skin exported");
    NSURL* packageURL=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:@"combo-roundtrip.deltaskin"];
    ActionCheck([package writeToURL:packageURL atomically:YES],@"enhanced archive written");
    CaraSkin* roundtrip=[CaraSkin loadURL:packageURL error:nil];
    UIImage* transparent=[UIImage imageWithData:roundtrip.assets[@"transparent.png"]];
    ActionCheck(transparent.CGImage && CGImageGetWidth(transparent.CGImage)==2 && CGImageGetHeight(transparent.CGImage)==2,@"transparent backing has fixed pixel dimensions");
    std::array<unsigned char,16> transparentPixels{};
    CGColorSpaceRef colors=CGColorSpaceCreateDeviceRGB();
    CGContextRef context=CGBitmapContextCreate(transparentPixels.data(),2,2,8,8,colors,CGBitmapInfo(kCGImageAlphaPremultipliedLast)|CGBitmapInfo(kCGBitmapByteOrder32Big));
    ActionCheck(context!=nullptr,@"transparent backing decode context");
    CGContextDrawImage(context,CGRectMake(0,0,2,2),transparent.CGImage); CGContextRelease(context); CGColorSpaceRelease(colors);
    for(unsigned char pixel:transparentPixels) ActionCheck(pixel==0,@"exported backing pixels remain fully transparent");
    NSDictionary* decoded=CaraActionLayoutFromSkin(roundtrip,nil);
    ActionCheck(decoded!=nil && [decoded[@"portrait"][0][@"bindings"] isEqual:@[@"a",@"r"]],@"enhanced Delta combo round trip");
    [NSFileManager.defaultManager removeItemAtURL:packageURL error:nil];
    NSURL* registrationFolder=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    NSDictionary* registered=CaraRegisterActionLayout(comboLayout,registrationFolder,nil);
    ActionCheck(registered[@"registrationID"]!=nil,@"authored skin registration gets a UUID");
    NSURL* registeredURL=[registrationFolder URLByAppendingPathComponent:[registered[@"registrationID"] stringByAppendingPathExtension:@"deltaskin"]];
    NSData* originalRegistration=[NSData dataWithContentsOfURL:registeredURL];
    NSMutableDictionary* independent=[registered mutableCopy]; [independent removeObjectForKey:@"registrationID"]; independent[@"name"]=@"Independent copy";
    NSDictionary* registeredCopy=CaraRegisterActionLayout(independent,registrationFolder,nil);
    ActionCheck(registeredCopy && ![registeredCopy[@"registrationID"] isEqual:registered[@"registrationID"]],@"copied/imported skin gets independent registration");
    ActionCheck([[NSData dataWithContentsOfURL:registeredURL] isEqual:originalRegistration],@"copy registration preserves original package");
    ActionCheck([NSFileManager.defaultManager contentsOfDirectoryAtURL:registrationFolder includingPropertiesForKeys:nil options:0 error:nil].count==2,@"both registered packages retained");
    NSMutableDictionary* traversal=[comboLayout mutableCopy]; traversal[@"registrationID"]=@"../escape";
    ActionCheck(CaraRegisterActionLayout(traversal,registrationFolder,nil)==nil,@"registration rejects path-like identifiers");
    [NSFileManager.defaultManager removeItemAtURL:registrationFolder error:nil];
    NSMutableDictionary* legacy=[starter mutableCopy]; legacy[@"version"]=@1;
    ActionCheck(CaraValidateActionLayout(legacy,nil),@"legacy v1 layout remains valid");
    ActionCheck(CGRectIsEmpty(CaraActionFrame(c,CGSizeZero)),@"zero-sized canvas does not produce invalid geometry");
    CaraActionTestEditor* editor=[CaraActionTestEditor new]; editor.layout=comboLayout;
    __block NSDictionary* edited=nil; editor.saved=^(NSDictionary* saved) { edited=saved; };
    [editor loadViewIfNeeded];
    [editor update:@{@"kind":@"button",@"binding":@"unbound",@"toggle":@NO}];
    [editor save];
    ActionCheck(edited && [CaraActionBindings(edited[@"portrait"][0]) count]==0,@"changing control kind clears stale combo");
    ActionCheck([edited[@"landscape"] isEqual:comboLayout[@"landscape"]],@"portrait edit preserves independent landscape");
    view.controls=@[c];
    c=[c mutableCopy]; c[@"toggle"]=@YES; view.controls=@[c]; [view updateContact:one atPoint:CGPointMake(200,200)]; [view endContact:one]; ActionCheck([buttons containsObject:@"a"],@"toggle latches after lift");
    [view updateContact:one atPoint:CGPointMake(200,200)]; [view endContact:one]; ActionCheck(buttons.count==0,@"second tap unlatches toggle");
    [view updateContact:one atPoint:CGPointMake(200,200)]; [view touchesCancelled:[NSSet set] withEvent:nil]; ActionCheck(buttons.count==0,@"cancellation releases toggle"); [view endContact:one];
    [view updateContact:one atPoint:CGPointMake(200,200)]; [NSNotificationCenter.defaultCenter postNotificationName:UIApplicationWillResignActiveNotification object:nil]; ActionCheck(buttons.count==0,@"background releases toggle"); [view updateContact:one atPoint:CGPointMake(200,200)]; ActionCheck(buttons.count==0,@"reset contact quarantined until lift"); [view endContact:one];
    c=[c mutableCopy]; c[@"toggle"]=@NO; c[@"kind"]=@"floating"; c[@"binding"]=@"circle"; view.controls=@[c];
    [view updateContact:one atPoint:CGPointMake(150,150)]; ActionCheck(CGPointEqualToPoint(circle,CGPointZero),@"floating begins at neutral"); [view updateContact:one atPoint:CGPointMake(250,150)]; ActionCheck(circle.x>.99,@"floating analog follows owned touch"); [view endContact:one]; ActionCheck(CGPointEqualToPoint(circle,CGPointZero),@"analog lift centers");
    c=[c mutableCopy]; c[@"hidden"]=@YES; c[@"travel"]=@32; view.controls=@[c];
    [view updateContact:one atPoint:CGPointMake(150,150)]; [view updateContact:one atPoint:CGPointMake(182,150)]; ActionCheck(circle.x>.99,@"invisible floating zone has independent travel"); [view endContact:one];
    c=[c mutableCopy]; c[@"kind"]=@"stick"; c[@"binding"]=@"cstick"; view.controls=@[c]; [view updateContact:one atPoint:CGPointMake(299.9,200)]; ActionCheck(stick.x>.99,@"fixed stick binding"); [view endContact:one];
    c=[c mutableCopy]; c[@"kind"]=@"dpad"; c[@"binding"]=@"dpad"; view.controls=@[c]; [view updateContact:one atPoint:CGPointMake(200,110)]; ActionCheck([buttons containsObject:@"up"],@"digital dpad binding"); [view endContact:one];
    c=[c mutableCopy]; c[@"binding"]=@"circle"; view.controls=@[c]; [view updateContact:one atPoint:CGPointMake(280,120)]; ActionCheck(circle.x>.70 && circle.y>.70 && std::hypot(circle.x,circle.y)<1.001,@"analog D-pad diagonals stay in unit range"); [view endContact:one];
    c=[c mutableCopy]; c[@"kind"]=@"drag"; c[@"binding"]=@"cstick"; view.controls=@[c]; [view updateContact:one atPoint:CGPointMake(120,120)]; [view updateContact:one atPoint:CGPointMake(120,220)]; ActionCheck(stick.y<-.99,@"drag zone uses displacement"); [view endContact:one];
    c=[c mutableCopy]; c[@"kind"]=@"button"; c[@"binding"]=@"a"; view.controls=@[c];
    NSMutableDictionary* second=[c mutableCopy]; second[@"id"]=@"second"; second[@"binding"]=@"b"; second[@"frame"]=@{@"x":@.5,@"y":@.5,@"width":@.2,@"height":@.2}; view.controls=@[c,second];
    [view updateContact:one atPoint:CGPointMake(200,200)]; [view updateContact:two atPoint:CGPointMake(600,600)]; ActionCheck([buttons containsObject:@"a"] && [buttons containsObject:@"b"],@"independent multi-touch owners"); [view endContact:one]; ActionCheck(![buttons containsObject:@"a"] && [buttons containsObject:@"b"],@"one owner release retains other"); [view reset]; [view endContact:two];
    [hub updateSurface:@"skin" buttons:[NSSet set] circle:CGPointZero cstick:CGPointZero touch:CGPointMake(123,100)];
    [view updateContact:one atPoint:CGPointMake(200,200)]; ActionCheck([buttons containsObject:@"a"] && hub.touchPoint.x==123,@"touchscreen and action merge");
    hub.active=NO; ActionCheck(buttons.count==0 && hub.touchPoint.x<0,@"pause and stop clear all surface inputs"); [view reset]; [view endContact:one]; hub.active=YES; ActionCheck(buttons.count==0,@"resume does not revive surfaces");
    NSLog(@"PASS: action layout schema, JSON, ownership, hold/toggle, fixed/floating/dpad/drag, multi-touch, lifecycle release and existing input hub");
}
