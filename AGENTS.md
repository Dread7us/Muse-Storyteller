# Muse Storyteller maintenance

- Increment `MUSE_STORYTELLER_VERSION` in the edition header for every firmware update
  shipped to a device. Patch releases cover fixes; minor releases cover features.
- Keep the README and CHANGELOG aligned. Test-only rebuilds do not need a version bump.
- Preserve the separate upstream SDK/OTA build version.
- Follow docs/MAINTAINING.md and docs/VALIDATION.md. State exactly which source, compile
  and physical-device checks passed; do not turn a successful write into a boot claim.
- Never publish account state, credentials, recordings, pairing data or flash backups.
