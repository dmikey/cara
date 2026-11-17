// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#import <Cocoa/Cocoa.h>
#include <SDL.h>
#include <fstream>
#include <filesystem>
#include <memory>
#include "cara/native_mac/mac_preferences.h"
#include "cara/native_mac/mac_preferences_ui.h"
#include "cara/native_mac/game_menu.h"
#include "input_common/main.h"
#include "input_common/keyboard.h"
namespace {
using Cara::MacPreferences::Model;
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
std::string PickGame(){auto* p=[NSOpenPanel openPanel];p.title=@"Open Nintendo 3DS Game";p.prompt=@"Open";p.canChooseDirectories=NO;p.allowsMultipleSelection=NO;p.allowedFileTypes=@[@"3ds",@"cci",@"cxi",@"app",@"3dsx",@"elf",@"axf"];return [p runModal]==NSModalResponseOK&&p.URL.isFileURL?std::string(p.URL.fileSystemRepresentation):"";}
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
@interface CaraPreferencesActions:NSObject<NSTableViewDataSource,NSTableViewDelegate,NSWindowDelegate>{
@public std::unique_ptr<Model> model;NSTableView* table;NSPopUpButton* engine;NSTextField* status;NSButton* bind;
NSInteger waiting;id key_monitor;NSTimer* timer;std::vector<std::unique_ptr<InputCommon::Polling::DevicePoller>> pollers;
}
-(void)change:(id)sender;-(void)save:(id)sender;-(void)cancel:(id)sender;-(void)reset:(id)sender;-(void)tick:(id)sender;
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
-(void)save:(id)sender{
 (void)sender;[self stopBinding];try{
  const auto path=ConfigPath();if(Read(path)!=model->original)throw std::runtime_error("Settings changed elsewhere. Close and reopen Settings to reload them.");
  model->backend=String([engine.selectedItem representedObject]);const auto text=model->Serialize(Cara::MacPreferences::JitAvailable());
  if(std::filesystem::is_symlink(path)||std::filesystem::is_symlink(path.parent_path()))throw std::runtime_error("Settings path must be inside the Cara data directory");
  std::filesystem::create_directories(path.parent_path());NSError* error=nil;
  NSData* data=[NSData dataWithBytes:text.data() length:text.size()];
  if(![data writeToFile:Text(path.string()) options:NSDataWritingAtomic error:&error])throw std::runtime_error(String(error.localizedDescription));
  model->original=text;Settings::ApplyInputProfile(model->Controls(Settings::values.current_input_profile));[NSApp stopModalWithCode:NSModalResponseOK];
 }catch(const std::exception& e){Alert(@"Could not save settings",Text(e.what()));}
}
-(void)cancel:(id)sender{(void)sender;[self stopBinding];[NSApp abortModal];}
-(BOOL)windowShouldClose:(NSWindow*)window{(void)window;[self cancel:nil];return NO;}
-(void)reset:(id)sender{(void)sender;[self stopBinding];const auto original=model->original;const auto backend=model->backend;model=std::make_unique<Model>();model->original=original;model->backend=backend;[table reloadData];status.stringValue=@"Default bindings restored. Save to keep them.";}
@end
namespace {
CaraPreferencesActions* BuildPreferences(){
 auto* a=[[CaraPreferencesActions alloc]init];a->model=std::make_unique<Model>(Read(ConfigPath()));
 auto* window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,720,660) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];window.title=@"Cara Settings";window.releasedWhenClosed=NO;window.delegate=a;settings_window=window;
 window.contentView=[[[CaraPreferencesRoot alloc]initWithFrame:window.contentView.bounds]autorelease];auto* root=window.contentView;
 [root addSubview:Label(@"Emulation",NSMakeRect(24,614,270,28),22)];
 [root addSubview:Label(@"CPU engine",NSMakeRect(24,574,130,22))];
 a->engine=[[[NSPopUpButton alloc]initWithFrame:NSMakeRect(150,570,360,30) pullsDown:NO]autorelease];
 for(auto entry:{std::pair{"FastInterp","fastinterp"},std::pair{"JIT (Dynarmic, experimental)","jit"},std::pair{"Interpreter (compatibility)","dyncom"}}){[a->engine addItemWithTitle:Text(entry.first)];a->engine.lastItem.representedObject=Text(entry.second);if(entry.second==std::string("jit"))a->engine.lastItem.enabled=Cara::MacPreferences::JitAvailable();}
 a->engine.menu.autoenablesItems=NO;NSInteger selected=0;for(NSInteger i=0;i<a->engine.numberOfItems;++i)if(String([a->engine itemAtIndex:i].representedObject)==a->model->backend)selected=i;
 [a->engine selectItemAtIndex:selected];[root addSubview:a->engine];
 [root addSubview:Label(Cara::MacPreferences::JitAvailable()?@"FastInterp is the default. JIT is optional; game compatibility can differ.":@"This build has no JIT backend. FastInterp is the default.",NSMakeRect(24,534,672,30))];
 [root addSubview:Label(@"Controls",NSMakeRect(24,491,270,28),22)];
 [root addSubview:Label(@"Select an action, then change its keyboard key, controller button or stick.",NSMakeRect(24,460,672,25))];
 auto* scroll=[[[NSScrollView alloc]initWithFrame:NSMakeRect(24,146,672,310)]autorelease];scroll.hasVerticalScroller=YES;scroll.borderType=NSBezelBorder;
 a->table=[[[NSTableView alloc]initWithFrame:scroll.bounds]autorelease];a->table.rowHeight=28;a->table.allowsMultipleSelection=NO;a->table.dataSource=a;a->table.delegate=a;
 for(auto col:{std::pair{@"action",@"Action"},std::pair{@"binding",@"Binding"}}){auto* c=[[[NSTableColumn alloc]initWithIdentifier:col.first]autorelease];c.title=col.second;c.width=[col.first isEqualToString:@"action"]?250:390;[a->table addTableColumn:c];}
 scroll.documentView=a->table;[root addSubview:scroll];
 a->bind=Button(@"Change binding…",NSMakeRect(24,105,164,32),a,@selector(change:));a->bind.enabled=NO;[root addSubview:a->bind];[root addSubview:Button(@"Restore default controls",NSMakeRect(198,105,190,32),a,@selector(reset:))];
 a->status=Label(@"Controls apply on Save. CPU engine changes apply to the next game.",NSMakeRect(24,65,672,30));[root addSubview:a->status];
 [root addSubview:Button(@"Cancel",NSMakeRect(496,20,90,32),a,@selector(cancel:))];auto* save=Button(@"Save",NSMakeRect(596,20,100,32),a,@selector(save:));save.keyEquivalent=@"\r";[root addSubview:save];[window center];return a;
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
bool HandleStartupOpen(){if(!welcome_window)return false;const auto rom=PickGame();if(!rom.empty()){startup_rom=rom;[NSApp stopModalWithCode:NSModalResponseOK];}return true;}
void ShowPreferences(){
 if(settings_window){[settings_window makeKeyAndOrderFront:nil];return;}
 @autoreleasepool{try{auto* a=BuildPreferences();if(auto* k=InputCommon::GetKeyboard())k->ReleaseAllKeys();
  a->key_monitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown|NSEventMaskFlagsChanged handler:^NSEvent*(NSEvent* e){
   return [a handleKey:e];
  }];
  a->timer=[NSTimer scheduledTimerWithTimeInterval:.02 target:a selector:@selector(tick:) userInfo:nil repeats:YES];[[NSRunLoop mainRunLoop]addTimer:a->timer forMode:NSModalPanelRunLoopMode];
  [settings_window makeKeyAndOrderFront:nil];[NSApp runModalForWindow:settings_window];CleanupPreferences(a);
 }catch(const std::exception& e){Alert(@"Settings unavailable",Text(e.what()));}}
}
std::string Welcome(){
 @autoreleasepool{
 startup_rom.clear();auto* a=[[CaraWelcomeActions alloc]init];welcome_window=[[NSWindow alloc]initWithContentRect:NSMakeRect(0,0,600,400) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];welcome_window.title=@"Cara";welcome_window.releasedWhenClosed=NO;welcome_window.delegate=a;
 welcome_window.contentView=[[[CaraPreferencesRoot alloc]initWithFrame:welcome_window.contentView.bounds]autorelease];auto* root=welcome_window.contentView;auto* icon=[[[NSImageView alloc]initWithFrame:NSMakeRect(248,255,104,104)]autorelease];icon.image=NSApp.applicationIconImage;[root addSubview:icon];
 auto* title=Label(@"Welcome to Cara",NSMakeRect(50,206,500,42),30);title.alignment=NSTextAlignmentCenter;[root addSubview:title];auto* body=Label(@"Choose a Nintendo 3DS game to begin.\nYour controls and CPU engine are in Settings.",NSMakeRect(50,139,500,52),15);body.alignment=NSTextAlignmentCenter;[root addSubview:body];
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
  snapshot(settings_window,"settings.png");
  const bool cpu_before=Settings::values.use_cpu_jit;
  auto* save_timer=[NSTimer timerWithTimeInterval:.05 repeats:NO block:^(NSTimer*){[a save:nil];}];
  [[NSRunLoop mainRunLoop]addTimer:save_timer forMode:NSModalPanelRunLoopMode];
  require([NSApp runModalForWindow:settings_window]==NSModalResponseOK,"actual Save action failed");
  require(Settings::values.current_input_profile.buttons[Settings::NativeButton::B]==button,"Save did not update live input profile");
  require(Settings::values.use_cpu_jit==cpu_before,"Control Save changed running CPU choice");
  Model saved(Read(ConfigPath()));require(saved.buttons[0].value==MacPreferences::Keyboard(SDL_SCANCODE_Q)
      &&saved.buttons[1].value==button&&saved.analogs[0].controller==analog,"binding persistence failed");
  require(saved.backend==(MacPreferences::JitAvailable()?"jit":"fastinterp"),"backend persistence failed");
  CleanupPreferences(a);
  const auto before_cancel=Read(ConfigPath());a=BuildPreferences();[a reset:nil];
  auto* cancel_timer=[NSTimer timerWithTimeInterval:.05 repeats:NO block:^(NSTimer*){[a cancel:nil];}];
  [[NSRunLoop mainRunLoop]addTimer:cancel_timer forMode:NSModalPanelRunLoopMode];
  [NSApp runModalForWindow:settings_window];CleanupPreferences(a);
  require(Read(ConfigPath())==before_cancel,"Cancel changed settings");
  auto* welcome_timer=[NSTimer timerWithTimeInterval:.1 repeats:NO block:^(NSTimer*){
      snapshot(welcome_window,"welcome.png");[NSApp abortModal];}];
  [[NSRunLoop mainRunLoop]addTimer:welcome_timer forMode:NSModalPanelRunLoopMode];
  require(Welcome().empty(),"Welcome automatically selected a game");
  std::ofstream(out+"/ui-result.json")<<"{\"settings_window\":true,\"keyboard_capture\":true,\"virtual_controller_button\":true,\"virtual_controller_stick\":true,\"live_profile_updated\":true,\"cpu_choice_unchanged\":true,\"save_roundtrip\":true,\"cancel_unchanged\":true,\"welcome_empty\":true,\"jit_available\":"<<(MacPreferences::JitAvailable()?"true":"false")<<",\"game_loaded\":false}\n";
  return 0;
 }catch(const std::exception& e){std::ofstream(out+"/ui-error.txt")<<e.what();return 1;}}
}
}
