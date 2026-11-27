# Farm Radio Fleet — Android

A local-first radio-memory fleet manager for a mixed collection of Baofeng UV-5R and UV-5RM handhelds. Source lives in the MeshCore fork under a separate app directory; MeshCore firmware is untouched.

## Current build — v0.1

- One canonical channel plan stored on the Android phone.
- CHIRP CSV export/import supporting frequencies, split/offset, RX-only channels, CTCSS, bandwidth, and power.
- Register physical radios by label and exact model.
- Verify programming by importing a *fresh read-back CSV* exported from the physical radio. Displays per-memory mismatches and marks radios stale when the master changes.
- Starter memories 001–006 from the earlier farm emergency plan; replace or inspect before transmission.
- Includes a guided handoff to RadioDroid, whose CHIRP drivers support model-specific radio reads and writes over USB OTG or supported BLE UART adapters.
- A **read-only TD-BL1 Bluetooth inspector** scans nearby advertisements and reports GATT service/characteristic UUIDs and capabilities. Use this for validating the direct BLE adapter before enabling any app-side writes.

**No direct EEPROM write in this build.** The current APK manages a shared channel list and verifies radio read-backs. RadioDroid performs the actual read/write. A single-button Bluetooth writer requires confirming TD-BL1 BLE characteristics and validating UV-5R and UV-5RM identification/write protocols on real hardware. Writing model-specific raw images across models risks corruption; only channel-level CSVs are portable.

## Reproduce the user's ODmaster → new radio problem

1. Install RadioDroid from https://github.com/jnyer27/RadioDroid/releases (external project).
2. Open **Farm Radio Fleet**. Review/edit the six starter memory entries.
3. Register each physical UV-5R and UV-5RM under **Radio Inventory**.
4. Tap **Export CHIRP CSV master** and save to Downloads.
5. In RadioDroid, select **Baofeng / UV-5R** (or **UV-5RM**) and read the individual physical radio. Save its original full EEPROM image before editing.
6. After saving the original radio image, clear the target memory slots in the external programmer. RadioDroid's current CSV importer populates the first *empty* slots selected in the UI, rather than enforcing CSV Location values; choose the desired starting channel, check every destination slot and all receive/transmit/tone fields, and only then write.
7. Write and await success. Read the physical radio again, export CHIRP CSV.
8. Return to Farm Radio Fleet → **Compare a radio read-back CSV**, select the corresponding physical radio and the exported CSV.
9. A zero-diff result marks the physical radio synced to the master fingerprint. Any profile change marks previous verifications stale.

If the external programmer uses a different CSV slot convention, adjust the model-specific import location in that programmer. The app doesn't silently remap slots.

## Safety boundaries

- UV-5R/UV-5RM profiles are distinct. Never write one model's raw full EEPROM image to the other.
- Radio channel 005 in the starter configuration is receive-only (RT22 factory FRS channel 5); normal UV-5R transmitters are not FCC Part 95-certified for FRS/GMRS transmission.
- Edit the local frequencies and tones after confirming the current repeater owner's specifications.
- Do not treat an exported-but-unverified file, or a completed write dialog, as proof of synchronization; import a read-back and compare.
- The user controls the physical write. The app intentionally avoids claiming a successful device flash without reading the device.
- This app programs channel memories, not radio firmware.

## GitHub APK build

Built from the Lembas build pattern: JDK 17, Android/Gradle CI, run-number version codes, upload-artifact, immutable per-run Releases, plus a moving `radio-fleet-latest` tag.

Workflow: `.github/workflows/build-radio-fleet-apk.yml`
APK artifact: `farm-radio-fleet-apk`
Latest release: `https://github.com/mharlos/MeshCore/releases/tag/radio-fleet-latest`

The initial build uses a CI-generated debug key. Reinstalling a newly generated signed APK may require uninstalling the prior build, losing Android-local app data. Before relying on in-place updates, move to a persistent keystore held in GitHub Actions secrets. Export CSV before uninstalling.

Local build:

```sh
gradle -p apps/radio-fleet testDebugUnitTest assembleDebug
```

## Next milestone: direct Bluetooth programmer

- Record read-only GATT service/characteristic inventory for TD-BL1 and actual service UUIDs.
- Implement a BLE serial transport with MTU-aware chunking, ACK/timeout handling, disconnection safety, and a traffic log with no personal data.
- Port a model-specific CHIRP clone adapter for UV-5R and UV-5RM **separately**, with first-write protection and original-image backup.
- Gate the Write action on correct radio identification and approved band limitations; read back and compare after write.
- Add fleet-wide progress and a deliberate manual action per connected radio.

No third-party RadioDroid code is copied into this repository. Programming handoff uses its published Android APK and CHIRP interoperability.
