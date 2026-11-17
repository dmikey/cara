#import <Cocoa/Cocoa.h>

#include <SDL.h>
#include <string>
#include "cara/native_mac/game_menu.h"
#include "core/core.h"
#include "cara/native_mac/mac_preferences_ui.h"

namespace {
std::string requested_rom;
bool closing_for_open = false;
Cara::NativeGameMenu::StateRequest state_request;
}

@interface CaraGameMenuActions : NSObject
- (void)openROM:(id)sender;
- (void)preferences:(id)sender;
- (void)saveState:(id)sender;
- (void)loadState:(id)sender;
- (BOOL)validateMenuItem:(NSMenuItem*)item;
@end

@implementation CaraGameMenuActions
- (void)openROM:(id)sender {
    (void)sender;
    if (Cara::MacUI::HandleStartupOpen()) return;
    if (closing_for_open)
        return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Open Nintendo 3DS Game";
    panel.prompt = @"Open";
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedFileTypes = @[@"3ds", @"cci", @"cxi", @"app", @"3dsx", @"elf", @"axf"];
    if ([panel runModal] != NSModalResponseOK || !panel.URL.isFileURL)
        return;
    const char* path = panel.URL.fileSystemRepresentation;
    if (!path || !path[0])
        return;
    // SDL_QUIT follows the normal event-loop shutdown path. Never replace a
    // live Core, cache, queue owner, or GL context from an AppKit callback.
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    if (SDL_PushEvent(&quit) == 1) {
        requested_rom = path;
        closing_for_open = true;
    } else {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Could not switch games";
        alert.informativeText = @"Close the game window, then open Cara again.";
        [alert runModal];
        [alert release];
    }
}
- (void)preferences:(id)sender { (void)sender; Cara::MacUI::ShowPreferences(); }
- (void)saveState:(id)sender { Cara::NativeGameMenu::RequestState(Cara::NativeGameMenu::StateAction::Save, (unsigned)[sender tag]); }
- (void)loadState:(id)sender { Cara::NativeGameMenu::RequestState(Cara::NativeGameMenu::StateAction::Load, (unsigned)[sender tag]); }
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(saveState:) || item.action == @selector(loadState:))
        return Core::System::GetInstance().IsPoweredOn() && !closing_for_open;
    return YES;
}
@end

namespace Cara::NativeGameMenu {
bool RequestState(StateAction action, unsigned slot) {
    if (!Core::System::GetInstance().IsPoweredOn() || closing_for_open || slot < 1 || slot > 10 || action == StateAction::None || state_request.action != StateAction::None) return false;
    state_request = {action, slot};
    return true;
}
StateRequest TakeStateRequest() { auto result = state_request; state_request = {}; return result; }
void StateFailure(const std::string& message) {
    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    alert.messageText = @"Save state unavailable";
    alert.informativeText = [NSString stringWithUTF8String:message.c_str()];
    [alert runModal];
}
void Install() {
    NSMenu* main_menu = NSApp.mainMenu;
    if (!main_menu)
        return;
    static CaraGameMenuActions* actions = [[CaraGameMenuActions alloc] init];
    NSMenu* app = [main_menu itemAtIndex:0].submenu;
    if (app && ![app itemWithTag:7101]) {
        NSMenuItem* preferences = [[[NSMenuItem alloc] initWithTitle:@"Settings…" action:@selector(preferences:) keyEquivalent:@","] autorelease];
        preferences.target=actions; preferences.tag=7101;
        [app insertItem:preferences atIndex:MIN((NSInteger)2,app.numberOfItems)];
    }
    if ([main_menu itemWithTag:7102]) return;

    NSMenuItem* root = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    NSMenu* file = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem* open = [[NSMenuItem alloc] initWithTitle:@"Open…" action:@selector(openROM:)
                                         keyEquivalent:@"o"];
    open.target = actions;
    [file addItem:open];
    [file addItem:[NSMenuItem separatorItem]];
    for (int operation = 0; operation < 2; ++operation) {
        NSMenuItem* group = [[[NSMenuItem alloc] initWithTitle:operation ? @"Load State" : @"Save State" action:nil keyEquivalent:@""] autorelease];
        NSMenu* slots = [[[NSMenu alloc] initWithTitle:group.title] autorelease];
        for (unsigned slot = 1; slot <= 10; ++slot) {
            NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Slot %u", slot] action:operation ? @selector(loadState:) : @selector(saveState:) keyEquivalent:slot == 1 ? (operation ? @"l" : @"s") : @""] autorelease];
            item.target = actions; item.tag = slot;
            item.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
            [slots addItem:item];
        }
        group.submenu = slots; [file addItem:group];
    }
    root.submenu = file; root.tag=7102;
    [main_menu insertItem:root atIndex:MIN((NSInteger)1, main_menu.numberOfItems)];
    [open release];
    [file release];
    [root release];
}

std::string TakeRequestedROM() {
    std::string result = std::move(requested_rom);
    requested_rom.clear();
    closing_for_open = false;
    return result;
}
}
