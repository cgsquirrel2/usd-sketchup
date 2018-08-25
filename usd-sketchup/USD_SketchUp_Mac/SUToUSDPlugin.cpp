//
//  SUToUSDPlugin.mm
//  skp2usd
//
//  Created by Michael B. Johnson on 1/9/18.
//

#import "SUToUSDPlugin.h"
#import "USDSketchUpUtilities.h"

// Our shared base class USDExporterPlugin has just everything we need except
// the UI.  This connector lets us delegate the UI back to our obj-c plugin.
class USDExporterPluginMac : public USDExporterPlugin {

public:
    USDExporterPluginMac(SUToUSDPlugin*& p) : _plugin(p) { }
    
    void ShowOptionsDialog(bool model_has_selection) {
        [_plugin showOptionsDialog:model_has_selection];
    }
    void ShowSummaryDialog(const std::string& summary) {
        [_plugin showSummaryDialog:summary];
    }
    SUToUSDPlugin* _plugin;
};

@implementation SUToUSDPlugin

// Initialize the plugin and return an autoreleased instance of it.
+ (NSObject<SketchUpModelExporterPlugin>*)exporter {
    SUToUSDPlugin* theExporter = nil;
    theExporter = [[SUToUSDPlugin alloc] init];
#if __has_feature(objc_arc)
    return theExporter;
#else
    return [theExporter autorelease];
#endif
}

// This gets the underlying c++ interface.
- (SketchUpModelExporterInterface*)sketchUpModelExporterInterface {
    return _plugin;
}

- (id)init {
    self = [super init];
    
    _plugin = new USDExporterPluginMac(self);
    
    return self;
}

// assuming SketchUp is not auto-released?
- (void)dealloc {
    delete _plugin;
#if __has_feature(objc_arc)
    optionsPanel = nil;
    summaryPanel = nil;
#else
    [optionsPanel release];
    [summaryPanel release];
    [super dealloc];
#endif
}

- (bool)loadNib {
    if (!optionsPanel || !summaryPanel) {
        // need to replace this with:
        // need to make sure that the window in the nib is not set to
        // "Release when Closed" in IB.
        NSBundle* bundle = [NSBundle bundleForClass:[self class]];
        if (![bundle loadNibNamed:@"USDSketchUpMacUI" owner:self topLevelObjects:nil]) {
            NSLog(@"problem loading options bundle");
        }
#if __has_feature(objc_arc)
        // the fact that they're instance variables means they'll be retained
#else
        // we want these to hang around, and this newer API does not automatically
        // retain these guys, we need to do it
        [optionsPanel retain];
        [summaryPanel retain];
#endif
        [optionsPanel setReleasedWhenClosed:NO];
        [summaryPanel setReleasedWhenClosed:NO];
    }
    return optionsPanel && summaryPanel;
}


#define GET_STATE(c) ((c != nil) ? ([c state] == NSOnState) : false)
#define SET_STATE(c, b) if(c) [c setState:((b)?NSOnState:NSOffState)]

- (void)_initControls {
    // Initialize the controls in the options view from the options
    if (aspectRatioEntry) {
        double ar = _plugin->GetAspectRatio();
        [aspectRatioEntry setDoubleValue:ar];
    }
    SET_STATE(exportEdgesCheck, _plugin->GetExportEdges());
    SET_STATE(exportLinesCheck, _plugin->GetExportLines());
    SET_STATE(exportCurvesCheck, _plugin->GetExportCurves());
    SET_STATE(exportNormalsCheck, _plugin->GetExportNormals());
    SET_STATE(exportSingleFileCheck, _plugin->GetExportToSingleFile());
    SET_STATE(exportYUpCheck, _plugin->GetExportYUp());
}

// Set the options from those selected in the dialog
- (void)_getOptions {
    _plugin->SetAspectRatio([aspectRatioEntry doubleValue]);
    _plugin->SetExportEdges(GET_STATE(exportEdgesCheck));
    _plugin->SetExportLines(GET_STATE(exportLinesCheck));
    _plugin->SetExportCurves(GET_STATE(exportCurvesCheck));
    _plugin->SetExportNormals(GET_STATE(exportNormalsCheck));
    _plugin->SetExportToSingleFile(GET_STATE(exportSingleFileCheck));
    _plugin->SetExportYUp(GET_STATE(exportYUpCheck));
}


// Close the a panel started with [NSApp runModalForWindow:]
- (IBAction)closePanel:(id)sender {
    // The OK button on the options panel has a tag of 1.  While the Cancel
    // button and OK button on the report panel have a tag of zero.  All we care
    // about is that we can differentate between OK and Cancel on the options
    // dialog so we can save modified preferences.
    if ([sender tag] == 1) {
        [NSApp stopModal];
    } else { // cancel button
        [NSApp abortModal];
    }
}

- (void)showOptionsDialog:(bool)model_has_selection {
    if (![self loadNib]) {
        return;
    }
    [self _initControls];
    if (NSModalResponseStop == [NSApp runModalForWindow:optionsPanel]) {
        [self _getOptions];
    }
    [optionsPanel close];
    [optionsPanel orderOut:self];
}

- (void)showSummaryDialog:(const std::string&)summary {
    // Make sure we have our panel created before we set data on it.
    if (![self loadNib]) {
        return;
    }
    NSString* nsSummary = [NSString stringWithUTF8String:summary.c_str()];
    [summaryText setString:nsSummary];
    [NSApp runModalForWindow:summaryPanel];
    [summaryPanel close];
    [summaryPanel orderOut:self];
}

- (IBAction)takeAspectRatio:(id)sender {
    double ratio = [sender doubleValue];
    _plugin->SetAspectRatio(ratio);
}

- (IBAction)takeExportNormals:(id)sender {
    BOOL flag = [sender intValue];
    _plugin->SetExportNormals(flag);
}

- (IBAction)takeExportEdges:(id)sender {
    BOOL flag = [sender intValue];
    _plugin->SetExportEdges(flag);
}

- (IBAction)takeExportLines:(id)sender {
    BOOL flag = [sender intValue];
    _plugin->SetExportLines(flag);
}

- (IBAction)takeExportCurves:(id)sender {
    BOOL flag = [sender intValue];
    _plugin->SetExportCurves(flag);
}

- (IBAction)takeExportToSingleFile:(id)sender {
    BOOL flag = [sender intValue];
    _plugin->SetExportToSingleFile(flag);
}

- (IBAction)takeExportYUp:(id)sender {
    BOOL flag = [sender intValue];
    _plugin->SetExportYUp(flag);
}

@end


