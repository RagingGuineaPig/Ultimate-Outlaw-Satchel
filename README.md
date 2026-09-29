# HWC's Ultimate Outlaw Satchel

Source code for **HWC's Ultimate Outlaw Satchel**, a Red Dead Redemption 2 Story Mode ASI mod.

This repository is provided so Nexus Mods staff and other reviewers can inspect the exact source used for the current public release candidate.

## Current source

- Release source: `main.cpp`
- Release version: **v2.15.24**
- Output binary name: `UltimateOutlawSatchel.asi`
- Target game: Red Dead Redemption 2 Story Mode
- Platform: Windows x64

## What the mod does

Ultimate Outlaw Satchel increases the practical capacity of Rockstar's live item database at runtime rather than replacing `catalog_sp.ymt`.

The current release includes:

- near-bottomless satchel capacity
- near-bottomless ammunition capacity
- expanded cigarette-card and stackable pocket-watch capacities
- the custom satchel quantity text `Hoarding <quantity>`
- a first-run welcome screen
- Haris startup presentation
- optional presentation synchronization when Ultimate Frontier Stash is loaded

The mod does not replace Rockstar's `catalog_sp.ymt`.

## Why the source uses low-level Windows APIs

The source contains runtime memory inspection/patching and breakpoint-based native interception. These are used only inside the Red Dead Redemption 2 process to modify the game's runtime item-capacity data and satchel UI behavior.

Relevant Windows APIs visible in the source include `VirtualProtect`, `VirtualAlloc`, `AddVectoredExceptionHandler`, `FlushInstructionCache`, and related process-memory functions.

## Building

See **[BUILDING.md](BUILDING.md)** for the build requirements and step-by-step Visual Studio configuration used for the release build.

## Release assets

The Nexus download also contains YTD texture dictionaries used only for the welcome/startup artwork. Those assets are not required to review the C++ behavior of the ASI.

No precompiled ASI is stored in this repository.
