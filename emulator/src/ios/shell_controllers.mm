// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/shell_controllers.h"
#include <array>
#include <cstring>
#include <cmath>
#include <atomic>

static UIColor* CaraOrange() { return [UIColor colorWithRed:1 green:.48 blue:.24 alpha:1]; }
static UIColor* CaraBackground() {
    return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
        return traits.userInterfaceStyle==UIUserInterfaceStyleDark?UIColor.blackColor:UIColor.systemGroupedBackgroundColor;
    }];
}

// Read only the unencrypted icon section; never initialize or borrow the active
// emulator, decrypt a game, or read the whole ROM just to decorate the library.
static NSDictionary* ReadGameArtwork(CaraLibraryGame* game) {
    BOOL scoped=[game.url startAccessingSecurityScopedResource];
    @try {
        NSNumber* ubiquitous=nil; NSNumber* downloaded=nil;
        [game.url getResourceValue:&ubiquitous forKey:NSURLIsUbiquitousItemKey error:nil];
        [game.url getResourceValue:&downloaded forKey:NSURLUbiquitousItemIsDownloadedKey error:nil];
        if (ubiquitous.boolValue && !downloaded.boolValue) return nil;
        NSFileHandle* file=[NSFileHandle fileHandleForReadingFromURL:game.url error:nil];
        if (!file) return nil;
        NSData* icon=nil;
        @try {
            const auto read=[&](uint64_t offset,NSUInteger count)->NSData* {
                if (offset>game.fileSize || count>game.fileSize-offset) return nil;
                [file seekToFileOffset:offset]; NSData* data=[file readDataOfLength:count];
                return data.length==count?data:nil;
            };
            const auto word=[](const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; };
            NSData* header=read(0,512);
            if (header.length!=512) return nil;
            const uint8_t* p=(const uint8_t*)header.bytes;
            uint64_t base=0;
            if (!memcmp(p+0x100,"NCSD",4)) { base=uint64_t(word(p+0x120))*512; header=read(base,512); p=(const uint8_t*)header.bytes; }
            if (header.length!=512 || memcmp(p+0x100,"NCCH",4) || !(p[0x18f]&4) || p[0x18e]>8) return nil;
            const uint64_t unit=uint64_t(512)<<p[0x18e];
            const uint64_t exefs=base+uint64_t(word(p+0x1a0))*unit;
            NSData* table=read(exefs,512); if (!table) return nil;
            p=(const uint8_t*)table.bytes;
            for (unsigned n=0;n<10;++n) if (!memcmp(p+16*n,"icon\0\0\0\0",8)) {
                if (word(p+16*n+12)<0x36c0) return nil;
                icon=read(exefs+512+word(p+16*n+8),0x36c0); break;
            }
        } @finally { [file closeFile]; }
        if (icon.length!=0x36c0 || memcmp(icon.bytes,"SMDH",4)) return nil;
        const uint8_t* bytes=(const uint8_t*)icon.bytes;
        NSString* title=nil;
        for (unsigned language : {1u,0u}) {
            const uint8_t* text=bytes+8+language*0x200+0x80;
            NSUInteger count=0; while (count<0x80 && (text[2*count] || text[2*count+1])) ++count;
            NSString* candidate=[[NSString alloc] initWithBytes:text length:count*2 encoding:NSUTF16LittleEndianStringEncoding];
            if (candidate.length) { title=[[candidate stringByReplacingOccurrencesOfString:@"\n" withString:@" "] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]; break; }
        }
        std::array<uint8_t,48*48*4> rgba{};
        for (unsigned y=0;y<48;++y) for (unsigned x=0;x<48;++x) {
            unsigned morton=0; for (unsigned bit=0;bit<3;++bit) { morton|=((x>>bit)&1)<<(2*bit); morton|=((y>>bit)&1)<<(2*bit+1); }
            const unsigned index=((y/8)*6+x/8)*64+morton;
            const unsigned value=bytes[0x24c0+index*2] | (unsigned(bytes[0x24c0+index*2+1])<<8);
            auto* out=rgba.data()+(y*48+x)*4;
            out[0]=uint8_t(((value>>11)*255+15)/31); out[1]=uint8_t((((value>>5)&63)*255+31)/63); out[2]=uint8_t(((value&31)*255+15)/31); out[3]=255;
        }
        NSData* pixels=[NSData dataWithBytes:rgba.data() length:rgba.size()];
        CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
        CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
        CGImageRef cg=CGImageCreate(48,48,8,32,48*4,space,CGBitmapInfo(kCGImageAlphaLast)|kCGBitmapByteOrder32Big,provider,nullptr,false,kCGRenderingIntentDefault);
        UIImage* image=cg?[UIImage imageWithCGImage:cg]:nil;
        if (cg) CGImageRelease(cg); CGColorSpaceRelease(space); CGDataProviderRelease(provider);
        return image?@{@"image":image,@"title":title?:game.title}:nil;
    } @catch (NSException* exception) { return nil; }
    @finally { if (scoped) [game.url stopAccessingSecurityScopedResource]; }
}

@interface CaraGameCell : UICollectionViewCell
@property(strong,nonatomic) UIImageView* artwork;
@property(strong,nonatomic) UILabel* monogram;
@property(strong,nonatomic) UILabel* titleLabel;
@property(strong,nonatomic) UILabel* subtitle;
@property(copy,nonatomic) NSString* gameKey;
- (void)configure:(CaraLibraryGame*)game;
@end
@implementation CaraGameCell
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        UIView* tile=[UIView new]; tile.translatesAutoresizingMaskIntoConstraints=NO;
        tile.backgroundColor=[UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* t) { return t.userInterfaceStyle==UIUserInterfaceStyleDark?[UIColor colorWithRed:.16 green:.10 blue:.07 alpha:1]:[UIColor colorWithRed:1 green:.91 blue:.83 alpha:1]; }];
        tile.layer.cornerRadius=22; tile.clipsToBounds=YES;
        _artwork=[UIImageView new]; _artwork.translatesAutoresizingMaskIntoConstraints=NO; _artwork.contentMode=UIViewContentModeScaleAspectFill; _artwork.clipsToBounds=YES;
        _monogram=[UILabel new]; _monogram.translatesAutoresizingMaskIntoConstraints=NO; _monogram.font=[UIFont systemFontOfSize:42 weight:UIFontWeightSemibold]; _monogram.textColor=CaraOrange(); _monogram.textAlignment=NSTextAlignmentCenter;
        [tile addSubview:_artwork]; [tile addSubview:_monogram];
        _titleLabel=[UILabel new]; _titleLabel.numberOfLines=2; _titleLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]; _titleLabel.adjustsFontForContentSizeCategory=YES;
        _subtitle=[UILabel new]; _subtitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; _subtitle.adjustsFontForContentSizeCategory=YES; _subtitle.textColor=UIColor.secondaryLabelColor;
        UIStackView* stack=[[UIStackView alloc] initWithArrangedSubviews:@[tile,_titleLabel,_subtitle]]; stack.axis=UILayoutConstraintAxisVertical; stack.spacing=6; stack.translatesAutoresizingMaskIntoConstraints=NO; stack.alignment=UIStackViewAlignmentFill;
        [self.contentView addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.contentView.topAnchor], [stack.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor], [stack.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor], [stack.bottomAnchor constraintLessThanOrEqualToAnchor:self.contentView.bottomAnchor],
            [tile.heightAnchor constraintEqualToAnchor:tile.widthAnchor],
            [_artwork.topAnchor constraintEqualToAnchor:tile.topAnchor],[_artwork.bottomAnchor constraintEqualToAnchor:tile.bottomAnchor],[_artwork.leadingAnchor constraintEqualToAnchor:tile.leadingAnchor],[_artwork.trailingAnchor constraintEqualToAnchor:tile.trailingAnchor],
            [_monogram.centerXAnchor constraintEqualToAnchor:tile.centerXAnchor],[_monogram.centerYAnchor constraintEqualToAnchor:tile.centerYAnchor],
        ]];
        self.isAccessibilityElement=YES;
        self.accessibilityTraits=UIAccessibilityTraitButton;
    } return self;
}
- (void)configure:(CaraLibraryGame*)game {
    self.gameKey=[NSString stringWithFormat:@"%@-%@-%llu",game.url.absoluteString,game.modified,game.fileSize];
    self.artwork.image=nil; self.monogram.hidden=NO;
    NSArray* words=[game.title componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSMutableString* initials=[NSMutableString new];
    for (NSString* word in words) if (word.length && initials.length<2) [initials appendString:[word substringToIndex:1].uppercaseString];
    self.monogram.text=initials.length?initials:@"C";
    self.titleLabel.text=game.title;
    self.subtitle.text=[NSByteCountFormatter stringFromByteCount:game.fileSize countStyle:NSByteCountFormatterCountStyleFile];
    self.accessibilityLabel=[NSString stringWithFormat:@"%@, %@",game.title,self.subtitle.text];
    self.accessibilityHint=@"Opens this game";
}
- (void)setHighlighted:(BOOL)value { [super setHighlighted:value]; self.contentView.alpha=value?.65:1; }
@end

@interface CaraLibraryController () <UICollectionViewDataSource,UICollectionViewDelegate,UISearchResultsUpdating>
@end
@implementation CaraLibraryController {
    UICollectionView* collection;
    NSArray<CaraLibraryGame*>* games;
    NSArray<CaraLibraryGame*>* filtered;
    UILabel* countLabel;
    UILabel* locationLabel;
    UILabel* feedbackTitle;
    UILabel* feedbackDetail;
    UIActivityIndicatorView* feedbackSpinner;
    UIView* emptyView;
    UILabel* emptyTitle;
    UILabel* emptyDetail;
    UIRefreshControl* refreshControl;
    UISearchController* search;
    NSCache* artworkCache;
    dispatch_queue_t artworkQueue;
    BOOL scanning;
    BOOL reloadPending;
    std::atomic<unsigned> artworkGeneration;
    NSString* scanError;
    UIStackView* stage;
    UIView* hero;
    UIImageView* heroArtwork;
    UILabel* heroTitle;
    UILabel* heroSubtitle;
    UIButton* openButton;
    NSLayoutConstraint* heroHeight;
    NSLayoutConstraint* heroWidth;
    NSLayoutConstraint* feedbackHeight;
    UIStackView* feedbackCard;
    BOOL gridMode;
    NSInteger selectedIndex;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor=CaraBackground(); self.view.tintColor=CaraOrange(); self.title=@"HOME";
    self.navigationController.navigationBar.prefersLargeTitles=YES;
    self.navigationItem.largeTitleDisplayMode=UINavigationItemLargeTitleDisplayModeAlways;
    self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"gearshape"] style:UIBarButtonItemStylePlain target:self action:@selector(settingsTapped)];
    self.navigationItem.leftBarButtonItem.accessibilityLabel=@"Settings";
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"plus"] style:UIBarButtonItemStylePlain target:self action:@selector(addTapped)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel=@"Add games";
    search=[[UISearchController alloc] initWithSearchResultsController:nil]; search.searchResultsUpdater=self; search.obscuresBackgroundDuringPresentation=NO; search.searchBar.placeholder=@"Search your games";
    self.navigationItem.searchController=search; self.navigationItem.hidesSearchBarWhenScrolling=NO; self.definesPresentationContext=YES;
    countLabel=[UILabel new]; countLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle2]; countLabel.adjustsFontForContentSizeCategory=YES;
    locationLabel=[UILabel new]; locationLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline]; locationLabel.adjustsFontForContentSizeCategory=YES; locationLabel.textColor=UIColor.secondaryLabelColor; locationLabel.numberOfLines=2;
    UIStackView* heading=[[UIStackView alloc] initWithArrangedSubviews:@[countLabel,locationLabel]]; heading.axis=UILayoutConstraintAxisVertical; heading.spacing=5; heading.translatesAutoresizingMaskIntoConstraints=NO;
    [self.view addSubview:heading];
    UICollectionViewLayout* layout=[self gameLayout];
    collection=[[UICollectionView alloc] initWithFrame:CGRectZero collectionViewLayout:layout]; collection.translatesAutoresizingMaskIntoConstraints=NO; collection.backgroundColor=UIColor.clearColor; collection.alwaysBounceVertical=YES; collection.keyboardDismissMode=UIScrollViewKeyboardDismissModeOnDrag;
    collection.dataSource=self; collection.delegate=self; [collection registerClass:CaraGameCell.class forCellWithReuseIdentifier:@"game"];
    refreshControl=[UIRefreshControl new]; [refreshControl addTarget:self action:@selector(reloadLibrary) forControlEvents:UIControlEventValueChanged]; collection.refreshControl=refreshControl;
    hero=[UIView new]; hero.layer.cornerRadius=28; hero.clipsToBounds=YES;
    hero.backgroundColor=[UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) { return traits.userInterfaceStyle==UIUserInterfaceStyleDark?[UIColor colorWithRed:.085 green:.065 blue:.045 alpha:1]:[UIColor colorWithRed:1 green:.96 blue:.90 alpha:1]; }];
    heroArtwork=[UIImageView new]; heroArtwork.contentMode=UIViewContentModeScaleAspectFit; heroArtwork.clipsToBounds=YES; heroArtwork.layer.cornerRadius=24;
    heroTitle=[UILabel new]; heroTitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1]; heroTitle.adjustsFontForContentSizeCategory=YES; heroTitle.numberOfLines=2; heroTitle.textAlignment=NSTextAlignmentCenter;
    heroSubtitle=[UILabel new]; heroSubtitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline]; heroSubtitle.adjustsFontForContentSizeCategory=YES; heroSubtitle.numberOfLines=2; heroSubtitle.textAlignment=NSTextAlignmentCenter; heroSubtitle.textColor=UIColor.secondaryLabelColor;
    openButton=[UIButton buttonWithType:UIButtonTypeSystem]; openButton.configuration=[UIButtonConfiguration filledButtonConfiguration]; [openButton setTitle:@"Open" forState:UIControlStateNormal]; [openButton addTarget:self action:@selector(openSelected) forControlEvents:UIControlEventTouchUpInside];
    UIStackView* heroContent=[[UIStackView alloc] initWithArrangedSubviews:@[heroArtwork,heroTitle,heroSubtitle,openButton]]; heroContent.axis=UILayoutConstraintAxisVertical; heroContent.spacing=10; heroContent.alignment=UIStackViewAlignmentCenter; heroContent.translatesAutoresizingMaskIntoConstraints=NO;
    [hero addSubview:heroContent];
    NSLayoutConstraint* iconHeight=[heroArtwork.heightAnchor constraintEqualToAnchor:hero.heightAnchor multiplier:.32]; iconHeight.priority=750;
    [NSLayoutConstraint activateConstraints:@[[heroContent.centerYAnchor constraintEqualToAnchor:hero.centerYAnchor],[heroContent.topAnchor constraintGreaterThanOrEqualToAnchor:hero.topAnchor constant:16],[heroContent.bottomAnchor constraintLessThanOrEqualToAnchor:hero.bottomAnchor constant:-16],[heroContent.leadingAnchor constraintEqualToAnchor:hero.leadingAnchor constant:18],[heroContent.trailingAnchor constraintEqualToAnchor:hero.trailingAnchor constant:-18],iconHeight,[heroArtwork.widthAnchor constraintEqualToAnchor:heroArtwork.heightAnchor],[heroArtwork.heightAnchor constraintLessThanOrEqualToConstant:150],[openButton.widthAnchor constraintGreaterThanOrEqualToConstant:145]]];
    stage=[[UIStackView alloc] initWithArrangedSubviews:@[hero,collection]]; stage.axis=UILayoutConstraintAxisVertical; stage.spacing=8; stage.translatesAutoresizingMaskIntoConstraints=NO; [self.view addSubview:stage];
    heroHeight=[hero.heightAnchor constraintEqualToAnchor:stage.heightAnchor multiplier:.52]; heroHeight.priority=750; heroHeight.active=YES;
    heroWidth=[hero.widthAnchor constraintEqualToAnchor:stage.widthAnchor multiplier:.45]; heroWidth.priority=750;
    UIBarButtonItem* layoutToggle=[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"square.grid.2x2"] style:UIBarButtonItemStylePlain target:self action:@selector(toggleLayout)]; layoutToggle.accessibilityLabel=@"Switch library layout";
    self.navigationItem.leftBarButtonItems=@[self.navigationItem.leftBarButtonItem,layoutToggle];
    feedbackTitle=[UILabel new]; feedbackTitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline]; feedbackTitle.adjustsFontForContentSizeCategory=YES; feedbackTitle.numberOfLines=2;
    feedbackDetail=[UILabel new]; feedbackDetail.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; feedbackDetail.adjustsFontForContentSizeCategory=YES; feedbackDetail.textColor=UIColor.secondaryLabelColor; feedbackDetail.numberOfLines=3;
    UIStackView* feedbackLabels=[[UIStackView alloc] initWithArrangedSubviews:@[feedbackTitle,feedbackDetail]]; feedbackLabels.axis=UILayoutConstraintAxisVertical; feedbackLabels.spacing=3;
    UIImageView* cloud=[[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"icloud"]]; cloud.tintColor=CaraOrange(); cloud.contentMode=UIViewContentModeScaleAspectFit; [cloud.widthAnchor constraintEqualToConstant:26].active=YES;
    feedbackSpinner=[[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium]; feedbackSpinner.hidesWhenStopped=YES;
    UIStackView* feedback=[[UIStackView alloc] initWithArrangedSubviews:@[cloud,feedbackLabels,feedbackSpinner]]; feedback.spacing=12; feedback.alignment=UIStackViewAlignmentCenter; feedback.translatesAutoresizingMaskIntoConstraints=NO; feedback.layoutMarginsRelativeArrangement=YES; feedback.directionalLayoutMargins=NSDirectionalEdgeInsetsMake(14,16,14,16); feedback.backgroundColor=UIColor.secondarySystemGroupedBackgroundColor; feedback.layer.cornerRadius=20;
    feedbackCard=feedback; feedbackHeight=[feedback.heightAnchor constraintEqualToConstant:76]; feedbackHeight.active=YES;
    [self.view addSubview:feedback];
    UILayoutGuide* safe=self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [heading.topAnchor constraintEqualToAnchor:safe.topAnchor constant:12],[heading.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],[heading.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [stage.topAnchor constraintEqualToAnchor:heading.bottomAnchor constant:12],[stage.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:16],[stage.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-16],[stage.bottomAnchor constraintEqualToAnchor:feedback.topAnchor constant:-12],
        [feedback.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:20],[feedback.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-20],[feedback.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-12],
    ]];
    emptyTitle=[UILabel new]; emptyTitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle2]; emptyTitle.adjustsFontForContentSizeCategory=YES; emptyTitle.textAlignment=NSTextAlignmentCenter; emptyTitle.numberOfLines=0;
    emptyDetail=[UILabel new]; emptyDetail.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; emptyDetail.adjustsFontForContentSizeCategory=YES; emptyDetail.textColor=UIColor.secondaryLabelColor; emptyDetail.textAlignment=NSTextAlignmentCenter; emptyDetail.numberOfLines=0;
    UIImageView* emptyIcon=[[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"square.grid.2x2"]]; emptyIcon.contentMode=UIViewContentModeScaleAspectFit; emptyIcon.tintColor=CaraOrange(); [emptyIcon.heightAnchor constraintEqualToConstant:52].active=YES;
    UIButton* add=[UIButton buttonWithType:UIButtonTypeSystem]; add.configuration=[UIButtonConfiguration filledButtonConfiguration]; [add setTitle:@"Add a game" forState:UIControlStateNormal]; [add addTarget:self action:@selector(openTapped) forControlEvents:UIControlEventTouchUpInside];
    UIButton* folder=[UIButton buttonWithType:UIButtonTypeSystem]; [folder setTitle:@"Choose ROMs folder" forState:UIControlStateNormal]; [folder addTarget:self action:@selector(folderTapped) forControlEvents:UIControlEventTouchUpInside];
    UIStackView* empty=[[UIStackView alloc] initWithArrangedSubviews:@[emptyIcon,emptyTitle,emptyDetail,add,folder]]; empty.axis=UILayoutConstraintAxisVertical; empty.spacing=16; empty.alignment=UIStackViewAlignmentCenter; empty.translatesAutoresizingMaskIntoConstraints=NO;
    emptyView=[UIView new]; emptyView.translatesAutoresizingMaskIntoConstraints=NO; [emptyView addSubview:empty]; [self.view addSubview:emptyView];
    [NSLayoutConstraint activateConstraints:@[[emptyView.topAnchor constraintEqualToAnchor:collection.topAnchor],[emptyView.bottomAnchor constraintEqualToAnchor:collection.bottomAnchor],[emptyView.leadingAnchor constraintEqualToAnchor:collection.leadingAnchor],[emptyView.trailingAnchor constraintEqualToAnchor:collection.trailingAnchor],[empty.centerYAnchor constraintEqualToAnchor:emptyView.centerYAnchor],[empty.centerXAnchor constraintEqualToAnchor:emptyView.centerXAnchor],[empty.widthAnchor constraintLessThanOrEqualToConstant:360],[empty.leadingAnchor constraintGreaterThanOrEqualToAnchor:emptyView.leadingAnchor constant:28],[empty.trailingAnchor constraintLessThanOrEqualToAnchor:emptyView.trailingAnchor constant:-28]]];
    selectedIndex=0;
    artworkCache=[NSCache new]; artworkCache.countLimit=160; artworkQueue=dispatch_queue_create("com.dmikey.cara.artwork",DISPATCH_QUEUE_SERIAL);
    [self updateFeedback]; [self reloadLibrary];
}
- (UICollectionViewLayout*)gameLayout {
    const BOOL grid=gridMode;
    UICollectionViewCompositionalLayout* layout=[[UICollectionViewCompositionalLayout alloc] initWithSectionProvider:^NSCollectionLayoutSection*(NSInteger section,id<NSCollectionLayoutEnvironment> environment) {
        CGFloat width=environment.container.effectiveContentSize.width;
        if (!grid) {
            CGFloat availableHeight=environment.container.effectiveContentSize.height;
            CGFloat side=MAX(64,MIN(180,availableHeight-24));
            NSCollectionLayoutItem* item=[NSCollectionLayoutItem itemWithLayoutSize:[NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1] heightDimension:[NSCollectionLayoutDimension fractionalHeightDimension:1]]];
            NSCollectionLayoutGroup* group=[NSCollectionLayoutGroup verticalGroupWithLayoutSize:[NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension absoluteDimension:side] heightDimension:[NSCollectionLayoutDimension absoluteDimension:side]] subitems:@[item]];
            NSCollectionLayoutSection* result=[NSCollectionLayoutSection sectionWithGroup:group]; result.interGroupSpacing=18; result.contentInsets=NSDirectionalEdgeInsetsMake(MAX(12,(availableHeight-side)/2),MAX(18,(width-side)/2),MAX(12,(availableHeight-side)/2),MAX(18,(width-side)/2)); return result;
        }
        BOOL accessibility=UIContentSizeCategoryIsAccessibilityCategory(environment.traitCollection.preferredContentSizeCategory);
        NSInteger columns=MAX(1,MIN(6,NSInteger((width-32)/(accessibility?230:160))));
        CGFloat itemWidth=(width-40-16*(columns-1))/columns;
        NSCollectionLayoutItem* item=[NSCollectionLayoutItem itemWithLayoutSize:[NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1.0/columns] heightDimension:[NSCollectionLayoutDimension estimatedDimension:itemWidth+(accessibility?150:88)]]];
        NSCollectionLayoutGroup* group=[NSCollectionLayoutGroup horizontalGroupWithLayoutSize:[NSCollectionLayoutSize sizeWithWidthDimension:[NSCollectionLayoutDimension fractionalWidthDimension:1] heightDimension:[NSCollectionLayoutDimension estimatedDimension:itemWidth+(accessibility?150:88)]] subitem:item count:columns];
        group.interItemSpacing=[NSCollectionLayoutSpacing fixedSpacing:16]; NSCollectionLayoutSection* result=[NSCollectionLayoutSection sectionWithGroup:group]; result.interGroupSpacing=20; result.contentInsets=NSDirectionalEdgeInsetsMake(18,12,24,12); return result;
    }];
    UICollectionViewCompositionalLayoutConfiguration* config=[UICollectionViewCompositionalLayoutConfiguration new]; config.scrollDirection=grid?UICollectionViewScrollDirectionVertical:UICollectionViewScrollDirectionHorizontal; layout.configuration=config; return layout;
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    BOOL compact=self.view.bounds.size.height<500;
    locationLabel.hidden=compact; feedbackDetail.hidden=compact; heroArtwork.hidden=compact; heroSubtitle.hidden=compact;
    heroTitle.font=[UIFont preferredFontForTextStyle:compact?UIFontTextStyleHeadline:UIFontTextStyleTitle1]; heroTitle.numberOfLines=compact?1:2;
    CGFloat labelsWidth=MAX(100,feedbackCard.bounds.size.width-90);
    CGFloat measured=28+[feedbackTitle sizeThatFits:CGSizeMake(labelsWidth,CGFLOAT_MAX)].height+(compact?0:[feedbackDetail sizeThatFits:CGSizeMake(labelsWidth,CGFLOAT_MAX)].height+3);
    if (fabs(feedbackHeight.constant-measured)>.5) feedbackHeight.constant=measured;
    stage.axis=compact?UILayoutConstraintAxisHorizontal:UILayoutConstraintAxisVertical;
    heroWidth.active=compact && !gridMode; heroHeight.active=!compact && !gridMode;
    self.navigationItem.largeTitleDisplayMode=compact?UINavigationItemLargeTitleDisplayModeNever:UINavigationItemLargeTitleDisplayModeAlways;
}
- (void)toggleLayout {
    gridMode=!gridMode; heroHeight.active=!gridMode && self.view.bounds.size.height>=500; heroWidth.active=!gridMode && self.view.bounds.size.height<500; hero.hidden=gridMode; [collection setCollectionViewLayout:[self gameLayout] animated:YES]; [self.view setNeedsLayout];
}
- (void)openSelected { if (selectedIndex>=0 && selectedIndex<(NSInteger)filtered.count && self.openGame) self.openGame(filtered[selectedIndex].url); }
- (void)updateHero {
    if (!filtered.count) { heroHeight.active=NO; heroWidth.active=NO; hero.hidden=YES; openButton.enabled=NO; return; }
    hero.hidden=gridMode; [self.view setNeedsLayout]; selectedIndex=MAX(0,MIN(selectedIndex,(NSInteger)filtered.count-1));
    CaraLibraryGame* game=filtered[selectedIndex]; heroTitle.text=game.title; heroSubtitle.text=game.source; heroArtwork.image=[UIImage systemImageNamed:@"gamecontroller.fill"]; heroArtwork.tintColor=CaraOrange(); openButton.enabled=YES;
    NSString* key=[NSString stringWithFormat:@"%@-%@-%llu",game.url.absoluteString,game.modified,game.fileSize];
    NSDictionary* cached=[artworkCache objectForKey:key];
    if (cached.count) { heroArtwork.image=cached[@"image"]; heroTitle.text=cached[@"title"]; }
    else { const unsigned generation=artworkGeneration.load(); dispatch_async(artworkQueue,^{ @autoreleasepool {
        if (generation!=self->artworkGeneration.load()) return;
        NSDictionary* metadata=ReadGameArtwork(game)?:@{}; [self->artworkCache setObject:metadata forKey:key];
        dispatch_async(dispatch_get_main_queue(),^{ if (metadata.count && self->filtered.count && self->selectedIndex<(NSInteger)self->filtered.count && [self->filtered[self->selectedIndex].url isEqual:game.url]) { self->heroArtwork.image=metadata[@"image"]; self->heroTitle.text=metadata[@"title"]; } });
    } }); }
}
- (void)viewWillAppear:(BOOL)animated { [super viewWillAppear:animated]; self.navigationController.navigationBar.tintColor=CaraOrange(); [self updateResume]; }
- (void)setCanResume:(BOOL)value { _canResume=value; if (self.isViewLoaded) [self updateResume]; }
- (void)updateResume {
    UIBarButtonItem* add=[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"plus"] style:UIBarButtonItemStylePlain target:self action:@selector(addTapped)]; add.accessibilityLabel=@"Add games";
    if (self.canResume) {
        UIBarButtonItem* resume=[[UIBarButtonItem alloc] initWithTitle:@"Resume game" style:UIBarButtonItemStyleDone target:self action:@selector(resumeTapped)];
        self.navigationItem.rightBarButtonItems=@[add,resume];
    } else self.navigationItem.rightBarButtonItems=@[add];
}
- (void)setSyncTitle:(NSString*)value { _syncTitle=[value copy]; if (self.isViewLoaded) [self updateFeedback]; }
- (void)setSyncDetail:(NSString*)value { _syncDetail=[value copy]; if (self.isViewLoaded) [self updateFeedback]; }
- (void)setSyncing:(BOOL)value { _syncing=value; if (self.isViewLoaded) [self updateFeedback]; }
- (void)updateFeedback {
    feedbackTitle.text=self.syncTitle?:@"Saves stay with you";
    feedbackDetail.text=self.syncDetail?:@"Connect iCloud Drive in Settings to sync saves and states.";
    if (self.syncing) [feedbackSpinner startAnimating]; else [feedbackSpinner stopAnimating];
}
- (void)settingsTapped { if (self.showSettings) self.showSettings(); }
- (void)openTapped { if (self.addGame) self.addGame(); }
- (void)folderTapped { if (self.chooseFolder) self.chooseFolder(); }
- (void)resumeTapped { if (self.resumeGame) self.resumeGame(); }
- (void)addTapped {
    UIAlertController* sheet=[UIAlertController alertControllerWithTitle:@"Add games" message:@"Open a game from Files, or keep your collection in a ROMs folder." preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Open game…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { [self openTapped]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Choose ROMs folder…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { [self folderTapped]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Refresh library" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { [self reloadLibrary]; }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.barButtonItem=self.navigationItem.rightBarButtonItems.firstObject;
    [self presentViewController:sheet animated:YES completion:nil];
}
- (void)reloadLibrary {
    if (!self.isViewLoaded) return;
    if (scanning) { reloadPending=YES; return; }
    scanning=YES; countLabel.text=@"Finding your games…";
    __weak CaraLibraryController* weak=self;
    [self.store scan:^(NSArray<CaraLibraryGame*>* entries,NSString* error) {
        CaraLibraryController* owner=weak; if (!owner) return;
        owner->scanning=NO; owner->games=entries; owner->scanError=error;
        [owner->refreshControl endRefreshing]; [owner updateSearchResultsForSearchController:owner->search];
        if (owner->reloadPending) { owner->reloadPending=NO; [owner reloadLibrary]; }
    }];
}
- (void)updateSearchResultsForSearchController:(UISearchController*)controller {
    NSString* query=controller.searchBar.text?:@"";
    filtered=query.length?[games filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(CaraLibraryGame* game,NSDictionary* bindings) { return [game.title localizedStandardContainsString:query]; }]]:games;
    countLabel.text=[NSString stringWithFormat:@"%lu %@",(unsigned long)filtered.count,filtered.count==1?@"game":@"games"];
    locationLabel.text=scanError?:[NSString stringWithFormat:@"%@%@",self.store.folderName,self.store.hasCustomFolder?@" · includes games stored in Cara":@" · On My iPad / Cara"];
    if (UIDevice.currentDevice.userInterfaceIdiom!=UIUserInterfaceIdiomPad && !scanError && !self.store.hasCustomFolder) locationLabel.text=@"Roms · On My iPhone / Cara";
    locationLabel.textColor=scanError?UIColor.systemOrangeColor:UIColor.secondaryLabelColor;
    emptyView.hidden=filtered.count!=0;
    emptyTitle.text=query.length?@"No matching games":@"Your games live here";
    emptyDetail.text=query.length?@"Try another title.":@"Add a game from Files, or choose a folder for your collection.";
    [collection reloadData]; [self updateHero];
}
- (void)suspendArtwork { ++artworkGeneration; }
- (NSInteger)collectionView:(UICollectionView*)view numberOfItemsInSection:(NSInteger)section { return filtered.count; }
- (__kindof UICollectionViewCell*)collectionView:(UICollectionView*)view cellForItemAtIndexPath:(NSIndexPath*)path {
    CaraGameCell* cell=[view dequeueReusableCellWithReuseIdentifier:@"game" forIndexPath:path]; CaraLibraryGame* game=filtered[path.item]; [cell configure:game];
    cell.titleLabel.hidden=!gridMode; cell.subtitle.hidden=!gridMode;
    cell.artwork.superview.layer.borderColor=CaraOrange().CGColor; cell.artwork.superview.layer.borderWidth=path.item==selectedIndex && !gridMode?2.5:0;
    NSString* key=cell.gameKey;
    void(^apply)(NSDictionary*)=^(NSDictionary* metadata) {
        if (!metadata.count || ![cell.gameKey isEqual:key]) return;
        cell.artwork.image=metadata[@"image"]; cell.monogram.hidden=YES; cell.titleLabel.text=metadata[@"title"];
        cell.accessibilityLabel=[NSString stringWithFormat:@"%@, %@",cell.titleLabel.text,cell.subtitle.text];
    };
    NSDictionary* cached=[artworkCache objectForKey:key];
    if (cached) apply(cached);
    else { const unsigned generation=artworkGeneration.load(); dispatch_async(artworkQueue,^{ @autoreleasepool {
        if (generation!=self->artworkGeneration.load()) return;
        NSDictionary* metadata=ReadGameArtwork(game)?:@{}; [self->artworkCache setObject:metadata forKey:key];
        dispatch_async(dispatch_get_main_queue(),^{ if (metadata.count && generation==self->artworkGeneration.load()) apply(metadata); });
    } }); }
    return cell;
}
- (void)scrollViewDidEndDecelerating:(UIScrollView*)scroll { [self selectCenteredTile]; }
- (void)scrollViewDidEndScrollingAnimation:(UIScrollView*)scroll { [self selectCenteredTile]; }
- (void)selectCenteredTile {
    if (gridMode || !filtered.count) return;
    CGFloat center=collection.contentOffset.x+collection.bounds.size.width/2;
    NSIndexPath* nearest=nil; CGFloat distance=CGFLOAT_MAX;
    for (NSIndexPath* path in collection.indexPathsForVisibleItems) {
        UICollectionViewLayoutAttributes* attributes=[collection layoutAttributesForItemAtIndexPath:path]; CGFloat d=fabs(attributes.center.x-center);
        if (d<distance) { distance=d; nearest=path; }
    }
    if (!nearest || nearest.item==selectedIndex) return;
    selectedIndex=nearest.item;
    for (NSIndexPath* path in collection.indexPathsForVisibleItems) { CaraGameCell* cell=(CaraGameCell*)[collection cellForItemAtIndexPath:path]; cell.artwork.superview.layer.borderWidth=path.item==selectedIndex?2.5:0; }
    [self updateHero];
}
- (void)collectionView:(UICollectionView*)view didSelectItemAtIndexPath:(NSIndexPath*)path {
    if (gridMode || path.item==selectedIndex) { if (self.openGame) self.openGame(filtered[path.item].url); return; }
    selectedIndex=path.item; [collection reloadData]; [self updateHero];
    [collection scrollToItemAtIndexPath:path atScrollPosition:UICollectionViewScrollPositionCenteredHorizontally animated:YES];
}
@end

@implementation CaraSettingsController {
    BOOL completed;
}
- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }
- (void)viewDidLoad {
    [super viewDidLoad]; if (!self.title.length) self.title=@"Settings"; self.view.backgroundColor=CaraBackground(); self.view.tintColor=CaraOrange();
    self.tableView.rowHeight=UITableViewAutomaticDimension; self.tableView.estimatedRowHeight=62;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
}
- (void)refresh { if (self.isViewLoaded) [self.tableView reloadData]; }
- (void)done { completed=YES; [self dismissViewControllerAnimated:YES completion:^{ if (self.finished) self.finished(); }]; }
- (void)presentationControllerDidDismiss:(UIPresentationController*)controller { if (!completed && self.finished) self.finished(); completed=YES; }
- (NSInteger)numberOfSectionsInTableView:(UITableView*)table { return self.sections.count; }
- (NSInteger)tableView:(UITableView*)table numberOfRowsInSection:(NSInteger)section { return [self.sections[section][@"rows"] count]; }
- (NSString*)tableView:(UITableView*)table titleForHeaderInSection:(NSInteger)section { return self.sections[section][@"title"]; }
- (NSString*)tableView:(UITableView*)table titleForFooterInSection:(NSInteger)section { return self.sections[section][@"footer"]; }
- (UITableViewCell*)tableView:(UITableView*)table cellForRowAtIndexPath:(NSIndexPath*)path {
    NSDictionary* row=self.sections[path.section][@"rows"][path.row];
    UITableViewCell* cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text=row[@"title"]; cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; cell.textLabel.adjustsFontForContentSizeCategory=YES; cell.textLabel.numberOfLines=0;
    cell.detailTextLabel.text=row[@"subtitle"]; cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; cell.detailTextLabel.adjustsFontForContentSizeCategory=YES; cell.detailTextLabel.numberOfLines=0;
    cell.imageView.image=[UIImage systemImageNamed:row[@"symbol"]?:@"circle"]; cell.imageView.tintColor=[row[@"destructive"] boolValue]?UIColor.systemRedColor:CaraOrange();
    BOOL enabled=!row[@"enabled"] || [row[@"enabled"] boolValue];
    cell.textLabel.textColor=!enabled?UIColor.tertiaryLabelColor:([row[@"destructive"] boolValue]?UIColor.systemRedColor:UIColor.labelColor);
    cell.selectionStyle=row[@"action"] && enabled?UITableViewCellSelectionStyleDefault:UITableViewCellSelectionStyleNone;
    cell.userInteractionEnabled=enabled;
    if (row[@"value"]) {
        UISwitch* toggle=[UISwitch new]; toggle.onTintColor=CaraOrange(); toggle.on=[row[@"value"] boolValue]; toggle.enabled=enabled; toggle.accessibilityLabel=row[@"title"];
        __weak UISwitch* weakToggle=toggle;
        [toggle addAction:[UIAction actionWithHandler:^(UIAction* action) { void(^change)(BOOL)=row[@"change"]; if (change) change(weakToggle.on); }] forControlEvents:UIControlEventValueChanged]; cell.accessoryView=toggle;
    } else if (row[@"action"] && enabled) cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}
- (void)tableView:(UITableView*)table didSelectRowAtIndexPath:(NSIndexPath*)path {
    [table deselectRowAtIndexPath:path animated:YES]; NSDictionary* row=self.sections[path.section][@"rows"][path.row];
    void(^action)(void)=row[@"action"]; if (!action || (row[@"enabled"] && ![row[@"enabled"] boolValue])) return;
    completed=YES; [self dismissViewControllerAnimated:YES completion:action];
}
@end

@implementation CaraStatsView {
    UIStackView* bar;
    NSArray<UILabel*>* values;
    NSArray<UILabel*>* labels;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.backgroundColor=[UIColor colorWithRed:.025 green:.035 blue:.025 alpha:1]; self.layer.cornerRadius=12;
        self.layer.borderWidth=.5; self.layer.borderColor=[UIColor colorWithRed:.23 green:.29 blue:.20 alpha:1].CGColor;
        self.userInteractionEnabled=NO; self.isAccessibilityElement=YES;
        NSMutableArray* columns=[NSMutableArray new],*numbers=[NSMutableArray new],*captions=[NSMutableArray new];
        for (NSString* name in @[@"FPS",@"SPEED",@"GUEST CPU"]) {
            UILabel* value=[UILabel new]; value.text=@"—"; value.textColor=[UIColor colorWithRed:.90 green:.87 blue:.78 alpha:1]; value.textAlignment=NSTextAlignmentCenter; value.adjustsFontSizeToFitWidth=YES; value.minimumScaleFactor=.65;
            UILabel* label=[UILabel new]; label.text=name; label.textColor=[UIColor colorWithRed:.59 green:.64 blue:.51 alpha:1]; label.textAlignment=NSTextAlignmentCenter;
            UIStackView* column=[[UIStackView alloc] initWithArrangedSubviews:@[value,label]]; column.axis=UILayoutConstraintAxisVertical; column.spacing=1;
            [columns addObject:column]; [numbers addObject:value]; [captions addObject:label];
        }
        values=numbers; labels=captions;
        bar=[[UIStackView alloc] initWithArrangedSubviews:columns]; bar.distribution=UIStackViewDistributionFillEqually; bar.alignment=UIStackViewAlignmentCenter;
        [self addSubview:bar];
    } return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    bar.frame=CGRectInset(self.bounds,4,2);
    CGFloat numberSize=self.bounds.size.height<38?13:17;
    for (UILabel* value in values) value.font=[UIFont monospacedDigitSystemFontOfSize:numberSize weight:UIFontWeightSemibold];
    for (UILabel* label in labels) label.font=[UIFont systemFontOfSize:self.bounds.size.height<38?7:8.5 weight:UIFontWeightSemibold];
}
- (void)updateGameFPS:(double)fps speed:(double)speed guestClock:(unsigned)clock adaptive:(BOOL)adaptive {
    values[0].text=std::isfinite(fps) && fps>=0?[NSString stringWithFormat:@"%.0f",fps]:@"—";
    values[1].text=[NSString stringWithFormat:@"%.0f%%",speed];
    values[2].text=[NSString stringWithFormat:@"%u%%",clock];
    labels[2].text=adaptive?@"CPU AUTO":@"GUEST CPU";
    values[1].textColor=speed<95?CaraOrange():[UIColor colorWithRed:.75 green:.83 blue:.63 alpha:1];
    self.accessibilityLabel=[NSString stringWithFormat:@"%@ game frames per second, %.0f percent speed, guest CPU %u percent%@",values[0].text,speed,clock,adaptive?@", adaptive":@""];
}
@end

@implementation CaraQuickSettingsController {
    NSArray<UIButton*>* modeButtons;
    BOOL completed;
}
- (instancetype)init { if ((self=[super init])) _modeSelectionEnabled=YES; return self; }
- (void)viewDidLoad {
    [super viewDidLoad]; if (!self.title.length) self.title=@"Settings"; self.view.backgroundColor=CaraBackground(); self.view.tintColor=CaraOrange();
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
    UIScrollView* scroll=[UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints=NO; [self.view addSubview:scroll];
    UIStackView* body=[UIStackView new]; body.axis=UILayoutConstraintAxisVertical; body.spacing=16; body.translatesAutoresizingMaskIntoConstraints=NO;
    [scroll addSubview:body];
    UILabel* title=[UILabel new]; title.text=@"Play your way"; title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle1]; title.adjustsFontForContentSizeCategory=YES;
    [body addArrangedSubview:title];
    __weak CaraQuickSettingsController* weakSelf=self;
    NSMutableArray* modes=[NSMutableArray new];
    for (NSString* mode in @[@"accuracy",@"performance"]) {
        const BOOL accuracy=[mode isEqual:@"accuracy"];
        UIButton* button=[UIButton buttonWithType:UIButtonTypeSystem];
        UIButtonConfiguration* config=[UIButtonConfiguration borderedButtonConfiguration];
        config.title=accuracy?@"Accuracy Mode":@"Performance Mode";
        config.subtitle=accuracy?@"105% fixed CPU clock":@"Adaptive CPU clock";
        config.image=[UIImage systemImageNamed:accuracy?@"scope":@"bolt.fill"];
        config.imagePadding=18; config.titleAlignment=UIButtonConfigurationTitleAlignmentLeading;
        config.contentInsets=NSDirectionalEdgeInsetsMake(25,22,25,22); config.cornerStyle=UIButtonConfigurationCornerStyleLarge;
        config.titleTextAttributesTransformer=^NSDictionary*(NSDictionary* attributes) { NSMutableDictionary* result=[attributes mutableCopy]; result[NSFontAttributeName]=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle2]; return result; };
        config.subtitleTextAttributesTransformer=^NSDictionary*(NSDictionary* attributes) { NSMutableDictionary* result=[attributes mutableCopy]; result[NSFontAttributeName]=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline]; return result; };
        button.configuration=config; button.contentHorizontalAlignment=UIControlContentHorizontalAlignmentLeading; button.enabled=self.modeSelectionEnabled;
        [button addAction:[UIAction actionWithHandler:^(UIAction* action) { CaraQuickSettingsController* owner=weakSelf; if (!owner) return; owner.playMode=mode; if (owner.chooseMode) owner.chooseMode(mode); }] forControlEvents:UIControlEventTouchUpInside];
        button.accessibilityIdentifier=[@"mode-" stringByAppendingString:mode];
        [body addArrangedSubview:button]; [modes addObject:button];
    }
    modeButtons=modes; [self updateMode];
    UILabel* fpsLabel=[UILabel new]; fpsLabel.text=@"FPS meter"; fpsLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]; fpsLabel.adjustsFontForContentSizeCategory=YES;
    UISwitch* fpsToggle=[UISwitch new]; fpsToggle.on=self.showFPS; fpsToggle.onTintColor=CaraOrange(); fpsToggle.accessibilityLabel=@"FPS meter";
    __weak UISwitch* weakToggle=fpsToggle;
    [fpsToggle addAction:[UIAction actionWithHandler:^(UIAction* action) { CaraQuickSettingsController* owner=weakSelf; if (owner.toggleFPS) owner.toggleFPS(weakToggle.on); }] forControlEvents:UIControlEventValueChanged];
    UIStackView* fpsRow=[[UIStackView alloc] initWithArrangedSubviews:@[fpsLabel,[UIView new],fpsToggle]]; fpsRow.spacing=12; fpsRow.alignment=UIStackViewAlignmentCenter; fpsRow.layoutMarginsRelativeArrangement=YES; fpsRow.directionalLayoutMargins=NSDirectionalEdgeInsetsMake(18,20,18,20); fpsRow.backgroundColor=UIColor.secondarySystemGroupedBackgroundColor; fpsRow.layer.cornerRadius=18; [body addArrangedSubview:fpsRow];
    UILabel* detail=[UILabel new]; detail.text=@"Choose a mode, then leave the tuning to Cara. More controls are available in Advanced."; detail.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline]; detail.textColor=UIColor.secondaryLabelColor; detail.numberOfLines=0; detail.adjustsFontForContentSizeCategory=YES; [body addArrangedSubview:detail];
    UIStackView* row=nil;
    for (NSUInteger index=0;index<self.actions.count;++index) {
        if (index%2==0) { row=[UIStackView new]; row.spacing=14; row.distribution=UIStackViewDistributionFillEqually; [body addArrangedSubview:row]; }
        NSDictionary* item=self.actions[index];
        UIButton* button=[UIButton buttonWithType:UIButtonTypeSystem]; UIButtonConfiguration* config=[UIButtonConfiguration tintedButtonConfiguration];
        config.title=item[@"title"]; config.image=[UIImage systemImageNamed:item[@"symbol"]?:@"circle"]; config.imagePlacement=NSDirectionalRectEdgeTop; config.imagePadding=10;
        config.contentInsets=NSDirectionalEdgeInsetsMake(22,10,22,10); config.cornerStyle=UIButtonConfigurationCornerStyleLarge;
        config.titleTextAttributesTransformer=^NSDictionary*(NSDictionary* attributes) { NSMutableDictionary* result=[attributes mutableCopy]; result[NSFontAttributeName]=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]; return result; };
        button.configuration=config; button.enabled=!item[@"enabled"] || [item[@"enabled"] boolValue];
        [button addAction:[UIAction actionWithHandler:^(UIAction* action) {
            void(^run)(void)=item[@"action"]; if (!run) return;
            if ([item[@"keepOpen"] boolValue]) run();
            else { CaraQuickSettingsController* owner=weakSelf; if (!owner) return; owner->completed=YES; [owner dismissViewControllerAnimated:YES completion:run]; }
        }] forControlEvents:UIControlEventTouchUpInside];
        [row addArrangedSubview:button];
    }
    if (self.actions.count%2) [row addArrangedSubview:[UIView new]];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],[scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],[scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],[scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [body.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:22],[body.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],[body.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:22],[body.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-22],[body.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-44],
    ]];
}
- (void)setPlayMode:(NSString*)mode { _playMode=[mode copy]; if (self.isViewLoaded) [self updateMode]; }
- (void)updateMode {
    NSUInteger index=0;
    for (UIButton* button in modeButtons) {
        BOOL selected=[self.playMode isEqual:index++==0?@"accuracy":@"performance"];
        UIButtonConfiguration* config=button.configuration;
        config.baseForegroundColor=selected?CaraOrange():UIColor.labelColor;
        config.background.backgroundColor=selected?[CaraOrange() colorWithAlphaComponent:.12]:UIColor.secondarySystemGroupedBackgroundColor;
        config.background.strokeColor=selected?CaraOrange():UIColor.separatorColor; config.background.strokeWidth=selected?2:1;
        button.configuration=config;
        button.accessibilityTraits=UIAccessibilityTraitButton|(selected?UIAccessibilityTraitSelected:0);
    }
}
- (void)done { completed=YES; [self dismissViewControllerAnimated:YES completion:^{ if (self.finished) self.finished(); }]; }
- (void)presentationControllerDidDismiss:(UIPresentationController*)controller { if (!completed && self.finished) self.finished(); completed=YES; }
@end
