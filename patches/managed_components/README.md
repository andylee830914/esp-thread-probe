# Managed Component Patches

ESP-IDF downloads third-party dependencies into `managed_components/`.
Those files are generated build inputs and are not committed.

Local compatibility fixes live here as small unified patches. The recommended
way to build the ESP32-C6 firmware is the repository helper:

```bash
./scripts/build.sh
```

The helper lets ESP-IDF Component Manager fetch `managed_components/`, applies
these patches, then performs the real ESP32-C6 build. On a clean checkout, the
first dependency resolve can fail during configure because upstream components
have not been patched yet; the helper handles that case and retries after
patching.

For manual debugging, run from the repository root:

```bash
idf.py update-dependencies || true
scripts/apply_managed_component_patches.py
rm -rf build
idf.py set-target esp32c6
idf.py build
```

```bash
scripts/apply_managed_component_patches.py --check
```

Patch files are named after the managed component directory they modify.
Patches for components that are not present in the selected project are skipped.
Keep patches narrow and document why they exist in the patch context or commit
message so they can be removed when upstream releases include the fix.
