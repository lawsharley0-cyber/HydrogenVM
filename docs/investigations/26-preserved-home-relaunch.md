# #26: preserve and compare the sandbox between launches

The new tools/hvm_relaunch.py takes a SHA-256 inventory before launch and
after each separate-process launch, preserving the same home throughout.
It records added/changed/removed paths, commands, executable and fixture
fingerprints, host page size, logs and structured results. It never follows
home symlinks or clears state. Output must be a separate, empty directory.
The three-launch in-process mode records before/after the whole batch and
explicitly marks intermediate snapshots unavailable.

Failure-first tool regressions cover a failed second launch with retained save
bytes and a third attempt, path overlap, symlinks, and a process that returns
zero while its report says crashed. A zero process status alone is not success.
Five tests pass on Linux; POSIX execution fixtures actually run there.

## Exact private fixture observation

Tiny Tower v1.0 IPA SHA-256 is
7f84f2d2c561373917a05866337bdf8d8653b3ac8b801248d298f20b5c649578.
Main 4c09fa4708fe48796f3255695e97aef685087042, executable SHA-256
1b92fff9e5a9719e1088afac0e4428c360443a1752973c519a1da528e9ec39ad,
Linux x86_64/native pageSize 4096, software renderer: three separate
20-second launches returned [0,0,0], each report exitStatus=0/crashed=false.
A separate three-launch in-process batch also returned zero/crashed=false.
The home remained intact within each experiment. Neither reproduces #26.

The per-launch diff added localytics YAML session files and changed
Library/Preferences/com.eeenmachine.tinytowers.plist on launches two and three;
no paths were removed. Private raw snapshots and logs remain under
HVM_FIXTURE_ROOT/evidence/tiny-{separate,in-process}-1793574016011127542.
A changed hash alone does not establish an invalid writer or reader. No
guessed persistence fix is included and no saved state is discarded.

Local gates use the unchanged main runtime binary above: fulltest 487/487,
Test Lab 25/25 with zero skips. The final full tools result is recorded in the
PR; no C++ thread or memory implementation changes are made by this card.

## Capture the failing owner sandbox

1. Before opening Tiny Tower again, export the complete existing guest home
   privately. Record emulator full commit/build, device/iOS/native page size,
   IPA SHA/version, renderer and how the preceding close was performed.
2. Keep the original export untouched. On a Linux runner with that exact
   private fixture/home copy, run:

   python3 tools/hvm_relaunch.py --hvm build/hvm --app PRIVATE_APP \
     --home PRIVATE_PRESERVED_HOME --out PRIVATE_EMPTY_EVIDENCE --launches 3

   Run the separate and in-process modes against separate copies of the same
   original failing home, never an emptied substitute. Retain every snapshot,
   log, report and screenshot, including a failed second attempt.
3. On the iPhone, export the guest home after first launch, after its normal
   close, and immediately after the next-launch crash, without deleting the
   installation. Capture #264 recovered crash JSON/log and the matching iOS
   .ips/dSYM. Record exact elapsed close/relaunch inputs. Hash inventories can
   be made by importing snapshot_home from tools/hvm_relaunch.py.
4. Compare changed files privately, identify the crashing reader and producing
   writer from the stack/log, then minimize that data-format/lifetime failure
   into a failing test before altering either side. Do not upload save values
   or identify the Linux non-reproduction as resolution. #26 remains open.

Local enabled Linux tools suite for original candidate c0438238eeb8482e240b5da8bda4545cca463495: 207/207 tests, zero skips (HVM_TESTLAB_TOOLCHAIN=1).

Review follow-up requires an existing home, propagates unreadable-directory
errors and records incomplete capture metadata instead of inventing a diff.
POSIX evidence directories are created 0700 and public existing outputs are
rejected. The failure fixture now seeds bytes before the first launch and
checks both initial/first-launch hashes. Nine focused tests pass.
The existing CLI stops an in-process batch on its first failed launch; it has
no continuation flag. The tool records banner-derived attemptedLaunches and
allRequestedLaunchesAttempted, while separate mode still attempts all three.
No unsupported flag or guessed successful third launch is introduced.
The complete enabled Test Lab integration test is bounded to 240 seconds; a
timeout fails the suite.

The original head c0438238eeb8482e240b5da8bda4545cca463495 was independently
tested by [Actions CI 41364496774](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41364496774).
Its checkout log identifies merge 3ef9bba3a10b5f95b2f013c6525d36785c8fe84f;
the complete tools step passed 207 tests in 12.044 seconds. These Actions
results apply to the original head, while the new error/privacy regressions
require their own final run.

After the review fixes, the local enabled tools suite passes 211/211 with zero skips. Existing runtime fulltest487 and TestLab25 results still apply; no runtime source changed.
