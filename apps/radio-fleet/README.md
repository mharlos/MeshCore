# Farm Radio Fleet — Android

This application resides under `apps/radio-fleet/` in the MeshCore fork and is
built independently of the MeshCore firmware. The current development branch
targets a **single Android app** for memory-channel management across:

- Baofeng UV-5R — CHIRP `Baofeng / UV-5R` clone driver.
- Baofeng UV-5RM — CHIRP `Baofeng / 5RM` clone driver; distinct EEPROM layout.
- BTECH UV-PRO — native BLE Benshi command/region protocol, also used by the
  related VR-N76 family.

This programs **channel memories, not radio MCU firmware images**.

## Capability and validation status

| Capability | Implemented in source | Validated on user's radios |
|---|---|---|
| One canonical channel list + editor | Yes | No |
| CHIRP CSV import/export and fleet fingerprinting | Yes | No |
| Register radios, track revision drift | Yes | No |
| TD-BL1 BLE GATT discovery/serial I/O | Yes | No |
| TD-BL1 vendor AES authentication | **No; protocol not verified** | No |
| UV-5R model-specific memory clone via CHIRP | Yes, driver bridge | No |
| UV-5RM model-specific memory clone via CHIRP | Yes, driver bridge | No |
| BTECH UV-PRO Benshi BLE channel read/write | Yes, experimental | No |
| Atomic local backup prior to any write | Yes | No |
| Model-specific write with verification/read-back | Yes | No |
| Automated APK build + JVM unit tests | **Yes, CI passed** | N/A |

**CI passing does not prove a TD-BL1 adapter authenticated or a UV-PRO accepted a
write.** The TD-BL1 requires an ODmaster-compatible AES challenge on some
firmware. Without captured protocol details from the owner's hardware, UV-5R
and UV-5RM connections may reach GATT but fail the initial CHIRP handshake.
The app fails closed and requires a successful read + backup before enabling
writes.

The UV-PRO uses native Benshi BLE characteristics and commands based on
published protocol references; radio firmware differences require live
validation. Android's Bluetooth protocol scanner is included for identifying
and diagnosing real adapters.

## Use

1. Install the persistently signed APK from the
   [radio-fleet-latest release](https://github.com/mharlos/MeshCore/releases/tag/radio-fleet-latest).
2. Open **FARM RADIO FLEET**, review the master frequencies, tones, and TX
   disable settings. Import CHIRP CSV if needed.
3. Tap **READ / WRITE RADIOS OVER BLUETOOTH** and select the exact model.
4. Scan and connect to the TD-BL1 adapter (UV-5R/5RM) or UV-PRO BLE control
   service.
5. **Read radio + save backup.** The backup is written to Android app-private
   storage before Write becomes available. Do not proceed if Read fails.
6. Review the confirmation and choose **Write master channels + verify**.
7. Watch for **VERIFIED**. A success result requires read-back matching the
   master entries, not just a positive Bluetooth acknowledgment.

Each physical radio must be read separately. A raw EEPROM image from a UV-5R
must never be written to a UV-5RM. The app applies model-neutral channel
intent through the correct CHIRP driver and retains unrelated radio settings
from the radio's own source image.

## Source and licensing

The Android app's Kotlin files are original code built using protocol
documentation. For the Benshi transport, the implementation references the
Apache-2.0 protocol work in
[benlink](https://github.com/khusmann/benlink) and
[OpenHT](https://github.com/repins267/repins267-OpenHT).

CHIRP Python sources are bundled at build time from the version vendored in
[RadioDroid](https://github.com/jnyer27/RadioDroid), pinned to commit
`cafbed75a9a044628e8d374bde9c3062a7d469ff`. Those files retain their
upstream GPL notices and COPYING license. Distribution of an APK bundling CHIRP
must comply with the applicable GPL and provide corresponding source.

CI: `.github/workflows/build-radio-fleet-apk.yml`. Kotlin tests and APK
compile run automatically; the most recent successful build is tagged at
`radio-fleet-latest`.

## Permanent Android signing and in-place APK upgrades

Android verifies signing-certificate identity, package name, and increasing
versionCode before an app can be updated in place. Previous APKs carried a new
ephemeral debug certificate on each GitHub-hosted runner, so Android rejected
updates with **APP NOT INSTALLED / UPDATE INCOMPATIBLE**.

The workflow now builds a **persistently signed release APK**, provided
repository Actions secrets are configured. The moving `radio-fleet-latest`
release is updated **only when the stable signing certificate exists**. If
secrets are missing, the CI test and debug APK still build, but GitHub will not
publish another debug-signed APK under the normal download link.

### One-time operator setup on a computer with GitHub CLI, JDK/keytool and OpenSSL

```sh
git clone -b feature/radio-fleet-sync https://github.com/mharlos/MeshCore.git
cd MeshCore
gh auth login
bash apps/radio-fleet/scripts/configure-signing.sh
git commit --allow-empty -m "ci: build signed Radio Fleet APK"
git push origin feature/radio-fleet-sync
```

The script generates one private 3072-bit RSA PKCS#12 key pair at
`~/.config/farm-radio-fleet/release-signing.p12`, saves its password in
`release-signing-password`, and pushes only encrypted GitHub Actions secret
values under `RADIO_FLEET_KEYSTORE_B64` and
`RADIO_FLEET_SIGNING_PASSWORD`.

**Back up the key and password securely.** Never commit either file. GitHub
Actions will restore that same certificate for every release. The workflow
builds and publishes `app-release.apk` and renames it
`farm-radio-fleet.apk` in releases.

**One final uninstall is expected** when moving from the old per-run
debug-signed APK to the first persistently signed release. Preserve your master
channel list by exporting CSV before uninstalling. Subsequent signed APKs use
the same application ID, signing certificate, and increasing run-number
versionCode, supporting normal Android updates without losing app data.

Local debug APKs are CI-only and must never be offered as updates to the
production release. Do not run the signing setup script again after losing
the key: restore the original certificate and password from backup to avoid
breaking the established upgrade chain.

## Field-test requirements

- Capture a read-only TD-BL1 BLE GATT service report from **Inspect TD-BL1
  Bluetooth**. If generic CHIRP read fails, obtain an operator-approved Android
  Bluetooth HCI trace of a **successful** ODmaster read to document the
  authentication frames. Do not share pairing keys or personal data.
- Confirm UV-5R CHIRP read, baseline backup, one test channel write, and a
  matching independent read-back.
- Repeat UV-5RM against its own `5RM` driver.
- Validate UV-PRO BLE discovery, GET_DEV_INFO, group count/channel count,
  full read, one slot write, and per-slot read-back.
- Test interrupted connection, failed write, failed auth, and mixed-model
  inventory; never mark a radio synchronized after a failed operation.

**This branch stays a draft until those real-hardware tests pass.**
