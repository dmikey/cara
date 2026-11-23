# iOS attribution and source distribution

Settings → About Cara provides offline credits and acknowledgements, the full
GPLv2 text, and bundled third-party license notices. Cara is licensed under
GPL-2.0-or-later; upstream copyrights remain with their contributors.
The notices distinguish development reference projects from bundled code.

`tools/bundle_ios_licenses.py` collects the repository LICENSE and NOTICE,
dependency license/copyright/notice files, and menu-sound notices. Both the
CMake iOS app and the UIKit preview include these resources in `Legal/`.
The reader supports selection, scrolling, Dynamic Type and safe-area layout.

The About screen identifies the source commit and links to its repository tree
and source archive. Modified local builds are labeled and cannot offer the
unmodified commit archive as their matching download. Their local changes must
also accompany any distribution. The IPA release packager rejects absent legal
resources, a modified source identity, or a revision different from its audit.

Before distributing a binary, verify that its exact corresponding source is
available to recipients, including modifications, bundled dependency source,
and build scripts. Verify that the linked commit/archive is publicly accessible
and retain the matching source for every distributed release. Never represent
an unpublished local commit as publicly downloadable source.

Apple App Review guideline 5.2 and the GNU GPL are separate requirements.
An About screen does not by itself resolve GPL source-distribution obligations
or compatibility between distribution terms/DRM and GPL recipient rights.
Review the actual intended distribution terms before App Store submission;
these UI changes are not a legal compliance certification.

References:

- [GNU GPL v2, including sections 1–3 and 6](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)
- [GNU source distribution FAQ](https://www.gnu.org/licenses/gpl-faq.html#SourceAndBinaryOnDifferentSites)
- [Apple intellectual property review rules](https://developer.apple.com/app-store/review/guidelines/#intellectual-property)

## Validation

Run `python3 -m unittest discover -s tests -p test_release_packages.py`.
The suite checks notice inclusion, missing-license rejection, and rejection of
modified source metadata for releases, alongside existing bundle policy tests.

Build and run the ROM-free UIKit suite with a booted iPad simulator:

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  python3 tools/test_ios_library_ui.py --device SIMULATOR_UDID --output work/ios-ui
```

Also inspect About Cara in portrait and landscape with large accessibility text,
open each offline document without networking, navigate back to Settings, and
verify the source links against the release revision. Check library import,
controller/touch input, save/load, rotation, and background/resume on a physical
iPad before claiming frontend acceptance. These runtime checks have not been
performed for this change. Xcode 27.2 and iOS SDKs are now installed; the
UIKit Simulator fixture compiles, but no iOS Simulator runtime is installed.

## App Store audit status (November 20, 2026)

The publisher confirmed App Store distribution and a paid ElevenLabs account.
`ios-asset-rights.json` records that attestation and identifies the sound files
by hash. It does not claim invoice or generation-date verification. Preserve
those records and the applicable generation-time plan terms privately. Paid
commercial-use permission is not automatically a grant of GPL rights in audio;
treat the separately generated media according to its applicable terms.
The Carbon Duo generator describes its artwork as original Cara work.

The release workflow now attaches a clean-checkout source archive, including
tracked dependencies and build scripts, to regular and signed nightly releases.
The archive receipt records the exact commit and hash. This must still be
verified after an actual release; workflow source changes are not proof that an
artifact has been published. App Store metadata must also direct recipients to
the corresponding source for the version being distributed.

**App Store legal acceptance remains unresolved.** Apple's standard EULA limits
redistribution, with an open-source carve-out for some restrictions. GPLv2
section 6 prohibits further restrictions on recipients' exercise of GPL rights.
The FSF has challenged GPLv2 distribution through Apple's store in the past.
Review the actual current Apple agreements, Usage Rules, DRM, and selected EULA
with qualified licensing counsel before submission. A custom EULA alone must
not be represented as resolving platform-level restrictions. Cara cannot grant
exceptions for copyrights belonging to Citra/MMJ/Azahar contributors.

- [Apple standard EULA](https://www.apple.com/legal/internet-services/itunes/dev/stdeula/)
- [FSF account of the VLC enforcement dispute (historical)](https://www.fsf.org/blogs/licensing/vlc-enforcement)
- [ElevenLabs creative terms](https://elevenlabs.io/creative-terms)

The generic arm64 GLES3 app links against the iPhoneOS 27.2 SDK and passes the
no-JIT linkage audit. The unsigned local bundle includes the license, notice,
third-party acknowledgements and source metadata. The IPA packager enforces
clean source identity and rejects an app built from a modified worktree.
Legal acceptance, offline notice navigation, accessibility, physical-device
acceptance and live public source access still require verification. The UIKit
Simulator fixture compiles, and demapping input/schema checks have run with
Mac Catalyst.

The UIKit suite now includes About document navigation/back actions and library
landscape navigation. These authored checks still require execution on an iPad
simulator with the generated Legal bundle.

The November 20 Metal mainline (`f038ed4`) adds fetched, pinned glslang and
SPIRV-Cross compiler sources. The iOS notice collector accepts these external
source directories so their license texts are also readable from About. The
source archive job currently exports tracked sources: it is sufficient only for
configurations whose dependencies are all tracked (the existing GLES release).
Before distributing a native Metal binary, also archive the exact fetched shader
compiler sources and review their licenses, including SPIRV-Cross's Apache-2.0
terms, against the selected GPL version. The native Metal developer artifact is
not covered by a claim of full distribution compliance.
