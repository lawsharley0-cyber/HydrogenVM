# Angry Birds: `ByteArrayInputStream` Unexpected end of file while locale text loads (#106 / #89 / #67)

## Diagnostics and Reproduction Attempts
- **Environment:** Tested on the Linux emulator (`hvm_tests fulltest` and `hvm run`).
- **Path Testing:** Tested short paths (`/tmp/short/Payload/AngryBirds.app`) and long paths (over 90 characters long) to mimic the device installation paths. Both cases succeeded without crashes on the Linux build (the process correctly printed loading messages and didn't crash out).
- **Files Checked:** Verified the integrity of `TEXTS_BASIC.dat` and `TEXTS_LANGUAGE_SELECTION.dat` on both extracted paths compared to the base `AngryBirds_v1.0_os30.ipa`. The files are 5282 bytes and 326 bytes respectively. Their hashes match.
- **Differences from Device:** The `PathName::allocate` long-path bug (#80) is unrelated to this `ByteArrayInputStream` EOF bug, as the bug persists after the long-path logic is corrected. The failure is observed only on the device, potentially related to the 16KB device memory page size, unhandled short reads, or missing file layers in the iOS stub implementation.

## Requirements for Device Evidence
Since #80 handles the path-mapping and path-length issues, the true evaluation of this bug must be conducted from a post-#80 device build.

**Precise request for the device evidence:**
To fully diagnose the `ByteArrayInputStream` "Unexpected end of file" error during locale text load on the phone:
1. Provide a fresh full Share JSON crash/report bundle after the #80 build is integrated.
2. Include the emulator commit hash and `host.pageSize` from the environment.
3. Include the full failure backtrace.
4. Provide the `fileHistory` from the `#67` file open ring for the failing locale resource (`TEXTS_BASIC.dat` or `TEXTS_LANGUAGE_SELECTION.dat`): we need the host result, expected file size, bytes actually read, and whether the handle was still open at crash.
5. If available, please also supply the installed resource's SHA256 and length compared against the IPA, to rule out a missing or truncated installation.

Do not reupload the existing in-repo IPA or share credentials. This device evidence is required to unblock further investigation.
