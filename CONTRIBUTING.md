# Contributing

Thanks for helping improve ESP Thread Probe (`esp-thread-probe`).

## Local Checks

```bash
./tools/check_project.sh
./scripts/build.sh
```

For ESP-IDF 6.x, CMake downloads managed dependencies through the IDF Component
Manager. Do not commit `managed_components/`, build directories, local
`sdkconfig`, or dependency lock files generated during local builds.

## Pull Requests

- Keep endpoint response fields backward compatible when possible.
- Document new public APIs in `docs/api.md`.
- Keep Matter-specific code inside `components/probe_matter`.
- Keep network secrets out of commits.
