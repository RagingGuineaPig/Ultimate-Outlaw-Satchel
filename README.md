# HWC's Ultimate Outlaw Satchel

Source code for **HWC's Ultimate Outlaw Satchel**, a Red Dead Redemption 2 Story Mode ASI mod.

This repository is provided so Nexus Mods staff and other reviewers can inspect the exact source used for the current public release candidate.

## Current source

- Release source: `main.cpp`
- Release version: **V2**
- Output binary name: `UltimateOutlawSatchel.asi`
- Target game: Red Dead Redemption 2 Story Mode
- Platform: Windows x64

## What the mod does

Ultimate Outlaw Satchel increases the practical capacity of Rockstar's live item database at runtime rather than replacing `catalog_sp.ymt`.

The current release includes:

- near-bottomless satchel capacity
- near-bottomless ammunition capacity
- expanded cigarette-card and stackable pocket-watch capacities
- Rockstar's native `Carrying <quantity>` satchel display
- special runtime handling for Wild Mint, Creeping Thyme, and Oregano so the Ram Horn Trinket reward remains correct
- a first-run welcome screen
- Haris startup presentation
- optional presentation synchronization when Ultimate Frontier Stash is loaded

The mod does not replace Rockstar's `catalog_sp.ymt`.

## Runtime implementation

V2 resolves and patches Rockstar's live item-capacity records inside the Red Dead Redemption 2 process.

The current release **does not install the old `DATABINDING_WRITE_DATA_STRING` breakpoint/write hook** that earlier development builds used for the custom `Hoarding <quantity>` wording. V2 leaves Rockstar's native `Carrying <quantity>` presentation in place.

The source still contains some legacy helper code from earlier development iterations, but that DataBinding write-hook path is not activated by the current release.

Relevant Windows APIs visible in the source include `VirtualProtect`, `VirtualAlloc`, `AddVectoredExceptionHandler`, `FlushInstructionCache`, and related process-memory functions. They are used only inside the RDR2 process for the mod's runtime patching and legacy/internal support code.

## Building

See **[BUILDING.md](BUILDING.md)** for the build requirements and step-by-step Visual Studio configuration used for the release build.

## Release assets

The Nexus download also contains YTD texture dictionaries used only for the welcome/startup artwork. Those assets are not required to review the C++ behavior of the ASI.

No precompiled ASI is stored in this repository.
