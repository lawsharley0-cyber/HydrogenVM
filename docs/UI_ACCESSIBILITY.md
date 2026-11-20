# Library and player refresh

The Library keeps its iPhone and Android tabs, import progress, and Add button.
Search matches game names and bundle identifiers within the current tab. A
visible per-game actions menu exposes Files and maintenance alongside the
existing swipe and long-press gestures. Reset and delete ask for confirmation.
Unsupported games display the importer's explanation.

Settings separates Playing, Accessibility & controls, Storage, and Advanced
troubleshooting. Existing storage, Safe Mode, and performance preferences
retain their keys. The independent `player.showControlsButton` preference
defaults to off, preserving the full-screen view for existing games. VoiceOver
keeps the controls entry available. The three-finger hold remains available
when the controls button and debug overlay are hidden.

The collapsed player pill has a 44-point touch target and announces the game
state. It includes the frame rate when the debug overlay is enabled. HUD
expansion respects Reduce Motion. Scale Mode uses a menu and keeps the current
choice visible when closed.

## Validation before release

Run the macOS app build and existing player SwiftUI UI harness, including the new
gesture-free controls test. Keep the three-finger test with both overlay settings
explicitly off. The display tests cover crop controls, live global default changes, and touch
coordinates after each display mode.

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
