// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/action_layout.h"
#import "ios/input_hub.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <cmath>

static NSArray* Buttons() { return @[@"a",@"b",@"x",@"y",@"l",@"r",@"zl",@"zr",@"start",@"select",@"up",@"down",@"left",@"right"]; }
static NSDictionary* Control(NSString* label,NSString* kind,NSString* binding,double x,double y,double w,double h) {
    return @{@"id":NSUUID.UUID.UUIDString,@"label":label,@"kind":kind,@"binding":binding,@"toggle":@NO,@"hidden":@NO,@"travel":@64,@"frame":@{@"x":@(x),@"y":@(y),@"width":@(w),@"height":@(h)}};
}
NSDictionary* CaraActionLayoutSettingsRow(void(^openEditor)(void)) {
    return @{@"title":@"Controller Demapping",@"subtitle":@"Arrange actions and bind them to 3DS inputs",
             @"symbol":@"hand.draw",@"keepOpen":@YES,@"action":[openEditor copy]};
}

NSDictionary* CaraNewActionLayout() {
    // Deliberately unbound: action names never imply a particular game's controls.
    NSArray* portrait=@[Control(@"Move",@"stick",@"unbound",.06,.69,.27,.18),Control(@"Fire",@"button",@"unbound",.76,.72,.18,.12),Control(@"Jump",@"button",@"unbound",.56,.81,.18,.12)];
    NSArray* landscape=@[Control(@"Move",@"stick",@"unbound",.05,.55,.20,.34),Control(@"Fire",@"button",@"unbound",.81,.52,.12,.21),Control(@"Jump",@"button",@"unbound",.66,.72,.12,.21)];
    return @{@"version":@1,@"enabled":@NO,@"portrait":portrait,@"landscape":landscape};
}
BOOL CaraValidateActionLayout(id v,NSError** error) {
    BOOL ok=[v isKindOfClass:NSDictionary.class] && [v[@"version"] isKindOfClass:NSNumber.class] && [v[@"version"] isEqualToNumber:@1] && [v[@"enabled"] isKindOfClass:NSNumber.class];
    for (NSString* orientation in @[@"portrait",@"landscape"]) {
        id controls=ok?v[orientation]:nil;
        if (![controls isKindOfClass:NSArray.class] || [controls count]>24) { ok=NO; break; }
        NSMutableSet* ids=[NSMutableSet new];
        for (id c in controls) {
            if (![c isKindOfClass:NSDictionary.class]) { ok=NO; break; }
            NSString* kind=c[@"kind"],*binding=c[@"binding"],*label=c[@"label"],*identifier=c[@"id"];
            if (![identifier isKindOfClass:NSString.class] || !identifier.length || identifier.length>64 || [ids containsObject:identifier] || ![label isKindOfClass:NSString.class] || !label.length || label.length>32 || ![kind isKindOfClass:NSString.class] || ![@[@"button",@"stick",@"floating",@"dpad",@"drag"] containsObject:kind] || ![binding isKindOfClass:NSString.class] || ![c[@"toggle"] isKindOfClass:NSNumber.class]) { ok=NO; break; }
            [ids addObject:identifier];
            NSArray* allowed=[kind isEqual:@"button"]?[Buttons() arrayByAddingObject:@"unbound"]:[@[@"circle",@"cstick",@"unbound"] arrayByAddingObjectsFromArray:[kind isEqual:@"dpad"]?@[@"dpad"]:@[]];
            if (![allowed containsObject:binding] || (![kind isEqual:@"button"] && [c[@"toggle"] boolValue]) || ![c[@"frame"] isKindOfClass:NSDictionary.class]) { ok=NO; break; }
            if ((c[@"hidden"] && ![c[@"hidden"] isKindOfClass:NSNumber.class]) || (c[@"travel"] && (![c[@"travel"] isKindOfClass:NSNumber.class] || !std::isfinite([c[@"travel"] doubleValue]) || [c[@"travel"] doubleValue]<24 || [c[@"travel"] doubleValue]>160))) { ok=NO; break; }
            NSDictionary* f=c[@"frame"];
            for (NSString* k in @[@"x",@"y",@"width",@"height"]) if (![f[k] isKindOfClass:NSNumber.class] || !std::isfinite([f[k] doubleValue]) || [f[k] doubleValue]<0 || [f[k] doubleValue]>1) ok=NO;
            if (!ok) break;
            if ([f[@"width"] doubleValue]<.04 || [f[@"height"] doubleValue]<.04 || [f[@"x"] doubleValue]+[f[@"width"] doubleValue]>1.00001 || [f[@"y"] doubleValue]+[f[@"height"] doubleValue]>1.00001) ok=NO;
            if (!ok) break;
        }
    }
    if (!ok && error) *error=[NSError errorWithDomain:@"CaraActionLayout" code:1 userInfo:@{NSLocalizedDescriptionKey:@"This layout is invalid or uses an unsupported version. Your saved layout has not changed."}];
    return ok;
}
CGRect CaraActionFrame(NSDictionary* c,CGSize size) {
    NSDictionary* f=c[@"frame"]; CGFloat w=MIN(size.width,MAX(44,[f[@"width"] doubleValue]*size.width)),h=MIN(size.height,MAX(44,[f[@"height"] doubleValue]*size.height));
    return CGRectMake(MIN(MAX(0,[f[@"x"] doubleValue]*size.width),MAX(0,size.width-w)),MIN(MAX(0,[f[@"y"] doubleValue]*size.height),MAX(0,size.height-h)),w,h);
}

@implementation CaraActionControls {
    NSMutableDictionary<NSValue*,NSDictionary*>* contacts;
    NSMutableSet<NSString*>* toggles;
    NSMutableSet<NSValue*>* blockedContacts;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.multipleTouchEnabled=YES; self.opaque=NO; contacts=[NSMutableDictionary new]; toggles=[NSMutableSet new]; blockedContacts=[NSMutableSet new];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(reset) name:UIApplicationWillResignActiveNotification object:nil];
    } return self;
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)setControls:(NSArray*)controls { [self reset]; _controls=[controls copy]; [self setNeedsDisplay]; }
- (NSDictionary*)hit:(CGPoint)p {
    // Explicit buttons win over broad invisible analog zones.
    for (NSDictionary* c in self.controls.reverseObjectEnumerator) if ([c[@"kind"] isEqual:@"button"] && CGRectContainsPoint(CaraActionFrame(c,self.bounds.size),p)) return c;
    for (NSDictionary* c in self.controls.reverseObjectEnumerator) if (CGRectContainsPoint(CaraActionFrame(c,self.bounds.size),p)) return c;
    return nil;
}
- (BOOL)pointInside:(CGPoint)p withEvent:(UIEvent*)event { return [self hit:p]!=nil; }
- (void)updateContact:(NSValue*)key atPoint:(CGPoint)p {
    if ([blockedContacts containsObject:key]) return;
    NSDictionary* previous=contacts[key]; NSDictionary* c=previous[@"control"]?:[self hit:p]; if (!c) return;
    if (!previous) {
        // A control has one owner for its entire contact, including off-control movement.
        for (NSDictionary* contact in contacts.allValues) if ([contact[@"control"][@"id"] isEqual:c[@"id"]]) { [blockedContacts addObject:key]; return; }
        if ([c[@"toggle"] boolValue]) { if ([toggles containsObject:c[@"id"]]) [toggles removeObject:c[@"id"]]; else [toggles addObject:c[@"id"]]; }
    }
    contacts[key]=@{@"control":c,@"point":[NSValue valueWithCGPoint:p],@"origin":previous[@"origin"]?:[NSValue valueWithCGPoint:p]}; [self emit];
}
- (void)endContact:(NSValue*)key { [blockedContacts removeObject:key]; [contacts removeObjectForKey:key]; [self emit]; }
- (void)reset { [blockedContacts addObjectsFromArray:contacts.allKeys]; [contacts removeAllObjects]; [toggles removeAllObjects]; [self emit]; }
- (void)didMoveToWindow { [super didMoveToWindow]; if (!self.window) [self reset]; }
- (void)touchesBegan:(NSSet*)touches withEvent:(UIEvent*)event { for (UITouch* t in touches) { NSValue* key=[NSValue valueWithNonretainedObject:t]; [blockedContacts removeObject:key]; [self updateContact:key atPoint:[t locationInView:self]]; } }
- (void)touchesMoved:(NSSet*)touches withEvent:(UIEvent*)event { for (UITouch* t in touches) [self updateContact:[NSValue valueWithNonretainedObject:t] atPoint:[t locationInView:self]]; }
- (void)touchesEnded:(NSSet*)touches withEvent:(UIEvent*)event { for (UITouch* t in touches) [self endContact:[NSValue valueWithNonretainedObject:t]]; }
- (void)touchesCancelled:(NSSet*)touches withEvent:(UIEvent*)event { [self reset]; }
- (void)emit {
    NSMutableSet* pressed=[NSMutableSet new]; CGPoint circle=CGPointZero,cstick=CGPointZero;
    // Iterate controls in stable order for deterministic duplicate analog bindings.
    for (NSDictionary* c in self.controls) {
        NSDictionary* contact=nil; for (NSDictionary* value in contacts.allValues) if ([value[@"control"][@"id"] isEqual:c[@"id"]]) { contact=value; break; }
        NSString* kind=c[@"kind"],*binding=c[@"binding"]; if ([binding isEqual:@"unbound"]) continue;
        CGRect r=CaraActionFrame(c,self.bounds.size); CGPoint p=[contact[@"point"] CGPointValue];
        if ([kind isEqual:@"button"]) { if ([toggles containsObject:c[@"id"]] || (contact && ![c[@"toggle"] boolValue] && CGRectContainsPoint(r,p))) [pressed addObject:binding]; continue; }
        if (!contact) continue;
        CGPoint vector;
        if ([kind isEqual:@"floating"] || [kind isEqual:@"drag"]) { CGPoint origin=[contact[@"origin"] CGPointValue]; CGFloat travel=c[@"travel"]?[c[@"travel"] doubleValue]:64; r=CGRectMake(origin.x-travel,origin.y-travel,travel*2,travel*2); }
        vector=CaraStickPoint(p,r);
        if ([kind isEqual:@"dpad"]) { unsigned d=CaraDPadDirections(p,r); vector=CGPointMake(bool(d&8)-bool(d&4),bool(d&1)-bool(d&2)); if ([binding isEqual:@"dpad"]) { if (d&1) [pressed addObject:@"up"]; if (d&2) [pressed addObject:@"down"]; if (d&4) [pressed addObject:@"left"]; if (d&8) [pressed addObject:@"right"]; } }
        // An analog D-pad diagonal has the same unit range as a physical stick.
        const double magnitude=std::hypot(vector.x,vector.y);
        if (magnitude>1) { vector.x/=magnitude; vector.y/=magnitude; }
        if ([binding isEqual:@"circle"]) circle=vector; else if ([binding isEqual:@"cstick"]) cstick=vector;
    }
    if (self.inputChanged) self.inputChanged(pressed,circle,cstick); [self setNeedsDisplay];
}
- (void)drawRect:(CGRect)rect {
    for (NSDictionary* c in self.controls) {
        BOOL hidden=[c[@"hidden"] boolValue]; if (hidden && !self.showsHiddenControls) continue;
        CGRect r=CaraActionFrame(c,self.bounds.size); BOOL active=[toggles containsObject:c[@"id"]];
        for (NSDictionary* contact in contacts.allValues) if ([contact[@"control"][@"id"] isEqual:c[@"id"]]) active=YES;
        UIBezierPath* path=[UIBezierPath bezierPathWithRoundedRect:CGRectInset(r,1,1) cornerRadius:[c[@"kind"] isEqual:@"button"]?MIN(r.size.width,r.size.height)/2:20];
        [(hidden?[UIColor colorWithRed:.25 green:.65 blue:.8 alpha:.12]:active?[UIColor colorWithRed:1 green:.48 blue:.24 alpha:.7]:[UIColor colorWithWhite:.12 alpha:.82]) setFill]; [path fill];
        if (hidden) { CGFloat dash[]={6,4}; [path setLineDash:dash count:2 phase:0]; }
        [[UIColor colorWithWhite:1 alpha:.4] setStroke]; path.lineWidth=1.5; [path stroke];
        NSString* subtitle=hidden?@"Invisible zone":[c[@"binding"] isEqual:@"unbound"]?@"Bind input":[c[@"kind"] isEqual:@"dpad"]?@"✚":[c[@"kind"] isEqual:@"button"]?([c[@"toggle"] boolValue]?@"Toggle":@"Hold"):[c[@"kind"] isEqual:@"drag"]?@"Drag →":@"◎";
        NSMutableParagraphStyle* style=[NSMutableParagraphStyle new]; style.alignment=NSTextAlignmentCenter; style.lineBreakMode=NSLineBreakByTruncatingTail;
        CGFloat fontSize=15;
        while (fontSize>8 && [c[@"label"] sizeWithAttributes:@{NSFontAttributeName:[UIFont systemFontOfSize:fontSize weight:UIFontWeightSemibold]}].width>r.size.width-8) --fontSize;
        [c[@"label"] drawInRect:CGRectMake(r.origin.x+4,CGRectGetMidY(r)-17,r.size.width-8,22) withAttributes:@{NSFontAttributeName:[UIFont systemFontOfSize:fontSize weight:UIFontWeightSemibold],NSForegroundColorAttributeName:UIColor.whiteColor,NSParagraphStyleAttributeName:style}];
        [subtitle drawInRect:CGRectMake(r.origin.x+3,CGRectGetMidY(r)+5,r.size.width-6,18) withAttributes:@{NSFontAttributeName:[UIFont systemFontOfSize:10 weight:UIFontWeightMedium],NSForegroundColorAttributeName:[UIColor colorWithWhite:1 alpha:.65],NSParagraphStyleAttributeName:style}];
    }
}
@end

@interface CaraLayoutCanvas : UIView
@property(nonatomic,strong) CaraActionControls* controlsView;
@property(nonatomic,copy) NSArray* controls;
@property(nonatomic,copy) NSString* selected;
@property(nonatomic,strong) NSDictionary* representation;
@property(nonatomic) BOOL testing;
@property(nonatomic,copy) void(^selectedControl)(NSString*);
@property(nonatomic,copy) void(^movedControl)(NSString*,CGRect);
@end
@implementation CaraLayoutCanvas {
    NSString* dragging; CGRect starting; CGPoint anchor; BOOL resizing;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.backgroundColor=UIColor.blackColor; self.layer.cornerRadius=24; self.clipsToBounds=YES;
        _controlsView=[[CaraActionControls alloc] initWithFrame:self.bounds]; _controlsView.showsHiddenControls=YES; [self addSubview:_controlsView];
        UITapGestureRecognizer* tap=[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(selectAt:)]; [self addGestureRecognizer:tap];
        UIPanGestureRecognizer* pan=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(move:)]; pan.maximumNumberOfTouches=1; [self addGestureRecognizer:pan];
    } return self;
}
- (void)setTesting:(BOOL)testing { _testing=testing; self.controlsView.showsHiddenControls=!testing; self.controlsView.userInteractionEnabled=testing; for (UIGestureRecognizer* g in self.gestureRecognizers) g.enabled=!testing; [self.controlsView reset]; [self setNeedsDisplay]; }
- (void)setControls:(NSArray*)controls { _controls=controls; self.controlsView.controls=controls; [self setNeedsDisplay]; }
- (void)layoutSubviews { [super layoutSubviews]; self.controlsView.frame=self.bounds; [self setNeedsDisplay]; }
- (NSDictionary*)hit:(CGPoint)p {
    for (NSDictionary* c in self.controls) if ([c[@"id"] isEqual:self.selected]) { CGRect r=CaraActionFrame(c,self.bounds.size); if (std::hypot(p.x-CGRectGetMaxX(r),p.y-CGRectGetMaxY(r))<26) return c; }
    for (NSDictionary* c in self.controls.reverseObjectEnumerator) if ([c[@"kind"] isEqual:@"button"] && CGRectContainsPoint(CaraActionFrame(c,self.bounds.size),p)) return c;
    for (NSDictionary* c in self.controls.reverseObjectEnumerator) if (CGRectContainsPoint(CGRectInset(CaraActionFrame(c,self.bounds.size),-8,-8),p)) return c; return nil; }
- (void)selectAt:(UITapGestureRecognizer*)g { NSDictionary* c=[self hit:[g locationInView:self]]; if (c && self.selectedControl) self.selectedControl(c[@"id"]); }
- (void)move:(UIPanGestureRecognizer*)g {
    CGPoint p=[g locationInView:self];
    if (g.state==UIGestureRecognizerStateBegan) {
        NSDictionary* c=[self hit:p]; dragging=c[@"id"]; if (!dragging) return; starting=CaraActionFrame(c,self.bounds.size); anchor=p;
        resizing=std::hypot(p.x-CGRectGetMaxX(starting),p.y-CGRectGetMaxY(starting))<30;
        if (self.selectedControl) self.selectedControl(dragging);
    }
    if (!dragging) return;
    if (g.state==UIGestureRecognizerStateChanged || g.state==UIGestureRecognizerStateEnded) {
        CGRect r=starting; CGFloat dx=p.x-anchor.x,dy=p.y-anchor.y;
        if (resizing) { r.size.width=MIN(self.bounds.size.width-r.origin.x,MAX(44,r.size.width+dx)); r.size.height=MIN(self.bounds.size.height-r.origin.y,MAX(44,r.size.height+dy)); }
        else { r.origin.x=MIN(MAX(0,r.origin.x+dx),self.bounds.size.width-r.size.width); r.origin.y=MIN(MAX(0,r.origin.y+dy),self.bounds.size.height-r.size.height); }
        if (self.movedControl) self.movedControl(dragging,CGRectMake(r.origin.x/self.bounds.size.width,r.origin.y/self.bounds.size.height,r.size.width/self.bounds.size.width,r.size.height/self.bounds.size.height));
    }
    if (g.state==UIGestureRecognizerStateEnded || g.state==UIGestureRecognizerStateCancelled) dragging=nil;
}
- (void)drawRect:(CGRect)rect {
    CGSize size=self.bounds.size; NSDictionary* rep=self.representation; NSDictionary* mapping=rep[@"mappingSize"];
    NSUInteger n=0;
    for (NSDictionary* screen in rep[@"screens"]) {
        CGRect f=CaraSkinRect(screen[@"outputFrame"]); CGFloat sx=size.width/[mapping[@"width"] doubleValue],sy=size.height/[mapping[@"height"] doubleValue];
        CGRect r=CGRectMake(f.origin.x*sx,f.origin.y*sy,f.size.width*sx,f.size.height*sy);
        [[UIColor colorWithWhite:.075 alpha:1] setFill]; [[UIBezierPath bezierPathWithRoundedRect:r cornerRadius:8] fill];
        NSString* label=n++?@"TOUCH SCREEN":@"GAME SCREEN";
        [label drawAtPoint:CGPointMake(r.origin.x+12,r.origin.y+12) withAttributes:@{NSFontAttributeName:[UIFont systemFontOfSize:10 weight:UIFontWeightMedium],NSForegroundColorAttributeName:UIColor.secondaryLabelColor}];
    }
    if (self.testing) return;
    [[UIColor colorWithRed:.4 green:.85 blue:.7 alpha:.12] setFill];
    CGFloat radius=MIN(size.width*.4,size.height*.35);
    for (NSNumber* right in @[@NO,@YES]) [[UIBezierPath bezierPathWithOvalInRect:CGRectMake(right.boolValue?size.width-radius:-radius,size.height-radius,radius*2,radius*2)] fill];
    UIBezierPath* border=[UIBezierPath bezierPathWithRoundedRect:CGRectInset(self.bounds,2,2) cornerRadius:22]; CGFloat dash[]={5,5}; [border setLineDash:dash count:2 phase:0]; [[UIColor colorWithRed:.4 green:.85 blue:.7 alpha:.7] setStroke]; [border stroke];
    // Selection belongs above the input view; its border is also drawn on that view's layer.
}
@end

@implementation CaraActionLayoutEditor {
    NSMutableDictionary* draft;
    CaraLayoutCanvas* canvas;
    UISegmentedControl* orientation;
    UISegmentedControl* mode;
    UILabel* help;
    UILabel* output;
    UIStackView* inspector;
    UIScrollView* inspectorScroll;
    NSString* selection;
    UIView* selectionRing;
    UISwitch* enabled;
    NSURL* exportURL;
    CaraInputHub* previewHub;
}
- (NSString*)variant { return orientation.selectedSegmentIndex?@"landscape":@"portrait"; }
- (NSArray*)controls { return draft[self.variant]; }
- (NSDictionary*)selected { for (NSDictionary* c in self.controls) if ([c[@"id"] isEqual:selection]) return c; return nil; }
- (UIButton*)button:(NSString*)title action:(void(^)(void))action {
    UIButtonConfiguration* config=[UIButtonConfiguration tintedButtonConfiguration]; config.title=title; config.cornerStyle=UIButtonConfigurationCornerStyleCapsule;
    UIButton* b=[UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction* a) { action(); }]]; b.titleLabel.font=[UIFont systemFontOfSize:14 weight:UIFontWeightMedium]; return b;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=@"Controller Demapping"; self.overrideUserInterfaceStyle=UIUserInterfaceStyleDark;
    self.view.backgroundColor=[UIColor colorWithRed:.045 green:.05 blue:.065 alpha:1]; self.view.tintColor=[UIColor colorWithRed:1 green:.48 blue:.24 alpha:1];
    draft=[(CaraValidateActionLayout(self.layout,nil)?self.layout:CaraNewActionLayout()) mutableCopy];
    UINavigationBarAppearance* bar=[UINavigationBarAppearance new]; [bar configureWithOpaqueBackground]; bar.backgroundColor=self.view.backgroundColor; bar.titleTextAttributes=@{NSForegroundColorAttributeName:UIColor.whiteColor};
    self.navigationItem.standardAppearance=bar; self.navigationItem.scrollEdgeAppearance=bar; self.navigationItem.compactAppearance=bar;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Save" style:UIBarButtonItemStyleDone target:self action:@selector(save)];
    self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Cancel" style:UIBarButtonItemStylePlain target:self action:@selector(cancel)];
    orientation=[[UISegmentedControl alloc] initWithItems:@[@"Portrait",@"Landscape"]]; orientation.selectedSegmentIndex=0; [orientation addTarget:self action:@selector(refreshVariant) forControlEvents:UIControlEventValueChanged]; [self.view addSubview:orientation];
    mode=[[UISegmentedControl alloc] initWithItems:@[@"Arrange",@"Test touches"]]; mode.selectedSegmentIndex=0; [mode addTarget:self action:@selector(changeMode) forControlEvents:UIControlEventValueChanged]; [self.view addSubview:mode];
    help=[UILabel new]; help.text=@"Drag to move · Drag the corner to resize\nGreen guides show safe edges and approximate thumb reach."; help.numberOfLines=2; help.textAlignment=NSTextAlignmentCenter; help.font=[UIFont systemFontOfSize:12]; help.textColor=UIColor.secondaryLabelColor; [self.view addSubview:help];
    canvas=[[CaraLayoutCanvas alloc] initWithFrame:CGRectZero]; canvas.accessibilityIdentifier=@"Action canvas"; [self.view addSubview:canvas];
    selectionRing=[UIView new]; selectionRing.userInteractionEnabled=NO; selectionRing.layer.borderWidth=2; selectionRing.layer.borderColor=self.view.tintColor.CGColor; selectionRing.layer.cornerRadius=14; [canvas addSubview:selectionRing];
    UILabel* handle=[UILabel new]; handle.text=@"↘"; handle.textAlignment=NSTextAlignmentCenter; handle.backgroundColor=self.view.tintColor; handle.layer.cornerRadius=12; handle.clipsToBounds=YES; handle.frame=CGRectMake(0,0,26,26); handle.autoresizingMask=UIViewAutoresizingFlexibleLeftMargin|UIViewAutoresizingFlexibleTopMargin; [selectionRing addSubview:handle];
    __weak CaraActionLayoutEditor* owner=self;
    canvas.selectedControl=^(NSString* identifier) { CaraActionLayoutEditor* s=owner; if (!s) return; s->selection=identifier; [s refreshInspector]; };
    canvas.movedControl=^(NSString* identifier,CGRect r) { [owner update:@{@"frame":@{@"x":@(r.origin.x),@"y":@(r.origin.y),@"width":@(r.size.width),@"height":@(r.size.height)}}]; };
    output=[UILabel new]; output.font=[UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightMedium]; output.textColor=UIColor.secondaryLabelColor; output.textAlignment=NSTextAlignmentCenter; output.numberOfLines=2; [self.view addSubview:output];
    previewHub=[[CaraInputHub alloc] initWithDefaults:NSUserDefaults.standardUserDefaults observesDevices:NO]; previewHub.active=YES;
    canvas.controlsView.inputChanged=^(NSSet* pressed,CGPoint circle,CGPoint stick) { CaraActionLayoutEditor* s=owner; if (s) [s->previewHub updateSurface:@"preview" buttons:pressed circle:circle cstick:stick touch:CGPointMake(-1,-1)]; };
    previewHub.inputChanged=^(NSSet* pressed,CGPoint circle,CGPoint stick) { CaraActionLayoutEditor* s=owner; if (!s) return; s->output.text=[NSString stringWithFormat:@"%@  •  Move %.2f, %.2f  •  C %.2f, %.2f",[[pressed.allObjects sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@" + "],circle.x,circle.y,stick.x,stick.y]; };
    inspectorScroll=[UIScrollView new]; [self.view addSubview:inspectorScroll]; inspector=[[UIStackView alloc] init]; inspector.axis=UILayoutConstraintAxisVertical; inspector.spacing=10; inspector.translatesAutoresizingMaskIntoConstraints=NO; [inspectorScroll addSubview:inspector];
    [NSLayoutConstraint activateConstraints:@[[inspector.leadingAnchor constraintEqualToAnchor:inspectorScroll.contentLayoutGuide.leadingAnchor constant:4],[inspector.trailingAnchor constraintEqualToAnchor:inspectorScroll.contentLayoutGuide.trailingAnchor constant:-4],[inspector.topAnchor constraintEqualToAnchor:inspectorScroll.contentLayoutGuide.topAnchor],[inspector.bottomAnchor constraintEqualToAnchor:inspectorScroll.contentLayoutGuide.bottomAnchor constant:-16],[inspector.widthAnchor constraintEqualToAnchor:inspectorScroll.frameLayoutGuide.widthAnchor constant:-8]]];
    selection=self.controls.firstObject[@"id"]; [self refreshVariant];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews]; CGRect safe=UIEdgeInsetsInsetRect(self.view.bounds,self.view.safeAreaInsets); CGFloat w=safe.size.width; CGFloat y=safe.origin.y+8;
    CGFloat half=(w-40)/2; orientation.frame=CGRectMake(safe.origin.x+16,y,half,32); mode.frame=CGRectMake(safe.origin.x+24+half,y,half,32); y+=40;
    help.frame=CGRectMake(safe.origin.x+12,y,w-24,36); y+=44;
    BOOL columns=w>=760 || w>safe.size.height*1.3;
    CGFloat inspectorWidth=MIN(330,w*.40),bottom=MIN(230,safe.size.height*.32);
    CGRect area=columns?CGRectMake(safe.origin.x+16,y,w-inspectorWidth-48,MAX(80,CGRectGetMaxY(safe)-y-38)):CGRectMake(safe.origin.x+16,y,w-32,MAX(80,CGRectGetMaxY(safe)-bottom-y-40));
    NSDictionary* rep=canvas.representation; CGSize mapping=CGSizeMake([rep[@"mappingSize"][@"width"] doubleValue],[rep[@"mappingSize"][@"height"] doubleValue]); if (mapping.width<=0) mapping=orientation.selectedSegmentIndex?CGSizeMake(844,390):CGSizeMake(390,844);
    canvas.frame=CaraAspectFit(mapping,area); output.frame=CGRectMake(area.origin.x,CGRectGetMaxY(area)+4,area.size.width,30);
    inspectorScroll.frame=columns?CGRectMake(CGRectGetMaxX(safe)-inspectorWidth-12,y,inspectorWidth,CGRectGetMaxY(safe)-y):CGRectMake(safe.origin.x+12,CGRectGetMaxY(area)+38,w-24,bottom-4); [self positionSelection];
}
- (void)positionSelection {
    canvas.selected=selection; selectionRing.hidden=canvas.testing || !self.selected; if (!self.selected) return;
    CGRect selectionFrame=CaraActionFrame(self.selected,canvas.bounds.size);
    canvas.accessibilityValue=[NSString stringWithFormat:@"%.5f,%.5f,%.5f,%.5f",selectionFrame.origin.x/canvas.bounds.size.width,selectionFrame.origin.y/canvas.bounds.size.height,selectionFrame.size.width/canvas.bounds.size.width,selectionFrame.size.height/canvas.bounds.size.height];
    selectionRing.frame=selectionFrame; UIView* handle=selectionRing.subviews.firstObject; handle.frame=CGRectMake(selectionRing.bounds.size.width-21,selectionRing.bounds.size.height-21,26,26);
}
- (void)refreshVariant {
    [canvas.controlsView reset]; canvas.controls=self.controls; canvas.representation=[(orientation.selectedSegmentIndex==1?(self.landscapeSkin?:self.skin):self.skin) representationForPad:UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad landscape:orientation.selectedSegmentIndex==1];
    if (!self.selected) selection=self.controls.firstObject[@"id"]; [self refreshInspector]; [self.view setNeedsLayout];
}
- (void)changeMode { canvas.testing=mode.selectedSegmentIndex==1; [self positionSelection]; }
- (void)update:(NSDictionary*)changes {
    NSMutableArray* array=[self.controls mutableCopy];
    for (NSUInteger i=0;i<array.count;++i) if ([array[i][@"id"] isEqual:selection]) { NSMutableDictionary* c=[array[i] mutableCopy]; [c addEntriesFromDictionary:changes]; array[i]=c; break; }
    draft[self.variant]=array; canvas.controls=array; [self positionSelection];
}
- (void)refreshInspector {
    for (UIView* v in inspector.arrangedSubviews) { [inspector removeArrangedSubview:v]; [v removeFromSuperview]; }
    __weak CaraActionLayoutEditor* owner=self;
    UIStackView* toolbar=[[UIStackView alloc] init]; toolbar.spacing=8; toolbar.distribution=UIStackViewDistributionFillEqually;
    [toolbar addArrangedSubview:[self button:@"＋ Action" action:^{ [owner addAction]; }]];
    UIButton* more=[self button:@"Layout…" action:^{}];
    more.menu=[UIMenu menuWithChildren:@[
        [UIAction actionWithTitle:@"Import layout…" image:[UIImage systemImageNamed:@"square.and.arrow.down"] identifier:nil handler:^(UIAction* a) { [owner importLayout]; }],
        [UIAction actionWithTitle:@"Export layout…" image:[UIImage systemImageNamed:@"square.and.arrow.up"] identifier:nil handler:^(UIAction* a) { [owner exportLayout]; }],
        [UIAction actionWithTitle:@"Reset this orientation…" image:[UIImage systemImageNamed:@"arrow.counterclockwise"] identifier:nil handler:^(UIAction* a) { [owner resetVariant]; }],
        [UIAction actionWithTitle:@"Restore classic controls" image:[UIImage systemImageNamed:@"gamecontroller"] identifier:nil handler:^(UIAction* a) { CaraActionLayoutEditor* s=owner; if(s) { s->draft[@"enabled"]=@NO; [s refreshInspector]; } }]]]; more.showsMenuAsPrimaryAction=YES; [toolbar addArrangedSubview:more]; [inspector addArrangedSubview:toolbar];
    UIStackView* activation=[UIStackView new]; UILabel* label=[UILabel new]; label.text=@"Use action layout for this game"; label.font=[UIFont systemFontOfSize:14 weight:UIFontWeightMedium]; [activation addArrangedSubview:label]; enabled=[UISwitch new]; enabled.on=[draft[@"enabled"] boolValue]; [enabled addTarget:self action:@selector(enableChanged) forControlEvents:UIControlEventValueChanged]; [activation addArrangedSubview:enabled]; [inspector addArrangedSubview:activation];
    NSDictionary* c=self.selected;
    if (c) {
        UIStackView* row=[UIStackView new]; row.spacing=8; row.distribution=UIStackViewDistributionFillEqually;
        [row addArrangedSubview:[self button:[c[@"label"] stringByAppendingString:@" ✎"] action:^{ [owner rename]; }]];
        UIButton* bind=[self button:[@"Bind: " stringByAppendingString:[c[@"binding"] uppercaseString]] action:^{}]; NSMutableArray* actions=[NSMutableArray new];
        NSArray* bindings=[c[@"kind"] isEqual:@"button"]?[Buttons() arrayByAddingObject:@"unbound"]:[c[@"kind"] isEqual:@"dpad"]?@[@"circle",@"cstick",@"dpad",@"unbound"]:@[@"circle",@"cstick",@"unbound"];
        for (NSString* binding in bindings) [actions addObject:[UIAction actionWithTitle:binding.uppercaseString image:nil identifier:nil handler:^(UIAction* a) { [owner update:@{@"binding":binding}]; [owner refreshInspector]; }]];
        bind.menu=[UIMenu menuWithChildren:actions]; bind.showsMenuAsPrimaryAction=YES; [row addArrangedSubview:bind]; [inspector addArrangedSubview:row];
        UIStackView* types=[UIStackView new]; types.spacing=8; types.distribution=UIStackViewDistributionFillEqually;
        UIButton* kind=[self button:c[@"kind"] action:^{}]; NSMutableArray* kinds=[NSMutableArray new];
        for (NSString* k in @[@"button",@"stick",@"floating",@"dpad",@"drag"]) [kinds addObject:[UIAction actionWithTitle:k image:nil identifier:nil handler:^(UIAction* a) { [owner update:@{@"kind":k,@"binding":@"unbound",@"toggle":@NO}]; [owner refreshInspector]; }]];
        kind.menu=[UIMenu menuWithChildren:kinds]; kind.showsMenuAsPrimaryAction=YES; [types addArrangedSubview:kind];
        if ([c[@"kind"] isEqual:@"button"]) [types addArrangedSubview:[self button:[c[@"toggle"] boolValue]?@"Toggle ✓":@"Hold" action:^{ [owner update:@{@"toggle":@(![owner.selected[@"toggle"] boolValue])}]; [owner refreshInspector]; }]];
        [types addArrangedSubview:[self button:@"Remove" action:^{ CaraActionLayoutEditor* s=owner; if (!s) return; NSMutableArray* a=[s.controls mutableCopy]; [a removeObject:s.selected]; s->draft[s.variant]=a; s->selection=nil; [s refreshVariant]; }]]; [inspector addArrangedSubview:types];
        UIStackView* mobile=[UIStackView new]; mobile.spacing=8; mobile.distribution=UIStackViewDistributionFillEqually;
        [mobile addArrangedSubview:[self button:[c[@"hidden"] boolValue]?@"Invisible ✓":@"Visible" action:^{ [owner update:@{@"hidden":@(![owner.selected[@"hidden"] boolValue])}]; [owner refreshInspector]; }]];
        if ([c[@"kind"] isEqual:@"floating"] || [c[@"kind"] isEqual:@"drag"]) {
            UIButton* travel=[self button:[NSString stringWithFormat:@"Travel: %@ pt",c[@"travel"]?:@64] action:^{}]; NSMutableArray* distances=[NSMutableArray new];
            for (NSNumber* distance in @[@32,@48,@64,@96,@128]) [distances addObject:[UIAction actionWithTitle:[NSString stringWithFormat:@"%@ pt to full tilt",distance] image:nil identifier:nil handler:^(UIAction* a) { [owner update:@{@"travel":distance}]; [owner refreshInspector]; }]];
            travel.menu=[UIMenu menuWithChildren:distances]; travel.showsMenuAsPrimaryAction=YES; [mobile addArrangedSubview:travel];
        }
        [inspector addArrangedSubview:mobile];
    }
    UILabel* note=[UILabel new]; note.text=@"Choose bindings to match your game. Invisible zones are outlined only while arranging. Floating / drag zones start wherever you touch; travel sets the distance to full tilt. Lift to center. Toggles release on pause or cancel. Save applies both orientations."; note.numberOfLines=0; note.font=[UIFont systemFontOfSize:12]; note.textColor=UIColor.secondaryLabelColor; [inspector addArrangedSubview:note]; [self positionSelection];
}
- (void)enableChanged { draft[@"enabled"]=@(enabled.on); }
- (void)addAction {
    if (self.controls.count>=24) return;
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Add an action" message:@"Name the action first, then choose its 3DS input." preferredStyle:UIAlertControllerStyleActionSheet];
    [alert addAction:[UIAlertAction actionWithTitle:@"Invisible Move zone · Circle Pad" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { [self addHiddenZone:NO]; }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Invisible Look zone · C-stick" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { [self addHiddenZone:YES]; }]];
    for (NSString* name in @[@"Fire",@"Jump",@"Interact",@"Aim",@"Move",@"Look",@"Run",@"Reload",@"Crouch",@"Map",@"Custom action"]) [alert addAction:[UIAlertAction actionWithTitle:name style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) {
        NSString* kind=([name isEqual:@"Move"] || [name isEqual:@"Look"])?@"stick":@"button"; NSDictionary* c=Control(name,kind,@"unbound",.4,.65,.18,.15);
        self->draft[self.variant]=[self.controls arrayByAddingObject:c]; self->selection=c[@"id"]; [self refreshVariant]; if ([name isEqual:@"Custom action"]) [self rename];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; alert.popoverPresentationController.sourceView=inspector; alert.popoverPresentationController.sourceRect=inspector.bounds; [self presentViewController:alert animated:YES completion:nil];
}
- (void)addHiddenZone:(BOOL)look {
    NSMutableDictionary* c=[Control(look?@"Look":@"Move",@"floating",look?@"cstick":@"circle",look?.52:.02,.3,.46,.68) mutableCopy]; c[@"hidden"]=@YES;
    draft[self.variant]=[self.controls arrayByAddingObject:c]; selection=c[@"id"]; [self refreshVariant];
}
- (void)rename {
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Action name" message:@"Visible on the control. Maximum 32 characters." preferredStyle:UIAlertControllerStyleAlert]; [alert addTextFieldWithConfigurationHandler:^(UITextField* f) { f.text=self.selected[@"label"]; f.autocapitalizationType=UITextAutocapitalizationTypeWords; }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Apply" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { NSString* name=[alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]; if (name.length && name.length<=32) { [self update:@{@"label":name}]; [self refreshInspector]; } }]]; [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; [self presentViewController:alert animated:YES completion:nil];
}
- (void)resetVariant {
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Reset this orientation?" message:@"Replace this draft orientation with unbound starter actions. Your saved layout changes only when you tap Save." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Reset" style:UIAlertActionStyleDestructive handler:^(UIAlertAction* a) { self->draft[self.variant]=CaraNewActionLayout()[self.variant]; self->selection=nil; [self refreshVariant]; }]]; [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; [self presentViewController:alert animated:YES completion:nil];
}
- (void)message:(NSString*)message { UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Controller layout" message:message preferredStyle:UIAlertControllerStyleAlert]; [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]]; [self presentViewController:alert animated:YES completion:nil]; }
- (void)importLayout { UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeJSON] asCopy:YES]; picker.delegate=self; [self presentViewController:picker animated:YES completion:nil]; }
- (void)exportLayout {
    NSError* error=nil; NSData* data=[NSJSONSerialization dataWithJSONObject:draft options:NSJSONWritingPrettyPrinted error:&error]; if (!data) { [self message:error.localizedDescription]; return; }
    NSURL* folder=[[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES]; [NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil]; exportURL=[folder URLByAppendingPathComponent:@"Cara-controller-v1.json"];
    if (![data writeToURL:exportURL options:NSDataWritingAtomic error:&error]) { [self message:error.localizedDescription]; return; }
    UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForExportingURLs:@[exportURL] asCopy:YES]; [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController*)picker didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url=urls.firstObject; BOOL scoped=[url startAccessingSecurityScopedResource]; NSError* error=nil; NSNumber* size=nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:&error];
    NSData* data=size && size.unsignedLongLongValue<=262144?[NSData dataWithContentsOfURL:url options:0 error:&error]:nil; id layout=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:&error]:nil;
    if (scoped) [url stopAccessingSecurityScopedResource];
    if (!CaraValidateActionLayout(layout,&error)) { [self message:error.localizedDescription]; return; }
    draft=[layout mutableCopy]; selection=nil; [self refreshVariant];
}
- (void)save { NSError* error=nil; if (!CaraValidateActionLayout(draft,&error)) { [self message:error.localizedDescription]; return; } [canvas.controlsView reset]; if (self.saved) self.saved([draft copy]); [self.navigationController popViewControllerAnimated:YES]; }
- (void)cancel { [canvas.controlsView reset]; [self.navigationController popViewControllerAnimated:YES]; }
- (void)viewWillDisappear:(BOOL)animated { [super viewWillDisappear:animated]; [canvas.controlsView reset]; }
@end
