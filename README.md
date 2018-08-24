Universal Scene Description Exporter for SketchUp
=================================================

This plug-in adds the ability to export [Universal Scene
Description](http://openusd.org/docs/index.html) files from [SketchUp
Pro](https://www.sketchup.com). It has been tested with SketchUp Pro 2016, 2017 and 2018.

It adds three options to the **File**->**Export**->**3D Model** menu:

- Pixar USD binary File (*.usd)
- Pixar USD ASCII File (*.usda)
- Pixar USDZ  File (*.usdz)

There are also options on the export panel to conditionally export
**normals**, **curves**, **edges**, and **lines**, as well as the ability to organize
the USD as a **single file** or as a **set of files** that reference each
other.

The [**usdz**](https://graphics.pixar.com/usd/docs/Usdz-File-Format-Specification.html) files this exporter writes out take care to write out in a way that is compatible with Apple's [ARKit 2](https://developer.apple.com/arkit/), which is more constrained than the general specification, but that support can be toggled on or off programmatically.

The exporter also leverages the new [UsdPreviewSurface](https://graphics.pixar.com/usd/docs/UsdPreviewSurface-Proposal.html) to support texture export from SketchUp.


Getting Help
------------

Need help understanding certain concepts in USD? See [Getting Help
with USD](http://openusd.org/docs/Getting-Help-with-USD.html) or visit
the [forum](https://groups.google.com/forum/#!forum/usd-interest).

If you are experiencing undocumented problems with the software, please 
[file a bug](https://github.com/drwave/usd-sketchup/issues/new).

Supported Platforms
-------------------

SketchUp Pro runs on macOS and Windows, but this plug-in is **currently
only supported on macOS**.

This plug-in was developed on macOS but care has been taken to make
sure that, as much as possible, it should be straightforward to port
to Windows. Both [SketchUp's SDK (*developer account required*)](https://extensions.sketchup.com/en/developer_center/sketchup_sdk) on Windows and macOS contain an
example plug-in called `skp_to_xml`, which this plug-in took
inspiration from, so if someone wants to port this to Windows they
should just need to look in the `SDK/samples/skp_to_xml/win/` and in
the `USD SketchUp Mac` folder here and do the equivalent for Windows.

Dependencies
------------

| Name | Version |
| ---- | ------- |
| [macOS](https://www.apple.com/mac/) | 10.13 or higher | 
| [SketchUp Pro](https://www.sketchup.com/download/all) | 2016 or higher | 
| [Xcode](https://developer.apple.com) | 9 or higher |
| [SketchUp SDK](https://extensions.sketchup.com/en/developer_center/sketchup_sdk) | recent|
| [USD](https://github.com/PixarAnimationStudios/USD) | 18.09 or higher |

Getting and Building the Code
-----------------------------


#### 1. Install prerequisites (see [Dependencies](#dependencies) for required versions)

Note: to build this plug-in you will need a [Trimble account](http://identity.trimble.com) and an [Apple Developer account](https://developer.apple.com/account/).

The rest of these instructions assume you have a [Mac](https://www.apple.com/mac/) with [Xcode](developer.apple.com) and [SketchUp Pro](https://www.sketchup.com/download/all) installed.

#### 2. Download the USD source code

You can download source code archives from [GitHub](https://www.github.com/PixarAnimationStudios/USD) or use ```git``` to clone the repository.

```
> git clone https://github.com/PixarAnimationStudios/USD
Cloning into 'USD'...
```

#### 3. Run the USD build script

##### MacOS:

In a terminal, run ```xcode-select``` to ensure command line developer tools are 
installed. Then run the script. We recommend building without Python, without imaging, and as a monolithic library. The Xcode project assumes that it has been built that way and installed into ```/opt/local/USDNoPythonNoImagingMonolithic```.

```
> python USD/build_scripts/build_usd.py --no-python --no-imaging --build-monolithic /opt/local/USDNoPythonNoImagingMonolithic
```

#### 3. Download the SketchUp SDK

Once you login to the Trimble/SketchUp developer account, download the [SDK](https://extensions.sketchup.com/en/developer_center/sketchup_sdk). Unzip and install this somewhere on your machine, for example, ```~/SketchUpSDKs/SDK_Mac_18-0-18665```

#### 3. Download the USD SketchUp exporter plug-in source code

You can download source code archives from [GitHub](https://www.github.com/drwave/usd-sketchup) or use ```git``` to clone the repository.

```
> git clone https://github.com/drwave/usd-sketchup
Cloning into 'usd-sketchup'...
```
At the top level of the repository, make a link to the SketchUp SDK you installed. For example:

```
> cd usd-sketchup
> ln -s ~/SketchUpSDKs/SDK_Mac_18-0-18665 SDK_Mac
```

Launch Xcode on the project file. You may need to fix up various things in the Xcode file that are specific to your build if you have changed them (i.e. installed USD in a different location, have a different version of SketchUp installed, etc.).

You will almost certainly have to update the **Development Team** in the Build Settings (**need a screenshot here**).

The Xcode project assumes that you are building for SketchUp Pro 2018, and building the target will actually copy the resulting ```USDExporter.plugin``` into SketchUp Pro's app bundle in the PlugIns directory, i.e. ```/Applications/SketchUp\ 2018/SketchUp.app/Contents/PlugIns/```.

Initially, that directory will probably not be writable on your machine, so you may want to make it writable:

```
> sudo chmod a+w /Applications/SketchUp\ 2018/SketchUp.app/Contents/PlugIns/
```

Copying the plug-in to the directory makes it very easy to debug the plug-in, as you can launch SketchUp Pro from inside of Xcode, set breakpoints in your plug-in, etc. Very handy when doing development.





