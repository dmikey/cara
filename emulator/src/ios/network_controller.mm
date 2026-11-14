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
