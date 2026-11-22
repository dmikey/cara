// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#import <Cocoa/Cocoa.h>
#include <SDL.h>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <memory>
#include <utility>
#include "cara/native_mac/mac_preferences.h"
#include "cara/native_mac/mac_preferences_ui.h"
#include "cara/native_mac/game_menu.h"
#include "input_common/main.h"
#include "input_common/keyboard.h"
namespace {
using Cara::MacPreferences::DisplayScale;
using Cara::MacPreferences::Model;
using Cara::MacPreferences::PlaySettings;
using Cara::PlayMode;
std::optional<PlaySettings> saved_play_settings;
bool preferences_closed = false;
NSWindow* welcome_window=nil;
NSWindow* settings_window=nil;
std::string startup_rom;
std::string Read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
std::filesystem::path ConfigPath(){const char* p=std::getenv("CARA_USER_DIR");if(!p||p[0]!='/')throw std::runtime_error("Choose a valid Cara data directory");return std::filesystem::path(p)/"config/sdl2-config.ini";}
NSString* Text(const std::string& s){return [NSString stringWithUTF8String:s.c_str()]?:@"";}
std::string String(NSString* s){return s.UTF8String?:"";}
void Alert(NSString* title,NSString* detail){NSAlert* a=[[[NSAlert alloc]init]autorelease];a.messageText=title;a.informativeText=detail;[a runModal];}
NSTextField* Label(NSString* text,NSRect frame,CGFloat size=13){auto* f=[NSTextField labelWithString:text];f.frame=frame;f.font=[NSFont systemFontOfSize:size];f.lineBreakMode=NSLineBreakByWordWrapping;return f;}
NSButton* Button(NSString* text,NSRect frame,id target,SEL action){auto* b=[NSButton buttonWithTitle:text target:target action:action];b.frame=frame;b.bezelStyle=NSBezelStyleRounded;return b;}
std::string PickGame(){auto* p=[NSOpenPanel openPanel];p.title=@"Open Nintendo 3DS Game";p.prompt=@"Open";p.canChooseDirectories=NO;p.allowsMultipleSelection=NO;p.allowedFileTypes=@[@"3ds",@"z3ds",@"zcci",@"7z", @"zip",@"cci",@"cxi",@"app",@"3dsx",@"elf",@"axf"];return [p runModal]==NSModalResponseOK&&p.URL.isFileURL?std::string(p.URL.fileSystemRepresentation):"";}
std::string Description(const std::string& value){Common::ParamPackage p(value);const auto engine=p.Get("engine","");
 if(engine=="keyboard"){const int code=p.Get("code",0);const char* name=SDL_GetScancodeName(SDL_Scancode(code));return name&&*name?name:"Unbound";}
 if(engine=="sdl"){if(p.Has("axis_x"))return "Controller stick · axes "+p.Get("axis_x","")+" / "+p.Get("axis_y","");if(p.Has("button"))return "Controller button "+p.Get("button","");if(p.Has("hat"))return "Controller D-pad "+p.Get("direction","");return "Controller axis "+p.Get("axis","")+" "+p.Get("direction","");}
 return "Unbound";
}
SDL_Scancode Key(NSEvent* e){
 // Physical macOS virtual key positions, independent of typed text/Shift.
 static const std::map<unsigned,SDL_Scancode> keys={{0,SDL_SCANCODE_A},{1,SDL_SCANCODE_S},{2,SDL_SCANCODE_D},{3,SDL_SCANCODE_F},{4,SDL_SCANCODE_H},{5,SDL_SCANCODE_G},{6,SDL_SCANCODE_Z},{7,SDL_SCANCODE_X},{8,SDL_SCANCODE_C},{9,SDL_SCANCODE_V},{11,SDL_SCANCODE_B},{12,SDL_SCANCODE_Q},{13,SDL_SCANCODE_W},{14,SDL_SCANCODE_E},{15,SDL_SCANCODE_R},{16,SDL_SCANCODE_Y},{17,SDL_SCANCODE_T},{18,SDL_SCANCODE_1},{19,SDL_SCANCODE_2},{20,SDL_SCANCODE_3},{21,SDL_SCANCODE_4},{22,SDL_SCANCODE_6},{23,SDL_SCANCODE_5},{24,SDL_SCANCODE_EQUALS},{25,SDL_SCANCODE_9},{26,SDL_SCANCODE_7},{27,SDL_SCANCODE_MINUS},{28,SDL_SCANCODE_8},{29,SDL_SCANCODE_0},{30,SDL_SCANCODE_RIGHTBRACKET},{31,SDL_SCANCODE_O},{32,SDL_SCANCODE_U},{33,SDL_SCANCODE_LEFTBRACKET},{34,SDL_SCANCODE_I},{35,SDL_SCANCODE_P},{36,SDL_SCANCODE_RETURN},{37,SDL_SCANCODE_L},{38,SDL_SCANCODE_J},{39,SDL_SCANCODE_APOSTROPHE},{40,SDL_SCANCODE_K},{41,SDL_SCANCODE_SEMICOLON},{42,SDL_SCANCODE_BACKSLASH},{43,SDL_SCANCODE_COMMA},{44,SDL_SCANCODE_SLASH},{45,SDL_SCANCODE_N},{46,SDL_SCANCODE_M},{47,SDL_SCANCODE_PERIOD},{48,SDL_SCANCODE_TAB},{49,SDL_SCANCODE_SPACE},{50,SDL_SCANCODE_GRAVE},{51,SDL_SCANCODE_BACKSPACE},{53,SDL_SCANCODE_ESCAPE},{56,SDL_SCANCODE_LSHIFT},{58,SDL_SCANCODE_LALT},{59,SDL_SCANCODE_LCTRL},{60,SDL_SCANCODE_RSHIFT},{61,SDL_SCANCODE_RALT},{62,SDL_SCANCODE_RCTRL},{123,SDL_SCANCODE_LEFT},{124,SDL_SCANCODE_RIGHT},{125,SDL_SCANCODE_DOWN},{126,SDL_SCANCODE_UP}};
 const auto i=keys.find(e.keyCode);return i==keys.end()?SDL_SCANCODE_UNKNOWN:i->second;
}
}
// Paint the window background as part of the content view as well, so native
// view snapshots contain the same background as the displayed window.
@interface CaraPreferencesRoot:NSView
@end
@implementation CaraPreferencesRoot
-(void)drawRect:(NSRect)rect{[[NSColor windowBackgroundColor]setFill];NSRectFill(rect);[super drawRect:rect];}
@end
@interface CaraPreferencesActions:NSObject<NSTableViewDataSource,NSTableViewDelegate,NSWindowDelegate,NSTabViewDelegate>{
@public std::unique_ptr<Model> model;NSTableView* table;NSPopUpButton* engine;NSTextField* status;NSButton* bind;
NSTabView* tabs;
NSButton* tab_buttons[3];
NSPopUpButton *mode, *scale;
NSButton *fixedClock, *limitSpeed, *frameSkip, *resetTuning, *asyncShaders;
NSSlider *guestClock, *maximumSpeed;
NSTextField *guestClockValue, *maximumSpeedValue, *modeDescription;
NSInteger waiting;id key_monitor;NSTimer* timer;std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> pollers;
}
-(void)change:(id)sender;-(void)save:(id)sender;-(void)cancel:(id)sender;-(void)reset:(id)sender;-(void)tick:(id)sender;
-(void)updatePlayControls;
-(void)selectSettingsTab:(NSButton*)sender;
-(void)changePlayOption:(id)sender;
-(void)restorePlayTuning:(id)sender;
-(void)accept:(const std::string&)value;-(void)stopBinding;-(NSEvent*)handleKey:(NSEvent*)event;
@end
@implementation CaraPreferencesActions
-(id)init {if((self=[super init]))waiting=-1;return self;}
-(NSInteger)numberOfRowsInTableView:(NSTableView*)view{(void)view;return model->buttons.size()+10;}
-(id)tableView:(NSTableView*)view objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row{
 (void)view;const bool name=[column.identifier isEqualToString:@"action"];
 if(row<(NSInteger)model->buttons.size()){const auto& b=model->buttons[row];return Text(name?b.label:Description(b.value));}
 const auto n=row-model->buttons.size();const auto& a=model->analogs[n/5];const unsigned d=n%5;
 const char* dirs[]={"Up","Down","Left","Right"};if(d==4)return Text(name?a.label+" · controller stick":a.use_controller?Description(a.controller):"Keyboard / button directions");
 return Text(name?a.label+" "+dirs[d]:a.use_controller?"Using controller stick":Description(a.directions[d]));
}
-(void)tableViewSelectionDidChange:(NSNotification*)note{(void)note;bind.enabled=table.selectedRow>=0;}
-(NSEvent*)handleKey:(NSEvent*)e{
   if(self->waiting<0||e.modifierFlags&NSEventModifierFlagCommand)return e;
   if(e.keyCode==53){[self stopBinding];self->status.stringValue=@"Binding cancelled.";return nil;}
   const auto n=self->waiting-(NSInteger)self->model->buttons.size();if(n>=0&&n%5==4)return e;
   const auto code=Key(e);if(code!=SDL_SCANCODE_UNKNOWN){[self accept:Cara::MacPreferences::Keyboard(code)];return nil;}return e;
}
-(void)stopBinding{for(auto& p:pollers)p->Stop();pollers.clear();waiting=-1;}
-(void)change:(id)sender{
 (void)sender;[self stopBinding];waiting=table.selectedRow;if(waiting<0)return;
 const bool analog=waiting>=(NSInteger)model->buttons.size()&&(waiting-model->buttons.size())%5==4;
 pollers=InputCommon::Polling::GetPollers(analog?InputCommon::Polling::DeviceType::Analog:InputCommon::Polling::DeviceType::Button);
 for(auto& p:pollers)p->Start();
 status.stringValue=analog?@"Move the stick right, then down. Escape cancels.":@"Press a key or press and release a controller button. Escape cancels.";
}
-(void)accept:(const std::string&)value{
 if(waiting<0)return;const auto row=waiting;
 if(row<(NSInteger)model->buttons.size())model->buttons[row].value=value;
 else{const auto n=row-model->buttons.size();auto& a=model->analogs[n/5];if(n%5==4){a.controller=value;a.use_controller=true;}else{a.directions[n%5]=value;a.use_controller=false;}}
 [self stopBinding];[table reloadData];status.stringValue=@"Binding updated. Save applies controls to your current game.";
}
-(void)tick:(id)sender{
 (void)sender;SDL_PumpEvents();if(waiting<0)return;
 for(auto& p:pollers){auto value=p->GetNextInput();if(value.Has("engine")){[self accept:value.Serialize()];break;}}
}
-(void)updatePlayControls {
    const auto& play = model->play;
    const bool performance = play.mode == PlayMode::Performance;
    [mode selectItemAtIndex:performance ? 1 : 0];
    fixedClock.state = !performance || play.fixed_guest_clock ? NSControlStateValueOn : NSControlStateValueOff;
    limitSpeed.state = !performance || play.limit_speed ? NSControlStateValueOn : NSControlStateValueOff;
    frameSkip.state = play.SkipHostFrames() ? NSControlStateValueOn : NSControlStateValueOff;
    asyncShaders.state = play.UseAsyncMetalShaders() ? NSControlStateValueOn : NSControlStateValueOff;
    asyncShaders.enabled = performance;
    fixedClock.enabled = limitSpeed.enabled = frameSkip.enabled = resetTuning.enabled = performance;
    guestClock.enabled = performance && play.fixed_guest_clock;
    maximumSpeed.enabled = performance && play.limit_speed;
    guestClock.integerValue = performance ? play.guest_clock_percent : 100;
    maximumSpeed.integerValue = performance ? play.maximum_speed_percent : 100;
    guestClock.hidden = !guestClock.enabled;
    maximumSpeed.hidden = !maximumSpeed.enabled;
    guestClockValue.stringValue = performance && !play.fixed_guest_clock
        ? @"Automatic" : [NSString stringWithFormat:@"%ld%%", long(guestClock.integerValue)];
    maximumSpeedValue.stringValue = performance && !play.limit_speed
        ? @"Unlimited" : [NSString stringWithFormat:@"%ld%%", long(maximumSpeed.integerValue)];
    modeDescription.stringValue = performance
        ? @"Cara adjusts the guest clock under load unless you set a fixed clock. Guest GPU timing applies when you open a game."
        : @"Runs the guest clock and speed limit at 100% with adaptive frame skip off.";
    [scale selectItemWithTag:static_cast<NSInteger>(play.display_scale)];
}
-(void)changePlayOption:(id)sender {
    auto& play = model->play;
    if (sender == mode)
        play.mode = mode.indexOfSelectedItem == 0 ? PlayMode::Accuracy : PlayMode::Performance;
    else if (sender == fixedClock)
        play.fixed_guest_clock = fixedClock.state == NSControlStateValueOn;
    else if (sender == limitSpeed)
        play.limit_speed = limitSpeed.state == NSControlStateValueOn;
    else if (sender == frameSkip)
        play.adaptive_frame_skip = frameSkip.state == NSControlStateValueOn;
    else if (sender == asyncShaders)
        play.async_metal_shaders = asyncShaders.state == NSControlStateValueOn;
    else if (sender == guestClock)
        play.guest_clock_percent = 5 * std::lround(guestClock.doubleValue / 5);
    else if (sender == maximumSpeed)
        play.maximum_speed_percent = 5 * std::lround(maximumSpeed.doubleValue / 5);
    else if (sender == scale)
        play.display_scale = static_cast<DisplayScale>(scale.selectedTag);
    [self updatePlayControls];
}
-(void)restorePlayTuning:(id)sender {
    (void)sender;
    auto& play = model->play;
    const auto modeBefore = play.mode;
    const auto scaleBefore = play.display_scale;
    const auto asyncBefore = play.async_metal_shaders;
    play = PlaySettings{};
    play.mode = modeBefore;
    play.display_scale = scaleBefore;
    play.async_metal_shaders = asyncBefore;
    [self updatePlayControls];
}
-(void)selectSettingsTab:(NSButton*)sender {
    const auto index = sender.tag;
    [tabs selectTabViewItemAtIndex:index];
    // Clicking the active control should leave it selected.
    sender.state = NSControlStateValueOn;
}
-(void)tabView:(NSTabView*)tabView didSelectTabViewItem:(NSTabViewItem*)item {
    const auto selected = [tabView indexOfTabViewItem:item];
    for (NSInteger index = 0; index < tabView.numberOfTabViewItems; ++index) {
        auto* control = tab_buttons[index];
        const bool active = index == selected;
        control.state = active ? NSControlStateValueOn : NSControlStateValueOff;
        const auto size = [NSFont systemFontSize];
        control.font = active ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    }
}
-(void)tabView:(NSTabView*)tabView willSelectTabViewItem:(NSTabViewItem*)item {
    (void)tabView;
    if (![item.label isEqualToString:@"Controls"] && waiting >= 0) {
        [self stopBinding];
        status.stringValue = @"Binding cancelled.";
    }
}
-(void)save:(id)sender{
 (void)sender;[self stopBinding];try{
  const auto path=ConfigPath();if(Read(path)!=model->original)throw std::runtime_error("Settings changed elsewhere. Close and reopen Settings to reload them.");
  model->backend=String([engine.selectedItem representedObject]);const auto text=model->Serialize(Cara::MacPreferences::JitAvailable());
  if(std::filesystem::is_symlink(path)||std::filesystem::is_symlink(path.parent_path()))throw std::runtime_error("Settings path must be inside the Cara data directory");
  std::filesystem::create_directories(path.parent_path());NSError* error=nil;
  NSData* data=[NSData dataWithBytes:text.data() length:text.size()];
  if(![data writeToFile:Text(path.string()) options:NSDataWritingAtomic error:&error])throw std::runtime_error(String(error.localizedDescription));
  model->original=text;Settings::ApplyInputProfile(model->Controls(Settings::values.current_input_profile));
  saved_play_settings=model->play;[NSApp stopModalWithCode:NSModalResponseOK];
 }catch(const std::exception& e){Alert(@"Could not save settings",Text(e.what()));}
}
-(void)cancel:(id)sender{(void)sender;[self stopBinding];[NSApp abortModal];}
-(BOOL)windowShouldClose:(NSWindow*)window{(void)window;[self cancel:nil];return NO;}
-(void)reset:(id)sender{(void)sender;[self stopBinding];const auto original=model->original;const auto backend=model->backend;const auto play=model->play;model=std::make_unique<Model>();model->original=original;model->backend=backend;model->play=play;[table reloadData];status.stringValue=@"Default bindings restored. Save to keep them.";}
@end
namespace {
NSView* PreferencesTab(NSTabView* tabs, NSString* title) {
    auto* item = [[[NSTabViewItem alloc] initWithIdentifier:title] autorelease];
    item.label = title;
    item.view = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 560, 321)] autorelease];
    [tabs addTabViewItem:item];
    return item.view;
}
NSButton* PlayCheckbox(NSString* title, NSRect frame, CaraPreferencesActions* actions) {
    auto* button = [NSButton checkboxWithTitle:title target:actions action:@selector(changePlayOption:)];
    button.frame = frame;
    return button;
}
NSSlider* PlaySlider(NSString* title, CGFloat y, double minimum, CaraPreferencesActions* actions) {
    auto* slider = [NSSlider sliderWithValue:100 minValue:minimum maxValue:200
                                      target:actions action:@selector(changePlayOption:)];
    slider.frame = NSMakeRect(235, y, 242, 22);
    slider.continuous = YES;
    slider.numberOfTickMarks = static_cast<NSInteger>((200 - minimum) / 5) + 1;
    slider.allowsTickMarkValuesOnly = YES;
    [slider setAccessibilityLabel:title];
    return slider;
}
CaraPreferencesActions* BuildPreferences(){
 auto* a=[[CaraPreferencesActions alloc]init];a->model=std::make_unique<Model>(Read(ConfigPath()));
 auto* window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,620,439) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];window.title=@"Cara Settings";window.releasedWhenClosed=NO;window.delegate=a;settings_window=window;
 window.contentView=[[[CaraPreferencesRoot alloc]initWithFrame:window.contentView.bounds]autorelease];auto* root=window.contentView;
 a->tabs=[[[NSTabView alloc]initWithFrame:NSMakeRect(18,63,584,326)]autorelease];
 a->tabs.tabViewType=NSNoTabsBezelBorder;
 auto* emulation=PreferencesTab(a->tabs,@"Emulation");
 auto* display=PreferencesTab(a->tabs,@"Display");
 auto* controls=PreferencesTab(a->tabs,@"Controls");
 a->tabs.delegate=a;
 [root addSubview:a->tabs];
 NSArray<NSString*>* tabTitles = @[@"Emulation", @"Display", @"Controls"];
 for (NSInteger index = 0; index < tabTitles.count; ++index) {
     auto* button = Button(tabTitles[index], NSMakeRect(18 + 116 * index, 392, 108, 32),
                           a, @selector(selectSettingsTab:));
     [button setButtonType:NSButtonTypePushOnPushOff];
     button.tag=index;
     a->tab_buttons[index]=button;
     [root addSubview:button];
 }
 [a tabView:a->tabs didSelectTabViewItem:a->tabs.selectedTabViewItem];

 [emulation addSubview:Label(@"CPU engine",NSMakeRect(15,281,125,22))];
 a->engine=[[[NSPopUpButton alloc]initWithFrame:NSMakeRect(141,277,340,30) pullsDown:NO]autorelease];
 [a->engine setAccessibilityLabel:@"CPU engine"];
 for(auto entry:{std::pair{"FastInterp","fastinterp"},std::pair{"JIT (Dynarmic, experimental)","jit"},std::pair{"Interpreter (compatibility)","dyncom"}}){[a->engine addItemWithTitle:Text(entry.first)];a->engine.lastItem.representedObject=Text(entry.second);if(entry.second==std::string("jit"))a->engine.lastItem.enabled=Cara::MacPreferences::JitAvailable();}
 a->engine.menu.autoenablesItems=NO;NSInteger selected=0;for(NSInteger i=0;i<a->engine.numberOfItems;++i)if(String([a->engine itemAtIndex:i].representedObject)==a->model->backend)selected=i;
 [a->engine selectItemAtIndex:selected];[emulation addSubview:a->engine];
 [emulation addSubview:Label(@"CPU engine changes apply to the next game.",NSMakeRect(15,253,522,20))];
 [emulation addSubview:Label(@"Play mode",NSMakeRect(15,209,125,22))];
 a->mode=[[[NSPopUpButton alloc]initWithFrame:NSMakeRect(141,205,194,30) pullsDown:NO]autorelease];
 [a->mode setAccessibilityLabel:@"Play mode"];
 [a->mode addItemsWithTitles:@[@"Accuracy",@"Performance"]];
 a->mode.target=a;a->mode.action=@selector(changePlayOption:);[emulation addSubview:a->mode];
 a->modeDescription=Label(@"",NSMakeRect(15,172,528,28));
 [emulation addSubview:a->modeDescription];

 a->fixedClock=PlayCheckbox(@"Fixed Guest Clock",NSMakeRect(15,134,213,22),a);
 a->guestClock=PlaySlider(@"Guest Clock percentage",134,25,a);
 a->guestClockValue=Label(@"",NSMakeRect(486,134,78,22));
 a->limitSpeed=PlayCheckbox(@"Limit game speed",NSMakeRect(15,101,213,22),a);
 a->maximumSpeed=PlaySlider(@"Game speed limit percentage",101,50,a);
 a->maximumSpeedValue=Label(@"",NSMakeRect(486,101,78,22));
 a->frameSkip=PlayCheckbox(@"Adaptive Frame Skip",NSMakeRect(15,67,220,22),a);
 [emulation addSubview:a->fixedClock];[emulation addSubview:a->guestClock];[emulation addSubview:a->guestClockValue];
 [emulation addSubview:a->limitSpeed];[emulation addSubview:a->maximumSpeed];[emulation addSubview:a->maximumSpeedValue];
 [emulation addSubview:a->frameSkip];
 [emulation addSubview:Label(@"Presents at most one frame per display interval.",NSMakeRect(239,64,301,22))];
 a->resetTuning=Button(@"Reset performance settings",NSMakeRect(15,16,213,32),a,@selector(restorePlayTuning:));
 [emulation addSubview:a->resetTuning];

 [display addSubview:Label(@"Scaling",NSMakeRect(15,281,125,22))];
 a->scale=[[[NSPopUpButton alloc]initWithFrame:NSMakeRect(141,277,194,30) pullsDown:NO]autorelease];
 [a->scale setAccessibilityLabel:@"Display scaling"];
 [a->scale addItemsWithTitles:@[@"Native (Fit)",@"Lossless 2×",@"Lossless 4×"]];
 [a->scale itemAtIndex:0].tag=static_cast<NSInteger>(DisplayScale::Fit);
 [a->scale itemAtIndex:1].tag=static_cast<NSInteger>(DisplayScale::Lossless2x);
 [a->scale itemAtIndex:2].tag=static_cast<NSInteger>(DisplayScale::Lossless4x);
 a->scale.target=a;a->scale.action=@selector(changePlayOption:);[display addSubview:a->scale];
 [display addSubview:Label(@"Lossless modes center the screens at whole pixel multiples. Smaller windows use the largest whole multiple that fits. Below 1×, the screens fit the window.",NSMakeRect(15,205,528,59))];
 a->asyncShaders=PlayCheckbox(@"Async Shaders",NSMakeRect(15,173,220,22),a);
 [display addSubview:a->asyncShaders];
 [display addSubview:Label(@"Build eligible shaders in the background when you open a game with Metal in Performance mode.",NSMakeRect(15,123,528,42))];
 [a updatePlayControls];

 [controls addSubview:Label(@"Select an action to change its keyboard key, controller button or stick.",NSMakeRect(15,283,528,25))];
 auto* scroll=[[[NSScrollView alloc]initWithFrame:NSMakeRect(15,81,528,198)]autorelease];scroll.hasVerticalScroller=YES;scroll.borderType=NSBezelBorder;
 a->table=[[[NSTableView alloc]initWithFrame:scroll.bounds]autorelease];a->table.rowHeight=28;a->table.allowsMultipleSelection=NO;a->table.dataSource=a;a->table.delegate=a;
 for(auto col:{std::pair{@"action",@"Action"},std::pair{@"binding",@"Binding"}}){auto* c=[[[NSTableColumn alloc]initWithIdentifier:col.first]autorelease];c.title=col.second;c.width=[col.first isEqualToString:@"action"]?214:292;[a->table addTableColumn:c];}
 scroll.documentView=a->table;[controls addSubview:scroll];
 a->bind=Button(@"Change binding…",NSMakeRect(15,44,164,32),a,@selector(change:));a->bind.enabled=NO;[controls addSubview:a->bind];[controls addSubview:Button(@"Restore default controls",NSMakeRect(189,44,190,32),a,@selector(reset:))];
 a->status=Label(@"Controls apply on Save.",NSMakeRect(15,8,528,30));[controls addSubview:a->status];
 [root addSubview:Button(@"Cancel",NSMakeRect(402,18,90,32),a,@selector(cancel:))];auto* save=Button(@"Save",NSMakeRect(502,18,100,32),a,@selector(save:));save.keyEquivalent=@"\r";[root addSubview:save];
 window.initialFirstResponder=a->tab_buttons[0];
 [window center];return a;
}
void CleanupPreferences(CaraPreferencesActions* a){[a stopBinding];if(a->key_monitor)[NSEvent removeMonitor:a->key_monitor];[a->timer invalidate];if(auto* k=InputCommon::GetKeyboard())k->ReleaseAllKeys();SDL_FlushEvents(SDL_KEYDOWN,SDL_KEYUP);[settings_window close];[settings_window release];settings_window=nil;[a release];}
}
@interface CaraWelcomeActions:NSObject<NSWindowDelegate>
-(void)open:(id)sender;-(void)settings:(id)sender;-(void)quit:(id)sender;
@end
@implementation CaraWelcomeActions
-(void)open:(id)sender{(void)sender;Cara::MacUI::HandleStartupOpen();}
-(void)settings:(id)sender{(void)sender;Cara::MacUI::ShowPreferences();}
-(void)quit:(id)sender{(void)sender;[NSApp abortModal];}
-(BOOL)windowShouldClose:(NSWindow*)window{(void)window;[NSApp abortModal];return NO;}
@end
namespace Cara::MacUI {
std::optional<PlaySettings> TakeSavedPlaySettings(){return std::exchange(saved_play_settings,std::nullopt);}
bool TakePreferencesClosed(){return std::exchange(preferences_closed,false);}
bool HandleStartupOpen(){if(!welcome_window)return false;const auto rom=PickGame();if(!rom.empty()){startup_rom=rom;[NSApp stopModalWithCode:NSModalResponseOK];}return true;}
void ShowPreferences(){
 if(settings_window){[settings_window makeKeyAndOrderFront:nil];return;}
 @autoreleasepool{try{auto* a=BuildPreferences();if(auto* k=InputCommon::GetKeyboard())k->ReleaseAllKeys();
  a->key_monitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown|NSEventMaskFlagsChanged handler:^NSEvent*(NSEvent* e){
   return [a handleKey:e];
  }];
  a->timer=[NSTimer scheduledTimerWithTimeInterval:.02 target:a selector:@selector(tick:) userInfo:nil repeats:YES];[[NSRunLoop mainRunLoop]addTimer:a->timer forMode:NSModalPanelRunLoopMode];
  [settings_window makeKeyAndOrderFront:nil];[NSApp runModalForWindow:settings_window];CleanupPreferences(a);
  preferences_closed = true;
 }catch(const std::exception& e){Alert(@"Settings unavailable",Text(e.what()));}}
}
std::string Welcome(){
 @autoreleasepool{
 startup_rom.clear();auto* a=[[CaraWelcomeActions alloc]init];welcome_window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,600,400) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];welcome_window.title=@"Cara";welcome_window.releasedWhenClosed=NO;welcome_window.delegate=a;
 welcome_window.contentView=[[[CaraPreferencesRoot alloc]initWithFrame:welcome_window.contentView.bounds]autorelease];auto* root=welcome_window.contentView;auto* icon=[[[NSImageView alloc]initWithFrame:NSMakeRect(248,255,104,104)]autorelease];icon.image=NSApp.applicationIconImage;[root addSubview:icon];
 auto* title=Label(@"Welcome to Cara",NSMakeRect(50,206,500,42),30);title.alignment=NSTextAlignmentCenter;[root addSubview:title];auto* body=Label(@"Choose a Nintendo 3DS game to begin.\nAdjust emulation, display and controls in Settings.",NSMakeRect(50,139,500,52),15);body.alignment=NSTextAlignmentCenter;[root addSubview:body];
 auto* open=Button(@"Open Game…",NSMakeRect(187,83,226,40),a,@selector(open:));open.keyEquivalent=@"\r";[root addSubview:open];[root addSubview:Button(@"Settings…",NSMakeRect(235,35,130,32),a,@selector(settings:))];
 NativeGameMenu::Install();[welcome_window center];[welcome_window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];[NSApp runModalForWindow:welcome_window];[welcome_window close];[welcome_window release];welcome_window=nil;[a release];return startup_rom;
 }
}
int SelfTest(const std::string& out){
 @autoreleasepool{try{
  const auto require=[](bool ok,const char* what){if(!ok)throw std::runtime_error(what);};
  const auto snapshot=[&](NSWindow* window,const char* name){
   [window displayIfNeeded];auto* view=window.contentView;
   auto* image=[view bitmapImageRepForCachingDisplayInRect:view.bounds];
   [view cacheDisplayInRect:view.bounds toBitmapImageRep:image];
   require([[image representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
       writeToFile:Text(out+"/"+name) atomically:YES],"view snapshot failed");
  };
  auto* a=BuildPreferences();[settings_window makeKeyAndOrderFront:nil];
  require(a->engine.numberOfItems==3,"CPU choices missing");
  require([a->engine itemAtIndex:1].enabled==MacPreferences::JitAvailable(),"JIT availability mismatch");
  [a->engine selectItemAtIndex:MacPreferences::JitAvailable()?1:0];
  require(a->mode.numberOfItems==2&&a->scale.numberOfItems==3,"play and scaling choices missing");
  snapshot(settings_window,"settings.png");
  [a->mode selectItemAtIndex:1];[a changePlayOption:a->mode];
  require(a->asyncShaders.enabled && a->asyncShaders.state==NSControlStateValueOn,
          "Performance async shaders default missing");
  a->fixedClock.state=NSControlStateValueOn;[a changePlayOption:a->fixedClock];
  a->guestClock.integerValue=155;[a changePlayOption:a->guestClock];
  a->limitSpeed.state=NSControlStateValueOn;[a changePlayOption:a->limitSpeed];
  a->maximumSpeed.integerValue=125;[a changePlayOption:a->maximumSpeed];
  [a->mode selectItemAtIndex:0];[a changePlayOption:a->mode];
  require(!a->fixedClock.enabled && a->fixedClock.state==NSControlStateValueOn &&
          !a->guestClock.enabled && a->guestClock.integerValue==100 && !a->maximumSpeed.enabled &&
          a->maximumSpeed.integerValue==100 && !a->frameSkip.enabled && !a->asyncShaders.enabled &&
          a->asyncShaders.state==NSControlStateValueOff,"Accuracy controls not locked");
  [a->mode selectItemAtIndex:1];[a changePlayOption:a->mode];
  require(a->guestClock.enabled && a->maximumSpeed.enabled && a->guestClock.integerValue==155 &&
          a->maximumSpeed.integerValue==125,"Performance did not restore saved tuning");
  [a selectSettingsTab:a->tab_buttons[1]];
  require(a->tab_buttons[0].state==NSControlStateValueOff &&
          a->tab_buttons[1].state==NSControlStateValueOn,"Display tab selection failed");
  [a->scale selectItemWithTag:static_cast<NSInteger>(DisplayScale::Lossless4x)];[a changePlayOption:a->scale];
  snapshot(settings_window,"display.png");
  a->asyncShaders.state=NSControlStateValueOff;[a changePlayOption:a->asyncShaders];
  [a selectSettingsTab:a->tab_buttons[2]];
  require(a->tab_buttons[1].state==NSControlStateValueOff &&
          a->tab_buttons[2].state==NSControlStateValueOn,"Controls tab selection failed");
  [a->table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];[a change:nil];
  NSEvent* key=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
      timestamp:0 windowNumber:settings_window.windowNumber context:nil characters:@"q"
      charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:12];
  require([a handleKey:key]==nil && a->model->buttons[0].value==MacPreferences::Keyboard(SDL_SCANCODE_Q),"keyboard capture failed");
  const int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,2,2,0);
  require(index>=0,"SDL virtual controller unavailable");
  SDL_Joystick* joystick=SDL_JoystickOpen(index);require(joystick!=nullptr,"virtual controller open failed");
  SDL_PumpEvents();
  [a->table selectRowIndexes:[NSIndexSet indexSetWithIndex:1] byExtendingSelection:NO];[a change:nil];
  require(SDL_JoystickSetVirtualButton(joystick,0,1)==0,"virtual button down failed");
  SDL_JoystickUpdate();[a tick:nil];
  require(SDL_JoystickSetVirtualButton(joystick,0,0)==0,"virtual button release failed");
  SDL_JoystickUpdate();[a tick:nil];
  require(Common::ParamPackage(a->model->buttons[1].value).Get("engine","")=="sdl","controller button capture failed");
  [a->table selectRowIndexes:[NSIndexSet indexSetWithIndex:19] byExtendingSelection:NO];[a change:nil];
  require(SDL_JoystickSetVirtualAxis(joystick,0,26000)==0,"virtual axis X failed");
  SDL_JoystickUpdate();[a tick:nil];
  require(SDL_JoystickSetVirtualAxis(joystick,1,26000)==0,"virtual axis Y failed");
  SDL_JoystickUpdate();[a tick:nil];
  require(a->model->analogs[0].use_controller,"controller stick capture failed");
  const auto button=a->model->buttons[1].value,analog=a->model->analogs[0].controller;
  SDL_JoystickClose(joystick);SDL_JoystickDetachVirtual(index);SDL_PumpEvents();
  snapshot(settings_window,"controls.png");
  const bool cpu_before=Settings::values.use_cpu_jit;
  auto* save_timer=[NSTimer timerWithTimeInterval:.05 repeats:NO block:^(NSTimer*){[a save:nil];}];
  [[NSRunLoop mainRunLoop]addTimer:save_timer forMode:NSModalPanelRunLoopMode];
  require([NSApp runModalForWindow:settings_window]==NSModalResponseOK,"actual Save action failed");
  require(Settings::values.current_input_profile.buttons[Settings::NativeButton::B]==button,"Save did not update live input profile");
  require(Settings::values.use_cpu_jit==cpu_before,"Control Save changed running CPU choice");
  Model saved(Read(ConfigPath()));require(saved.buttons[0].value==MacPreferences::Keyboard(SDL_SCANCODE_Q)
      &&saved.buttons[1].value==button&&saved.analogs[0].controller==analog,"binding persistence failed");
  require(saved.backend==(MacPreferences::JitAvailable()?"jit":"fastinterp"),"backend persistence failed");
  require(saved.play.guest_clock_percent==155 && saved.play.maximum_speed_percent==125 &&
          saved.play.display_scale==DisplayScale::Lossless4x && !saved.play.async_metal_shaders,
          "play settings persistence failed");
  const auto applied = TakeSavedPlaySettings();
  require(applied && *applied==saved.play,"Save did not publish the runtime play settings");
  CleanupPreferences(a);
  const auto before_cancel=Read(ConfigPath());a=BuildPreferences();[a reset:nil];
  require(a->model->play==saved.play,"control reset changed play settings");
  [a restorePlayTuning:nil];
  require(a->model->play.guest_clock_percent==100 && a->model->play.maximum_speed_percent==100 &&
          a->model->play.display_scale==saved.play.display_scale &&
          !a->model->play.async_metal_shaders,"reset tuning changed display settings");
  auto* cancel_timer=[NSTimer timerWithTimeInterval:.05 repeats:NO block:^(NSTimer*){[a cancel:nil];}];
  [[NSRunLoop mainRunLoop]addTimer:cancel_timer forMode:NSModalPanelRunLoopMode];
  [NSApp runModalForWindow:settings_window];CleanupPreferences(a);
  require(Read(ConfigPath())==before_cancel && !TakeSavedPlaySettings(),"Cancel changed settings");
  auto* welcome_timer=[NSTimer timerWithTimeInterval:.1 repeats:NO block:^(NSTimer*){
      snapshot(welcome_window,"welcome.png");[NSApp abortModal];}];
  [[NSRunLoop mainRunLoop]addTimer:welcome_timer forMode:NSModalPanelRunLoopMode];
  require(Welcome().empty(),"Welcome automatically selected a game");
  std::ofstream(out+"/ui-result.json")<<"{\"settings_window\":true,\"keyboard_capture\":true,\"virtual_controller_button\":true,\"virtual_controller_stick\":true,\"live_profile_updated\":true,\"cpu_choice_unchanged\":true,\"save_roundtrip\":true,\"play_controls_locked_and_restored\":true,\"display_scale_saved\":true,\"cancel_unchanged\":true,\"welcome_empty\":true,\"jit_available\":"<<(MacPreferences::JitAvailable()?"true":"false")<<",\"game_loaded\":false}\n";
  return 0;
 }catch(const std::exception& e){std::ofstream(out+"/ui-error.txt")<<e.what();return 1;}}
}
}
