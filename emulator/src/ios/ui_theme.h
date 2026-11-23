// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 Cara contributors.
#pragma once
#import <UIKit/UIKit.h>

// Shared library/settings presentation; never changes the emulated LCD colors.
static inline UIColor* CaraUIAccent() {
    NSInteger index=[NSUserDefaults.standardUserDefaults integerForKey:@"libraryAccent"];
    return @[[UIColor colorWithRed:0 green:.67 blue:.70 alpha:1],UIColor.systemBlueColor,UIColor.systemPinkColor,UIColor.systemPurpleColor,UIColor.systemGrayColor][MAX(0,MIN(4,index))];
}
static inline UIColor* CaraUIBackground() {
    return [UIColor colorWithDynamicProvider:^UIColor*(UITraitCollection* traits) {
        return traits.userInterfaceStyle==UIUserInterfaceStyleDark?
            [UIColor colorWithRed:.055 green:.065 blue:.08 alpha:1]:
            [UIColor colorWithRed:.96 green:.97 blue:.98 alpha:1];
    }];
}
static inline UIFont* CaraUIDisplayFont(CGFloat size, UIFontTextStyle style) {
    UIFontDescriptor* descriptor=[[UIFont systemFontOfSize:size weight:UIFontWeightBold].fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded];
    return [[UIFontMetrics metricsForTextStyle:style] scaledFontForFont:[UIFont fontWithDescriptor:descriptor size:size]];
}
static inline void CaraUIApplyChrome(UIViewController* controller) {
    controller.view.backgroundColor=CaraUIBackground();
    controller.view.tintColor=CaraUIAccent();
    UINavigationBar* bar=controller.navigationController.navigationBar;
    bar.tintColor=CaraUIAccent();
    UINavigationBarAppearance* appearance=[UINavigationBarAppearance new];
    [appearance configureWithDefaultBackground];
    appearance.backgroundColor=CaraUIBackground();
    appearance.shadowColor=UIColor.clearColor;
    bar.standardAppearance=appearance;
    bar.scrollEdgeAppearance=appearance;
    bar.compactAppearance=appearance;
}
