# Library and player refresh

The library leads with an import card, followed by searchable app rows. Each row
has a visible actions menu; Files and maintenance actions no longer require a
swipe or long press. Reset and delete explain what they remove before acting.
Unsupported rows explain why they cannot launch. A runnable ARMv7 binary is not
labelled as verified compatible.

Settings separates Playing, Accessibility & controls, Storage and Advanced
troubleshooting. Existing storage and performance preferences retain their keys.
The independent `player.showControlsButton` preference defaults to true. VoiceOver
keeps the controls entry available even if that preference is false. Users who
hide both the controls button and performance overlay can still summon controls
with the existing three-finger hold.

The adaptive indigo accent has separate light and dark variants. Library labels
use semantic system text colors and wrapping system fonts. Status includes text
and a symbol. Player controls use opaque system surfaces, labelled icon actions,
44-point minimum action targets and a scrollable panel. Display menus replace
crowded segmented pickers; expansion respects Reduce Motion.

## Validation before release

Windows source checks are not a native build or accessibility certification.
Run the macOS app build and existing player SwiftUI UI harness, including the new
gesture-free controls test. Keep the three-finger test with both overlay settings
explicitly off. Updated display tests must still assert crop control appearance,
live global default changes and touch coordinates after each display mode.

On an iPhone, check:

- Empty library, multiple imports, progress, failed import and pull to refresh.
- Search by app name and bundle identifier, no results, then clearing search.
- Supported launch, unsupported explanation and Home Screen deep links.
- Every app action from the visible menu, cancellation of reset/delete and reinstall.
- Settings presentation, nested batch testing and dismissal back to the library.
- Light/dark appearance, Increase Contrast and the largest accessibility text size.
- VoiceOver reading order and names for import, app launch, app actions and settings.
- Controls button on/off, VoiceOver overriding the off preference, and three-finger fallback.
- Portrait and both landscape orientations: scroll to volume/log/quit, return to
  minimize, then resume the game without intercepting touches outside the panel.
- Reduce Motion enabled, volume announced as a percentage, pause/resume and save/quit.

Guest app accessibility is unchanged: this work improves HydrogenVM's host shell.
