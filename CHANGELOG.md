# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The version matches `fap_version` in `application.fam`.

## [Unreleased]

## [1.1] - 2026-11-19

### Added

- Scan an NFC tag to start an activity: press Down on the tracker screen and
  hold a tag to the Flipper. A new tag is linked to an activity you pick the
  first time it is scanned; after that, scanning it switches to that activity.
  Any ISO14443-3A tag works (NTAG, Mifare Ultralight, Mifare Classic, DESFire).
- "Link NFC tag" and "Unlink NFC tag" in the Edit menu.
- `tags.txt` on the SD card stores the links (`uid,activity`).

### Changed

- Down on the tracker screen now scans an NFC tag instead of opening the
  activity picker (OK and Up still open it).

## [1.0] - 2026-11-18

### Added

- Continuous time tracking: exactly one activity is tracked at a time, and
  switching activity ends the current interval and starts the next.
- Tracker screen with the current activity, live elapsed timer, start time,
  today's total for that activity and a clock.
- Activity picker (OK / Up / Down) with the current activity marked.
- Daily stats screen (Right) showing time per activity with bars, sorted by
  time, with Left / Right to move between days.
- Edit menu (Left) to add or delete activities and to stop tracking.
- Ten default activities, created on first launch.
- SD card storage in `/ext/apps_data/timeto/`:
  - `current.txt` stores the running interval, written on every start or switch,
    so tracking continues after the app is closed or the Flipper is restarted.
  - `log.csv` gets a row for every finished interval
    (`start_ts,end_ts,duration_s,start,end,activity`).
  - `activities.txt` holds the activity list, one per line, and can be edited
    on a computer.
- GitHub Actions workflow that builds the `.fap` against the release and dev SDKs.

[Unreleased]: https://github.com/daniebker/flipper_timeto/compare/v1.1...HEAD
[1.1]: https://github.com/daniebker/flipper_timeto/compare/v1.0...v1.1
[1.0]: https://github.com/daniebker/flipper_timeto/releases/tag/v1.0
