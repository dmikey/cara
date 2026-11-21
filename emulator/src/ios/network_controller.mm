// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#import "ios/network_controller.h"

@implementation CaraNetworkController {
    UITextField* endpointField;
    UITextField* roomField;
    UITextField* passwordField;
    UILabel* statusLabel;
    NSTimer* refreshTimer;
    NSString* formError;
    BOOL displayedActive;
}
- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}
- (UITextField*)field:(NSString*)placeholder identifier:(NSString*)identifier {
    UITextField* field=[UITextField new];
    field.placeholder=placeholder;
    field.accessibilityIdentifier=identifier;
    field.autocorrectionType=UITextAutocorrectionTypeNo;
    field.autocapitalizationType=UITextAutocapitalizationTypeNone;
    field.clearButtonMode=UITextFieldViewModeWhileEditing;
    field.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    field.adjustsFontForContentSizeCategory=YES;
    return field;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title=@"Local multiplayer";
    self.tableView.keyboardDismissMode=UIScrollViewKeyboardDismissModeInteractive;
    self.tableView.accessibilityIdentifier=@"cara.network.rooms";
    endpointField=[self field:@"wss://carap2p.therad.ninja/relay" identifier:@"cara.network.endpoint"];
    endpointField.text=@"wss://carap2p.therad.ninja/relay";
    endpointField.keyboardType=UIKeyboardTypeURL;
    roomField=[self field:@"Room name" identifier:@"cara.network.room"];
    passwordField=[self field:@"Room password" identifier:@"cara.network.password"];
    passwordField.secureTextEntry=YES;
    passwordField.textContentType=UITextContentTypePassword;
    statusLabel=[UILabel new];
    statusLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    statusLabel.adjustsFontForContentSizeCategory=YES;
    statusLabel.numberOfLines=0;
    statusLabel.accessibilityIdentifier=@"cara.network.status";
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self updateStatus];
    [refreshTimer invalidate];
    __weak CaraNetworkController* owner=self;
    refreshTimer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* timer) {
        [owner updateStatus];
    }];
}
- (void)viewWillDisappear:(BOOL)animated {
    [refreshTimer invalidate];
    refreshTimer=nil;
    passwordField.text=@"";
    [super viewWillDisappear:animated];
}
- (void)dealloc {
    [refreshTimer invalidate];
}
- (NSDictionary*)currentStatus {
    return self.connectionStatus?self.connectionStatus():@{};
}
- (void)updateStatus {
    NSDictionary* state=[self currentStatus];
    NSString* text=formError?:state[@"detail"]?:@"Not connected";
    const BOOL active=[state[@"active"] boolValue];
    // Replacing live action cells every second can interrupt a tap or accessibility focus.
    if([statusLabel.text isEqual:text] && displayedActive==active) return;
    statusLabel.text=text;
    displayedActive=active;
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(1,2)]
                 withRowAnimation:UITableViewRowAnimationNone];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView { return 3; }
- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    return section==0?3:section==1?1:2;
}
- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
    return @[@"Private room",@"Connection",@" "][section];
}
- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
    if(section==0) return @"Use an existing room and its password on both devices. Passwords are kept only for this connection.";
    if(section==2) return @"Choose local wireless inside the game after joining. Games keep running while connected, including under menus. Leaving Cara or loading a state disconnects the room. StreetPass and Nintendo online services are not available.";
    return nil;
}
- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    UITableViewCell* cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    cell.selectionStyle=UITableViewCellSelectionStyleNone;
    if(indexPath.section<2) {
        UIView* content=indexPath.section==0?@[endpointField,roomField,passwordField][indexPath.row]:statusLabel;
        [cell.contentView addSubview:content];
        content.translatesAutoresizingMaskIntoConstraints=NO;
        [NSLayoutConstraint activateConstraints:@[
            [content.leadingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.leadingAnchor],
            [content.trailingAnchor constraintEqualToAnchor:cell.contentView.layoutMarginsGuide.trailingAnchor],
            [content.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:14],
            [content.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-14]
        ]];
    } else {
        NSDictionary* state=[self currentStatus];
        BOOL active=[state[@"active"] boolValue];
        BOOL enabled=indexPath.row==0?!active:active;
        cell.textLabel.text=indexPath.row==0?@"Join room":@"Disconnect";
        cell.isAccessibilityElement=YES;
        cell.accessibilityLabel=cell.textLabel.text;
        cell.accessibilityIdentifier=indexPath.row==0?@"cara.network.join":@"cara.network.leave";
        cell.textLabel.textColor=enabled?(indexPath.row==0?self.view.tintColor:UIColor.systemRedColor):UIColor.tertiaryLabelColor;
        cell.userInteractionEnabled=enabled;
        cell.selectionStyle=enabled?UITableViewCellSelectionStyleDefault:UITableViewCellSelectionStyleNone;
        cell.accessibilityTraits=UIAccessibilityTraitButton|(enabled?0:UIAccessibilityTraitNotEnabled);
    }
    return cell;
}
- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if(indexPath.section!=2) return;
    const BOOL active=[[self currentStatus][@"active"] boolValue];
    if((indexPath.row==0 && active) || (indexPath.row==1 && !active)) {
        [self updateStatus];
        return;
    }
    formError=nil;
    if(indexPath.row==1) {
        if(self.leaveRoom) self.leaveRoom();
        [self updateStatus];
        return;
    }
    NSURLComponents* endpoint=[NSURLComponents componentsWithString:endpointField.text];
    NSString* room=[roomField.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if(![endpoint.scheme.lowercaseString isEqual:@"wss"] || !endpoint.host.length || !endpoint.URL || endpoint.user!=nil || endpoint.password!=nil || endpoint.query!=nil || endpoint.fragment!=nil || ![endpoint.path isEqual:@"/relay"]) {
        formError=@"Use a secure wss:// server address ending in /relay, without credentials or query parameters.";
    } else if(!room.length || !passwordField.text.length) {
        formError=@"Enter the room name and password.";
    } else if([room lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>64 ||
              [passwordField.text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>512) {
        formError=@"That room name or password is too long. Try a shorter value.";
    } else {
        NSString* password=[passwordField.text copy];
        passwordField.text=@"";
        [self.view endEditing:YES];
        if(self.joinRoom) self.joinRoom(endpoint.URL.absoluteString,room,password);
    }
    [self updateStatus];
}
@end

namespace {
NSString* GameVersionLabel(NSString* version) {
    NSArray<NSString*>* parts = [version componentsSeparatedByString:@":"];
    if (parts.count == 2) {
        NSDictionary* labels = @{@"base": @"Base revision", @"update": @"Installed update",
                                  @"update-revision": @"Installed update revision"};
        NSString* prefix = labels[parts.firstObject];
        if (prefix) return [NSString stringWithFormat:@"%@ %@", prefix, parts.lastObject];
    }
    return version;
}
} // namespace

@interface CaraRoomDirectoryDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation CaraRoomDirectoryDelegate
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest*))completionHandler {
    completionHandler(nil);
}
@end

@implementation CaraOnlineRoomsController {
    NSURLSession* directory;
    NSURLSessionDataTask* requestTask;
    NSArray<NSDictionary*>* rooms;
    NSString* directoryStatus;
    NSString* connectionDetail;
    NSTimer* refreshTimer;
    BOOL connectedOrConnecting;
}

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Online Rooms";
    self.tableView.accessibilityIdentifier = @"cara.online.rooms";
    if (self.finished)
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
            initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(closeRooms)];
    NSURLSessionConfiguration* config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest = 15;
    config.timeoutIntervalForResource = 20;
    directory = [NSURLSession sessionWithConfiguration:config
                                              delegate:[CaraRoomDirectoryDelegate new] delegateQueue:nil];
    rooms = @[];
    directoryStatus = @"Choose Browse Rooms to look for players.";
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self updateConnection];
    [refreshTimer invalidate];
    __weak CaraOnlineRoomsController* owner = self;
    refreshTimer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer* timer) {
        [owner updateConnection];
    }];
}

- (void)viewWillDisappear:(BOOL)animated {
    [refreshTimer invalidate];
    refreshTimer = nil;
    if (self.isMovingFromParentViewController || self.navigationController.isBeingDismissed) {
        [requestTask cancel];
        requestTask = nil;
    }
    [super viewWillDisappear:animated];
}

- (void)dealloc {
    [refreshTimer invalidate];
    [directory invalidateAndCancel];
}

- (void)closeRooms {
    [requestTask cancel];
    requestTask = nil;
    if (self.finished) self.finished();
}

- (void)updateConnection {
    NSDictionary* state = self.connectionStatus ? self.connectionStatus() : @{};
    NSString* detail = state[@"detail"] ?: @"Not connected";
    BOOL active = [state[@"active"] boolValue];
    if ([detail isEqualToString:connectionDetail] && active == connectedOrConnecting) return;
    connectionDetail = detail;
    connectedOrConnecting = active;
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(1, 3)]
                 withRowAnimation:UITableViewRowAnimationNone];
}

- (BOOL)roomMatchesGame:(NSDictionary*)room {
    return [room[@"game_hash"] isEqual:self.gameIdentity[@"game_hash"]] &&
           [room[@"game_version"] isEqual:self.gameIdentity[@"game_version"]] &&
           [room[@"title_id"] isEqual:self.gameIdentity[@"title_id"]];
}

- (BOOL)isValidRoom:(NSDictionary*)room {
    if (![room isKindOfClass:NSDictionary.class]) return NO;
    for (NSString* key in @[@"id", @"name", @"game_hash", @"game_version", @"title_id"]) {
        if (![room[key] isKindOfClass:NSString.class] || [room[key] length] == 0 ||
            [room[key] lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 96) return NO;
    }
    if (![room[@"players"] isKindOfClass:NSNumber.class]) return NO;
    NSInteger players = [room[@"players"] integerValue];
    return players >= 0 && players <= 4 &&
           [room[@"game_hash"] isEqual:self.gameIdentity[@"game_hash"]] &&
           [room[@"title_id"] isEqual:self.gameIdentity[@"title_id"]];
}

- (void)joinListedRoom:(NSDictionary*)room {
    if (connectedOrConnecting || ![self roomMatchesGame:room] || [room[@"players"] integerValue] >= 4)
        return;
    NSString* error = self.joinRoom ? self.joinRoom(room[@"id"]) : @"Open this game to join a room.";
    if (error) directoryStatus = error;
    [self updateConnection];
    [self.tableView reloadData];
}

- (void)loadDirectory:(NSString*)roomName {
    if (requestTask || (roomName && connectedOrConnecting)) return;
    const BOOL creating = roomName != nil;
    NSURL* url = [NSURL URLWithString:creating ?
        @"https://carap2p.therad.ninja/rooms/v1/create" : @"https://carap2p.therad.ninja/rooms/v1/browse"];
    NSMutableDictionary* fields = [self.gameIdentity mutableCopy];
    if (creating) fields[@"name"] = roomName;
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
    request.HTTPMethod = @"POST";
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    request.HTTPBody = [NSJSONSerialization dataWithJSONObject:fields options:0 error:nil];
    directoryStatus = creating ? @"Creating room…" : @"Looking for rooms…";

    __weak CaraOnlineRoomsController* owner = self;
    requestTask = [directory dataTaskWithRequest:request
                              completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            CaraOnlineRoomsController* c = owner;
            if (!c || !c->requestTask) return;
            c->requestTask = nil;
            if (!c.viewIfLoaded.window) {
                c->directoryStatus = @"Choose Browse Rooms to look for players.";
                [c.tableView reloadData];
                return;
            }
            NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class] ?
                ((NSHTTPURLResponse*)response).statusCode : 0;
            id json = !error && data.length && data.length <= 256 * 1024 ?
                [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            NSDictionary* reply = [json isKindOfClass:NSDictionary.class] ? json : @{};
            if (status != 200 || error) {
                NSString* detail = [reply[@"error"] isKindOfClass:NSString.class] ? reply[@"error"] : nil;
                c->directoryStatus = detail.length && detail.length <= 180 && status >= 400 ?
                    detail : @"Online rooms are unavailable. Check your connection and try again.";
            } else if (creating) {
                NSDictionary* room = reply[@"room"];
                if ([c isValidRoom:room] && [c roomMatchesGame:room]) {
                    c->rooms = @[room];
                    c->directoryStatus = nil;
                    [c joinListedRoom:room];
                } else {
                    c->directoryStatus = @"The relay returned a room with different game details.";
                }
            } else if ([reply[@"rooms"] isKindOfClass:NSArray.class]) {
                NSMutableArray* valid = [NSMutableArray array];
                for (id room in reply[@"rooms"]) {
                    if ([c isValidRoom:room]) [valid addObject:room];
                }
                c->rooms = valid;
                c->directoryStatus = valid.count ? nil :
                    @"No rooms for this hash. Create one or compare Your Game Hash with the host.";
            } else {
                c->directoryStatus = @"The relay returned an invalid room list.";
            }
            [c.tableView reloadData];
        });
    }];
    [requestTask resume];
    [self.tableView reloadData];
}

- (void)promptForRoomName {
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:@"Create Room"
        message:@"Players with this game hash and version can find and join the room."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField* field) {
        field.placeholder = @"Room name";
        field.text = @"Game room";
        field.accessibilityIdentifier = @"cara.online.roomName";
    }];
    __weak CaraOnlineRoomsController* owner = self;
    __weak UIAlertController* prompt = alert;
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Create" style:UIAlertActionStyleDefault
                                          handler:^(UIAlertAction* action) {
        CaraOnlineRoomsController* c = owner;
        if (!c) return;
        NSString* name = [prompt.textFields.firstObject.text
                           stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (!name.length || [name lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 64) {
            c->directoryStatus = @"Enter a room name up to 64 bytes.";
            [c.tableView reloadData];
            return;
        }
        [c loadDirectory:name];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView*)tableView {
    return 4;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section {
    if (section == 0 || section == 1) return 2;
    if (section == 2) return rooms.count + (directoryStatus ? 1 : 0);
    return connectedOrConnecting ? 2 : 1;
}

- (NSString*)tableView:(UITableView*)tableView titleForHeaderInSection:(NSInteger)section {
    if (section == 0) return @"Your Game Hash";
    if (section == 2) return @"Online Rooms for this Hash";
    if (section == 3) return @"Connection";
    return nil;
}

- (NSString*)tableView:(UITableView*)tableView titleForFooterInSection:(NSInteger)section {
    if (section == 0)
        return @"Cara calculates SHA-256 from the game code it loaded. Open this screen on both devices and compare the hash and Game Version.";
    if (section == 3)
        return @"After joining, return to the game and choose its local wireless mode.";
    return nil;
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)path {
    UITableViewCell* cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.numberOfLines = 0;
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    BOOL actionable = NO;
    if (path.section == 0) {
        if (path.row == 0) {
            cell.textLabel.text = self.gameIdentity[@"game_hash"];
            cell.textLabel.lineBreakMode = NSLineBreakByCharWrapping;
            cell.textLabel.font = [UIFontMetrics.defaultMetrics scaledFontForFont:
                [UIFont monospacedSystemFontOfSize:14 weight:UIFontWeightRegular]];
            cell.detailTextLabel.text = [@"Game Version · " stringByAppendingString:GameVersionLabel(self.gameIdentity[@"game_version"])];
            cell.accessibilityIdentifier = @"cara.online.gameHash";
        } else {
            cell.textLabel.text = @"Copy Hash";
            actionable = YES;
        }
    } else if (path.section == 1) {
        cell.textLabel.text = path.row == 0 ? @"Create Room" : @"Browse Rooms";
        actionable = !requestTask && (path.row == 1 || !connectedOrConnecting);
    } else if (path.section == 2) {
        if (path.row == rooms.count) {
            cell.textLabel.text = directoryStatus;
        } else {
            NSDictionary* room = rooms[path.row];
            NSInteger players = [room[@"players"] integerValue];
            cell.textLabel.text = room[@"name"];
            if ([self roomMatchesGame:room]) {
                cell.detailTextLabel.text = players == 4 ? @"Room full" :
                    [NSString stringWithFormat:@"%ld / 4 players · Tap to join", (long)players];
                actionable = !requestTask && !connectedOrConnecting && players < 4;
            } else {
                cell.detailTextLabel.text = [@"Different Game Version · Requires "
                    stringByAppendingString:GameVersionLabel(room[@"game_version"])];
            }
        }
    } else {
        cell.textLabel.text = path.row == 0 ? connectionDetail : @"Disconnect";
        actionable = path.row == 1;
    }
    if (actionable) {
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        cell.accessibilityTraits |= UIAccessibilityTraitButton;
        if (path.section != 2) cell.textLabel.textColor = self.view.tintColor;
    }
    return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (path.section == 0 && path.row == 1) {
        UIPasteboard.generalPasteboard.string = self.gameIdentity[@"game_hash"];
        UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, @"Game hash copied");
        return;
    }
    if (path.section == 3 && path.row == 1) {
        if (self.leaveRoom) self.leaveRoom();
        [self updateConnection];
        return;
    }
    if (requestTask) return;
    if (path.section == 1) {
        if (path.row == 1) [self loadDirectory:nil];
        else if (!connectedOrConnecting) [self promptForRoomName];
    } else if (path.section == 2 && path.row < rooms.count) {
        [self joinListedRoom:rooms[path.row]];
    }
}
@end
