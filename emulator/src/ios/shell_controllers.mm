// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/shell_controllers.h"
#import <QuartzCore/QuartzCore.h>
#import <AudioToolbox/AudioToolbox.h>
#include <array>
#include <cstring>
#include <cmath>
#include <atomic>

// These short bundled UI sounds use the existing system-sound session policy.
// Never activate or reconfigure the emulator's AudioQueue session here.
static NSDictionary<NSString*,NSNumber*>* CaraMenuSoundIDs;
static void CaraPrepareMenuSounds() {
    static dispatch_once_t once;
    dispatch_once(&once,^{
        [NSUserDefaults.standardUserDefaults registerDefaults:@{@"libraryMenuSounds":@YES,@"libraryTheme":@1}];
        NSMutableDictionary* sounds=[NSMutableDictionary new];
        for(NSString* name in @[@"focus",@"select",@"back",@"favorite",@"confirm",@"cancel",@"launch",@"toggle"]) {
            NSURL* url=[NSBundle.mainBundle URLForResource:name withExtension:@"caf" subdirectory:@"MenuSounds"];
            SystemSoundID identifier=0;
            if(url && AudioServicesCreateSystemSoundID((__bridge CFURLRef)url,&identifier)==kAudioServicesNoError) sounds[name]=@(identifier);
        }
        CaraMenuSoundIDs=sounds;
        if([NSProcessInfo.processInfo.environment[@"CARA_MENU_SOUND_SELF_TEST"] isEqual:@"1"]) {
            if(sounds.count!=8) @throw [NSException exceptionWithName:@"CaraMenuSounds" reason:@"All eight bundled sounds must preload with AudioServices." userInfo:nil];
            NSLog(@"Cara menu sound self-test: all eight bundled CAF sounds preloaded.");
        }
    });
}
static void CaraMenuSound(NSString* name) {
    CaraPrepareMenuSounds();
    if(![NSUserDefaults.standardUserDefaults boolForKey:@"libraryMenuSounds"]) return;
    static CFTimeInterval last=0;
    CFTimeInterval now=CACurrentMediaTime(); if(now-last<.075) return;
    SystemSoundID identifier=CaraMenuSoundIDs[name].unsignedIntValue;
    if(identifier) { last=now; AudioServicesPlaySystemSound(identifier); }
}

static UIColor* CaraOrange() { return [UIColor colorWithRed:1 green:.48 blue:.24 alpha:1]; }
static UIColor* CaraAccent() {
    NSInteger index=[NSUserDefaults.standardUserDefaults integerForKey:@"libraryAccent"];
    return @[[UIColor colorWithRed:0 green:.67 blue:.70 alpha:1],UIColor.systemBlueColor,UIColor.systemPinkColor,UIColor.systemPurpleColor,UIColor.systemGrayColor][MAX(0,MIN(4,index))];
}
static UIColor* CaraBackground() {
    return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
        return traits.userInterfaceStyle==UIUserInterfaceStyleDark?[UIColor colorWithRed:.08 green:.09 blue:.10 alpha:1]:[UIColor colorWithRed:.98 green:.97 blue:.97 alpha:1];
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
        tile.backgroundColor=[UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* t) { return t.userInterfaceStyle==UIUserInterfaceStyleDark?[UIColor colorWithRed:.10 green:.20 blue:.21 alpha:1]:[UIColor colorWithRed:.84 green:.95 blue:.94 alpha:1]; }];
        tile.layer.cornerRadius=22; tile.clipsToBounds=YES;
        _artwork=[UIImageView new]; _artwork.translatesAutoresizingMaskIntoConstraints=NO; _artwork.contentMode=UIViewContentModeScaleAspectFill; _artwork.clipsToBounds=YES;
        _monogram=[UILabel new]; _monogram.translatesAutoresizingMaskIntoConstraints=NO; _monogram.font=[UIFont systemFontOfSize:42 weight:UIFontWeightSemibold]; _monogram.textColor=CaraAccent(); _monogram.textAlignment=NSTextAlignmentCenter;
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
    self.accessibilityHint=@"Opens game details. Long press for library actions.";
}
- (void)setHighlighted:(BOOL)value { [super setHighlighted:value]; self.contentView.alpha=value?.65:1; }
@end

@interface CaraLibraryController () <UICollectionViewDataSource,UICollectionViewDelegate,UISearchResultsUpdating>
@end
@implementation CaraLibraryController {
    UICollectionView* collection;
    NSArray<CaraLibraryGame*>* games;
    NSArray<CaraLibraryGame*>* filtered;
    UISegmentedControl* tabs;
    UISearchController* search;
    UILabel* countLabel;
    UILabel* heroTitle;
    UILabel* heroSubtitle;
    UIImageView* heroArtwork;
    UIView* hero;
    UIButton* heroPlay;
    NSCache* artworkCache;
    dispatch_queue_t artworkQueue;
    std::atomic<unsigned> artworkGeneration;
    CaraLibraryGame* recent;
    BOOL scanning;
    BOOL reloadPending;
    BOOL sortRecent;
    UILabel* syncLabel;
    UIStackView* libraryBody;
}
- (void)viewDidLoad {
    [super viewDidLoad]; CaraPrepareMenuSounds(); self.title=@"Cara"; self.view.backgroundColor=CaraBackground(); self.view.tintColor=CaraAccent();
    self.navigationController.navigationBar.prefersLargeTitles=NO;
    tabs=[[UISegmentedControl alloc] initWithItems:@[@"Collection",@"Favorites",@"Recently played"]]; tabs.selectedSegmentIndex=0; tabs.selectedSegmentTintColor=CaraAccent();
    [tabs addTarget:self action:@selector(tabChanged) forControlEvents:UIControlEventValueChanged]; self.navigationItem.titleView=tabs;
    UIBarButtonItem* settings=[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"gearshape"] style:UIBarButtonItemStylePlain target:self action:@selector(settingsTapped)]; settings.accessibilityLabel=@"Settings";
    UIImageView* brandIcon=[[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"rectangle.on.rectangle"]]; brandIcon.contentMode=UIViewContentModeScaleAspectFit; [brandIcon.widthAnchor constraintEqualToConstant:28].active=YES;
    UILabel* brandTitle=[UILabel new]; brandTitle.text=@"Cara"; brandTitle.font=[UIFont systemFontOfSize:25 weight:UIFontWeightBold]; UIStackView* brand=[[UIStackView alloc] initWithArrangedSubviews:@[brandIcon,brandTitle]]; brand.spacing=8; brand.alignment=UIStackViewAlignmentCenter; brand.userInteractionEnabled=NO;
    self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithCustomView:brand];
    self.navigationItem.rightBarButtonItem=settings;
    search=[[UISearchController alloc] initWithSearchResultsController:nil]; search.searchResultsUpdater=self; search.obscuresBackgroundDuringPresentation=NO; search.searchBar.placeholder=@"Search your collection"; self.navigationItem.searchController=search; self.navigationItem.hidesSearchBarWhenScrolling=NO; self.definesPresentationContext=YES;
    artworkCache=[NSCache new]; artworkQueue=dispatch_queue_create("com.dmikey.cara.library.artwork",DISPATCH_QUEUE_SERIAL);
    hero=[UIView new]; hero.backgroundColor=UIColor.secondarySystemGroupedBackgroundColor; hero.layer.cornerRadius=24; hero.clipsToBounds=YES;
    heroArtwork=[UIImageView new]; heroArtwork.contentMode=UIViewContentModeScaleAspectFit; heroArtwork.layer.cornerRadius=20; heroArtwork.clipsToBounds=YES; heroArtwork.tintColor=CaraAccent();
    UILabel* eyebrow=[UILabel new]; eyebrow.text=@"JUMP BACK IN"; eyebrow.font=[UIFont systemFontOfSize:12 weight:UIFontWeightBold]; eyebrow.textColor=CaraAccent();
    heroTitle=[UILabel new]; heroTitle.font=[UIFont systemFontOfSize:28 weight:UIFontWeightBold]; heroTitle.numberOfLines=2;
    heroSubtitle=[UILabel new]; heroSubtitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline]; heroSubtitle.textColor=UIColor.secondaryLabelColor; heroSubtitle.numberOfLines=2;
    heroPlay=[UIButton buttonWithType:UIButtonTypeSystem]; heroPlay.configuration=[UIButtonConfiguration filledButtonConfiguration]; [heroPlay addTarget:self action:@selector(openRecent) forControlEvents:UIControlEventTouchUpInside]; heroPlay.layer.cornerRadius=20;
    UIStackView* copy=[[UIStackView alloc] initWithArrangedSubviews:@[eyebrow,heroTitle,heroSubtitle,heroPlay]]; copy.axis=UILayoutConstraintAxisVertical; copy.spacing=10;
    UIStackView* heroBody=[[UIStackView alloc] initWithArrangedSubviews:@[copy,heroArtwork]]; heroBody.spacing=24; heroBody.alignment=UIStackViewAlignmentCenter; heroBody.translatesAutoresizingMaskIntoConstraints=NO; [hero addSubview:heroBody];
    [NSLayoutConstraint activateConstraints:@[[heroBody.leadingAnchor constraintEqualToAnchor:hero.leadingAnchor constant:24],[heroBody.trailingAnchor constraintEqualToAnchor:hero.trailingAnchor constant:-24],[heroBody.topAnchor constraintEqualToAnchor:hero.topAnchor constant:20],[heroBody.bottomAnchor constraintEqualToAnchor:hero.bottomAnchor constant:-20],[heroArtwork.widthAnchor constraintEqualToAnchor:heroBody.widthAnchor multiplier:.30],[heroArtwork.heightAnchor constraintEqualToConstant:145]]];
    countLabel=[UILabel new]; countLabel.font=[UIFont systemFontOfSize:23 weight:UIFontWeightBold]; countLabel.text=@"Your collection";
    UIButton* sort=[UIButton buttonWithType:UIButtonTypeSystem]; sort.configuration=[UIButtonConfiguration borderedButtonConfiguration]; [sort setTitle:@"Sort" forState:UIControlStateNormal]; sort.showsMenuAsPrimaryAction=YES;
    __weak CaraLibraryController* owner=self;
    sort.menu=[UIMenu menuWithChildren:@[[UIAction actionWithTitle:@"Title" image:[UIImage systemImageNamed:@"textformat.abc"] identifier:nil handler:^(UIAction* a) { CaraLibraryController* c=owner; if(c) { c->sortRecent=NO; CaraMenuSound(@"select"); [c filterGames]; } }],[UIAction actionWithTitle:@"Recently played" image:[UIImage systemImageNamed:@"clock"] identifier:nil handler:^(UIAction* a) { CaraLibraryController* c=owner; if(c) { c->sortRecent=YES; CaraMenuSound(@"select"); [c filterGames]; } }]]];
    UIStackView* heading=[[UIStackView alloc] initWithArrangedSubviews:@[countLabel,[UIView new],sort]]; heading.alignment=UIStackViewAlignmentCenter;
    UICollectionViewFlowLayout* layout=[UICollectionViewFlowLayout new]; layout.minimumInteritemSpacing=18; layout.minimumLineSpacing=22;
    collection=[[UICollectionView alloc] initWithFrame:CGRectZero collectionViewLayout:layout]; collection.backgroundColor=UIColor.clearColor; collection.alwaysBounceVertical=YES; collection.delegate=self; collection.dataSource=self; [collection registerClass:CaraGameCell.class forCellWithReuseIdentifier:@"game"];
    UIRefreshControl* refresh=[UIRefreshControl new]; [refresh addTarget:self action:@selector(reloadLibrary) forControlEvents:UIControlEventValueChanged]; collection.refreshControl=refresh;
    UIButton* add=[UIButton buttonWithType:UIButtonTypeSystem]; add.configuration=[UIButtonConfiguration borderedButtonConfiguration]; [add setTitle:@"＋  Add games" forState:UIControlStateNormal]; [add addTarget:self action:@selector(addTapped) forControlEvents:UIControlEventTouchUpInside];
    syncLabel=[UILabel new]; syncLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; syncLabel.textColor=UIColor.secondaryLabelColor; syncLabel.numberOfLines=2;
    UIStackView* footer=[[UIStackView alloc] initWithArrangedSubviews:@[add,syncLabel]]; footer.spacing=20; footer.alignment=UIStackViewAlignmentCenter;
    UIStackView* body=[[UIStackView alloc] initWithArrangedSubviews:@[hero,heading,collection,footer]]; libraryBody=body; body.axis=UILayoutConstraintAxisVertical; body.spacing=18; body.translatesAutoresizingMaskIntoConstraints=NO; [self.view addSubview:body];
    [NSLayoutConstraint activateConstraints:@[[body.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24],[body.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24],[body.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:12],[body.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-12]]];
    [self reloadLibrary];
}
- (void)viewWillAppear:(BOOL)animated { [super viewWillAppear:animated]; [self applyAppearance]; }
- (void)applyAppearance { NSInteger style=[NSUserDefaults.standardUserDefaults integerForKey:@"libraryTheme"]; self.navigationController.overrideUserInterfaceStyle=style==1?UIUserInterfaceStyleLight:style==2?UIUserInterfaceStyleDark:UIUserInterfaceStyleUnspecified; self.view.tintColor=CaraAccent(); self.navigationController.navigationBar.tintColor=CaraAccent(); tabs.selectedSegmentTintColor=CaraAccent(); [collection reloadData]; }
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews]; CGFloat width=collection.bounds.size.width; NSInteger columns=MAX(2,MIN(6,NSInteger(width/158))); CGFloat side=floor((width-(columns-1)*18)/columns);
    UICollectionViewFlowLayout* layout=(id)collection.collectionViewLayout; CGSize size=CGSizeMake(side,side+64); if(!CGSizeEqualToSize(layout.itemSize,size)) layout.itemSize=size;
    hero.hidden=!recent || self.view.bounds.size.height<600; heroArtwork.hidden=self.view.bounds.size.width<600;
    tabs.apportionsSegmentWidthsByContent=YES;
    BOOL narrow=self.view.bounds.size.width<700; self.navigationItem.title=narrow?@"":@"Cara";
    if(narrow && tabs.superview!=libraryBody) { self.navigationItem.titleView=nil; [libraryBody insertArrangedSubview:tabs atIndex:0]; }
    else if(!narrow && tabs.superview==libraryBody) { [libraryBody removeArrangedSubview:tabs]; [tabs removeFromSuperview]; self.navigationItem.titleView=tabs; }
    if(!narrow) tabs.frame=CGRectMake(0,0,MIN(560,self.view.bounds.size.width-200),34);
}
- (void)reloadLibrary { [self applyAppearance]; if(scanning) { reloadPending=YES; return; } scanning=YES; [self.store scan:^(NSArray* result,NSString* error) { self->scanning=NO; self->games=result; [self->collection.refreshControl endRefreshing]; [self filterGames]; if(error) self->syncLabel.text=error; if(self->reloadPending) { self->reloadPending=NO; [self reloadLibrary]; } }]; }
- (void)tabChanged { CaraMenuSound(@"focus"); [self filterGames]; }
- (void)filterGames {
    NSString* query=search.searchBar.text?:@""; NSInteger tab=tabs.selectedSegmentIndex;
    filtered=[games filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(CaraLibraryGame* game,NSDictionary* b) { NSDictionary* metadata=[self.store metadataForGame:game.url]; return (!query.length || [game.title localizedStandardContainsString:query]) && (tab!=1 || [metadata[@"favorite"] boolValue]) && (tab!=2 || metadata[@"lastPlayed"]); }]];
    filtered=[filtered sortedArrayUsingComparator:^NSComparisonResult(CaraLibraryGame* a,CaraLibraryGame* b) { if(self->sortRecent || tab==2) { NSComparisonResult date=[[self.store metadataForGame:b.url][@"lastPlayed"]?:NSDate.distantPast compare:[self.store metadataForGame:a.url][@"lastPlayed"]?:NSDate.distantPast]; if(date!=NSOrderedSame) return date; } return [a.title localizedStandardCompare:b.title]; }];
    countLabel.text=[NSString stringWithFormat:@"%@ · %lu",tab==1?@"Favorites":tab==2?@"Recently played":@"Your collection",(unsigned long)filtered.count];
    recent=nil; NSDate* latest=NSDate.distantPast; for(CaraLibraryGame* game in games) { NSDate* date=[self.store metadataForGame:game.url][@"lastPlayed"]; if(date && [date compare:latest]==NSOrderedDescending) { latest=date; recent=game; } }
    if(recent) { heroTitle.text=recent.title; heroSubtitle.text=[@"Last played " stringByAppendingString:[NSDateFormatter localizedStringFromDate:latest dateStyle:NSDateFormatterMediumStyle timeStyle:NSDateFormatterShortStyle]]; BOOL resume=self.canResume && [recent.url.URLByStandardizingPath isEqual:self.activeGameURL.URLByStandardizingPath]; [heroPlay setTitle:resume?@"▶  Resume game":@"▶  Game details" forState:UIControlStateNormal]; [self artwork:recent completion:^(NSDictionary* m) { self->heroArtwork.image=m[@"image"]?:[UIImage systemImageNamed:@"gamecontroller"]; self->heroTitle.text=m[@"title"]?:self->recent.title; }]; }
    syncLabel.text=self.syncDetail?:@"Add games from Files. Your saves stay on this device."; [collection reloadData]; [self.view setNeedsLayout];
    if(!filtered.count) { UILabel* empty=[UILabel new]; empty.text=games.count?@"No games match this view.":@"Your collection starts here.\nAdd a game or choose your ROMs folder."; empty.numberOfLines=0; empty.textAlignment=NSTextAlignmentCenter; empty.textColor=UIColor.secondaryLabelColor; collection.backgroundView=empty; } else collection.backgroundView=nil;
}
- (void)updateSearchResultsForSearchController:(UISearchController*)controller { [self filterGames]; }
- (void)artwork:(CaraLibraryGame*)game completion:(void(^)(NSDictionary*))apply {
    NSString* key=[NSString stringWithFormat:@"%@-%@-%llu",game.url,game.modified,game.fileSize]; NSDictionary* cached=[artworkCache objectForKey:key]; if(cached) { apply(cached); return; }
    unsigned generation=artworkGeneration.load(); dispatch_async(artworkQueue,^{ NSDictionary* metadata=ReadGameArtwork(game)?:@{}; [self->artworkCache setObject:metadata forKey:key]; dispatch_async(dispatch_get_main_queue(),^{ if(generation==self->artworkGeneration.load()) apply(metadata); }); });
}
- (NSInteger)collectionView:(UICollectionView*)view numberOfItemsInSection:(NSInteger)section { return filtered.count; }
- (UICollectionViewCell*)collectionView:(UICollectionView*)view cellForItemAtIndexPath:(NSIndexPath*)path {
    CaraGameCell* cell=[view dequeueReusableCellWithReuseIdentifier:@"game" forIndexPath:path]; CaraLibraryGame* game=filtered[path.item]; [cell configure:game]; NSString* key=cell.gameKey; cell.titleLabel.hidden=[NSUserDefaults.standardUserDefaults boolForKey:@"libraryHideTitles"]; cell.subtitle.text=[[self.store metadataForGame:game.url][@"favorite"] boolValue]?@"★ Favorite":@"Nintendo 3DS";
    [self artwork:game completion:^(NSDictionary* m) { if([cell.gameKey isEqual:key] && m.count) { cell.artwork.image=m[@"image"]; cell.monogram.hidden=YES; cell.titleLabel.text=m[@"title"]; } }]; return cell;
}
- (void)collectionView:(UICollectionView*)view didSelectItemAtIndexPath:(NSIndexPath*)path { [self showDetails:filtered[path.item]]; }
- (void)openRecent { if(self.canResume && [recent.url.URLByStandardizingPath isEqual:self.activeGameURL.URLByStandardizingPath]) { CaraMenuSound(@"launch"); if(self.resumeGame) self.resumeGame(); } else if(recent) [self showDetails:recent]; }
- (void)showDetails:(CaraLibraryGame*)game {
    CaraMenuSound(@"select");
    CaraSettingsController* details=[CaraSettingsController new]; details.title=@"Game details"; details.usesLibraryAppearance=YES; __weak CaraLibraryController* owner=self;
    NSString* artworkKey=[NSString stringWithFormat:@"%@-%@-%llu",game.url,game.modified,game.fileSize]; NSString* displayTitle=[artworkCache objectForKey:artworkKey][@"title"]?:game.title;
    NSMutableArray* rows=[NSMutableArray new]; NSDictionary* info=[self.store metadataForGame:game.url];
    [rows addObject:@{@"title":displayTitle,@"subtitle":[NSString stringWithFormat:@"Nintendo 3DS · %@\n%@%@",[NSByteCountFormatter stringFromByteCount:game.fileSize countStyle:NSByteCountFormatterCountStyleFile],game.url.lastPathComponent,info[@"titleID"]?[@"\nTitle ID: " stringByAppendingString:info[@"titleID"]]:@""],@"symbol":@"gamecontroller"}];
    [rows addObject:@{@"title":@"Favorite",@"symbol":@"star",@"value":info[@"favorite"]?:@NO,@"change":^(BOOL value) { CaraMenuSound(@"favorite"); [owner.store setFavorite:value game:game.url]; [owner filterGames]; }}];
    BOOL active=self.canResume && [game.url.URLByStandardizingPath isEqual:self.activeGameURL.URLByStandardizingPath];
    if(active) [rows addObject:@{@"title":@"Resume game",@"sound":@"launch",@"symbol":@"play.fill",@"action":^{ if(owner.resumeGame) owner.resumeGame(); }}];
    NSArray* states=self.statesForGame?self.statesForGame(game.url):@[]; NSDictionary* latest=nil; for(NSDictionary* record in states) if([record[@"compatible"] boolValue]) { latest=record; break; }
    if(!active && latest) { NSString* filename=latest[@"filename"]; [rows addObject:@{@"title":@"Resume latest state",@"sound":@"launch",@"symbol":@"play.fill",@"action":^{ if(owner.startFromState) owner.startFromState(game.url,filename); }}]; }
    [rows addObject:@{@"title":active||latest?@"Start from beginning":@"Start game",@"subtitle":@"Keeps in-game saves and save states.",@"sound":@"launch",@"symbol":@"play",@"action":^{ if(owner.openGame) owner.openGame(game.url); }}];
    NSMutableArray* sections=[NSMutableArray arrayWithObject:@{@"title":@"GAME",@"rows":rows}]; NSMutableArray* stateRows=[NSMutableArray new];
    for(NSDictionary* record in states) { NSMutableDictionary* row=[@{@"title":record[@"title"]?:@"Save state",@"subtitle":record[@"subtitle"]?:@"",@"symbol":@"clock.arrow.circlepath",@"enabled":record[@"compatible"]?:@NO} mutableCopy]; if([record[@"compatible"] boolValue] && self.startFromState) { row[@"sound"]=@"launch"; row[@"action"]=^{ if(owner.startFromState) owner.startFromState(game.url,record[@"filename"]); }; } [stateRows addObject:row]; }
    if(!stateRows.count) [stateRows addObject:@{@"title":@"No save states on this device",@"subtitle":@"Create a state from the game menu. In-game saves are separate.",@"symbol":@"tray"}];
    [sections addObject:@{@"title":@"Save states",@"rows":stateRows,@"footer":@"States require the same engine, game revision, CPU backend, and system mode. Cara checks compatibility again when loading."}];
    if(self.gameSettings) [sections addObject:@{@"title":@"Preferences",@"rows":@[@{@"title":@"Game settings",@"symbol":@"slider.horizontal.3",@"action":^{ if(owner.gameSettings) owner.gameSettings(game.url); }}]}];
    [sections addObject:@{@"title":@"Manage game",@"rows":@[@{@"title":@"Clear game settings",@"symbol":@"arrow.counterclockwise",@"action":^{ [owner confirmGame:game operation:@"settings"]; }},@{@"title":@"Remove from library…",@"symbol":@"minus.circle",@"destructive":@YES,@"action":^{ [owner confirmGame:game operation:@"remove"]; }},@{@"title":@"Delete game file…",@"symbol":@"trash",@"destructive":@YES,@"action":^{ [owner confirmGame:game operation:@"delete"]; }}]}];
    details.sections=sections;
    [details loadViewIfNeeded];
    UIView* banner=[[UIView alloc] initWithFrame:CGRectMake(0,0,600,190)]; banner.backgroundColor=[CaraAccent() colorWithAlphaComponent:.08];
    UIImageView* image=[[UIImageView alloc] initWithFrame:CGRectMake(0,14,600,164)]; image.autoresizingMask=UIViewAutoresizingFlexibleWidth; image.contentMode=UIViewContentModeScaleAspectFit; image.image=[UIImage systemImageNamed:@"gamecontroller"]; image.tintColor=CaraAccent(); [banner addSubview:image]; details.tableView.tableHeaderView=banner;
    [self artwork:game completion:^(NSDictionary* metadata) { if(metadata[@"image"]) image.image=metadata[@"image"]; }];
    UINavigationController* nav=[[UINavigationController alloc] initWithRootViewController:details]; nav.modalPresentationStyle=UIModalPresentationPageSheet; nav.preferredContentSize=CGSizeMake(620,850); nav.sheetPresentationController.detents=@[UISheetPresentationControllerDetent.largeDetent]; nav.sheetPresentationController.prefersGrabberVisible=YES; nav.sheetPresentationController.preferredCornerRadius=28;
    [self presentViewController:nav animated:YES completion:nil];
}
- (UIContextMenuConfiguration*)collectionView:(UICollectionView*)view contextMenuConfigurationForItemAtIndexPath:(NSIndexPath*)path point:(CGPoint)point {
    CaraMenuSound(@"focus"); CaraLibraryGame* game=filtered[path.item]; __weak CaraLibraryController* owner=self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil actionProvider:^UIMenu*(NSArray* suggested) {
        BOOL favorite=[[owner.store metadataForGame:game.url][@"favorite"] boolValue];
        UIAction* star=[UIAction actionWithTitle:favorite?@"Remove favorite":@"Favorite" image:[UIImage systemImageNamed:@"star"] identifier:nil handler:^(UIAction* a) { CaraMenuSound(@"favorite"); [owner.store setFavorite:!favorite game:game.url]; [owner filterGames]; }];
        NSMutableArray* actions=[NSMutableArray arrayWithObject:star]; for(NSString* operation in @[@"settings",@"remove",@"delete"]) { UIAction* action=[UIAction actionWithTitle:[operation isEqual:@"settings"]?@"Clear game settings…":[operation isEqual:@"remove"]?@"Remove from library…":@"Delete game file…" image:[UIImage systemImageNamed:[operation isEqual:@"settings"]?@"arrow.counterclockwise":@"trash"] identifier:nil handler:^(UIAction* a) { [owner confirmGame:game operation:operation]; }]; if(![operation isEqual:@"settings"]) action.attributes=UIMenuElementAttributesDestructive; [actions addObject:action]; } return [UIMenu menuWithTitle:game.title children:actions]; }];
}
- (void)confirmGame:(CaraLibraryGame*)game operation:(NSString*)operation {
    BOOL deleteFile=[operation isEqual:@"delete"], settings=[operation isEqual:@"settings"];
    if(deleteFile && (self.gameFileInUse?self.gameFileInUse(game.url):(self.canResume && [game.url.URLByStandardizingPath isEqual:self.activeGameURL.URLByStandardizingPath]))) { UIAlertController* active=[UIAlertController alertControllerWithTitle:@"Stop this game first" message:@"Stop the running game from the game menu before deleting its file." preferredStyle:UIAlertControllerStyleAlert]; [active addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]]; [self presentViewController:active animated:YES completion:nil]; return; }
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:settings?@"Clear game settings?":deleteFile?@"Delete game file?":@"Remove from library?" message:settings?@"This game will use your global settings. Saves and states are kept.":deleteFile?[NSString stringWithFormat:@"Permanently delete %@ from its original location? In-game saves and save states are kept.",game.url.lastPathComponent]:@"The game file, in-game saves, and save states are kept. Add the game from Files to show it again." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* a) { CaraMenuSound(@"cancel"); }]];
    [alert addAction:[UIAlertAction actionWithTitle:settings?@"Clear settings":deleteFile?@"Delete game file":@"Remove from library" style:UIAlertActionStyleDestructive handler:^(UIAlertAction* a) { CaraMenuSound(@"confirm"); if(settings) { if(self.clearGameSettings) self.clearGameSettings(game.url); } else if(deleteFile) { if(self.gameFileInUse && self.gameFileInUse(game.url)) return; NSError* error=nil; if(![self.store deleteGameFile:game.url error:&error]) { UIAlertController* failure=[UIAlertController alertControllerWithTitle:@"Couldn’t delete game" message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert]; [failure addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]]; [self presentViewController:failure animated:YES completion:nil]; } } else [self.store removeGame:game.url]; [self reloadLibrary]; }]]; [self presentViewController:alert animated:YES completion:nil];
}
- (void)settingsTapped { CaraMenuSound(@"select"); if(self.showSettings) self.showSettings(); }
- (void)addTapped {
    CaraMenuSound(@"select");
    UIAlertController* sheet=[UIAlertController alertControllerWithTitle:@"Add games" message:@"Games stay in their original location." preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Open game from Files…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { if(self.addGame) self.addGame(); }]]; [sheet addAction:[UIAlertAction actionWithTitle:@"Choose ROMs folder…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { if(self.chooseFolder) self.chooseFolder(); }]]; [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* a) { CaraMenuSound(@"cancel"); }]]; sheet.popoverPresentationController.sourceView=self.view; sheet.popoverPresentationController.sourceRect=CGRectMake(30,self.view.bounds.size.height-40,1,1); [self presentViewController:sheet animated:YES completion:nil];
}
- (void)setSyncDetail:(NSString*)value { _syncDetail=[value copy]; syncLabel.text=value; }
- (void)suspendArtwork { ++artworkGeneration; }
@end

@interface CaraSliderCell : UITableViewCell
- (void)configure:(NSDictionary*)row enabled:(BOOL)enabled tint:(UIColor*)tint;
@end
@implementation CaraSliderCell
- (void)configure:(NSDictionary*)row enabled:(BOOL)enabled tint:(UIColor*)tint {
    NSDictionary* spec=row[@"slider"];
    UILabel* title=[UILabel new]; title.text=row[@"title"]; title.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; title.adjustsFontForContentSizeCategory=YES; title.numberOfLines=0;
    UILabel* value=[UILabel new]; value.font=[UIFont monospacedDigitSystemFontOfSize:16 weight:UIFontWeightSemibold]; value.textAlignment=NSTextAlignmentRight; value.accessibilityElementsHidden=YES;
    title.textColor=value.textColor=enabled?UIColor.labelColor:UIColor.tertiaryLabelColor;
    UISlider* slider=[UISlider new]; slider.minimumValue=[spec[@"min"] floatValue]; slider.maximumValue=[spec[@"max"] floatValue]; slider.value=[spec[@"value"] floatValue]; slider.enabled=enabled; slider.minimumTrackTintColor=tint; slider.accessibilityLabel=row[@"title"];
    const float step=MAX(1,[spec[@"step"] floatValue]);
    value.text=[NSString stringWithFormat:@"%.0f%%",slider.value]; slider.accessibilityValue=value.text;
    __weak UISlider* weakSlider=slider; __weak UILabel* weakValue=value;
    [slider addAction:[UIAction actionWithHandler:^(UIAction* action) {
        UISlider* control=weakSlider; if (!control) return;
        const float rounded=MAX(control.minimumValue,MIN(control.maximumValue,std::round(control.value/step)*step)); control.value=rounded;
        weakValue.text=[NSString stringWithFormat:@"%.0f%%",rounded]; control.accessibilityValue=weakValue.text;
        void(^change)(double)=row[@"sliderChange"]; if (change) change(rounded);
    }] forControlEvents:UIControlEventValueChanged];
    UIStackView* heading=[[UIStackView alloc] initWithArrangedSubviews:@[title,value]]; heading.spacing=12;
    [value setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal]; [value setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UILabel* detail=[UILabel new]; detail.text=row[@"subtitle"]; detail.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; detail.adjustsFontForContentSizeCategory=YES; detail.textColor=UIColor.secondaryLabelColor; detail.numberOfLines=0;
    UIStackView* body=[[UIStackView alloc] initWithArrangedSubviews:@[heading,slider,detail]]; body.axis=UILayoutConstraintAxisVertical; body.spacing=6; body.translatesAutoresizingMaskIntoConstraints=NO;
    [self.contentView addSubview:body]; [NSLayoutConstraint activateConstraints:@[[body.leadingAnchor constraintEqualToAnchor:self.contentView.layoutMarginsGuide.leadingAnchor],[body.trailingAnchor constraintEqualToAnchor:self.contentView.layoutMarginsGuide.trailingAnchor],[body.topAnchor constraintEqualToAnchor:self.contentView.topAnchor constant:14],[body.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor constant:-14]]];
    self.selectionStyle=UITableViewCellSelectionStyleNone;
}
@end

static UIImage* CaraThemePreview(NSInteger style) {
    UIGraphicsImageRenderer* renderer=[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(100,65)];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
        BOOL dark=style==2; [(dark?[UIColor colorWithWhite:.16 alpha:1]:[UIColor colorWithWhite:.96 alpha:1]) setFill]; [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0,0,100,65) cornerRadius:8] fill];
        [[CaraAccent() colorWithAlphaComponent:.45] setFill]; [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(8,17,84,22) cornerRadius:4] fill];
        [CaraAccent() setFill]; [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(8,7,25,5) cornerRadius:2] fill];
        NSArray* colors=@[CaraAccent(),UIColor.systemOrangeColor,UIColor.systemPurpleColor,UIColor.systemGreenColor]; for(NSUInteger i=0;i<4;++i) { [[colors[i] colorWithAlphaComponent:.45] setFill]; [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(8+i*22,45,18,14) cornerRadius:3] fill]; }
        if(style==0) { [[UIColor colorWithWhite:.12 alpha:.65] setFill]; UIBezierPath* half=[UIBezierPath bezierPath]; [half moveToPoint:CGPointMake(100,0)]; [half addLineToPoint:CGPointMake(100,65)]; [half addLineToPoint:CGPointMake(25,65)]; [half closePath]; [half fill]; }
    }];
}
@implementation CaraSettingsController {
    BOOL completed;
    UIStackView* sidebar;
    NSString* selectedCategory;
    NSArray* visibleSections;
}
- (instancetype)init { return [super init]; }
- (void)viewDidLoad {
    [super viewDidLoad]; if (!self.title.length) self.title=@"Settings"; self.view.backgroundColor=CaraBackground(); self.view.tintColor=(self.categorized || self.usesLibraryAppearance)?CaraAccent():CaraOrange();
    self.tableView=[[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStyleInsetGrouped]; self.tableView.dataSource=self; self.tableView.delegate=self; self.tableView.backgroundColor=UIColor.clearColor; [self.view addSubview:self.tableView];
    __weak CaraSettingsController* weakSettings=self;
    if(self.categorized) {
        selectedCategory=@"General"; sidebar=[UIStackView new]; sidebar.axis=UILayoutConstraintAxisVertical; sidebar.spacing=10;
        for(NSString* category in @[@"General",@"Appearance",@"Emulation",@"Audio",@"Controls",@"Library & storage",@"Save data",@"About Cara"]) { UIButton* button=[UIButton buttonWithType:UIButtonTypeSystem]; UIButtonConfiguration* config=[UIButtonConfiguration tintedButtonConfiguration]; config.image=[UIImage systemImageNamed:@{@"General":@"gearshape",@"Appearance":@"paintpalette",@"Emulation":@"cpu",@"Audio":@"speaker.wave.2",@"Controls":@"gamecontroller",@"Library & storage":@"externaldrive",@"Save data":@"sdcard",@"About Cara":@"info.circle"}[category]]; config.imagePadding=12; button.configuration=config; [button setTitle:category forState:UIControlStateNormal]; button.contentHorizontalAlignment=UIControlContentHorizontalAlignmentLeading; [button.heightAnchor constraintEqualToConstant:52].active=YES; [button addAction:[UIAction actionWithHandler:^(UIAction* action) { CaraSettingsController* controller=weakSettings; if(controller) { CaraMenuSound(@"select"); controller->selectedCategory=category; [controller refresh]; } }] forControlEvents:UIControlEventTouchUpInside]; [sidebar addArrangedSubview:button]; } [self.view addSubview:sidebar];
        self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Categories" style:UIBarButtonItemStylePlain target:self action:@selector(chooseCategory)];
    }
    [self updateVisibleSections];
    self.tableView.rowHeight=UITableViewAutomaticDimension; self.tableView.estimatedRowHeight=62;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews]; BOOL wide=self.categorized && self.view.bounds.size.width>=800; CGFloat left=wide?260:0; sidebar.hidden=!wide; sidebar.frame=CGRectMake(20,self.view.safeAreaInsets.top+24,220,500); self.tableView.frame=CGRectMake(left,0,self.view.bounds.size.width-left,self.view.bounds.size.height);
}
- (void)chooseCategory {
    UIAlertController* menu=[UIAlertController alertControllerWithTitle:@"Settings" message:nil preferredStyle:UIAlertControllerStyleActionSheet]; for(NSString* name in @[@"General",@"Appearance",@"Emulation",@"Audio",@"Controls",@"Library & storage",@"Save data",@"About Cara"]) [menu addAction:[UIAlertAction actionWithTitle:name style:UIAlertActionStyleDefault handler:^(UIAlertAction* a) { self->selectedCategory=name; [self refresh]; }]]; [menu addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* a) { CaraMenuSound(@"cancel"); }]]; menu.popoverPresentationController.barButtonItem=self.navigationItem.leftBarButtonItem; [self presentViewController:menu animated:YES completion:nil];
}
- (void)updateVisibleSections {
    if(self.sectionsProvider) self.sections=self.sectionsProvider();
    if(!self.categorized) { visibleSections=self.sections; return; }
    self.title=selectedCategory;
    self.tableView.tableHeaderView=nil;
    for(UIButton* button in sidebar.arrangedSubviews) { UIButtonConfiguration* config=button.configuration; BOOL selected=[button.currentTitle isEqual:selectedCategory]; config.baseForegroundColor=selected?CaraAccent():UIColor.labelColor; config.background.backgroundColor=selected?[CaraAccent() colorWithAlphaComponent:.12]:UIColor.clearColor; button.configuration=config; button.accessibilityTraits=UIAccessibilityTraitButton|(selected?UIAccessibilityTraitSelected:0); }
    if([selectedCategory isEqual:@"Appearance"]) {
        __weak CaraSettingsController* owner=self;
        NSInteger theme=[NSUserDefaults.standardUserDefaults integerForKey:@"libraryTheme"];
        UIView* preview=[[UIView alloc] initWithFrame:CGRectMake(0,0,600,180)];
        UIStackView* cards=[UIStackView new]; cards.axis=UILayoutConstraintAxisHorizontal; cards.distribution=UIStackViewDistributionFillEqually; cards.spacing=12; cards.frame=CGRectMake(20,10,560,150); cards.autoresizingMask=UIViewAutoresizingFlexibleWidth; [preview addSubview:cards];
        for(NSInteger i=0;i<3;++i) { NSInteger chosen=i; UIButton* card=[UIButton buttonWithType:UIButtonTypeSystem]; UIButtonConfiguration* config=[UIButtonConfiguration borderedButtonConfiguration]; config.title=@[@"System",@"Light",@"Dark"][i]; config.image=CaraThemePreview(i); config.imagePlacement=NSDirectionalRectEdgeTop; config.imagePadding=12; config.background.strokeColor=theme==i?CaraAccent():UIColor.separatorColor; config.background.strokeWidth=theme==i?2:1; config.baseForegroundColor=UIColor.labelColor; card.configuration=config; card.accessibilityTraits=UIAccessibilityTraitButton|(theme==i?UIAccessibilityTraitSelected:0); [card addAction:[UIAction actionWithHandler:^(UIAction* a) { [NSUserDefaults.standardUserDefaults setInteger:chosen forKey:@"libraryTheme"]; CaraMenuSound(@"select"); owner.navigationController.overrideUserInterfaceStyle=chosen==1?UIUserInterfaceStyleLight:chosen==2?UIUserInterfaceStyleDark:UIUserInterfaceStyleUnspecified; [owner refresh]; }] forControlEvents:UIControlEventTouchUpInside]; [cards addArrangedSubview:card]; }
        self.tableView.tableHeaderView=preview;
        NSMutableArray* accents=[NSMutableArray new]; for(NSInteger i=0;i<5;++i) { NSInteger value=i; [accents addObject:@{@"title":@[@"Teal",@"Blue",@"Rose",@"Purple",@"Graphite"][i],@"tint":@[[UIColor colorWithRed:0 green:.67 blue:.70 alpha:1],UIColor.systemBlueColor,UIColor.systemPinkColor,UIColor.systemPurpleColor,UIColor.systemGrayColor][i],@"symbol":[NSUserDefaults.standardUserDefaults integerForKey:@"libraryAccent"]==i?@"checkmark.circle.fill":@"circle.fill",@"keepOpen":@YES,@"action":^{ CaraMenuSound(@"select"); [NSUserDefaults.standardUserDefaults setInteger:value forKey:@"libraryAccent"]; owner.view.tintColor=CaraAccent(); owner.navigationController.view.tintColor=CaraAccent(); [owner refresh]; }}]; }
        visibleSections=@[@{@"title":@"Accent color",@"rows":accents},@{@"title":@"Collection",@"rows":@[@{@"title":@"Menu sounds",@"symbol":@"speaker.wave.2",@"value":@(![NSUserDefaults.standardUserDefaults objectForKey:@"libraryMenuSounds"] || [NSUserDefaults.standardUserDefaults boolForKey:@"libraryMenuSounds"]),@"change":^(BOOL on) { [NSUserDefaults.standardUserDefaults setBool:on forKey:@"libraryMenuSounds"]; }},@{@"title":@"Show game titles",@"value":@(![NSUserDefaults.standardUserDefaults boolForKey:@"libraryHideTitles"]),@"change":^(BOOL on) { [NSUserDefaults.standardUserDefaults setBool:!on forKey:@"libraryHideTitles"]; }}],@"footer":@"Artwork comes from the game’s embedded icon. The OLED game skin stays independent of the library theme."}]; return;
    }
    if([selectedCategory isEqual:@"About Cara"]) { visibleSections=@[@{@"title":@"Cara",@"rows":@[@{@"title":@"Nintendo 3DS emulation",@"subtitle":@"Cara is built on the open-source Citra emulator. See the bundled source and license notices.",@"symbol":@"info.circle"},@{@"title":@"Version",@"subtitle":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]?:@"Development build",@"symbol":@"app"}]}]; return; }
    visibleSections=[self.sections filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSDictionary* section,NSDictionary* b) { NSString* title=section[@"title"]; if([self->selectedCategory isEqual:@"General"]) return [title isEqual:@"Current game"] || [title isEqual:@"Play mode"]; if([self->selectedCategory isEqual:@"Controls"]) return [title hasSuffix:@"skin"]; if([self->selectedCategory isEqual:@"Library & storage"]) return [title isEqual:@"Game library"]; if([self->selectedCategory isEqual:@"Save data"]) return [title isEqual:@"iCloud & Files"]; return [title isEqual:self->selectedCategory]; }]];
    if(!visibleSections.count) visibleSections=@[@{@"title":selectedCategory,@"rows":@[@{@"title":@"Ready to play",@"subtitle":@"Use Emulation to choose a play mode, or Library & storage to add your games.",@"symbol":@"checkmark.circle"}]}];
}
- (void)refresh {
    [self updateVisibleSections];
    if (!self.isViewLoaded) return;
    NSMutableArray* views=[NSMutableArray arrayWithArray:self.tableView.visibleCells];
    while (views.count) { UIView* view=views.lastObject; [views removeLastObject]; if ([view isKindOfClass:UISlider.class] && ((UISlider*)view).tracking) return; [views addObjectsFromArray:view.subviews]; }
    [self.tableView reloadData];
}
- (void)done { CaraMenuSound(@"back"); completed=YES; [self dismissViewControllerAnimated:YES completion:^{ if (self.finished) self.finished(); }]; }
- (void)presentationControllerDidDismiss:(UIPresentationController*)controller { if (!completed && self.finished) self.finished(); completed=YES; }
- (NSInteger)numberOfSectionsInTableView:(UITableView*)table { return visibleSections.count; }
- (NSInteger)tableView:(UITableView*)table numberOfRowsInSection:(NSInteger)section { return [visibleSections[section][@"rows"] count]; }
- (NSString*)tableView:(UITableView*)table titleForHeaderInSection:(NSInteger)section { return visibleSections[section][@"title"]; }
- (NSString*)tableView:(UITableView*)table titleForFooterInSection:(NSInteger)section { return visibleSections[section][@"footer"]; }
- (UITableViewCell*)tableView:(UITableView*)table cellForRowAtIndexPath:(NSIndexPath*)path {
    NSDictionary* row=visibleSections[path.section][@"rows"][path.row];
    if (row[@"slider"]) { CaraSliderCell* cell=[[CaraSliderCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil]; [cell configure:row enabled:!row[@"enabled"] || [row[@"enabled"] boolValue] tint:(self.categorized || self.usesLibraryAppearance)?CaraAccent():CaraOrange()]; return cell; }
    UITableViewCell* cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text=row[@"title"]; cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; cell.textLabel.adjustsFontForContentSizeCategory=YES; cell.textLabel.numberOfLines=0;
    cell.detailTextLabel.text=row[@"subtitle"]; cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; cell.detailTextLabel.adjustsFontForContentSizeCategory=YES; cell.detailTextLabel.numberOfLines=0;
    cell.imageView.image=[UIImage systemImageNamed:row[@"symbol"]?:@"circle"]; cell.imageView.tintColor=row[@"tint"]?:([row[@"destructive"] boolValue]?UIColor.systemRedColor:((self.categorized || self.usesLibraryAppearance)?CaraAccent():CaraOrange()));
    BOOL enabled=!row[@"enabled"] || [row[@"enabled"] boolValue];
    cell.textLabel.textColor=!enabled?UIColor.tertiaryLabelColor:([row[@"destructive"] boolValue]?UIColor.systemRedColor:UIColor.labelColor);
    cell.selectionStyle=row[@"action"] && enabled?UITableViewCellSelectionStyleDefault:UITableViewCellSelectionStyleNone;
    cell.userInteractionEnabled=enabled;
    if (row[@"value"]) {
        __weak CaraSettingsController* weakSettings=self;
        UISwitch* toggle=[UISwitch new]; toggle.onTintColor=(self.categorized || self.usesLibraryAppearance)?CaraAccent():CaraOrange(); toggle.on=[row[@"value"] boolValue]; toggle.enabled=enabled; toggle.accessibilityLabel=row[@"title"];
        __weak UISwitch* weakToggle=toggle;
        [toggle addAction:[UIAction actionWithHandler:^(UIAction* action) { void(^change)(BOOL)=row[@"change"]; if (change) change(weakToggle.on); CaraMenuSound(@"toggle"); [weakSettings refresh]; }] forControlEvents:UIControlEventValueChanged]; cell.accessoryView=toggle;
    } else if (row[@"action"] && enabled) cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}
- (void)tableView:(UITableView*)table didSelectRowAtIndexPath:(NSIndexPath*)path {
    [table deselectRowAtIndexPath:path animated:YES]; NSDictionary* row=visibleSections[path.section][@"rows"][path.row];
    void(^action)(void)=row[@"action"]; if (!action || (row[@"enabled"] && ![row[@"enabled"] boolValue])) return;
    CaraMenuSound(row[@"sound"]?:@"select");
    if([row[@"keepOpen"] boolValue]) { action(); return; }
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
- (void)showInactiveGuestClock:(unsigned)clock adaptive:(BOOL)adaptive {
    values[0].text=@"—"; values[1].text=@"—";
    values[2].text=adaptive?@"AUTO":[NSString stringWithFormat:@"%u%%",clock];
    labels[2].text=@"GUEST CPU";
    values[1].textColor=UIColor.secondaryLabelColor;
    self.accessibilityLabel=adaptive?@"No live game metrics, adaptive guest CPU":
        [NSString stringWithFormat:@"No live game metrics, configured guest CPU %u percent",clock];
}
@end

@implementation CaraSessionIndicator {
    CALayer* socket;
    CALayer* light;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self=[super initWithFrame:frame])) {
        self.userInteractionEnabled=NO; self.isAccessibilityElement=YES;
        self.accessibilityIdentifier=@"game-status-light"; self.accessibilityLabel=@"Game status";
        socket=[CALayer layer]; socket.backgroundColor=[UIColor colorWithWhite:.035 alpha:1].CGColor;
        socket.borderWidth=1; socket.borderColor=[UIColor colorWithWhite:.24 alpha:1].CGColor;
        light=[CALayer layer]; light.shadowOffset=CGSizeZero; light.shadowRadius=5;
        [self.layer addSublayer:socket]; [self.layer addSublayer:light];
        [self updateLight:CaraStatusLightDim accessibilityStatus:@"Stopped"];
    } return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    const CGFloat diameter=MAX(4,MIN(18,MIN(self.bounds.size.width,self.bounds.size.height)-2));
    socket.frame=CGRectMake(CGRectGetMidX(self.bounds)-diameter/2,CGRectGetMidY(self.bounds)-diameter/2,diameter,diameter); socket.cornerRadius=diameter/2;
    light.frame=CGRectInset(socket.frame,diameter*2/9,diameter*2/9); light.cornerRadius=light.bounds.size.width/2;
    light.shadowPath=[UIBezierPath bezierPathWithOvalInRect:light.bounds].CGPath;
}
- (void)updateLight:(CaraStatusLight)state accessibilityStatus:(NSString*)status {
    UIColor* color=state==CaraStatusLightGreen?[UIColor colorWithRed:.43 green:.82 blue:.36 alpha:1]:
        state==CaraStatusLightAmber?[UIColor colorWithRed:1 green:.67 blue:.23 alpha:1]:
        state==CaraStatusLightRed?[UIColor colorWithRed:1 green:.28 blue:.23 alpha:1]:
        [UIColor colorWithRed:.25 green:.29 blue:.23 alpha:1];
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    light.backgroundColor=color.CGColor; light.shadowColor=color.CGColor;
    light.shadowOpacity=state==CaraStatusLightDim?0:.55;
    [CATransaction commit]; self.accessibilityValue=status;
}
@end

@implementation CaraQuickSettingsController {
    NSArray<UIButton*>* modeButtons;
    BOOL completed;
}
- (void)setPerformanceDetail:(NSString*)value {
    _performanceDetail=[value copy];
    if (modeButtons.count>1) { UIButtonConfiguration* config=modeButtons[1].configuration; config.subtitle=_performanceDetail?:@"Adaptive CPU clock"; modeButtons[1].configuration=config; }
}
- (instancetype)init { if ((self=[super init])) _modeSelectionEnabled=YES; return self; }
- (void)viewDidLoad {
    [super viewDidLoad]; if (!self.title.length) self.title=@"Settings"; self.view.backgroundColor=CaraBackground(); self.view.tintColor=CaraOrange();
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
    UIScrollView* scroll=[UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints=NO; [self.view addSubview:scroll];
    UIStackView* body=[UIStackView new]; body.axis=UILayoutConstraintAxisVertical; body.spacing=16; body.translatesAutoresizingMaskIntoConstraints=NO;
    [scroll addSubview:body];
    __weak CaraQuickSettingsController* weakSelf=self;
    NSMutableArray* modes=[NSMutableArray new];
    for (NSString* mode in @[@"accuracy",@"performance"]) {
        const BOOL accuracy=[mode isEqual:@"accuracy"];
        UIButton* button=[UIButton buttonWithType:UIButtonTypeSystem];
        UIButtonConfiguration* config=[UIButtonConfiguration borderedButtonConfiguration];
        config.title=accuracy?@"Accuracy Mode":@"Performance Mode";
        config.subtitle=accuracy?@"100% speed · 100% guest CPU":(self.performanceDetail?:@"Adaptive CPU clock");
        config.image=[UIImage systemImageNamed:accuracy?@"scope":@"bolt.fill"];
        config.imagePadding=18; config.titleAlignment=UIButtonConfigurationTitleAlignmentLeading;
        config.contentInsets=NSDirectionalEdgeInsetsMake(25,22,25,22); config.cornerStyle=UIButtonConfigurationCornerStyleLarge;
        config.titleTextAttributesTransformer=^NSDictionary*(NSDictionary* attributes) { NSMutableDictionary* result=[attributes mutableCopy]; result[NSFontAttributeName]=[UIFont preferredFontForTextStyle:UIFontTextStyleTitle2]; return result; };
        config.subtitleTextAttributesTransformer=^NSDictionary*(NSDictionary* attributes) { NSMutableDictionary* result=[attributes mutableCopy]; result[NSFontAttributeName]=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline]; return result; };
        button.configuration=config; button.contentHorizontalAlignment=UIControlContentHorizontalAlignmentLeading; button.enabled=self.modeSelectionEnabled;
        [button addAction:[UIAction actionWithHandler:^(UIAction* action) { CaraQuickSettingsController* owner=weakSelf; if (!owner) return; CaraMenuSound(@"select"); owner.playMode=mode; if (owner.chooseMode) owner.chooseMode(mode); }] forControlEvents:UIControlEventTouchUpInside];
        button.accessibilityIdentifier=[@"mode-" stringByAppendingString:mode];
        [body addArrangedSubview:button]; [modes addObject:button];
    }
    modeButtons=modes; [self updateMode];
    UILabel* fpsLabel=[UILabel new]; fpsLabel.text=@"FPS meter"; fpsLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline]; fpsLabel.adjustsFontForContentSizeCategory=YES;
    UISwitch* fpsToggle=[UISwitch new]; fpsToggle.on=self.showFPS; fpsToggle.onTintColor=CaraOrange(); fpsToggle.accessibilityLabel=@"FPS meter";
    __weak UISwitch* weakToggle=fpsToggle;
    [fpsToggle addAction:[UIAction actionWithHandler:^(UIAction* action) { CaraQuickSettingsController* owner=weakSelf; if (owner.toggleFPS) owner.toggleFPS(weakToggle.on); CaraMenuSound(@"toggle"); }] forControlEvents:UIControlEventValueChanged];
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
            void(^run)(void)=item[@"action"]; if (!run) return; CaraMenuSound(@"select");
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
- (void)done { CaraMenuSound(@"back"); completed=YES; [self dismissViewControllerAnimated:YES completion:^{ if (self.finished) self.finished(); }]; }
- (void)presentationControllerDidDismiss:(UIPresentationController*)controller { if (!completed && self.finished) self.finished(); completed=YES; }
@end
