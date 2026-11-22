# Signed nightly releases

The Release packages action builds all products from one pinned `main`
commit. A scheduled run starts daily at 08:23 UTC. To start one manually,
select **Release packages**, choose **main**, enable **Build signed
prerelease packages from main**, and leave the existing release tag blank.

| Platform | GitHub nightly package |
| --- | --- |
| macOS arm64 | Self-contained Cara app with Developer ID signing and notarization. Optional CPU JIT, with shader JIT disabled. |
| iOS arm64 | GLES3 IPA built with the iOS 26 SDK or newer, with App Store distribution signing and both JITs disabled. App Store Connect handles distribution to devices. |
| Android arm64 | GLES atlas APK for direct installation and AAB for Play Console, both signed with the configured upload key. CPU JIT is included. |
| Linux x86_64 | Unsigned desktop OpenGL tarball. Built on Ubuntu 24.04 with FastInterp. |
| Windows x86_64 | Unsigned desktop OpenGL ZIP with SDL2. Requires the Microsoft Visual C++ x64 runtime and an OpenGL-capable driver. |

The action publishes a GitHub prerelease after all five packages pass
verification. It attaches build and signing receipts, `SHA256SUMS.txt`,
and a link to the corresponding source. The stable release keeps its
latest status. If credentials are missing, the run summary lists the
missing secret names and the build artifacts remain available in
GitHub Actions. Apple and Android store submissions use their respective
consoles.

## Signing environment

Before adding secrets, create a GitHub Actions environment named
`nightly-signing` under **Settings → Environments**. Select the
deployment rules that allow only the `main` branch. The package build
jobs need no secrets. Only the Apple and Android signing jobs use this environment.

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
| `CARA_ANDROID_KEYSTORE_BASE64` | Base64-encoded Android upload keystore registered for `org.cara.emu` in Play Console |
| `CARA_ANDROID_KEYSTORE_PASSWORD` | Upload keystore password |
| `CARA_ANDROID_KEY_ALIAS` | Alias of the registered upload key |
| `CARA_ANDROID_KEY_PASSWORD` | Password for that key (same as the store password for a PKCS#12 keystore) |

Use dedicated automation signing material and keep it in Actions secrets.
Private keys, PKCS#12 bundles, and Android keystores stay in temporary runner storage. A public
IPA contains the App Store provisioning profile used for its signature.
The workflow rejects development, ad hoc and enterprise profiles to
avoid exposing registered device identifiers or using enterprise signing
for a public download. The workflow does not submit the IPA to App Store
Connect or publish it to TestFlight.

## Android Play Console

Register the Android **upload certificate** for `org.cara.emu` in Play
Console. Use the matching upload key for the signing secrets. Play App
Signing uses its app signing key on APKs it serves. If that key differs
from the upload key, a GitHub APK and Play installation cannot update
each other in place.

The Android release build targets API 36. Before the first Play upload,
set `CARA_ANDROID_VERSION_CODE_BASE` as an Actions repository variable if
needed. The built version code is the base plus the **Release packages**
run number. Choose a base that puts the next nightly above every version
code previously uploaded for the app. Keep it fixed across subsequent
runs. Upload the signed AAB to the chosen track through Play Console.

The current GitHub build uses the source in `main`. Local device-test
builds and their private shader catalogs are outside this workflow.
