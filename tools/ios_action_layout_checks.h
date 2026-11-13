// SPDX-License-Identifier: GPL-2.0-or-later
// Runs only in the ROM-free UIKit fixture host.
#import "ios/action_layout.h"
#import "ios/input_hub.h"
#include <cmath>
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
