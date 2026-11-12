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
    [controller.extendedGamepad.buttonA setValue:1]; event(controller.extendedGamepad.buttonA); Check([buttons containsObject:@"a"],@"native GameController snapshot button routed");
    [controller.extendedGamepad.buttonA setValue:0]; event(controller.extendedGamepad.buttonA); Check(![buttons containsObject:@"a"],@"native GameController snapshot release routed");
    [controller.extendedGamepad.leftThumbstick.xAxis setValue:.8]; event(controller.extendedGamepad.leftThumbstick.xAxis); Check(circle.x>.7 && circle.y==0,@"native controller analog and deadzone");
    // Menu callback can synchronously deactivate the hub; stale commands must
    // not re-enable held fast-forward after that nested reset.
    __weak CaraInputHub* weakHub=hub;
    hub.commandChanged=^(NSString* command,BOOL down) { if ([command isEqual:@"menu"] && down) weakHub.active=NO; if ([command isEqual:@"fastForward"]) fast=down; };
    [hub handleKeyboardCode:GCKeyCodeSpacebar pressed:YES]; [hub handleKeyboardCode:GCKeyCodeF1 pressed:YES]; Check(!hub.active && !fast && buttons.count==0,@"reentrant menu action leaves no stuck input");
    puts("PASS: keyboard/controller input, analog deadzone, mappings, capture quarantine, held commands, lifecycle reset and reentrant menu (native synthetic GameController; no physical controller claim)");
} }
