# Signed Cara packages

The **Release packages** workflow builds and verifies macOS, iOS and Android
packages. A signing run publishes assets only when all three platforms pass
signing verification and the receipts identify the same source commit. Signing
runs fail early if the configuration is incomplete. Push and pull-request
builds keep their unsigned or internally signed test artifacts in Actions.

## Build channels

| Channel | Start | Result |
| --- | --- | --- |
| Nightly | Daily at 08:23 UTC, or dispatch from `main` with **nightly** enabled | A verified GitHub prerelease. It leaves the latest stable release in place. |
| Release candidate | Create a `vMAJOR.MINOR.PATCH-rc.NUMBER` tag on a commit in `main`. Dispatch from `main` with that tag and **nightly** off. | A GitHub draft prerelease with signed assets. Publish after testing. |
| Stable | Create a `vMAJOR.MINOR.PATCH` tag on a commit in `main`. Dispatch from `main` with that tag and **nightly** off. | A GitHub draft release with signed assets. Publish after reviewing the final binaries. |

Tagged signing resolves the exact tag commit and checks that it is in the
history of `main`. An existing release is left alone. Keep release tags
immutable and use a new tag for a new build. GitHub releases include
`SHA256SUMS.txt` and receipts. A stable tag creates a new build. To certify
the same bytes testers used before public release, test the final draft's
assets before publishing it.

## What testers install

- **Mac** gets a Developer ID-signed, notarized Apple Silicon app, with optional
  CPU JIT and a bundled dependency closure.
- **iPhone and iPad** get the Apple Distribution IPA through App Store Connect.
  Upload the verified IPA with Transporter, wait for processing, then assign the
  build to a TestFlight group. The GitHub IPA is the App Store Connect input.
  Cara builds it with GLES3, FastInterp, and CPU and shader JIT disabled.
  [Apple upload instructions](https://developer.apple.com/help/app-store-connect/manage-builds/upload-builds/)
- **Android** gets a directly installable signed APK and an upload-key-signed
  AAB for Play Console. Use the AAB in the **Internal testing** track for a Play
  install. If Play uses a separate app-signing key, its installed copy cannot be
  updated using the GitHub APK signed by the upload key. Avoid uninstalling a
  tester's existing app to switch channels without first backing up local data.
  [Play App Signing](https://developer.android.com/studio/publish/app-signing)

The workflow prepares GitHub assets. Store uploads, assignment to testing
groups, and public store rollout are reviewed in the respective consoles.

## Set up the protected signing environment

Use the GitHub environment `nightly-signing` and allow only the `main` branch.
Add the following as **environment secrets**. Enter values directly in GitHub
Settings or through an authorized local `gh secret set --env nightly-signing`
session. Keep private key material out of source files, chat and command logs.

| Secret | Value |
| --- | --- |
| `CARA_MAC_CERTIFICATE_BASE64` | Base64 Developer ID Application PKCS#12 export |
| `CARA_MAC_CERTIFICATE_PASSWORD` | Mac PKCS#12 password |
| `CARA_NOTARY_KEY_BASE64` | Base64 App Store Connect team API key for notarization |
| `CARA_NOTARY_KEY_ID` | Notarization API key ID |
| `CARA_NOTARY_ISSUER_ID` | App Store Connect API issuer ID |
| `CARA_IOS_CERTIFICATE_BASE64` | Base64 Apple Distribution PKCS#12 export |
| `CARA_IOS_CERTIFICATE_PASSWORD` | iOS PKCS#12 password |
| `CARA_IOS_APP_STORE_PROFILE_BASE64` | Base64 App Store distribution profile for `com.dmikey.cara` with the matching certificate |
| `CARA_ANDROID_KEYSTORE_BASE64` | Base64 Play upload keystore for `org.cara.emu` |
| `CARA_ANDROID_KEYSTORE_PASSWORD` | Play upload keystore password |
| `CARA_ANDROID_KEY_ALIAS` | Play upload key alias |
| `CARA_ANDROID_KEY_PASSWORD` | Play upload key password |

Set `CARA_ANDROID_UPLOAD_CERT_SHA256` as an **environment variable** using the
upload certificate SHA-256 from Play Console. This is a public fingerprint
and binds the signed APK and AAB to the expected certificate. The Android build
uses the workflow run number plus an optional repository variable
`CARA_ANDROID_VERSION_CODE_BASE`. Choose an offset that makes new signed
version codes exceed any already uploaded to Play Console.

The signing runners keep private material in temporary storage. The iOS signing
tool rejects development, ad hoc and enterprise profiles. GitHub CI can test
Android packaging with a disposable keystore. That test leaves distribution
secrets unused.

## Upload an iOS build for TestFlight

From **Release packages → Run workflow**, choose `main`, enable
**testflight**, leave **nightly** off and leave the release tag blank. This
route builds and signs only iOS, so it can run while Mac and Android signing
are still being configured. It preserves the signed IPA in Actions, verifies
the bundle ID and source receipt and submits the IPA to App Store Connect.

Set the three `CARA_IOS_*` secrets and these App Store Connect upload credentials
in the protected environment:

| Name | Value |
| --- | --- |
| `CARA_ASC_UPLOAD_KEY_BASE64` | Base64 App Store Connect team API key for TestFlight uploads |
| `CARA_ASC_UPLOAD_KEY_ID` | TestFlight upload API key ID |
| `CARA_ASC_UPLOAD_ISSUER_ID` | App Store Connect API issuer ID |

Mac notarization uses its own `CARA_NOTARY_*` credentials. A successful upload
moves the build to Apple processing. Assign it to a TestFlight group after
processing and review any required export or beta questions in App Store Connect.
