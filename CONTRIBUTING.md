# Contributing

Thanks for helping improve ESP Thread Probe (`esp-thread-probe`).

## Local Checks

```bash
./tools/check_project.sh
./scripts/build.sh
```

For ESP-IDF 6.x, CMake downloads managed dependencies through the IDF Component
Manager. Do not commit `managed_components/`; put local changes in
`patches/managed_components` and verify them with:

```bash
scripts/apply_managed_component_patches.py --project . --check
```

## Pull Requests

- Keep endpoint response fields backward compatible when possible.
- Document new public APIs in `docs/api.md`.
- Keep Matter-specific code inside `components/probe_matter`.
- Keep network secrets out of commits.
- Keep patches small and remove them when the upstream component releases the fix.
