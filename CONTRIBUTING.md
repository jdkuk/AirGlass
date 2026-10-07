# Contributing to AirGlass

Thanks for helping! AirGlass is a small, focused app, so the most valuable contributions are:

* **Device reports:** "works / doesn't work on iPhone 15 with iOS 26.x, Windows 11 24H2, Wi-Fi 6 router". Open an issue
  with the *Device report* label, or post in Discussions.
* **Bug reports** with a log (see below).
* **Pull requests** that fix bugs or improve quality, latency or compatibility.

For bigger features, please open an issue first so we can agree on the approach before you invest time.

## Reporting a bug

Use the [bug report form](https://github.com/jdkuk/AirGlass/issues/new?template=bug_report.yml). The most useful
attachment is `%LOCALAPPDATA%\AirGlass\airglass.log` with `debuglog=1` set under `[app]` in `config.ini`. Remove anything
you consider private first: device names and local IP addresses appear in the log.

## Development setup

```powershell
powershell -ExecutionPolicy Bypass -File tools\fetch-deps.ps1
python build.py
```

See [docs/building.md](docs/building.md) and [docs/development.md](docs/development.md). Everything can be tested on one
PC with the loopback sender, and headless, so no window appears.

## Pull requests

1. Fork, then create a branch from `main` (`fix/…`, `feat/…`).
2. Keep changes focused. Match the surrounding style (see *Code style* in [development.md](docs/development.md)).
3. Make sure that:
   * `python build.py` is warning-free
   * `airglass_test resampler`, the bplist cross-check and `AirGlass.exe --selftest` pass
   * if you touched networking or media, the loopback `stream` and `music` tests pass
4. Update the docs and add a line under *Unreleased* in [CHANGELOG.md](CHANGELOG.md).
5. Open the PR and fill in the template. CI builds and tests every PR on Windows.

## Licensing of contributions

AirGlass is GPL-3.0. By contributing you agree that your contribution is licensed under the GPL-3.0.

## Ground rules

* AirGlass talks AirPlay as documented by the open-source community. We don't accept code that circumvents DRM
  (FairPlay *streaming* protection, HDCP or similar).
* Please be kind. See the [Code of Conduct](CODE_OF_CONDUCT.md).
