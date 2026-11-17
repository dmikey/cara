#import <Cocoa/Cocoa.h>

#include <SDL.h>
#include <string>
#include "cara/native_mac/game_menu.h"

namespace {
std::string requested_rom;
bool closing_for_open = false;
}

@interface CaraGameMenuActions : NSObject
- (void)openROM:(id)sender;
@end

@implementation CaraGameMenuActions
- (void)openROM:(id)sender {
    (void)sender;
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
@end

namespace Cara::NativeGameMenu {
void Install() {
    NSMenu* main_menu = NSApp.mainMenu;
    if (!main_menu)
        return;
    static CaraGameMenuActions* actions = [[CaraGameMenuActions alloc] init];
    NSMenuItem* root = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
    NSMenu* file = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem* open = [[NSMenuItem alloc] initWithTitle:@"Open…" action:@selector(openROM:)
                                         keyEquivalent:@"o"];
    open.target = actions;
    [file addItem:open];
    root.submenu = file;
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
