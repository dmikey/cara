// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free UI-only fake. No transport, sockets, engine or game services are linked.
#import <UIKit/UIKit.h>
#import "ios/network_controller.h"

static UIView* Find(UIView* view, NSString* identifier) {
    if ([view.accessibilityIdentifier isEqual:identifier]) return view;
    for (UIView* child in view.subviews) { UIView* found=Find(child,identifier); if(found) return found; }
    return nil;
}
@interface NetworkFixture : UIViewController
@end
@implementation NetworkFixture {
    BOOL active;
    NSString* detail;
    NSUInteger joins, leaves, generation;
    UILabel* receipt;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=@"Network UI fixture"; self.view.backgroundColor=UIColor.systemBackgroundColor;
    detail=@"Not connected";
    UIButton* open=[UIButton buttonWithType:UIButtonTypeSystem];
    [open setTitle:@"Open rooms" forState:UIControlStateNormal];
    open.accessibilityIdentifier=@"fixture.network.open";
    [open addTarget:self action:@selector(openRooms) forControlEvents:UIControlEventTouchUpInside];
    receipt=[UILabel new]; receipt.numberOfLines=0; receipt.accessibilityIdentifier=@"fixture.network.receipt";
    UIStackView* stack=[[UIStackView alloc] initWithArrangedSubviews:@[open,receipt]];
    stack.axis=UILayoutConstraintAxisVertical; stack.spacing=20;
    stack.translatesAutoresizingMaskIntoConstraints=NO; [self.view addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[[stack.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.leadingAnchor constraintGreaterThanOrEqualToAnchor:self.view.leadingAnchor constant:20]]];
    [self updateReceipt];
}
- (void)updateReceipt { receipt.text=[NSString stringWithFormat:@"UI fake only · joins %lu · disconnects %lu",(unsigned long)joins,(unsigned long)leaves]; }
- (void)openRooms {
    CaraNetworkController* rooms=[CaraNetworkController new];
    __weak NetworkFixture* owner=self;
    __weak CaraNetworkController* weakRooms=rooms;
    rooms.connectionStatus=^NSDictionary* { NetworkFixture* f=owner;
        return f?@{@"active":@(f->active),@"detail":f->detail}:@{}; };
    rooms.joinRoom=^(NSString* endpoint,NSString* room,NSString* password) {
        NetworkFixture* f=owner; if(!f)return;
        NSCAssert(!f->active,@"Duplicate join while active");
        NSCAssert([endpoint isEqual:@"wss://carap2p.therad.ninja/relay"],@"Unexpected fixture endpoint");
        NSCAssert([room isEqual:@"test-room"],@"Room whitespace not trimmed");
        NSCAssert([password isEqual:@"fixture-secret"],@"Password changed before callback");
        UITextField* field=(UITextField*)Find(weakRooms.view,@"cara.network.password");
        NSCAssert(field && field.text.length==0,@"Password not cleared before callback");
        ++f->joins; f->active=YES; f->detail=@"Connecting (UI fake)"; const NSUInteger token=++f->generation;
        [f updateReceipt];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(),^{
            NetworkFixture* current=owner;
            if(current && current->active && current->generation==token) current->detail=@"Connected (UI fake)";
        });
    };
    rooms.leaveRoom=^{ NetworkFixture* f=owner;if(!f)return;
        NSCAssert(f->active,@"Disconnect called while inactive");
        f->active=NO; ++f->leaves; ++f->generation; f->detail=@"Disconnected (UI fake)"; [f updateReceipt]; };
    [self.navigationController pushViewController:rooms animated:YES];
}
@end
@interface NetworkPreviewScene : UIResponder <UIWindowSceneDelegate>
@property(strong,nonatomic) UIWindow* window;
@end
@implementation NetworkPreviewScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController=[[UINavigationController alloc] initWithRootViewController:[NetworkFixture new]];
    [self.window makeKeyAndVisible];
}
@end
@interface NetworkPreviewApp : UIResponder <UIApplicationDelegate>
@end
@implementation NetworkPreviewApp
- (UISceneConfiguration*)application:(UIApplication*)application configurationForConnectingSceneSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* configuration=[[UISceneConfiguration alloc] initWithName:@"Network UI fixture" sessionRole:session.role];
    configuration.delegateClass=NetworkPreviewScene.class; return configuration;
}
@end
int main(int argc,char** argv) { @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(NetworkPreviewApp.class)); } }
