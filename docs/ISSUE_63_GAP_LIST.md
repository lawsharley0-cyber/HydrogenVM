# Issue #63 Census: Ranked Gap List and Implementation Roadmap

## 1. Executive Summary

This document provides a gap analysis for **Issue #63** (*Device reports: missing Foundation/AV methods and a message to a freed object*), incorporating merged reports #47, #53, #55, #59, #60, #91, #111 (RETRY console report), and #113 (Subway Surfers console report). External measurements below are unverified in this checkout.

The analysis cross-checks every class, selector, and symbol against the HydrogenVM support manifest emitted by:
```bash
build/hvm --dump-support
```
**Baseline manifest statistics on `main`**:
- **Symbols**: 4,076 (Real, Approx, Stub)
- **Classes**: 272 (Real, Stub)
- **Selectors**: 2,649 (Real)

### Evidence Sources
1. **Host & Device Execution Logs**:
   - `/home/lawson/compat/runs/retry/keepgoing.json` (RETRY: 102 reported problems)
   - `/home/lawson/compat/runs/subwaysurfers/keepgoing.json` (Subway Surfers: 76 reported problems)
   - `/home/lawson/compat/runs/jetpack/keepgoing.json`
   - `/home/lawson/compat/runs/Tiny Tower/keepgoing.json`
   - Additional test runs in `/home/lawson/compat/runs/` (GoatGame, TalkingBird, terraria, anthill, Mega Jump, GameApp)
2. **Mach-O Binary Disassembly & Symbol Analysis**: reported external measurements (`tools/census.py` via `llvm-nm` and `llvm-objdump`) across 6 test IPAs. The full input set and machine-readable outputs are not committed or tied to these measurements by hashes, so these counts are unverified, including the reported Tiny Tower count:
   - `RETRY_1_5_0_ios_6_0.ipa` (56 missing classes, 136 missing symbols, 576 missing selectors)
   - `Subway_Surfers_1_41_0.ipa` (36 missing classes, 106 missing symbols, 643 missing selectors)
   - `Hungry_Shark_Evolution_2_5_0_ios_4_3.ipa` (64 missing classes, 147 missing symbols, 592 missing selectors)
   - `The_Sandbox_v1.02_iPhone_OS_3.1_2012-05-23.ipa` (12 missing classes, 12 missing symbols, 128 missing selectors)
   - `Tiny_Tower_v1.0_os30.ipa` (6 missing classes, 0 missing symbols, 40 missing selectors)
   - `Jetpack_Joyride.ipa` (13 missing classes, 40 missing symbols, 203 missing selectors)

The reproducible census baseline is the three committed fixtures (Angry Birds, Art of Glow, and Tiny Tower), with hashes, slices, and reference counts in `tools/census.py` and `tools/research_census/README.md`. Those references do not verify the six measurements above.

---

## 2. Issue #63 Core API Audit & Today's Honest Status

Issue #63 highlights five primary blockers from the initial device test cohort. Below is their verification status against `main`:

| Class / Component | Selector / Symbol | Today's Honest Status | Evidence & Code Location |
| :--- | :--- | :--- | :--- |
| **NSString** (#60 Tiny Tower) | `initWithContentsOfURL:`<br>`initWithContentsOfURL:encoding:error:`<br>`initWithContentsOfURL:usedEncoding:error:`<br>`stringWithContentsOfURL:`<br>`stringWithContentsOfURL:encoding:error:`<br>`stringWithContentsOfURL:usedEncoding:error:` | **Already Real** | Implemented in `src/runtime/foundation/String.cpp`. Reads local `file://` URLs from disk; non-file URLs fail cleanly with `NSURLErrorCannotOpenFile` / `kCFURLErrorFileDoesNotExist`. |
| **NSThread** (#47 Jetpack Joyride) | `isFinished`<br>`isExecuting`<br>`isCancelled`<br>`cancel`<br>`name` / `setName:`<br>`threadDictionary` | **Already Real** | Backed by `ThreadData` in `src/runtime/foundation/RunLoop.cpp:760-800`. Accurately tracks thread lifecycle and state flags. |
| **NSThread** (#47 Jetpack Joyride) | `threadPriority`<br>`setThreadPriority:` | **Present but Shallow (Approx)** | `th_priority` returns fixed `0.5` (`RunLoop.cpp:793`). `th_setPriority` (`RunLoop.cpp:794`) is a no-op returning `true` without storing the guest priority. |
| **NSThread** (#47 Jetpack Joyride) | `stackSize`<br>`setStackSize:` | **Missing** | Not declared in `RunLoop.cpp` nor registered in manifest. Calling `-[NSThread stackSize]` sends unrecognized selector. |
| **AVAudioSession** (#53 Hungry Shark) | `mode`<br>`category`<br>`categoryOptions`<br>`sampleRate` / `preferredSampleRate`<br>`outputVolume`<br>`isOtherAudioPlaying`<br>`secondaryAudioShouldBeSilencedHint` | **Present but Shallow (Approx)** | Implemented in `src/runtime/uikit/PlatformStubs.cpp:1125-1170`. Getters return static constants (`"AVAudioSessionModeDefault"`, `"AVAudioSessionCategorySoloAmbient"`, 44100.0, volume 1.0). Mutators (`setCategory:`, `setMode:error:`) accept inputs and return `true`, but do not mutate state. |
| **AVAudioSession** (#53 Hungry Shark) | `currentRoute` | **Missing** | Not declared on `AVAudioSession`. Calling `-[AVAudioSession currentRoute]` triggers unrecognized selector. |
| **AVAudioSession** Constants | `AVAudioSessionCategory*`<br>`AVAudioSessionMode*` | **Mixed (Real / Missing)** | **Real**: `_AVAudioSessionCategoryAmbient`, `_AVAudioSessionCategorySoloAmbient`, `_AVAudioSessionCategoryPlayback`, `_AVAudioSessionCategoryRecord`, `_AVAudioSessionCategoryPlayAndRecord`, `_AVAudioSessionModeDefault`.<br>**Missing**: `_AVAudioSessionCategoryAudioProcessing`, `_AVAudioSessionCategoryMultiRoute`, `_AVAudioSessionModeVoiceChat`, `_AVAudioSessionModeGameChat`, `_AVAudioSessionModeVideoRecording`, `_AVAudioSessionModeMeasurement`, `_AVAudioSessionModeMoviePlayback`. |
| **NSInvocation** (#55 Subway Surfers) | `+[NSInvocation invocationWithMethodSignature:]`<br>`-[NSInvocation invoke]` | **Already Real** | Full host invocation dispatch implemented in `src/runtime/foundation/Invocation.cpp`. On device, a nil method signature arose from unrecognized guest selectors without type encodings. |
| **Object Lifetime** (#59 The Sandbox) | Message sent to a deallocated instance (freed object isa 0) | **Addressed via Diagnostics** | `HVM_ZOMBIES=1` environment flag was introduced to prevent immediate recycling and report the precise deallocated class on illegal access. |

---

## 3. Ranked Gap List: Classes

Reported across console run warnings, crash reports, and Mach-O import references across the cohort IPAs; the external counts are unverified in this checkout:

| Rank | Class | Mentions | Status in Manifest | Category / Framework | Impacted Applications | Notes / Missing Selectors |
| :---: | :--- | :---: | :---: | :--- | :--- | :--- |
| **1** | `GKAchievement` | 6 IPAs | **Stub** | GameKit | Tiny Tower, Hungry Shark, The Sandbox, Subway Surfers, RETRY, Jetpack | Stubbed class; lacks leaderboard/progress persistence. |
| **2** | `GKLocalPlayer` | 6 IPAs | **Stub** | GameKit | All 6 test IPAs | Needs `authenticateWithCompletionHandler:`, `localPlayer`. |
| **3** | `GKPlayer` | 6 IPAs | **Stub** | GameKit | All 6 test IPAs | Needs `playerID`, `alias`, `displayName`. |
| **4** | `GKScore` | 6 IPAs | **Stub** | GameKit | All 6 test IPAs | Needs `reportScoreWithCompletionHandler:`, `value`. |
| **5** | `NSURLCredentialStorage` | 5 IPAs, 2 runs | **Missing** | Foundation / Security | Hungry Shark, The Sandbox, Subway Surfers, RETRY, Jetpack | Needs `sharedCredentialStorage`, `defaultCredentialForProtectionSpace:`. |
| **6** | `GKLeaderboard` | 5 IPAs | **Stub** | GameKit | Tiny Tower, Hungry Shark, The Sandbox, Subway Surfers, Jetpack | Needs `loadScoresWithCompletionHandler:`, `category`. |
| **7** | `EKEventStore` | 3 IPAs, 3 runs | **Missing** | EventKit | Subway Surfers, RETRY, GoatGame | Core calendar database handle. |
| **8** | `EKEvent` | 3 IPAs, 3 runs | **Missing** | EventKit | Subway Surfers, RETRY, GoatGame | Event model: `eventWithEventStore:`, `title`, `startDate`. |
| **9** | `EKAlarm` | 3 IPAs, 3 runs | **Missing** | EventKit | Subway Surfers, RETRY, GoatGame | Alarm specification: `alarmWithRelativeOffset:`. |
| **10** | `EKRecurrenceRule` | 3 IPAs, 3 runs | **Missing** | EventKit | Subway Surfers, RETRY, GoatGame | Recurrence patterns for calendar events. |
| **11** | `EKRecurrenceDayOfWeek` | 3 IPAs, 3 runs | **Missing** | EventKit | Subway Surfers, RETRY, GoatGame | Day-of-week specifications for recurring rules. |
| **12** | `EKRecurrenceEnd` | 3 IPAs, 3 runs | **Missing** | EventKit | Subway Surfers, RETRY, GoatGame | Recurrence end bounds. |
| **13** | `EKEventEditViewController` | 3 IPAs, 2 runs | **Missing** | EventKitUI | Subway Surfers, RETRY | Modal editor view controller for calendar events. |
| **14** | `NSDataDetector` | 3 IPAs, 2 runs | **Missing** | Foundation | Subway Surfers, RETRY, Jetpack Joyride | Regex subclass for phone/link/date extraction. |
| **15** | `SKStoreProductViewController` | 3 IPAs, 2 runs | **Missing** | StoreKit | Subway Surfers, Hungry Shark, RETRY | In-app App Store product sheet. |
| **16** | `ADBannerView` | 3 IPAs | **Missing** | iAd | RETRY, Hungry Shark, Subway Surfers | Banner ad view container. |
| **17** | `ADInterstitialAd` | 3 IPAs | **Missing** | iAd | RETRY, Hungry Shark, Subway Surfers | Interstitial full-screen ad. |
| **18** | `CMMotionManager` | 3 IPAs | **Missing** | CoreMotion | RETRY, Hungry Shark, Subway Surfers | Gyroscope / accelerometer manager. |
| **19** | `SLComposeViewController` | 3 IPAs | **Missing** | Social | RETRY, Hungry Shark, Subway Surfers | Social sharing sheet (Facebook, Twitter, Weibo). |
| **20** | `NSManagedObjectContext` | 1 IPA, 1 run | **Missing** | CoreData | RETRY | Managed object context coordinator. |
| **21** | `NSManagedObject` | 1 IPA, 1 run | **Missing** | CoreData | RETRY | Core data entity instance. |
| **22** | `NSManagedObjectModel` | 1 IPA, 1 run | **Missing** | CoreData | RETRY | Schema representation. |
| **23** | `NSPersistentStoreCoordinator`| 1 IPA, 1 run | **Missing** | CoreData | RETRY | Core data persistent storage layer. |
| **24** | `NSFetchRequest` | 1 IPA, 1 run | **Missing** | CoreData | RETRY | Query request descriptor. |
| **25** | `CAGradientLayer` | 1 IPA, 1 run | **Missing** | QuartzCore / CoreAnimation | RETRY | Layer for multi-stop linear color gradients. |
| **26** | `UIPickerView` | 1 IPA, 1 run | **Missing** | UIKit | RETRY | Wheel selection view control. |
| **27** | `UIPopoverController` | 1 IPA, 2 runs | **Missing** | UIKit | RETRY, GoatGame | iPad popover presentation controller. |
| **28** | `UIActivity` | 1 IPA, 1 run | **Missing** | UIKit | RETRY | Custom activity for `UIActivityViewController`. |
| **29** | `UIDocumentInteractionController` | 1 IPA, 1 run | **Missing** | UIKit | RETRY | File preview and external interaction sheet. |
| **30** | `NSValueTransformer` | 1 IPA, 1 run | **Missing** | Foundation | Subway Surfers | Reversible value transformation utility. |
| **31** | `NSNetService` | 1 IPA, 1 run | **Missing** | Foundation / Bonjour | Subway Surfers | Network Bonjour service publication / discovery. |
| **32** | `CLRegion` | 1 IPA, 1 run | **Missing** | CoreLocation | RETRY | Circular geographic region monitoring. |
| **33** | `NSMutableOrderedSet` | 1 IPA, 1 run | **Missing** | Foundation | RETRY | Mutable ordered set collection. |
| **34** | `NSOrderedSet` | 1 IPA, 1 run | **Missing** | Foundation | RETRY | Immutable ordered set collection. |
| **35** | `MKAnnotationView` | 1 IPA, 3 runs | **Missing** | MapKit | Anthill, Mega Jump, Jetpack Joyride | Map pin annotation view. |

---

## 4. Ranked Gap List: Selectors

Top missing selectors referenced by guest binaries that are missing from `build/hvm --dump-support`:

| Rank | Selector | Mentioning IPAs / Runs | Status | Target Class / Context |
| :---: | :--- | :---: | :---: | :--- |
| **1** | `stackSize` | 2 IPAs | **Missing** | `NSThread` |
| **2** | `setStackSize:` | 2 IPAs | **Missing** | `NSThread` |
| **3** | `currentRoute` | 2 IPAs | **Missing** | `AVAudioSession` |
| **4** | `loadFriendsWithCompletionHandler:` | 4 IPAs | **Missing** | `GKLocalPlayer` |
| **5** | `setAlertLaunchImage:` | 4 IPAs | **Missing** | `UILocalNotification` |
| **6** | `defaultCredentialForProtectionSpace:` | 4 IPAs | **Missing** | `NSURLCredentialStorage` |
| **7** | `sharedCredentialStorage` | 4 IPAs | **Missing** | `NSURLCredentialStorage` |
| **8** | `authenticateWithCompletionHandler:` | 4 IPAs | **Missing** | `GKLocalPlayer` |
| **9** | `loadScoresWithCompletionHandler:` | 4 IPAs | **Missing** | `GKLeaderboard` |
| **10** | `reportScoreWithCompletionHandler:` | 4 IPAs | **Missing** | `GKScore` |
| **11** | `isAvailableForServiceType:` | 3 IPAs | **Missing** | `SLComposeViewController` |
| **12** | `composeViewControllerForServiceType:` | 3 IPAs | **Missing** | `SLComposeViewController` |
| **13** | `requestStoreProductPageWithParameters:` | 3 IPAs | **Missing** | `SKStoreProductViewController` |
| **14** | `saveEvent:span:error:` | 3 IPAs | **Missing** | `EKEventStore` |
| **15** | `requestAccessToEntityType:completion:` | 3 IPAs | **Missing** | `EKEventStore` |
| **16** | `authorizationStatusForEntityType:` | 3 IPAs | **Missing** | `EKEventStore` |
| **17** | `matchesInString:options:range:` | 3 IPAs | **Missing** | `NSDataDetector` |
| **18** | `dataDetectorWithTypes:error:` | 3 IPAs | **Missing** | `NSDataDetector` |
| **19** | `startAccelerometerUpdatesToQueue:withHandler:` | 3 IPAs | **Missing** | `CMMotionManager` |
| **20** | `setColors:` / `setLocations:` | 2 IPAs | **Missing** | `CAGradientLayer` |

---

## 5. Ranked Gap List: Symbols & Constants

Top missing non-ObjC imports and external data constants identified in run logs and Mach-O symbols:

| Rank | Symbol / Constant | Mentions | Kind | Status | Impacted Modules |
| :---: | :--- | :---: | :---: | :---: | :--- |
| **1** | `_NSURLAuthenticationMethodServerTrust` | 4 IPAs | Data | **Missing** | Security / Foundation URL loading |
| **2** | `_kCFBundleVersionKey` | 4 IPAs | Data | **Missing** | CoreFoundation bundle keys |
| **3** | `_kCFBundleIdentifierKey` | 3 IPAs | Data | **Missing** | CoreFoundation bundle keys |
| **4** | `_kCLLocationAccuracyBest` | 4 IPAs | Data | **Missing** | CoreLocation constants |
| **5** | `_kCLLocationAccuracyHundredMeters` | 3 IPAs | Data | **Missing** | CoreLocation constants |
| **6** | `_kCATransactionDisableActions` | 2 IPAs, 1 run | Data | **Missing** | CoreAnimation transaction keys |
| **7** | `AudioQueueAllocateBufferWithPacketDescriptions` | 4 runs | Func | **Missing** | AudioToolbox recording/streaming |
| **8** | `protocol_conformsToProtocol` | 4 runs | Func | **Missing** | Objective-C runtime inspection |
| **9** | `protocol_copyMethodDescriptionList` | 4 runs | Func | **Missing** | Objective-C runtime inspection |
| **10** | `protocol_copyProtocolList` | 4 runs | Func | **Missing** | Objective-C runtime inspection |
| **11** | `CFRunLoopAddSource` / `CFRunLoopRemoveSource` | 3 runs | Func | **Missing** | CoreFoundation RunLoop integration |
| **12** | `glBlitFramebuffer` / `glDrawBuffers` | 3 runs | Func | **Missing** | OpenGL ES 2.0 / 3.0 framebuffers |
| **13** | `_AVAudioSessionCategoryAudioProcessing` | 2 IPAs | Data | **Missing** | AVFoundation constants |
| **14** | `_AVAudioSessionCategoryMultiRoute` | 2 IPAs | Data | **Missing** | AVFoundation constants |
| **15** | `_AVAudioSessionModeVoiceChat` | 2 IPAs | Data | **Missing** | AVFoundation constants |
| **16** | `_AVAudioSessionModeGameChat` | 2 IPAs | Data | **Missing** | AVFoundation constants |

---

## 6. Proposed Future Workstreams & Branches

To implement these gaps safely without modifying runtime behavior in this PR or destabilizing current gates, work is partitioned into the following atomic branches:

### Workstream 1: Foundation Threading & Audio Session Completion
- **Proposed Branch**: `feature/nsthread-and-audiosession`
- **Scope**:
  - `NSThread`: Implement `stackSize` and `setStackSize:` with backing integer property; back `threadPriority` and `setThreadPriority:` with real guest storage.
  - `AVAudioSession`: Add `currentRoute` returning an `AVAudioSessionRouteDescription` stub; back `category` and `mode` with mutable storage so `setCategory:...` and `setMode:...` persist across queries; define missing category/mode string symbols.
- **Verification**: Dedicated test in `build/hvm_tests fulltest` validating property round-trips.

### Workstream 2: EventKit & EventKitUI Stubs
- **Proposed Branch**: `feature/eventkit-stubs`
- **Scope**:
  - Implement stub classes for `EKEventStore`, `EKEvent`, `EKAlarm`, `EKRecurrenceRule`, `EKRecurrenceDayOfWeek`, `EKRecurrenceEnd`, and `EKEventEditViewController`.
  - Until authorization is implemented, have `authorizationStatusForEntityType:` return denied and `requestAccessToEntityType:completion:` invoke its completion with `granted = false` and an error describing the unsupported service.
- **Verification**: Test Lab case validating instantiation, denied status, and completion with `granted = false` and a non-nil error.

### Workstream 3: Security & Network Credentials
- **Proposed Branch**: `feature/security-url-credential-storage`
- **Scope**:
  - Implement `NSURLCredentialStorage` (singleton `sharedCredentialStorage`, empty credential lookup responses).
  - Add `_NSURLAuthenticationMethodServerTrust` and `_NSURLAuthenticationMethodClientCertificate` data constants.
- **Verification**: Retain `tools/tests/test_t4_support.py` manifest and status checks. When implementing storage, add behavior tests proving repeated `sharedCredentialStorage` calls return the same non-nil instance and default credential lookups return nil for distinct protection spaces. Demonstrate each test fails before the implementation and passes afterward.

### Workstream 4: GameKit Minimal Stubs
- **Proposed Branch**: `feature/gamekit-stubs`
- **Scope**:
  - Complete remaining methods on `GKAchievement`, `GKLeaderboard`, `GKScore`, `GKPlayer`, `GKLocalPlayer`. Existing authentication, score loading, and score reporting callbacks complete asynchronously with GameKit errors when unavailable.
- **Verification**: Retain manifest and status checks, and test singleton identity, callback delivery and error results. Add behavior tests for each remaining method before implementing it, showing failure before and success afterward. Run `tools/census.py` against the three committed fixtures to check for regressions; the six external measurements above are not a reproducible verification baseline.

### Workstream 5: UIKit & CoreAnimation Visual Elements
- **Proposed Branch**: `feature/uikit-gradient-picker`
- **Scope**:
  - Implement `CAGradientLayer` (inheriting `CALayer`, backing `colors` and `locations`).
  - Implement `UIPickerView`, `UIPopoverController`, `UIActivity`, `UIDocumentInteractionController`.
  - Define `_kCATransactionDisableActions`.
- **Verification**: Fulltest and Test Lab headless render execution.

### Workstream 6: CoreData Stubs
- **Proposed Branch**: `feature/coredata-stubs`
- **Scope**:
  - Provide lightweight stubs for `NSManagedObjectContext`, `NSManagedObject`, `NSManagedObjectModel`, `NSPersistentStoreCoordinator`, `NSFetchRequest`, `NSEntityDescription`, and `NSAttributeDescription`.
- **Verification**: RETRY boot verification past CoreData initialization warnings.
