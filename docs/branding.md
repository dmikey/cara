# Cara source identity

Cara owns its product branding and the Cara-specific implementations and changes
identified in `NOTICE`. Current application names, build targets, frontend source
paths, package identifiers, shader identifiers, logging and artwork use Cara.

Original copyright headers, license texts, upstream repository/revision records
and factual comparisons retain their original names. Cara's GPL-2.0-or-later
license continues to apply; renaming a fork does not make the inherited engine
proprietary or remove another contributor's rights.

Some old strings remain solely for compatibility: existing cheat markers, trace
file magic, multiplayer authentication issuers, and fallback reads of old account
settings or Android data folders. New product names and settings use Cara. These
exceptions are documented in the source and checked separately from branding.
The iOS app identifier and its existing save folder remain `com.dmikey.cara` and
`Documents/cara`.

Run `python3 tools/audit_branding.py` to check the shipping engine and frontend
source. GitHub runs this alongside bundled-source validation and the no-JIT builds.
