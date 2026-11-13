#import <XCTest/XCTest.h>
@interface CaraUISmoke : XCTestCase
@end
@implementation CaraUISmoke
- (void)shot:(XCUIApplication*)app name:(NSString*)name { XCTAttachment* shot=[XCTAttachment attachmentWithScreenshot:app.screenshot]; shot.name=name; shot.lifetime=XCTAttachmentLifetimeKeepAlways; [self addAttachment:shot]; }
- (void)testActionLayoutEditor {
 self.continueAfterFailure=NO; XCUIApplication* app=[[XCUIApplication alloc] initWithBundleIdentifier:@"com.dmikey.cara.shellpreview"];
 XCUIDevice.sharedDevice.orientation=UIDeviceOrientationPortrait; app.launchEnvironment=@{@"CARA_UI_SCENE":@"actions"}; [app launch];
 XCTAssertTrue([app.navigationBars[@"Controller Demapping"] waitForExistenceWithTimeout:15]);
 [self shot:app name:@"Action editor portrait"];
 [app.buttons[@"＋ Action"] tap]; [app.buttons[@"Invisible Look zone · C-stick"] tap];
 XCTAssertTrue([app.buttons[@"Bind: CSTICK"] waitForExistenceWithTimeout:5]);
 [app.segmentedControls.buttons[@"Test touches"] tap]; [self shot:app name:@"Invisible Look zone in touch test"];
 [app.segmentedControls.buttons[@"Arrange"] tap]; [self shot:app name:@"Invisible Look zone outlined while arranging"];
 [app.segmentedControls.buttons[@"Landscape"] tap]; [app.buttons[@"＋ Action"] tap]; [app.buttons[@"Invisible Move zone · Circle Pad"] tap];
 XCTAssertTrue([app.buttons[@"Bind: CIRCLE"] waitForExistenceWithTimeout:5]); [self shot:app name:@"Independent landscape Move zone"];
 [app.navigationBars.buttons[@"Save"] tap]; XCTAssertTrue([app.navigationBars[@"Fixture game settings"] waitForExistenceWithTimeout:5]);
 [app.staticTexts[@"Controller Demapping"] tap]; XCTAssertTrue([app.navigationBars[@"Controller Demapping"] waitForExistenceWithTimeout:5]);
 // Portrait retains Look while landscape retains Move, across a fresh editor instance.
 XCUIElement* canvas=app.otherElements[@"Action canvas"]; XCTAssertTrue(canvas.exists);
 XCUICoordinate* look=[canvas coordinateWithNormalizedOffset:CGVectorMake(.8,.4)]; [look tap]; XCTAssertTrue(app.buttons[@"Bind: CSTICK"].exists);
 [app.segmentedControls.buttons[@"Landscape"] tap]; [[canvas coordinateWithNormalizedOffset:CGVectorMake(.15,.4)] tap]; XCTAssertTrue(app.buttons[@"Bind: CIRCLE"].exists);
 [app.buttons[@"Layout…"] tap]; [app.buttons[@"Reset this orientation…"] tap]; XCTAssertTrue([app.alerts[@"Reset this orientation?"] waitForExistenceWithTimeout:5]); [app.alerts.buttons[@"Cancel"] tap];
 [app.navigationBars.buttons[@"Cancel"] tap]; XCTAssertTrue([app.navigationBars[@"Fixture game settings"] waitForExistenceWithTimeout:5]);
}
- (void)testActionDragResizeAndRestore {
 self.continueAfterFailure=NO; XCUIApplication* app=[[XCUIApplication alloc] initWithBundleIdentifier:@"com.dmikey.cara.shellpreview"];
 XCUIDevice.sharedDevice.orientation=UIDeviceOrientationPortrait; app.launchEnvironment=@{@"CARA_UI_SCENE":@"actions",@"CARA_UI_CHECK_GEOMETRY":@"1"}; [app launch];
 XCTAssertTrue([app.navigationBars[@"Controller Demapping"] waitForExistenceWithTimeout:15]);
 XCUIElement* canvas=app.otherElements[@"Action canvas"];
 [[canvas coordinateWithNormalizedOffset:CGVectorMake(.19,.78)] pressForDuration:.2 thenDragToCoordinate:[canvas coordinateWithNormalizedOffset:CGVectorMake(.32,.78)]];
 // Read the exposed selection frame so the corner gesture remains valid at both device sizes.
 NSArray* values=[canvas.value componentsSeparatedByString:@","]; XCTAssertEqual(values.count,4);
 double x=[values[0] doubleValue], y=[values[1] doubleValue], w=[values[2] doubleValue], h=[values[3] doubleValue];
 XCTAssertGreaterThan(x,.06);
 [[canvas coordinateWithNormalizedOffset:CGVectorMake(x+w-.005,y+h-.005)] pressForDuration:.2 thenDragToCoordinate:[canvas coordinateWithNormalizedOffset:CGVectorMake(MIN(.95,x+w+.12),MIN(.98,y+h+.08))]];
 [self shot:app name:@"Dragged and resized action control"];
 [app.switches.firstMatch tap]; [app.navigationBars.buttons[@"Save"] tap]; XCTAssertTrue([app.navigationBars[@"Fixture game settings"] waitForExistenceWithTimeout:5]);
 [app.staticTexts[@"Controller Demapping"] tap]; XCTAssertTrue([app.navigationBars[@"Controller Demapping"] waitForExistenceWithTimeout:5]); XCTAssertEqualObjects(app.switches.firstMatch.value,@"1");
 [app.buttons[@"Layout…"] tap]; [app.buttons[@"Restore classic controls"] tap]; XCTAssertEqualObjects(app.switches.firstMatch.value,@"0");
 // Cancel discards that restore; the saved enabled layout must remain enabled.
 [app.navigationBars.buttons[@"Cancel"] tap]; [app.staticTexts[@"Controller Demapping"] tap]; XCTAssertTrue([app.navigationBars[@"Controller Demapping"] waitForExistenceWithTimeout:5]); XCTAssertEqualObjects(app.switches.firstMatch.value,@"1");
 XCUIDevice.sharedDevice.orientation=UIDeviceOrientationLandscapeLeft; [app.segmentedControls.buttons[@"Landscape"] tap]; [self shot:app name:@"Editor in physical landscape"];
 XCUIDevice.sharedDevice.orientation=UIDeviceOrientationPortrait;
}
- (void)testCollectionDetailsAndFavorites {
 self.continueAfterFailure=NO; XCUIApplication* app=[[XCUIApplication alloc] initWithBundleIdentifier:@"com.dmikey.cara.shellpreview"]; XCUIDevice.sharedDevice.orientation=UIDeviceOrientationPortrait; app.launchEnvironment=@{@"CARA_UI_SCENE":@"library",@"CARA_MENU_SOUND_SELF_TEST":@"1"}; [app launch];
 XCTAssertTrue([app.buttons[@"Settings"] waitForExistenceWithTimeout:15]);
 [self shot:app name:@"Collection with embedded fixture icons"];
 XCUIElement* game=app.collectionViews.cells.firstMatch; XCTAssertTrue([game waitForExistenceWithTimeout:10]); [game tap];
 XCTAssertTrue([app.navigationBars[@"Game details"] waitForExistenceWithTimeout:5]);
 XCTAssertTrue(app.staticTexts[@"No save states on this device"].exists);
 [app.switches[@"Favorite"] tap]; [self shot:app name:@"Details and real empty states"];
 [app.buttons[@"Done"] tap]; [app.buttons[@"Favorites"] tap]; XCTAssertEqual(app.collectionViews.cells.count,1);
 [app.buttons[@"Collection"] tap]; [app.collectionViews.cells.firstMatch pressForDuration:1.2]; XCTAssertTrue([app.buttons[@"Remove from library…"] waitForExistenceWithTimeout:5]); [app.buttons[@"Remove from library…"] tap];
 XCTAssertTrue([app.alerts[@"Remove from library?"] waitForExistenceWithTimeout:5]); [self shot:app name:@"Native removal confirmation preserves file and saves"]; [app.alerts.buttons[@"Cancel"] tap]; XCTAssertTrue(app.staticTexts[@"Your collection · 8"].exists);
 [app.buttons[@"Settings"] tap]; if(!app.buttons[@"Appearance"].exists) [app.buttons[@"Categories"] tap]; XCTAssertTrue([app.buttons[@"Appearance"] waitForExistenceWithTimeout:5]); [app.buttons[@"Appearance"] tap]; XCTAssertTrue([app.staticTexts[@"Light"] waitForExistenceWithTimeout:5]); [app.staticTexts[@"Light"] tap]; [self shot:app name:@"Full appearance settings"];
 XCUIElement* sounds=app.switches[@"Menu sounds"]; if(!sounds.hittable) [app.tables.firstMatch swipeUp]; XCTAssertTrue(sounds.exists); NSString* original=[sounds.value description]; [sounds tap]; XCTAssertNotEqualObjects([sounds.value description],original); [sounds tap]; XCTAssertEqualObjects([sounds.value description],original);
}
- (void)testDeleteInUseIsBlocked {
 self.continueAfterFailure=NO; XCUIApplication* app=[[XCUIApplication alloc] initWithBundleIdentifier:@"com.dmikey.cara.shellpreview"];
 XCUIDevice.sharedDevice.orientation=UIDeviceOrientationPortrait;
 app.launchEnvironment=@{@"CARA_UI_SCENE":@"library",@"CARA_MENU_SOUND_SELF_TEST":@"1",@"CARA_UI_FILE_IN_USE":@"1"}; [app launch];
 XCUIElement* game=app.collectionViews.cells.firstMatch; XCTAssertTrue([game waitForExistenceWithTimeout:15]); [game pressForDuration:1.2];
 XCTAssertTrue([app.buttons[@"Delete game file…"] waitForExistenceWithTimeout:5]); [app.buttons[@"Delete game file…"] tap];
 XCTAssertTrue([app.alerts[@"Stop this game first"] waitForExistenceWithTimeout:5]); XCTAssertFalse(app.alerts[@"Delete game file?"].exists);
 [self shot:app name:@"Live game file deletion is blocked"];
 [app.alerts.buttons[@"OK"] tap]; XCTAssertTrue(app.staticTexts[@"Your collection · 8"].exists);
}
@end
