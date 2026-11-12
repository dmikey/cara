// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/feature_controllers.h"

NSString* CaraInputActionTitle(NSString* action) {
    NSDictionary* names=@{@"menu":@"Menu",@"quickSave":@"Quick Save",@"quickLoad":@"Quick Load",@"fastForward":@"Fast Forward",@"circleUp":@"Circle Pad Up",@"circleDown":@"Circle Pad Down",@"circleLeft":@"Circle Pad Left",@"circleRight":@"Circle Pad Right",@"cstickUp":@"C-Stick Up",@"cstickDown":@"C-Stick Down",@"cstickLeft":@"C-Stick Left",@"cstickRight":@"C-Stick Right"};
    return names[action]?:action.uppercaseString;
}
static void FeatureError(UIViewController* owner,NSString* message) {
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Cara" message:message preferredStyle:UIAlertControllerStyleAlert]; [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]]; [owner presentViewController:alert animated:YES completion:nil];
}
@implementation CaraMappingController
- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=self.keyboard?@"Keyboard mappings":@"Controller mappings";
    self.tableView.rowHeight=UITableViewAutomaticDimension; self.tableView.estimatedRowHeight=58;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Reset" style:UIBarButtonItemStylePlain target:self action:@selector(reset)];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(refresh:) name:CaraInputHubDidChangeNotification object:self.hub];
}
- (void)refresh:(NSNotification*)note { [self.tableView reloadData]; }
- (NSInteger)tableView:(UITableView*)table numberOfRowsInSection:(NSInteger)section { return self.hub.actions.count; }
- (NSString*)tableView:(UITableView*)table titleForHeaderInSection:(NSInteger)section { return self.keyboard?@"Keyboard":self.hub.controllerName; }
- (NSString*)tableView:(UITableView*)table titleForFooterInSection:(NSInteger)section { return @"Tap an action, then press a key or controller button. Assigning an input replaces its previous mapping. Left and right sticks keep their native analog behavior."; }
- (UITableViewCell*)tableView:(UITableView*)table cellForRowAtIndexPath:(NSIndexPath*)path {
    UITableViewCell* cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    NSString* action=self.hub.actions[path.row]; cell.textLabel.text=CaraInputActionTitle(action); cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; cell.textLabel.adjustsFontForContentSizeCategory=YES;
    cell.detailTextLabel.text=[self.hub bindingForAction:action keyboard:self.keyboard]; cell.detailTextLabel.adjustsFontForContentSizeCategory=YES; cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator; return cell;
}
- (void)tableView:(UITableView*)table didSelectRowAtIndexPath:(NSIndexPath*)path {
    [table deselectRowAtIndexPath:path animated:YES]; NSString* action=self.hub.actions[path.row];
    UIAlertController* capture=[UIAlertController alertControllerWithTitle:CaraInputActionTitle(action) message:self.keyboard?@"Press a keyboard key…":@"Press a controller button…" preferredStyle:UIAlertControllerStyleAlert];
    [capture addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* item) { [self.hub cancelCapture]; }]];
    __weak CaraMappingController* owner=self;
    [self.hub captureAction:action keyboard:self.keyboard completion:^(NSError* error) {
        CaraMappingController* controller=owner; if (!controller) return;
        [capture dismissViewControllerAnimated:YES completion:^{ [controller.tableView reloadData]; if (error) FeatureError(controller,error.localizedDescription); }];
    }];
    [self presentViewController:capture animated:YES completion:nil];
}
- (void)reset {
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Reset mappings?" message:@"This restores the built-in button assignments." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Reset" style:UIAlertActionStyleDestructive handler:^(UIAlertAction* action) { [self.hub resetMappings:self.keyboard]; [self.tableView reloadData]; }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; [self presentViewController:alert animated:YES completion:nil];
}
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; if (self.isMovingFromParentViewController || self.navigationController.isBeingDismissed) [self.hub cancelCapture]; }
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
@end

@implementation CaraStateManagerController {
    NSArray<NSArray<CaraStateRecord*>*>* groups;
    NSArray<NSString*>* titles;
}
- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=@"Save states"; self.tableView.rowHeight=UITableViewAutomaticDimension; self.tableView.estimatedRowHeight=68;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(newState)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel=@"Create save state";
    [self reloadStates];
}
- (void)reloadStates {
    NSMutableArray* values=[NSMutableArray new],*names=[NSMutableArray new]; NSArray* all=self.catalog.records;
    for (NSString* kind in @[@"locked",@"quick",@"manual",@"auto",@"backup"]) {
        NSArray* subset=[all filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(CaraStateRecord* record,NSDictionary* binding) { return [kind isEqual:@"locked"]?record.locked:(!record.locked && [record.kind isEqual:kind]); }]];
        if (subset.count) { [values addObject:subset]; [names addObject:@{@"locked":@"Locked",@"quick":@"Quick save",@"manual":@"Your states",@"auto":@"Automatic states",@"backup":@"Backups"}[kind]]; }
    }
    groups=values; titles=names; [self.tableView reloadData];
    UILabel* empty=[UILabel new]; empty.text=@"No states yet. Tap + to save this moment."; empty.textAlignment=NSTextAlignmentCenter; empty.numberOfLines=0; empty.textColor=UIColor.secondaryLabelColor; self.tableView.backgroundView=all.count?nil:empty;
}
- (NSInteger)numberOfSectionsInTableView:(UITableView*)table { return groups.count; }
- (NSInteger)tableView:(UITableView*)table numberOfRowsInSection:(NSInteger)section { return groups[section].count; }
- (NSString*)tableView:(UITableView*)table titleForHeaderInSection:(NSInteger)section { return titles[section]; }
- (NSString*)tableView:(UITableView*)table titleForFooterInSection:(NSInteger)section { return section==(NSInteger)groups.count-1?@"Locked states are protected from replacement and cleanup. Three unlocked automatic states and three replacement backups are kept. States need the same Cara build, game revision and system mode. Keep in-game saves too.":nil; }
- (UITableViewCell*)tableView:(UITableView*)table cellForRowAtIndexPath:(NSIndexPath*)path {
    CaraStateRecord* record=groups[path.section][path.row]; UITableViewCell* cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text=record.name; cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]; cell.textLabel.adjustsFontForContentSizeCategory=YES; cell.textLabel.numberOfLines=0;
    NSString* date=[NSDateFormatter localizedStringFromDate:record.date dateStyle:NSDateFormatterMediumStyle timeStyle:NSDateFormatterShortStyle];
    cell.detailTextLabel.text=[date stringByAppendingString:record.compatible?@"":@" · Incompatible build or game"]; cell.detailTextLabel.numberOfLines=0; cell.detailTextLabel.adjustsFontForContentSizeCategory=YES;
    cell.imageView.image=[UIImage systemImageNamed:record.locked?@"lock.fill":[record.kind isEqual:@"auto"]?@"clock.arrow.circlepath":@"bookmark.fill"]; cell.imageView.tintColor=record.compatible?self.view.tintColor:UIColor.secondaryLabelColor; cell.accessoryType=UITableViewCellAccessoryDetailButton;
    return cell;
}
- (void)tableView:(UITableView*)table didSelectRowAtIndexPath:(NSIndexPath*)path {
    [table deselectRowAtIndexPath:path animated:YES]; CaraStateRecord* record=groups[path.section][path.row];
    if (!record.compatible) { FeatureError(self,@"This state belongs to another Cara build, game revision, CPU backend or system mode. The file is kept; use an in-game save for this version."); return; }
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Load this state?" message:@"Unsaved progress since this moment will be replaced." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Load" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { if (self.load) self.load(record.filename); }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; [self presentViewController:alert animated:YES completion:nil];
}
- (void)newState {
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"New save state" message:nil preferredStyle:UIAlertControllerStyleAlert]; [alert addTextFieldWithConfigurationHandler:^(UITextField* field) { field.placeholder=@"Name this moment"; field.text=@"Save state"; }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { NSString* name=alert.textFields.firstObject.text; if (self.save) self.save([self.catalog newFilename:@"manual"],name.length?name:@"Save state"); }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; [self presentViewController:alert animated:YES completion:nil];
}
- (void)tableView:(UITableView*)table accessoryButtonTappedForRowWithIndexPath:(NSIndexPath*)path {
    CaraStateRecord* record=groups[path.section][path.row];
    UIAlertController* options=[UIAlertController alertControllerWithTitle:record.name message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    [options addAction:[UIAlertAction actionWithTitle:record.locked?@"Unlock":@"Lock" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { NSError* error=nil; if (![self.catalog setLocked:!record.locked filename:record.filename error:&error]) FeatureError(self,error.localizedDescription); else { [self reloadStates]; if (self.changed) self.changed(); } }]];
    [options addAction:[UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
        UIAlertController* rename=[UIAlertController alertControllerWithTitle:@"Rename state" message:nil preferredStyle:UIAlertControllerStyleAlert]; [rename addTextFieldWithConfigurationHandler:^(UITextField* field) { field.text=record.name; }];
        [rename addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(UIAlertAction* item) { NSError* error=nil; if (![self.catalog rename:record.filename name:rename.textFields.firstObject.text error:&error]) FeatureError(self,error.localizedDescription); else { [self reloadStates]; if (self.changed) self.changed(); } }]];
        [rename addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; [self presentViewController:rename animated:YES completion:nil];
    }]];
    if (!record.locked) [options addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction* action) {
        UIAlertController* confirm=[UIAlertController alertControllerWithTitle:@"Delete this state?" message:@"Your in-game saves are kept." preferredStyle:UIAlertControllerStyleAlert];
        [confirm addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(UIAlertAction* item) { NSError* error=nil; if (![self.catalog remove:record.filename error:&error]) FeatureError(self,error.localizedDescription); else { [self reloadStates]; if (self.changed) self.changed(); } }]];
        [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]]; [self presentViewController:confirm animated:YES completion:nil];
    }]];
    [options addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    options.popoverPresentationController.sourceView=[table cellForRowAtIndexPath:path]; options.popoverPresentationController.sourceRect=[table cellForRowAtIndexPath:path].bounds; [self presentViewController:options animated:YES completion:nil];
}
@end
