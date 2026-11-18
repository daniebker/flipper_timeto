# TimeTo for Flipper Zero

A continuous time tracker for the Flipper Zero, inspired by [timeto.me](https://github.com/Medvedev91/timeto.me).

You always track exactly one activity. When you switch to another one, the
current interval ends and the next one starts. There is no "pause": the time
until you switch belongs to the activity you are on. There are no tasks or
checklists, just activities and time.

## Controls

| Screen   | Button        | Action                                      |
|----------|---------------|---------------------------------------------|
| Tracker  | OK / Up / Down| Switch activity                              |
| Tracker  | Right         | Stats (time per activity, per day)           |
| Tracker  | Left          | Edit: add / delete activities, stop tracking |
| Tracker  | Back          | Exit (tracking keeps running)                |
| Stats    | Left / Right  | Previous / next day                          |
| Stats    | Up / Down     | Scroll                                       |

The tracker screen shows the current activity, how long you've been on it, when
it started, and the total for that activity today.

## Saving to the SD card

Everything goes in `/ext/apps_data/timeto/`:

- `current.txt`: the running interval (`start_ts,activity`). Written each time
  you start or switch, so tracking continues after you close the app or restart
  the Flipper.
- `log.csv`: one row is added for every finished interval:

  ```
  start_ts,end_ts,duration_s,start,end,activity
  1763456400,1763460000,3600,2025-11-18 09:00:00,2025-11-18 10:00:00,Work
  ```

- `activities.txt`: one activity per line. You can edit it on a computer or in
  the app. If it's missing, a default list is created.

Times come from the Flipper's clock (local time), so set the clock correctly.

## Building

```bash
pip install ufbt
ufbt          # builds dist/timeto.fap
ufbt launch   # builds, installs and runs on a connected Flipper
```

GitHub Actions builds the `.fap` against the release and dev SDKs on every push.
The builds are available as workflow artifacts.
