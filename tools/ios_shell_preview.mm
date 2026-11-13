// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free UIKit fixture host. This is not compiled into the Cara app.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "ios/shell_controllers.h"
#import "ios/skin_controls.h"
#include "ios/session_presentation.h"
#include "ios/play_profile.h"
#include <array>
#include <cstring>

static NSString* Scenario() { return NSProcessInfo.processInfo.environment[@"CARA_UI_SCENE"]?:@"library"; }
static void FixtureGame(NSURL* url,NSString* title,unsigned seed) {
    NSMutableData* file=[NSMutableData dataWithLength:512+512+0x36c0]; auto* b=(uint8_t*)file.mutableBytes;
    memcpy(b+0x100,"NCCH",4); b[0x18f]=4; b[0x1a0]=1;
    memcpy(b+512,"icon",4); b[512+12]=0xc0; b[512+13]=0x36;
    auto* smdh=b+1024; memcpy(smdh,"SMDH",4);
    NSData* text=[title dataUsingEncoding:NSUTF16LittleEndianStringEncoding]; memcpy(smdh+0x288,text.bytes,MIN(text.length,254u));
    for (unsigned y=0;y<48;++y) for (unsigned x=0;x<48;++x) {
        unsigned morton=0; for (unsigned n=0;n<3;++n) { morton|=((x>>n)&1)<<(2*n); morton|=((y>>n)&1)<<(2*n+1); }
        unsigned index=((y/8)*6+x/8)*64+morton;
        unsigned r=(seed*7+x/2+y/4)%32, g=(seed*13+y+x/3)%64, blue=(seed*5+x/3)%32;
        if ((x-24)*(x-24)+(y-24)*(y-24)<110) { r=31;g=45;blue=18; }
        unsigned color=(r<<11)|(g<<5)|blue; smdh[0x24c0+index*2]=color&255; smdh[0x24c0+index*2+1]=color>>8;
    }
    [file writeToURL:url atomically:YES];
}

@interface PreviewRoot : UIViewController <UIDocumentPickerDelegate>
@end
@implementation PreviewRoot {
    CaraLibraryController* library;
    CaraLibraryStore* store;
    CaraSettingsController* settings;
    NSURL* fixture;
    unsigned toggleActions;
    CaraSkinControls* previewControls;
    UIImageView* previewArtwork;
    CaraStatsView* previewStats;
    CaraSessionIndicator* previewStatus;
    NSArray<UIView*>* previewScreens;
    NSDictionary* previewRepresentation;
    BOOL fixtureFixed,fixtureLimit;
    NSUInteger fixtureClock,fixtureMaximum;
}
- (BOOL)prefersStatusBarHidden { return [Scenario() containsString:@"skin"]; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return [Scenario() containsString:@"landscape"]?UIInterfaceOrientationMaskLandscape:UIInterfaceOrientationMaskPortrait; }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor=UIColor.blackColor;
    fixtureFixed=NSProcessInfo.processInfo.environment[@"CARA_UI_FIXED_CPU"]!=nil;
    fixtureClock=fixtureFixed?[NSProcessInfo.processInfo.environment[@"CARA_UI_FIXED_CPU"] integerValue]:100;
    fixtureLimit=![NSProcessInfo.processInfo.environment[@"CARA_UI_UNCAPPED"] isEqual:@"1"]; fixtureMaximum=100;
    self.overrideUserInterfaceStyle=[NSProcessInfo.processInfo.environment[@"CARA_UI_THEME"] isEqual:@"dark"]?UIUserInterfaceStyleDark:UIUserInterfaceStyleLight;
    NSString* folder=[NSTemporaryDirectory() stringByAppendingPathComponent:@"CaraShellFixture"];
    fixture=[NSURL fileURLWithPath:folder isDirectory:YES];
    [NSFileManager.defaultManager removeItemAtURL:fixture error:nil];
    NSUserDefaults* fixtureDefaults=[[NSUserDefaults alloc] initWithSuiteName:@"com.dmikey.cara.shellfixture"]; [fixtureDefaults removePersistentDomainForName:@"com.dmikey.cara.shellfixture"];
    store=[[CaraLibraryStore alloc] initWithDocuments:fixture defaults:fixtureDefaults];
    [store useDefaultFolder];
    if (![Scenario() isEqual:@"empty"]) {
        unsigned seed=1;
        for (NSString* title in @[@"Moon Meadow",@"Citrus Valley",@"Little Lantern",@"Starlight Trail",@"Afterglow",@"Garden Notes",@"Pocket Adventure",@"Cloud Harbor"]) FixtureGame([store.defaultFolder URLByAppendingPathComponent:[title stringByAppendingPathExtension:@"3ds"]],title,seed++);
    }
    if ([Scenario() containsString:@"skin"]) { [self showSkin]; return; }
    [store recordPlayed:[store.defaultFolder URLByAppendingPathComponent:@"Moon Meadow.3ds"]];
    library=[CaraLibraryController new]; library.store=store;
    library.syncTitle=[Scenario() isEqual:@"sync-error"]?@"States: Choose a version":@"Saves & states";
    library.syncDetail=[Scenario() isEqual:@"sync-error"]?@"Both versions are preserved. Resolve this in Settings.":@"Saves: Folder updated · States: Waiting for iCloud";
    __weak PreviewRoot* owner=self;
    library.gameFileInUse=^BOOL(NSURL* url) { return [NSProcessInfo.processInfo.environment[@"CARA_UI_FILE_IN_USE"] isEqual:@"1"]; };
    library.showSettings=^{ [owner showSettings]; };
    library.addGame=^{ [owner showFilePicker:NO]; };
    library.chooseFolder=^{ [owner showFilePicker:YES]; };
    library.openGame=^(NSURL* url) { UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Fixture selected" message:url.lastPathComponent preferredStyle:UIAlertControllerStyleAlert]; [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]]; [owner presentViewController:alert animated:YES completion:nil]; };
    UINavigationController* nav=[[UINavigationController alloc] initWithRootViewController:library];
    [self addChildViewController:nav]; nav.view.frame=self.view.bounds; nav.view.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight; [self.view addSubview:nav.view]; [nav didMoveToParentViewController:self];
}
- (void)showFilePicker:(BOOL)folder {
    UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[folder?UTTypeFolder:UTTypeData] asCopy:NO]; picker.delegate=self; [self presentViewController:picker animated:YES completion:nil];
}
- (void)showSettings {
    settings=[CaraSettingsController new]; settings.categorized=![Scenario() isEqual:@"advanced"];
    __weak PreviewRoot* owner=self;
    void(^toggle)(BOOL)=^(BOOL value) { PreviewRoot* root=owner; if (root) ++root->toggleActions; };
    settings.sections=@[
        @{@"title":@"Game library",@"rows":@[@{@"title":@"ROMs folder",@"subtitle":@"Roms",@"symbol":@"folder",@"action":^{ [owner showFilePicker:YES]; }},@{@"title":@"Open game from Files…",@"symbol":@"doc.badge.plus",@"action":^{ [owner showFilePicker:NO]; }}],@"footer":@"Keep games in Cara’s Roms folder, or choose your own location."},
        @{@"title":@"iCloud & Files",@"rows":@[@{@"title":@"In-game saves",@"subtitle":@"Folder updated\nToday, 2:45 PM",@"symbol":@"icloud"},@{@"title":@"Save states",@"subtitle":@"Waiting for iCloud\n2 of 3 files uploaded.",@"symbol":@"icloud"},@{@"title":@"Choose iCloud Drive folder…",@"symbol":@"folder.badge.gearshape",@"action":^{ [owner showFilePicker:YES]; }}],@"footer":@"Choose the same iCloud Drive folder on each device. Save states need a matching Cara build."},
        @{@"title":@"Audio",@"rows":@[@{@"title":@"Sound",@"symbol":@"speaker.wave.2",@"value":@YES,@"change":toggle},@{@"title":@"Audio stretching",@"symbol":@"waveform",@"value":@NO,@"change":toggle}]},
        @{@"title":@"Emulation",@"rows":@[@{@"title":@"Adaptive guest CPU",@"subtitle":@"Adjust the guest clock when needed.",@"symbol":@"speedometer",@"value":@YES,@"change":toggle}]},
        @{@"title":@"Landscape skin",@"rows":@[@{@"title":@"Performance bar",@"subtitle":@"Game FPS, speed, and guest CPU.",@"symbol":@"chart.bar",@"value":@YES,@"change":toggle}]},
        @{@"title":@"Portrait skin",@"rows":@[@{@"title":@"Performance bar",@"subtitle":@"Independent of landscape.",@"symbol":@"chart.bar",@"value":@NO,@"change":toggle}]},
    ];
    UINavigationController* nav=[[UINavigationController alloc] initWithRootViewController:settings]; nav.modalPresentationStyle=settings.categorized?UIModalPresentationFullScreen:UIModalPresentationPageSheet; nav.view.tintColor=[UIColor systemTealColor];
    nav.sheetPresentationController.detents=@[UISheetPresentationControllerDetent.largeDetent]; nav.sheetPresentationController.prefersGrabberVisible=YES; nav.sheetPresentationController.preferredCornerRadius=28; nav.presentationController.delegate=settings;
    [self presentViewController:nav animated:YES completion:nil];
}
- (NSDictionary*)advancedFixtureSection {
    __weak PreviewRoot* owner=self;
    BOOL accuracy=[NSProcessInfo.processInfo.environment[@"CARA_UI_MODE"] isEqual:@"accuracy"];
    const auto policy=Cara::MakePlayPolicy(accuracy?Cara::PlayMode::Accuracy:Cara::PlayMode::Performance,unsigned(fixtureMaximum),false,-1,fixtureFixed,unsigned(fixtureClock),fixtureLimit);
    return @{@"title":@"Emulation",@"rows":@[
        @{@"title":@"Use fixed guest clock",@"value":@(accuracy||fixtureFixed),@"enabled":@(!accuracy),@"change":^(BOOL on) { PreviewRoot* root=owner; if (!root) return; root->fixtureFixed=on; [root refreshAdvancedFixture]; }},
        @{@"title":@"Guest clock",@"subtitle":@"25–200% · Normal guest clock: 100%.",@"enabled":@(!accuracy&&fixtureFixed),@"slider":@{@"min":@25,@"max":@200,@"step":@5,@"value":@(policy.guest_clock)},@"sliderChange":^(double value) { PreviewRoot* root=owner; if (root) root->fixtureClock=NSUInteger(value); }},
        @{@"title":@"Limit game speed",@"value":@(policy.limit_speed),@"enabled":@(!accuracy),@"change":^(BOOL on) { PreviewRoot* root=owner; if (!root) return; root->fixtureLimit=on; [root refreshAdvancedFixture]; }},
        @{@"title":@"Maximum speed",@"subtitle":@"50–200% · Default limit: 100%.",@"enabled":@(!accuracy&&policy.limit_speed),@"slider":@{@"min":@50,@"max":@200,@"step":@5,@"value":@(policy.maximum_speed)},@"sliderChange":^(double value) { PreviewRoot* root=owner; if (root) root->fixtureMaximum=NSUInteger(value); }},
    ],@"footer":accuracy?@"Accuracy locks the guest clock at 100% and speed at 100%.":@"Guest clock and game speed are separate controls."};
}
- (void)refreshAdvancedFixture { settings.sections=@[[self advancedFixtureSection]]; [settings refresh]; }
- (void)showSkin {
    NSURL* url=[NSBundle.mainBundle URLForResource:@"CarbonDuo" withExtension:@"deltaskin"];
    CaraSkin* skin=[CaraSkin loadURL:url error:nil];
    CaraSkinControls* controls=[[CaraSkinControls alloc] initWithFrame:CGRectZero]; controls.skin=skin;
    const BOOL landscape=[Scenario() containsString:@"landscape"];
    NSDictionary* rep=[skin strictRepresentationForPad:UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad landscape:landscape]; controls.representation=rep;
    UIImageView* image=[[UIImageView alloc] initWithImage:controls.artwork]; image.contentMode=UIViewContentModeScaleAspectFit; image.frame=self.view.bounds; image.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight; [self.view addSubview:image];
    previewControls=controls; previewArtwork=image; previewRepresentation=rep;
    [self.view addSubview:controls];
    NSMutableArray* screens=[NSMutableArray new];
    for (NSDictionary* screen in rep[@"screens"]) { UIView* game=[UIView new]; [self.view insertSubview:game belowSubview:controls]; [screens addObject:game]; }
    previewScreens=screens; previewStats=[CaraStatsView new]; [self.view addSubview:previewStats];
    previewStatus=[CaraSessionIndicator new]; [self.view addSubview:previewStatus];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!previewRepresentation) return;
    NSDictionary* mapping=previewRepresentation[@"mappingSize"];
    CGRect fitted=CaraAspectFit(CGSizeMake([mapping[@"width"] doubleValue],[mapping[@"height"] doubleValue]),UIEdgeInsetsInsetRect(self.view.bounds,self.view.safeAreaInsets));
    previewArtwork.frame=previewControls.frame=fitted;
    const CGFloat scale=fitted.size.width/[mapping[@"width"] doubleValue];
    const auto convert=[&](CGRect r) { return CGRectMake(fitted.origin.x+r.origin.x*scale,fitted.origin.y+r.origin.y*scale,r.size.width*scale,r.size.height*scale); };
    NSString* phase=NSProcessInfo.processInfo.environment[@"CARA_UI_SESSION"]?:@"running";
    const auto state=[phase isEqual:@"stopped"]?Cara::SessionPresentation::Stopped:[phase isEqual:@"paused"]?Cara::SessionPresentation::Paused:[phase isEqual:@"loading"]?Cara::SessionPresentation::Loading:[phase isEqual:@"error"]?Cara::SessionPresentation::Failed:Cara::SessionPresentation::Running;
    const BOOL inactive=Cara::ClearSessionPresentation(state);
    for (NSUInteger i=0;i<previewScreens.count;++i) {
        previewScreens[i].frame=convert(CaraSkinRect(previewRepresentation[@"screens"][i][@"outputFrame"]));
        previewScreens[i].backgroundColor=inactive?UIColor.blackColor:[UIColor colorWithRed:.07 green:.16 blue:.23 alpha:1];
    }
    CGRect r=CaraSkinRect(previewRepresentation[@"caraStatsFrame"]);
    const CaraHUDLayout hud=CaraSkinHUDLayout(previewRepresentation,fitted);
    previewStats.frame=hud.metrics;
    previewStats.hidden=[NSProcessInfo.processInfo.environment[@"CARA_UI_FPS"] isEqual:@"0"];
    if (inactive) [previewStats showInactiveGuestClock:100 adaptive:YES]; else [previewStats updateGameFPS:30 speed:100 guestClock:75 adaptive:YES];
    const auto light=state==Cara::SessionPresentation::Running?CaraStatusLightGreen:state==Cara::SessionPresentation::Failed?CaraStatusLightRed:state==Cara::SessionPresentation::Stopped?CaraStatusLightDim:CaraStatusLightAmber;
    [previewStatus updateLight:light accessibilityStatus:[NSString stringWithUTF8String:Cara::SessionPresentationLabel(state)]];
    previewStatus.frame=hud.indicator;
    NSAssert([previewStatus.accessibilityValue isEqual:[NSString stringWithUTF8String:Cara::SessionPresentationLabel(state)]],@"The LED must expose its explicit lifecycle status");
    NSAssert(!inactive || [previewStats.accessibilityLabel containsString:@"No live game metrics"],@"Inactive sessions must not claim live FPS or speed");
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if ([Scenario() isEqual:@"settings"] && !self.presentedViewController) [self showSettings];
    if ([Scenario() isEqual:@"advanced"] && !self.presentedViewController) { [self showSettings]; settings.title=@"Advanced"; [self refreshAdvancedFixture]; }
    if ([Scenario() isEqual:@"quick-settings"] && !self.presentedViewController) {
        CaraQuickSettingsController* quick=[CaraQuickSettingsController new]; quick.playMode=@"performance"; quick.showFPS=YES;
        quick.actions=@[@{@"title":@"Library & iCloud",@"symbol":@"folder",@"keepOpen":@YES,@"action":^{ [self showSettings]; }},@{@"title":@"Skins",@"symbol":@"gamecontroller",@"keepOpen":@YES,@"action":^{ [self showSettings]; }},@{@"title":@"Controllers",@"symbol":@"gamecontroller.fill",@"keepOpen":@YES,@"action":^{ [self showSettings]; }},@{@"title":@"Advanced",@"symbol":@"slider.horizontal.3",@"keepOpen":@YES,@"action":^{ [self showSettings]; }}];
        UINavigationController* nav=[[UINavigationController alloc] initWithRootViewController:quick]; nav.modalPresentationStyle=UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad?UIModalPresentationFormSheet:UIModalPresentationPageSheet; nav.preferredContentSize=CGSizeMake(620,850); nav.presentationController.delegate=quick; [self presentViewController:nav animated:YES completion:nil];
    }
    if ([Scenario() containsString:@"landscape"]) {
        UIWindowScene* scene=self.view.window.windowScene;
        [scene requestGeometryUpdateWithPreferences:[[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscapeLeft] errorHandler:^(NSError* error) { NSLog(@"Orientation: %@",error); }];
    }
}
@end

@interface PreviewScene : UIResponder <UIWindowSceneDelegate>
@property(strong,nonatomic) UIWindow* window;
@end
@implementation PreviewScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene]; self.window.overrideUserInterfaceStyle=[NSProcessInfo.processInfo.environment[@"CARA_UI_THEME"] isEqual:@"dark"]?UIUserInterfaceStyleDark:UIUserInterfaceStyleLight; self.window.rootViewController=[PreviewRoot new]; [self.window makeKeyAndVisible];
}
@end
@interface PreviewApp : UIResponder <UIApplicationDelegate>
@end
@implementation PreviewApp
- (UISceneConfiguration*)application:(UIApplication*)application configurationForConnectingSceneSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* config=[[UISceneConfiguration alloc] initWithName:@"Shell test" sessionRole:session.role]; config.delegateClass=PreviewScene.class; return config;
}
@end
int main(int argc,char** argv) { @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(PreviewApp.class)); } }
