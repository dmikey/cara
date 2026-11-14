// SPDX-License-Identifier: GPL-2.0-or-later
#import <XCTest/XCTest.h>
@interface CaraNetworkUISmoke : XCTestCase
@end
@implementation CaraNetworkUISmoke
- (XCUIApplication*)openFixture {
    self.continueAfterFailure=NO;
    XCUIApplication* app=[[XCUIApplication alloc] initWithBundleIdentifier:@"com.dmikey.cara.networkpreview"];
    [app launch]; XCTAssertTrue([app.buttons[@"fixture.network.open"] waitForExistenceWithTimeout:10]);
    [app.buttons[@"fixture.network.open"] tap];
    XCTAssertTrue([app.textFields[@"cara.network.endpoint"] waitForExistenceWithTimeout:5]);
    return app;
}
- (XCUIElement*)action:(XCUIApplication*)app name:(NSString*)identifier {
    // iOS27 exposes UITableViewCell button traits with a legacy Cell type.
    return [[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:identifier].firstMatch;
}
- (void)replace:(XCUIElement*)field text:(NSString*)text {
    [field tap];
    // A real default value may equal the placeholder; use the visible clear control.
    XCUIElement* clear=field.buttons[@"Clear text"];
    if(clear.exists) { [clear tap]; [field tap]; }
    [field typeText:text];
    if(field.elementType!=XCUIElementTypeSecureTextField) XCTAssertEqualObjects(field.value,text);
}
- (void)ready:(XCUIApplication*)app {
    [self replace:app.textFields[@"cara.network.room"] text:@"  test-room  "];
    [self replace:app.secureTextFields[@"cara.network.password"] text:@"fixture-secret"];
    [app swipeUp];
}
- (void)status:(XCUIApplication*)app equals:(NSString*)expected {
    NSPredicate* predicate=[NSPredicate predicateWithFormat:@"label == %@",expected];
    XCTNSPredicateExpectation* e=[[XCTNSPredicateExpectation alloc] initWithPredicate:predicate object:app.staticTexts[@"cara.network.status"]];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[e] timeout:8],XCTWaiterResultCompleted,@"Actual screen: %@",app.debugDescription);
}
- (void)testRoomFieldsJoinAndDisconnect {
    XCUIApplication* app=[self openFixture];
    XCTAssertEqualObjects(app.textFields[@"cara.network.endpoint"].value,@"wss://carap2p.therad.ninja/relay");
    XCTAssertTrue(app.secureTextFields[@"cara.network.password"].exists);
    [self status:app equals:@"Not connected"];
    [self ready:app]; [[self action:app name:@"cara.network.join"] tap];
    [self status:app equals:@"Connecting (UI fake)"];
    XCTAssertFalse([self action:app name:@"cara.network.join"].enabled);
    XCTAssertTrue([self action:app name:@"cara.network.leave"].enabled);
    XCTAssertEqualObjects(app.secureTextFields[@"cara.network.password"].value,@"Room password");
    [self status:app equals:@"Connected (UI fake)"];
    [[self action:app name:@"cara.network.leave"] tap]; [self status:app equals:@"Disconnected (UI fake)"];
    XCTAssertTrue([self action:app name:@"cara.network.join"].enabled);
    XCTAssertFalse([self action:app name:@"cara.network.leave"].enabled);
    [app.navigationBars.buttons.firstMatch tap];
    XCTAssertTrue([app.staticTexts[@"fixture.network.receipt"].label containsString:@"joins 1 · disconnects 1"]);
}
- (void)testRejectsCredentialsAndQueryIncludingEmptyComponents {
    XCUIApplication* app=[self openFixture];
    [self ready:app];
    for(NSString* endpoint in @[@"wss://@example.test/relay",@"wss://example.test/relay?",@"wss://user:secret@example.test/relay",@"wss://example.test/relay?token=x",@"wss://example.test/relay#"]) {
        [app swipeDown]; [self replace:app.textFields[@"cara.network.endpoint"] text:endpoint]; [app swipeUp];
        [[self action:app name:@"cara.network.join"] tap];
        [self status:app equals:@"Use a secure wss:// server address ending in /relay, without credentials or query parameters."];
        XCTAssertTrue([self action:app name:@"cara.network.join"].enabled);
    }
    [app.navigationBars.buttons.firstMatch tap];
    XCTAssertTrue([app.staticTexts[@"fixture.network.receipt"].label containsString:@"joins 0"]);
}
- (void)testPasswordClearsWhenLeavingAndPendingDisconnectStaysDisconnected {
    XCUIApplication* app=[self openFixture];
    [self ready:app]; [app.navigationBars.buttons.firstMatch tap];
    [app.buttons[@"fixture.network.open"] tap];
    XCTAssertEqualObjects(app.secureTextFields[@"cara.network.password"].value,@"Room password");
    [self ready:app]; [[self action:app name:@"cara.network.join"] tap];
    [self status:app equals:@"Connecting (UI fake)"]; [[self action:app name:@"cara.network.leave"] tap];
    [self status:app equals:@"Disconnected (UI fake)"];
    // Fake completion arrives after five seconds; cancellation must win.
    XCTestExpectation* elapsed=[self expectationWithDescription:@"pending fake completion elapsed"];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,6*NSEC_PER_SEC),dispatch_get_main_queue(),^{[elapsed fulfill];});
    [self waitForExpectationsWithTimeout:8 handler:nil];
    [self status:app equals:@"Disconnected (UI fake)"];
}
@end
