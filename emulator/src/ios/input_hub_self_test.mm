// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/input_hub.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
@interface InputTestDefaults : NSUserDefaults
@property(strong,nonatomic) NSMutableDictionary* values;
@end
@implementation InputTestDefaults
- (instancetype)init { if ((self=[super init])) _values=[NSMutableDictionary new]; return self; }
- (NSDictionary*)dictionaryForKey:(NSString*)key { return self.values[key]; }
- (void)setObject:(id)value forKey:(NSString*)key { self.values[key]=[value copy]; }
- (void)removeObjectForKey:(NSString*)key { [self.values removeObjectForKey:key]; }
@end
static void Check(BOOL ok,NSString* text) { if (!ok) { NSLog(@"FAIL: %@",text); std::exit(1); } }
static void Drain() { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.03]]; }
static void WaitFor(BOOL(^condition)(void),NSString* message) {
    NSDate* deadline=[NSDate dateWithTimeIntervalSinceNow:5];
    while (!condition() && deadline.timeIntervalSinceNow>0) Drain();
    Check(condition(),message);
}
int main() { @autoreleasepool {
    InputTestDefaults* defaults=[InputTestDefaults new]; CaraInputHub* hub=[[CaraInputHub alloc] initWithDefaults:defaults];
    __block NSSet* buttons; __block CGPoint circle=CGPointZero; __block BOOL fast=NO;
    hub.inputChanged=^(NSSet* value,CGPoint left,CGPoint right) { buttons=value; circle=left; };
    hub.commandChanged=^(NSString* command,BOOL down) { if ([command isEqual:@"fastForward"]) fast=down; };
    hub.active=YES;
    [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:YES]; Check([buttons containsObject:@"a"],@"keyboard A press");
    [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:NO]; Check(![buttons containsObject:@"a"],@"keyboard A release");
    [hub handleKeyboardCode:GCKeyCodeKeyW pressed:YES]; [hub handleKeyboardCode:GCKeyCodeKeyD pressed:YES]; Check(circle.x>.7 && circle.y>.7 && std::hypot(circle.x,circle.y)<1.001,@"keyboard diagonal normalized");
    [hub handleKeyboardCode:GCKeyCodeSpacebar pressed:YES]; Check(fast,@"hold fast-forward"); hub.active=NO; Check(!fast && buttons.count==0 && CGPointEqualToPoint(circle,CGPointZero),@"background/menu clears buttons, axes and held commands");
    hub.active=YES;
    __block BOOL captured=NO;
    [hub captureAction:@"a" keyboard:YES completion:^(NSError* error) { Check(!error,@"mapping capture"); captured=YES; }];
    [hub handleKeyboardCode:GCKeyCodeKeyP pressed:YES]; Check(captured && ![buttons containsObject:@"a"],@"captured key quarantined until release");
    [hub handleKeyboardCode:GCKeyCodeKeyP pressed:NO]; [hub handleKeyboardCode:GCKeyCodeKeyP pressed:YES]; Check([buttons containsObject:@"a"],@"custom key mapping used");
    [hub handleKeyboardCode:GCKeyCodeKeyP pressed:NO]; [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:YES]; Check(![buttons containsObject:@"a"],@"old mapping replaced"); [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:NO];
    [hub resetMappings:YES]; [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:YES]; Check([buttons containsObject:@"a"],@"default mapping restored"); [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:NO];
    GCController* controller=[GCController controllerWithExtendedGamepad]; [hub useController:controller];
    // Snapshot setters update values but do not promise hardware callbacks.
    // Inject the public profile event after writing the native synthetic value.
    const auto event=[&](GCControllerElement* element) { if (controller.extendedGamepad.valueChangedHandler) controller.extendedGamepad.valueChangedHandler(controller.extendedGamepad,element); Drain(); };
    [controller.extendedGamepad.buttonA setValue:1]; WaitFor(^BOOL { return controller.extendedGamepad.buttonA.isPressed; },@"snapshot press value arrives"); event(controller.extendedGamepad.buttonA); Check([buttons containsObject:@"a"],@"native GameController snapshot button routed");
    [controller.extendedGamepad.buttonA setValue:0]; WaitFor(^BOOL { return !controller.extendedGamepad.buttonA.isPressed; },@"snapshot release value arrives"); event(controller.extendedGamepad.buttonA); Check(![buttons containsObject:@"a"],@"native GameController snapshot release routed");
    [controller.extendedGamepad.leftThumbstick.xAxis setValue:.8]; WaitFor(^BOOL { return controller.extendedGamepad.leftThumbstick.xAxis.value>.79; },@"snapshot axis value arrives"); event(controller.extendedGamepad.leftThumbstick.xAxis); Check(circle.x>.7 && circle.y==0,@"native controller analog and deadzone");
    [controller.extendedGamepad.buttonA setValue:1]; WaitFor(^BOOL { return controller.extendedGamepad.buttonA.isPressed; },@"snapshot reconnect press value arrives"); event(controller.extendedGamepad.buttonA);
    [NSNotificationCenter.defaultCenter postNotificationName:GCKeyboardDidConnectNotification object:nil]; Check([buttons containsObject:@"a"],@"unrelated device notifications retain an explicitly selected snapshot");
    [hub useController:nil]; Check(![buttons containsObject:@"a"] && CGPointEqualToPoint(circle,CGPointZero),@"controller removal clears held buttons and axes");
    // Menu callback can synchronously deactivate the hub; stale commands must
    // not re-enable held fast-forward after that nested reset.
    __weak CaraInputHub* weakHub=hub;
    hub.commandChanged=^(NSString* command,BOOL down) { if ([command isEqual:@"menu"] && down) weakHub.active=NO; if ([command isEqual:@"fastForward"]) fast=down; };
    [hub handleKeyboardCode:GCKeyCodeSpacebar pressed:YES]; [hub handleKeyboardCode:GCKeyCodeF1 pressed:YES]; Check(!hub.active && !fast && buttons.count==0,@"reentrant menu action leaves no stuck input");
    hub.active=YES;
    [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:YES];
    [hub updateSurface:@"actions" buttons:[NSSet setWithObject:@"a"] circle:CGPointMake(.8,0) cstick:CGPointZero touch:CGPointMake(-1,-1)];
    [hub updateSurface:@"skin" buttons:[NSSet setWithObject:@"b"] circle:CGPointZero cstick:CGPointMake(0,.5) touch:CGPointMake(123,99)];
    Check([buttons containsObject:@"a"] && [buttons containsObject:@"b"] && circle.x==.8 && hub.touchPoint.x==123,@"independent surfaces merge with keyboard");
    [hub clearSurfaces]; Check([buttons containsObject:@"a"] && ![buttons containsObject:@"b"] && hub.touchPoint.x<0,@"surface release retains hardware owner");
    [hub handleKeyboardCode:GCKeyCodeKeyJ pressed:NO];
    [hub updateSurface:@"actions" buttons:[NSSet setWithObject:@"b"] circle:CGPointMake(.8,0) cstick:CGPointZero touch:CGPointMake(-1,-1)];
    hub.active=NO; Check(buttons.count==0 && CGPointEqualToPoint(circle,CGPointZero),@"pause clears surfaces");
    hub.active=YES; Check(buttons.count==0,@"resume cannot revive surfaces");
    puts("PASS: keyboard/controller input, analog deadzone, mappings, capture quarantine, held commands, lifecycle reset, merged touch surfaces and reentrant menu (native synthetic GameController; no physical controller claim)");
} }
