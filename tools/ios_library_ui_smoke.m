#import <XCTest/XCTest.h>
@interface CaraUISmoke : XCTestCase
@end
@implementation CaraUISmoke
- (void)shot:(XCUIApplication*)app name:(NSString*)name { XCTAttachment* shot=[XCTAttachment attachmentWithScreenshot:app.screenshot]; shot.name=name; shot.lifetime=XCTAttachmentLifetimeKeepAlways; [self addAttachment:shot]; }
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
@end
