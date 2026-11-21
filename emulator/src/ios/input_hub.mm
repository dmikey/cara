// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/input_hub.h"
#include <cmath>
#include <algorithm>

NSNotificationName const CaraInputHubDidChangeNotification=@"CaraInputHubDidChange";
static NSDictionary* DefaultPadBindings() {
    return @{@"A":@"a",@"B":@"b",@"X":@"x",@"Y":@"y",@"L":@"l",@"R":@"r",@"ZL":@"zl",@"ZR":@"zr",@"Up":@"up",@"Down":@"down",@"Left":@"left",@"Right":@"right",@"Start":@"start",@"Select":@"select",@"L3":@"menu",@"R3":@"quickSave"};
}
static NSDictionary* DefaultKeyBindings() {
    return @{@(GCKeyCodeKeyJ).stringValue:@"a",@(GCKeyCodeKeyK).stringValue:@"b",@(GCKeyCodeKeyU).stringValue:@"x",@(GCKeyCodeKeyI).stringValue:@"y",@(GCKeyCodeKeyQ).stringValue:@"l",@(GCKeyCodeKeyE).stringValue:@"r",@(GCKeyCodeOne).stringValue:@"zl",@(GCKeyCodeThree).stringValue:@"zr",@(GCKeyCodeUpArrow).stringValue:@"up",@(GCKeyCodeDownArrow).stringValue:@"down",@(GCKeyCodeLeftArrow).stringValue:@"left",@(GCKeyCodeRightArrow).stringValue:@"right",@(GCKeyCodeReturnOrEnter).stringValue:@"start",@(GCKeyCodeRightShift).stringValue:@"select",@(GCKeyCodeKeyW).stringValue:@"circleUp",@(GCKeyCodeKeyS).stringValue:@"circleDown",@(GCKeyCodeKeyA).stringValue:@"circleLeft",@(GCKeyCodeKeyD).stringValue:@"circleRight",@(GCKeyCodeKeyT).stringValue:@"cstickUp",@(GCKeyCodeKeyG).stringValue:@"cstickDown",@(GCKeyCodeKeyF).stringValue:@"cstickLeft",@(GCKeyCodeKeyH).stringValue:@"cstickRight",@(GCKeyCodeF1).stringValue:@"menu",@(GCKeyCodeF5).stringValue:@"quickSave",@(GCKeyCodeF8).stringValue:@"quickLoad",@(GCKeyCodeSpacebar).stringValue:@"fastForward"};
}
static CGPoint NormalizeStick(CGPoint point,CGFloat deadzone) {
    const double radius=std::hypot(point.x,point.y); if (!std::isfinite(radius) || radius<=deadzone) return CGPointZero;
    double scale=std::min(1.0,(radius-deadzone)/(1-deadzone))/radius; return CGPointMake(point.x*scale,point.y*scale);
}

// Some older MFi devices expose only the original standard gamepad profile.
// Keep that fallback local so the extended profile remains the primary path.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static GCGamepad* StandardGamepad(GCController* controller) {
    return controller.gamepad;
}
#pragma clang diagnostic pop

static BOOL SupportsController(GCController* controller) {
    return controller.extendedGamepad != nil || StandardGamepad(controller) != nil;
}

static CGPoint ReadStick(GCControllerDirectionPad* stick) {
    return NormalizeStick(CGPointMake(stick.xAxis.value,stick.yAxis.value),.15);
}

@implementation CaraInputHub {
    NSUserDefaults* preferences;
    GCController* selected;
    NSMutableSet<NSString*>* keysDown;
    NSMutableSet<NSString*>* padDown;
    NSMutableSet<NSString*>* suppressedKeys;
    NSMutableSet<NSString*>* suppressedPad;
    NSSet<NSString*>* lastCommands;
    CGPoint padCircle,padCStick;
    BOOL suppressCircle,suppressCStick;
    NSDictionary<NSString*,GCControllerButtonInput*>* selectedButtons;
    NSString* captureTarget;
    BOOL captureKeyboard;
    void(^captureCompletion)(NSError*);
    unsigned captureGeneration;
    NSMutableDictionary* surfaces;
    NSDictionary* cachedPad;
    NSDictionary* cachedKeys;
}
- (instancetype)initWithDefaults:(NSUserDefaults*)defaults { return [self initWithDefaults:defaults observesDevices:YES]; }
- (instancetype)initWithDefaults:(NSUserDefaults*)defaults observesDevices:(BOOL)observesDevices {
    if ((self=[super init])) {
        preferences=defaults; surfaces=[NSMutableDictionary new]; _touchPoint=CGPointMake(-1,-1); keysDown=[NSMutableSet new]; padDown=[NSMutableSet new]; suppressedKeys=[NSMutableSet new]; suppressedPad=[NSMutableSet new]; lastCommands=[NSSet new];
        if (observesDevices) {
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(devicesChanged:) name:GCControllerDidConnectNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(devicesChanged:) name:GCControllerDidDisconnectNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(devicesChanged:) name:GCKeyboardDidConnectNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(devicesChanged:) name:GCKeyboardDidDisconnectNotification object:nil];
        [self devicesChanged:nil];
        }
    } return self;
}
- (NSArray<NSString*>*)actions { static NSArray* names; static dispatch_once_t once; dispatch_once(&once,^{ names=@[@"a",@"b",@"x",@"y",@"l",@"r",@"zl",@"zr",@"start",@"select",@"up",@"down",@"left",@"right",@"circleUp",@"circleDown",@"circleLeft",@"circleRight",@"cstickUp",@"cstickDown",@"cstickLeft",@"cstickRight",@"menu",@"quickSave",@"quickLoad",@"fastForward"]; }); return names; }
- (NSDictionary*)bindings:(BOOL)keyboard {
    NSDictionary* cached=keyboard?cachedKeys:cachedPad; if (cached) return cached;
    NSDictionary* saved=[preferences dictionaryForKey:keyboard?@"keyboardBindings":@"controllerBindings"];
    NSDictionary* defaults=keyboard?DefaultKeyBindings():DefaultPadBindings();
    BOOL valid=[saved isKindOfClass:NSDictionary.class] && saved.count<=96;
    if (valid) for (NSString* key in saved) if (![key isKindOfClass:NSString.class] || key.length>32 || ![self.actions containsObject:saved[key]]) { valid=NO; break; }
    NSDictionary* result=valid?saved:defaults;
    if (keyboard) cachedKeys=result; else cachedPad=result;
    return result;
}
- (void)announce { [NSNotificationCenter.defaultCenter postNotificationName:CaraInputHubDidChangeNotification object:self]; }
- (NSString*)controllerName { return selected.vendorName?: (selected?@"Game controller":@"No controller connected"); }
- (BOOL)controllerConnected { return selected!=nil; }
- (BOOL)controllerHasCirclePad { return selected.extendedGamepad.leftThumbstick!=nil; }
- (BOOL)keyboardConnected { return GCKeyboard.coalescedKeyboard!=nil; }
- (void)devicesChanged:(NSNotification*)notification {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(),^{ [self devicesChanged:notification]; }); return; }
    const BOOL controllerEvent=!notification || [notification.name isEqual:GCControllerDidConnectNotification] ||
                               [notification.name isEqual:GCControllerDidDisconnectNotification];
    if (controllerEvent && (!selected || (!selected.isSnapshot && ![GCController.controllers containsObject:selected]))) {
        GCController* replacement=nil;
        for (GCController* controller in GCController.controllers) {
            if (!SupportsController(controller)) continue;
            if (!replacement || controller.extendedGamepad) replacement=controller;
            if (controller.extendedGamepad) break;
        }
        [self useController:replacement];
    }
    if (notification && controllerEvent) return;
    __weak CaraInputHub* owner=self;
    GCKeyboard* keyboard=GCKeyboard.coalescedKeyboard; keyboard.handlerQueue=dispatch_get_main_queue();
    keyboard.keyboardInput.keyChangedHandler=^(GCKeyboardInput* input,GCDeviceButtonInput* key,GCKeyCode code,BOOL pressed) { [owner handleKeyboardCode:code pressed:pressed]; };
    [keysDown removeAllObjects]; [self rebuild]; [self announce];
}
- (void)useController:(GCController*)controller {
    if (!SupportsController(controller)) controller=nil;
    if (selected==controller) return;
    selected.extendedGamepad.valueChangedHandler=nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    StandardGamepad(selected).valueChangedHandler=nil;
#pragma clang diagnostic pop
    selected=controller;
    selectedButtons=[self makePadButtons];
    controller.handlerQueue=dispatch_get_main_queue(); __weak CaraInputHub* owner=self;
    if (controller.extendedGamepad) {
        controller.extendedGamepad.valueChangedHandler=^(GCExtendedGamepad* gamepad,GCControllerElement* element) {
            CaraInputHub* hub=owner;
            if (hub && hub->selected.extendedGamepad==gamepad) [hub pollPad];
        };
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        StandardGamepad(controller).valueChangedHandler=^(GCGamepad* gamepad,GCControllerElement* element) {
            CaraInputHub* hub=owner;
            if (hub && StandardGamepad(hub->selected)==gamepad) [hub pollPad];
        };
#pragma clang diagnostic pop
    }
    [self suppressHeldPadInputs];
    [self pollPad];
    [self announce];
}
- (NSDictionary<NSString*,GCControllerButtonInput*>*)makePadButtons {
    GCExtendedGamepad* p=selected.extendedGamepad;
    if (!p) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        GCGamepad* standard=StandardGamepad(selected);
        if (!standard) return @{};
        return @{@"A":standard.buttonA,@"B":standard.buttonB,@"X":standard.buttonX,@"Y":standard.buttonY,
                 @"L":standard.leftShoulder,@"R":standard.rightShoulder,@"Up":standard.dpad.up,
                 @"Down":standard.dpad.down,@"Left":standard.dpad.left,@"Right":standard.dpad.right};
#pragma clang diagnostic pop
    }
    NSMutableDictionary* buttons=[@{@"A":p.buttonA,@"B":p.buttonB,@"X":p.buttonX,@"Y":p.buttonY,@"L":p.leftShoulder,@"R":p.rightShoulder,@"ZL":p.leftTrigger,@"ZR":p.rightTrigger,@"Up":p.dpad.up,@"Down":p.dpad.down,@"Left":p.dpad.left,@"Right":p.dpad.right,@"Start":p.buttonMenu} mutableCopy];
    if (p.buttonOptions) buttons[@"Select"]=p.buttonOptions;
    if (p.leftThumbstickButton) buttons[@"L3"]=p.leftThumbstickButton;
    if (p.rightThumbstickButton) buttons[@"R3"]=p.rightThumbstickButton;
    return buttons;
}
- (void)suppressHeldPadInputs {
    [padDown removeAllObjects];
    [suppressedPad removeAllObjects];
    for (NSString* name in selectedButtons)
        if (selectedButtons[name].isPressed) [suppressedPad addObject:name];
    GCExtendedGamepad* pad=selected.extendedGamepad;
    suppressCircle=!CGPointEqualToPoint(ReadStick(pad.leftThumbstick),CGPointZero);
    suppressCStick=!CGPointEqualToPoint(ReadStick(pad.rightThumbstick),CGPointZero);
}
- (void)pollPad {
    NSDictionary* buttons=selectedButtons;
    for (NSString* name in buttons) {
        const BOOL pressed=[buttons[name] isPressed];
        if (!pressed) { [padDown removeObject:name]; [suppressedPad removeObject:name]; continue; }
        if ([suppressedPad containsObject:name]) continue;
        if (captureTarget && !captureKeyboard && ![padDown containsObject:name]) { [suppressedPad addObject:name]; [self finishCapture:name]; }
        else [padDown addObject:name];
    }
    GCExtendedGamepad* p=selected.extendedGamepad;
    const CGPoint circle=ReadStick(p.leftThumbstick),cstick=ReadStick(p.rightThumbstick);
    if (CGPointEqualToPoint(circle,CGPointZero)) suppressCircle=NO;
    if (CGPointEqualToPoint(cstick,CGPointZero)) suppressCStick=NO;
    padCircle=suppressCircle?CGPointZero:circle;
    padCStick=suppressCStick?CGPointZero:cstick;
    [self rebuild];
}
- (void)updateSurface:(NSString*)source buttons:(NSSet<NSString*>*)buttons circle:(CGPoint)circle cstick:(CGPoint)cstick touch:(CGPoint)touch {
    if (!self.active) return;
    surfaces[source]=@{@"buttons":[buttons copy],@"cx":@(circle.x),@"cy":@(circle.y),@"sx":@(cstick.x),@"sy":@(cstick.y),@"tx":@(touch.x),@"ty":@(touch.y)}; [self rebuild];
}
- (void)clearSurfaces { [surfaces removeAllObjects]; [self rebuild]; }
- (void)setActive:(BOOL)value {
    if (_active==value) return; _active=value;
    [keysDown removeAllObjects]; [padDown removeAllObjects];
    if (value) { [self suppressHeldPadInputs]; [self pollPad]; }
    else { [surfaces removeAllObjects]; padCircle=padCStick=CGPointZero; [self rebuild]; }
}
- (void)handleKeyboardCode:(GCKeyCode)code pressed:(BOOL)pressed {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(),^{ [self handleKeyboardCode:code pressed:pressed]; }); return; }
    NSString* key=@(code).stringValue;
    if (!pressed) { [keysDown removeObject:key]; [suppressedKeys removeObject:key]; }
    else if ([suppressedKeys containsObject:key]) return;
    else if (captureTarget && captureKeyboard) { [suppressedKeys addObject:key]; [self finishCapture:key]; }
    else [keysDown addObject:key];
    [self rebuild];
}
- (void)rebuild {
    NSMutableSet* logical=[NSMutableSet new];
    if (self.active && !captureTarget) {
        NSDictionary* pad=[self bindings:NO],*keyboard=[self bindings:YES];
        for (NSString* name in padDown) if (pad[name]) [logical addObject:pad[name]];
        for (NSString* key in keysDown) if (keyboard[key]) [logical addObject:keyboard[key]];
    }
    NSMutableSet* commands=[NSMutableSet new];
    for (NSString* command in @[@"menu",@"quickSave",@"quickLoad",@"fastForward"]) {
        BOOL pressed=[logical containsObject:command]; if (pressed) [commands addObject:command];
        [logical removeObject:command];
    }
    NSSet* previousCommands=lastCommands; lastCommands=commands;
    const BOOL wasActive=self.active; NSString* previousCapture=captureTarget;
    const auto axis=[&](NSString* positive,NSString* negative) { double value=int([logical containsObject:positive])-int([logical containsObject:negative]); [logical removeObject:positive]; [logical removeObject:negative]; return value; };
    CGPoint keyboardCircle=NormalizeStick(CGPointMake(axis(@"circleRight",@"circleLeft"),axis(@"circleUp",@"circleDown")),0);
    CGPoint keyboardCStick=NormalizeStick(CGPointMake(axis(@"cstickRight",@"cstickLeft"),axis(@"cstickUp",@"cstickDown")),0);
    CGPoint circle=self.active && !captureTarget?(CGPointEqualToPoint(padCircle,CGPointZero)?keyboardCircle:padCircle):CGPointZero;
    CGPoint cstick=self.active && !captureTarget?(CGPointEqualToPoint(padCStick,CGPointZero)?keyboardCStick:padCStick):CGPointZero;
    _touchPoint=CGPointMake(-1,-1);
    if (self.active && !captureTarget) for (NSString* source in [surfaces.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSDictionary* surface=surfaces[source]; [logical unionSet:surface[@"buttons"]];
        CGPoint surfaceCircle=CGPointMake([surface[@"cx"] doubleValue],[surface[@"cy"] doubleValue]),surfaceStick=CGPointMake([surface[@"sx"] doubleValue],[surface[@"sy"] doubleValue]),touch=CGPointMake([surface[@"tx"] doubleValue],[surface[@"ty"] doubleValue]);
        if (CGPointEqualToPoint(circle,CGPointZero)) circle=surfaceCircle;
        if (CGPointEqualToPoint(cstick,CGPointZero)) cstick=surfaceStick;
        if (touch.x>=0) _touchPoint=touch;
    }
    if (self.inputChanged) self.inputChanged(logical,circle,cstick);
    for (NSString* command in @[@"menu",@"quickSave",@"quickLoad",@"fastForward"]) {
        if (self.active!=wasActive || captureTarget!=previousCapture) break;
        const BOOL pressed=[commands containsObject:command];
        if (pressed!=[previousCommands containsObject:command] && self.commandChanged) self.commandChanged(command,pressed);
    }
}
- (NSString*)bindingForAction:(NSString*)action keyboard:(BOOL)keyboard {
    NSMutableArray* names=[NSMutableArray new];
    for (NSString* key in [self bindings:keyboard]) if ([[self bindings:keyboard][key] isEqual:action]) {
        if (keyboard) {
            NSString* name=[GCKeyboard.coalescedKeyboard.keyboardInput buttonForKeyCode:(GCKeyCode)key.integerValue].localizedName;
            NSDictionary* standard=@{@(GCKeyCodeKeyW).stringValue:@"W",@(GCKeyCodeKeyA).stringValue:@"A",@(GCKeyCodeKeyS).stringValue:@"S",@(GCKeyCodeKeyD).stringValue:@"D",@(GCKeyCodeKeyJ).stringValue:@"J",@(GCKeyCodeKeyK).stringValue:@"K",@(GCKeyCodeKeyU).stringValue:@"U",@(GCKeyCodeKeyI).stringValue:@"I",@(GCKeyCodeF1).stringValue:@"F1",@(GCKeyCodeF5).stringValue:@"F5",@(GCKeyCodeF8).stringValue:@"F8",@(GCKeyCodeReturnOrEnter).stringValue:@"Return",@(GCKeyCodeSpacebar).stringValue:@"Space"};
            [names addObject:name?:standard[key]?:[NSString stringWithFormat:@"Key %@",key]];
        } else [names addObject:selectedButtons[key].localizedName?:key];
    }
    return names.count?[[names sortedArrayUsingSelector:@selector(localizedStandardCompare:)] componentsJoinedByString:@", "]:@"Not assigned";
}
- (void)captureAction:(NSString*)action keyboard:(BOOL)keyboard completion:(void(^)(NSError*))completion {
    [self cancelCapture];
    if (![self.actions containsObject:action]) { completion([NSError errorWithDomain:@"CaraInput" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Unknown input action."}]); return; }
    captureTarget=action; captureKeyboard=keyboard; captureCompletion=[completion copy]; const unsigned generation=++captureGeneration;
    [self rebuild];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        if (generation==self->captureGeneration && self->captureTarget) { void(^done)(NSError*)=self->captureCompletion; [self cancelCapture]; if (done) done([NSError errorWithDomain:@"CaraInput" code:2 userInfo:@{NSLocalizedDescriptionKey:@"No input received. Connect a device and try again."}]); }
    });
}
- (void)finishCapture:(NSString*)physical {
    NSString* action=captureTarget; BOOL keyboard=captureKeyboard; void(^completion)(NSError*)=captureCompletion;
    NSMutableDictionary* map=[[self bindings:keyboard] mutableCopy];
    for (NSString* key in [map.allKeys copy]) if ([map[key] isEqual:action]) [map removeObjectForKey:key];
    map[physical]=action; if (keyboard) cachedKeys=[map copy]; else cachedPad=[map copy]; [preferences setObject:map forKey:keyboard?@"keyboardBindings":@"controllerBindings"];
    captureTarget=nil; captureCompletion=nil; ++captureGeneration; [keysDown removeAllObjects]; [padDown removeAllObjects]; [self announce];
    if (completion) completion(nil);
}
- (void)cancelCapture { captureTarget=nil; captureCompletion=nil; ++captureGeneration; [self rebuild]; }
- (void)reloadMappings { cachedPad=nil; cachedKeys=nil; [self rebuild]; [self announce]; }
- (void)resetMappings:(BOOL)keyboard { if (keyboard) cachedKeys=nil; else cachedPad=nil; [preferences removeObjectForKey:keyboard?@"keyboardBindings":@"controllerBindings"]; [self cancelCapture]; [self announce]; }
- (void)dealloc {
    selected.extendedGamepad.valueChangedHandler=nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    StandardGamepad(selected).valueChangedHandler=nil;
#pragma clang diagnostic pop
    [NSNotificationCenter.defaultCenter removeObserver:self];
}
@end
