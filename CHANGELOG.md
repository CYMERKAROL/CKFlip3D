# Changelog

## 1.0

- First public release
- Licensed under the PolyForm Noncommercial License 1.0.0
- Auto performance tune reworked: two-way quality ladder with hysteresis and a 60 Hz budget floor — fixes live previews silently freezing on mid-range GPUs and high-refresh (120/144 Hz) displays; quality now recovers automatically when headroom returns
- Enabled D3D11 multithread protection for the shared capture/render device — fixes wedged capture sessions on slower GPUs and virtual machines

## 0.95V Beta

- Critical error fixing & preparation for release
- Stability hardening: tray-icon logon-race retry, settings-app reload/restart handshake, capture teardown fixes
- Repository styling: full README, demo media (`assets/Repo`), published build scripts

## 0.91V Beta

- Codebase cleanup (legacy DWM-thumbnail stack removed)
- Initial public repository

## 0.9V Beta

- Close animation — windows closed while the cascade is open fade out while the stack reflows
- Settings improvements (per-animation toggles, background opacity)

## 0.8V Beta

- Installer (single-file setup with rollback & .NET runtime bootstrap)
- Major improvements across capture and rendering

## Earlier (Alpha)

- 0.75V: settings menu revamp
- 0.65V: full multi-monitor support
- 0.5V: first stable cascade (tilt fix, DWM exit-leak fix)
