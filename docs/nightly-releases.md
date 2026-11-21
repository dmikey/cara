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

Use dedicated automation signing material and keep it in Actions secrets.
Private keys and PKCS#12 files stay in temporary runner storage. A public
IPA contains the App Store provisioning profile used for its signature.
The workflow rejects development, ad hoc and enterprise profiles to
avoid exposing registered device identifiers or using enterprise signing
for a public download. The workflow does not submit the IPA to App Store
Connect or publish it to TestFlight.

The current GitHub build uses the source in `main`. Local device-test
builds and their private shader catalogs are outside this workflow.
