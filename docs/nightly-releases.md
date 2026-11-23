# Signed nightly releases

The Release packages action builds both products from the same commit. A
scheduled run starts daily at 08:23 UTC. To start one manually, select
**Release packages**, choose **main**, enable **Build signed prerelease
packages from main**, and leave the existing release tag blank.

The signed products are a self-contained Apple Silicon Mac build with
Developer ID signing and notarization, and an arm64 iOS GLES3 IPA with
App Store distribution signing. The Mac build includes optional CPU JIT
and defaults to FastInterp. Shader JIT is disabled on both platforms,
and the iOS binary contains no CPU JIT. The IPA is intended for App
Store Connect distribution through Apple. The existing unsigned IPA
workflow artifact remains available for users who sign for their devices.

The nightly uploads a GitHub prerelease only after both signed products
pass verification. It includes signing and source receipts plus
`SHA256SUMS.txt`. The nightly never replaces the latest stable release.
Builds without both sets of signing credentials retain their unsigned
workflow artifacts and identify missing secret names in the run summary.

## Release candidates

Use `Cara.RC.YYYYMMDDNN` for the GitHub prerelease title and tag. Use the UTC
release date and a two-digit sequence starting at `01` for that day. For
example, the first candidate on November 23, 2026 is `Cara.RC.2026112301`.

Dispatch **Release packages** on **main** with the **RC version** input
set to this tag. Leave both the nightly switch and the existing release
tag blank. Each run pins one source commit for all three platforms and
publishes a numbered prerelease after verification. The candidate contains:

- A Developer ID-signed and notarized Apple Silicon Mac ZIP.
- An unsigned arm64 GLES3 IPA for personal sideloading. Re-sign it before
  installing. TestFlight remains a separate Apple distribution channel.
- An ARM64 Android APK and a Play Console AAB signed with the configured
  upload key. Users with a Play installation should update through the Play
  testing track because Play may sign installs with a different key.
- Build receipts and `SHA256SUMS.txt`.

An existing tag or release blocks reuse of the version. If the Mac or Android
signing configuration is unavailable, the build artifacts remain in Actions
and no numbered release is published. Stable releases and scheduled nightlies
retain their existing tags and package policies.

The landing page links to the newest published numbered candidate with its
Mac ZIP, unsigned IPA, and checksums. It shows the Android candidate APK only
when that release includes the signed APK and its receipt. The initial
numbered candidate and the older experimental Android download serve as
explicit fallbacks when the public release listing is unavailable.

## Signing environment

Before adding secrets, create a GitHub Actions environment named
`nightly-signing` under **Settings → Environments**. Select the
deployment rules that allow only the `main` branch. The package build
jobs need no secrets. Only the signing job uses this environment.

Add the following as **environment secrets**. Avoid repository-level
signing secrets, which could be used by another branch's workflow.

| Secret | Value |
| --- | --- |
| `CARA_MAC_CERTIFICATE_BASE64` | Base64-encoded Developer ID Application PKCS#12 export issued for automated releases |
| `CARA_MAC_CERTIFICATE_PASSWORD` | Password for that PKCS#12 export |
| `CARA_NOTARY_KEY_BASE64` | Base64-encoded App Store Connect team API private key used by the notary service |
| `CARA_NOTARY_KEY_ID` | App Store Connect API key ID |
| `CARA_NOTARY_ISSUER_ID` | App Store Connect API issuer ID |
| `CARA_IOS_CERTIFICATE_BASE64` | Base64-encoded Apple Distribution PKCS#12 export for the iOS build |
| `CARA_IOS_CERTIFICATE_PASSWORD` | Password for that PKCS#12 export |
| `CARA_IOS_APP_STORE_PROFILE_BASE64` | Base64-encoded iOS App Store distribution provisioning profile for `com.dmikey.cara` that includes the distribution certificate |
| `CARA_ANDROID_KEYSTORE_BASE64` | Base64-encoded Android upload keystore for `org.cara.emu` |
| `CARA_ANDROID_KEYSTORE_PASSWORD` | Android upload keystore password |
| `CARA_ANDROID_KEY_ALIAS` | Android upload key alias |
| `CARA_ANDROID_KEY_PASSWORD` | Android upload key password |

Set the `nightly-signing` environment variable
`CARA_ANDROID_UPLOAD_CERT_SHA256` to the expected upload certificate
fingerprint from Play Console. Build version codes use the release workflow run
number plus the optional repository variable `CARA_ANDROID_VERSION_CODE_BASE`.
Set the offset so new codes exceed the highest version already uploaded to
Play Console.


Use dedicated automation signing material and keep it in Actions secrets.
Private keys and PKCS#12 files stay in temporary runner storage. A public
IPA contains the App Store provisioning profile used for its signature.
The workflow rejects development, ad hoc and enterprise profiles to
avoid exposing registered device identifiers or using enterprise signing
for a public download. The workflow does not submit the IPA to App Store
Connect or publish it to TestFlight.

The current GitHub build uses the source in `main`. Local device-test
builds and their private shader catalogs are outside this workflow.
