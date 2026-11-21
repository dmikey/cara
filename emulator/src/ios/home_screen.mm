// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/home_screen.h"
#import "ios/shell_controllers.h"
#include <atomic>

static UIButton* HomeButton(NSString* title, NSString* symbol, void (^action)(void)) {
    UIButtonConfiguration* config=[UIButtonConfiguration plainButtonConfiguration];
    config.title=title;
    config.image=[UIImage systemImageNamed:symbol];
    config.imagePadding=5;
    config.contentInsets=NSDirectionalEdgeInsetsMake(5,6,5,6);
    UIButton* button=[UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction* sender) { action(); }]];
    button.titleLabel.adjustsFontSizeToFitWidth=YES;
    return button;
}

@interface CaraHomeScreen () <UITableViewDataSource, UITableViewDelegate>
@end

@implementation CaraHomeScreen {
    UITableView* list;
    UILabel* gameTitle;
    UIImageView* artwork;
    NSCache<NSString*,NSDictionary*>* artworkCache;
    dispatch_queue_t artworkQueue;
    std::atomic<unsigned> previewGeneration;
    UILabel* subtitle;
    UIButton* playButton;
    UIButton* resumeButton;
    NSArray<CaraLibraryGame*>* games;
    NSInteger selected;
    NSSet<NSString*>* held;
    NSString* status;
    BOOL canResume;
}
- (instancetype)init {
    if (!(self=[super init])) return nil;
    games=@[];
    selected=NSNotFound;
    artworkCache=[NSCache new];
    artworkCache.countLimit=48;
    artworkQueue=dispatch_queue_create("com.dmikey.cara.home-artwork",DISPATCH_QUEUE_SERIAL);
    _upperScreen=[UIView new];
    _lowerScreen=[UIView new];
    for (UIView* panel in @[_upperScreen,_lowerScreen]) {
        panel.clipsToBounds=YES;
        panel.tintColor=[UIColor colorWithRed:1 green:.56 blue:.29 alpha:1];
        panel.backgroundColor=[UIColor colorWithRed:.07 green:.09 blue:.085 alpha:1];
    }
    UILabel* heading=[UILabel new];
    heading.text=@"Cara Home";
    heading.textColor=_upperScreen.tintColor;
    heading.font=[UIFont boldSystemFontOfSize:15];
    heading.accessibilityTraits=UIAccessibilityTraitHeader;
    gameTitle=[UILabel new];
    gameTitle.textColor=UIColor.whiteColor;
    gameTitle.font=[UIFont boldSystemFontOfSize:28];
    gameTitle.adjustsFontSizeToFitWidth=YES;
    gameTitle.minimumScaleFactor=.55;
    gameTitle.numberOfLines=2;
    subtitle=[UILabel new];
    subtitle.font=[UIFont systemFontOfSize:13];
    subtitle.textColor=[UIColor colorWithWhite:.76 alpha:1];
    subtitle.numberOfLines=2;
    __weak CaraHomeScreen* owner=self;
    playButton=HomeButton(@"Play",@"play.fill",^{ [owner playSelected]; });
    playButton.contentHorizontalAlignment=UIControlContentHorizontalAlignmentLeading;
    UIStackView* preview=[[UIStackView alloc] initWithArrangedSubviews:@[heading,gameTitle,subtitle,playButton]];
    preview.axis=UILayoutConstraintAxisVertical;
    preview.spacing=3;
    artwork=[UIImageView new];
    artwork.contentMode=UIViewContentModeScaleAspectFit;
    artwork.tintColor=_upperScreen.tintColor;
    [NSLayoutConstraint activateConstraints:@[
        [artwork.widthAnchor constraintEqualToConstant:48],
        [artwork.heightAnchor constraintEqualToConstant:48],
    ]];
    UIStackView* banner=[[UIStackView alloc] initWithArrangedSubviews:@[artwork,preview]];
    banner.alignment=UIStackViewAlignmentCenter;
    banner.spacing=12;
    [_upperScreen addSubview:banner];
    banner.translatesAutoresizingMaskIntoConstraints=NO;
    [NSLayoutConstraint activateConstraints:@[
        [banner.centerYAnchor constraintEqualToAnchor:_upperScreen.centerYAnchor],
        [banner.leadingAnchor constraintEqualToAnchor:_upperScreen.leadingAnchor constant:14],
        [banner.trailingAnchor constraintEqualToAnchor:_upperScreen.trailingAnchor constant:-14],
        [banner.topAnchor constraintGreaterThanOrEqualToAnchor:_upperScreen.topAnchor constant:4],
        [banner.bottomAnchor constraintLessThanOrEqualToAnchor:_upperScreen.bottomAnchor constant:-4],
    ]];

    list=[[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
    list.backgroundColor=UIColor.clearColor;
    list.separatorColor=[UIColor colorWithWhite:.25 alpha:1];
    list.rowHeight=44;
    list.dataSource=self;
    list.delegate=self;
    list.accessibilityLabel=@"Game library";
    [list registerClass:UITableViewCell.class forCellReuseIdentifier:@"game"];
    UIButton* add=HomeButton(@"Add Games",@"plus",^{ if (owner.addGame) owner.addGame(); });
    UIButton* settings=HomeButton(@"Settings",@"gearshape",^{ if (owner.settings) owner.settings(); });
    resumeButton=HomeButton(@"Resume",@"play.circle",^{ if (owner.resumeGame) owner.resumeGame(); });
    UIStackView* toolbar=[[UIStackView alloc] initWithArrangedSubviews:@[add,settings,resumeButton]];
    toolbar.distribution=UIStackViewDistributionFillEqually;
    UIStackView* library=[[UIStackView alloc] initWithArrangedSubviews:@[list,toolbar]];
    library.axis=UILayoutConstraintAxisVertical;
    [_lowerScreen addSubview:library];
    library.translatesAutoresizingMaskIntoConstraints=NO;
    [NSLayoutConstraint activateConstraints:@[
        [library.topAnchor constraintEqualToAnchor:_lowerScreen.topAnchor],
        [library.bottomAnchor constraintEqualToAnchor:_lowerScreen.bottomAnchor],
        [library.leadingAnchor constraintEqualToAnchor:_lowerScreen.leadingAnchor],
        [library.trailingAnchor constraintEqualToAnchor:_lowerScreen.trailingAnchor],
        [toolbar.heightAnchor constraintEqualToConstant:44],
    ]];
    [self updateGames:@[] detail:@"Loading your games…" canResume:NO];
    return self;
}
- (void)updateGames:(NSArray<CaraLibraryGame*>*)newGames detail:(NSString*)detail canResume:(BOOL)resume {
    NSURL* previous=selected<(NSInteger)games.count?games[selected].url:nil;
    games=[newGames copy];
    status=[detail copy];
    canResume=resume;
    resumeButton.hidden=!resume;
    selected=games.count?0:NSNotFound;
    if (previous) {
        NSUInteger match=[games indexOfObjectPassingTest:^BOOL(CaraLibraryGame* game,NSUInteger index,BOOL* stop) { return [game.url isEqual:previous]; }];
        if (match!=NSNotFound) selected=match;
    }
    [list reloadData];
    [self updateSelection:UITableViewScrollPositionNone];
}
- (void)updateSelection:(UITableViewScrollPosition)scroll {
    CaraLibraryGame* game=selected<(NSInteger)games.count?games[selected]:nil;
    gameTitle.text=game.title?:@"Your game library";
    artwork.image=[UIImage systemImageNamed:@"gamecontroller.fill"];
    const unsigned generation=++previewGeneration;
    if (game) {
        NSString* key=[NSString stringWithFormat:@"%@-%@-%llu",game.url.absoluteString,game.modified,game.fileSize];
        NSDictionary* cached=[artworkCache objectForKey:key];
        if (cached) {
            artwork.image=cached[@"image"]?:artwork.image;
            gameTitle.text=cached[@"title"]?:gameTitle.text;
        } else {
            dispatch_async(artworkQueue,^{
                if (generation!=self->previewGeneration.load()) return;
                NSDictionary* metadata=CaraReadGameArtwork(game)?:@{};
                [self->artworkCache setObject:metadata forKey:key];
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (generation!=self->previewGeneration.load() || !metadata.count) return;
                    self->artwork.image=metadata[@"image"];
                    self->gameTitle.text=metadata[@"title"];
                });
            });
        }
    }
    subtitle.text=game?(status.length?status:game.source):(status.length?status:@"Add a game from Files to get started.");
    playButton.hidden=!game;
    if (game) [list selectRowAtIndexPath:[NSIndexPath indexPathForRow:selected inSection:0] animated:NO scrollPosition:scroll];
}
- (void)playSelected {
    if (selected<(NSInteger)games.count && self.play) self.play(games[selected].url);
}
- (NSInteger)tableView:(UITableView*)table numberOfRowsInSection:(NSInteger)section { return games.count; }
- (UITableViewCell*)tableView:(UITableView*)table cellForRowAtIndexPath:(NSIndexPath*)path {
    UITableViewCell* cell=[table dequeueReusableCellWithIdentifier:@"game" forIndexPath:path];
    UIListContentConfiguration* content=[UIListContentConfiguration cellConfiguration];
    content.text=games[path.row].title;
    content.textProperties.color=UIColor.whiteColor;
    content.image=[UIImage systemImageNamed:@"gamecontroller"];
    content.imageProperties.tintColor=_lowerScreen.tintColor;
    cell.contentConfiguration=content;
    cell.backgroundColor=UIColor.clearColor;
    UIView* highlight=[UIView new];
    highlight.backgroundColor=[UIColor colorWithRed:.29 green:.18 blue:.10 alpha:1];
    cell.selectedBackgroundView=highlight;
    return cell;
}
- (void)tableView:(UITableView*)table didSelectRowAtIndexPath:(NSIndexPath*)path {
    selected=path.row;
    [self updateSelection:UITableViewScrollPositionNone];
}
- (void)resetInput { held=nil; }
- (void)handleButtons:(NSSet<NSString*>*)buttons circle:(CGPoint)circle {
    NSMutableSet<NSString*>* current=[buttons mutableCopy];
    if (circle.y>.6) [current addObject:@"up"];
    if (circle.y<-.6) [current addObject:@"down"];
    if (circle.x>.6) [current addObject:@"right"];
    if (circle.x<-.6) [current addObject:@"left"];
    NSMutableSet<NSString*>* edges=[current mutableCopy];
    if (held) [edges minusSet:held];
    held=[current copy];
    if ([edges containsObject:@"b"] && canResume && self.resumeGame) { self.resumeGame(); return; }
    if ([edges containsObject:@"a"]) { [self playSelected]; return; }
    if (!games.count) return;
    if ([edges containsObject:@"up"] || [edges containsObject:@"left"]) selected=MAX(0,selected-1);
    else if ([edges containsObject:@"down"] || [edges containsObject:@"right"]) selected=MIN((NSInteger)games.count-1,selected+1);
    else return;
    [self updateSelection:UITableViewScrollPositionMiddle];
}
@end
