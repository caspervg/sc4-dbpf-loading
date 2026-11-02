# sc4-dbpf-loading

A DLL Plugin for SimCity 4 that optimizes the DBPF plugin loading that the game performs on startup.   

The plugin can be downloaded from the Releases tab: https://github.com/0xC0000054/sc4-dbpf-loading/releases

## Features

* Reduces the time for the game to show the SC4 logo by up to 90%.
* Adds an optimization to the game code for large data reads.
* Allows the game to handle file paths that are longer than the standard Microsoft Windows limit of 260 characters.
* Reduces lag and city load times with large numbers of .SC4* plugins.
* Disables the game code that searches the entire file if the DBPF signature was not found.
* Changes the game code that loads the .SC4* files to ignore files with non-DBPF file extensions, .txt, .png, etc. 
    * With this change only .SC4Desc, .SC4Lot, .SC4Model, and files without any file extension will be loaded.
    * Files without any file extension are included due to some older plugins having that issue.
* Changes the 'missing plugin' dialog to display the plugin pack ID in hexadecimal.
* Reduces the lag after zooming or rotating the view, while the game loads the models for the new view.
    
## System Requirements

* SimCity 4 version 641
* Windows 10 or later
* [Microsoft Visual C++ 2022 x86 Redistribute](https://aka.ms/vs/17/release/vc_redist.x86.exe)

The plugin may work on Windows 7, but I do not have the ability to test that.

## Installation

1. Close SimCity 4.
2. Copy `SC4DBPFLoading.dll` into the top-level of the Plugins folder in the SimCity 4 installation directory or Documents/SimCity 4 directory.
3. Start SimCity 4.

## Configuration

The model streaming changes can be adjusted with an INI file that has the same name as the DLL and is
placed next to it, e.g. `SC4DBPFLoading.ini`. All keys are optional.

```ini
[ModelStreaming]
; 0 disables the model streaming changes.
Enabled=1
; The free memory, in MB, that the game may assume when it sizes the area around the view that it
; loads after a zoom or rotation change. The game scales that area with the free memory, which makes
; it cover the whole city on current PCs. 0 uses the actual free memory, like the game does.
LoadMarginMB=512
; The time per frame, in milliseconds, spent on loaded models and on redrawing the view.
; Higher values load faster at a lower frame rate.
FrameBudgetMs=8
; How far around the view center the models count as on screen, in half screen widths. The view is
; redrawn once these are in place; the rest is loaded without redraws.
VisibleProgress=2.0
; The longest the redraw waits for the models on screen, in milliseconds.
VisibleMaxMs=2000
; The size of the tiles the view is redrawn in, in pixels.
TileSize=512
; How long the model loading thread works before it pauses, in milliseconds.
WorkerSliceMs=4
; How long the model loading thread pauses between slices, in microseconds.
WorkerRestUs=500
; Writes a line to the log file for every zoom or rotation load.
LogStats=1
```

## Troubleshooting

The plugin should write a `SC4DBPFLoading.log` file in the same folder as the plugin.    
The log contains status information for the most recent run of the plugin.

### Command Line Argument

The plugin adds a `-StartupDBPFLoadTrace:` command line argument that can be used for advanced tracing/debugging 
of the plugin loading process.
It supports to following options (only 1 can be used at a time):

* `ShowLoadTime` - shows a message box with the resource loading time in milliseconds.
* `WinAPI` - shows message boxes before and after the resource loading code runs, this allows the user to start and stop a Process Monitor trace when the message box is shown.
* `ListLoadedFiles` - writes the loaded DBPF files to the plugin's log file in the order SC4 reads them.

# License

This project is licensed under the terms of the GNU Lesser General Public License version 2.1 or (at your option) any later version published by the Free Software Foundation.    
See [LICENSE.txt](LICENSE.txt) for more information.

## 3rd party code

[gzcom-dll](https://github.com/nsgomez/gzcom-dll/tree/master) - LGPL 2.1 or later License.    
[SC4Fix](https://github.com/nsgomez/sc4fix) - MIT License.     
[sc4-dll-utilities](https://github.com/0xC0000054/sc4-dll-utilities) - LGPL 2.1 or later License.    
[Detours](https://github.com/microsoft/Detours) - MIT License    
[Windows Implementation Library](https://github.com/microsoft/wil) - MIT License    
[Boost.Algorithm](https://www.boost.org/doc/libs/1_84_0/libs/algorithm/doc/html/index.html) - Boost Software License, Version 1.0.    
[Boost.Unordered](https://www.boost.org/doc/libs/1_84_0/libs/unordered/doc/html/unordered.html) - Boost Software License, Version 1.0.    

# Source Code

## Prerequisites

* Visual Studio 2022
* `git submodule update --init`
* [VCPkg](https://github.com/microsoft/vcpkg) with the Visual Studio integration

## Building the plugin

* Open the solution in the `src` folder
* Update the post build events to copy the build output to you SimCity 4 application plugins folder.
* Build the solution

## Debugging the plugin

Visual Studio can be configured to launch SimCity 4 on the Debugging page of the project properties.
I configured the debugger to launch the game in full screen with the following command line:    
`-intro:off -CPUcount:1 -w -CustomResolution:enabled -r1920x1080x32`

You may need to adjust the resolution for your primary screen.
