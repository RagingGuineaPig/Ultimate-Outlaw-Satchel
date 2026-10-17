# Building Ultimate Outlaw Satchel

These instructions describe the Visual Studio configuration used to build the current **v2.1** release source.

## Requirements

- Windows 10 or Windows 11
- Microsoft Visual Studio with the **Desktop development with C++** workload
- x64 MSVC toolchain
- Red Dead Redemption 2 ScriptHookRDR2 SDK
- the same `third-party` headers used by the ScriptHookRDR2 project setup

The source includes:

```cpp
#include "sdk/inc/main.h"
#include "natives.h"
```

The project therefore needs the ScriptHookRDR2 SDK headers available under `SDK\inc`, plus the project third-party include directory containing the required native header set.

## Suggested project layout

```text
Ultimate-Outlaw-Satchel/
├─ main.cpp
├─ SDK/
│  ├─ inc/
│  │  └─ main.h
│  └─ lib/
│     └─ ScriptHookRDR2.lib
└─ third-party/
   └─ ... required header files, including natives.h ...
```

The SDK and third-party dependency files are external build dependencies and are not redistributed here.

## Visual Studio configuration

Create an empty C++ project or DLL project and add the repository's `main.cpp`.

Use:

- **Configuration:** Release
- **Platform:** x64
- **Precompiled Headers:** Not Using Precompiled Headers

### C/C++ -> General -> Additional Include Directories

Add:

```text
$(ProjectDir)SDK\inc
$(ProjectDir)third-party
```

They may also be entered on one line separated by semicolons:

```text
$(ProjectDir)SDK\inc;$(ProjectDir)third-party
```

### Linker -> General -> Additional Library Directories

Add:

```text
$(ProjectDir)SDK\lib
```

### Linker -> Input -> Additional Dependencies

Add:

```text
ScriptHookRDR2.lib
```

Do not remove the normal Visual Studio default linker dependencies.

### Configuration Properties -> Advanced

Set:

```text
Target File Extension = .asi
```

### Target name

Set the project **Target Name** to:

```text
UltimateOutlawSatchel
```

The resulting release binary should therefore be:

```text
UltimateOutlawSatchel.asi
```

## Output directory

The development project used an outer `x64` output folder. A matching configuration is:

```text
Output Directory = $(SolutionDir)x64\
```

The exact output folder is not functionally important; the important part is that the compiled module is x64 and is emitted as `UltimateOutlawSatchel.asi`.

## Build

With **Release | x64** selected:

1. Build the Visual Studio solution.
2. Confirm the build completes successfully.
3. Confirm the output file is named `UltimateOutlawSatchel.asi`.

## Runtime dependencies / installation context

The ASI is intended to be loaded by the standard RDR2 ScriptHookRDR2 ASI environment.

The public release package also includes three YTD texture dictionaries placed under:

```text
Red Dead Redemption 2\lml\stream\
```

Those YTDs provide artwork only and are not part of the C++ compilation process.

## Notes for reviewers

The release source contains low-level Windows memory APIs because the mod resolves and patches Rockstar's runtime item-capacity data inside the Red Dead Redemption 2 process.

Some legacy helper code for earlier development experiments remains in the source, including vectored-exception and native-interception support. The current v2.1 release does **not** install the old `DATABINDING_WRITE_DATA_STRING` breakpoint/write hook.

The source does not include networking, downloading, credential access, browser access, or unrelated system modification. Its only persistence is its own small mod data/log files.
