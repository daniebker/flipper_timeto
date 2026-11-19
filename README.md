# TimeTo for Flipper Zero

A continuous time tracker for the Flipper Zero, inspired by [timeto.me](https://github.com/Medvedev91/timeto.me).

You always track exactly one activity. When you switch to another one, the
current interval ends and the next one starts. There is no "pause": the time
until you switch belongs to the activity you are on. There are no tasks or
checklists, just activities and time.

## Controls

| Screen   | Button        | Action                                      |
|----------|---------------|---------------------------------------------|
| Tracker  | OK / Up       | Switch activity                              |
| Tracker  | Down          | Scan an NFC tag to switch activity           |
| Tracker  | Right         | Stats (time per activity, per day)           |
| Tracker  | Left          | Edit: start time, activities, NFC tags, stop |
| Tracker  | Back          | Exit (tracking keeps running)                |
| Stats    | Left / Right  | Previous / next day                          |
| Stats    | Up / Down     | Scroll                                       |
| Start time | Left / Right | Pick hours or minutes                       |
| Start time | Up / Down    | Change the time (hold to repeat)            |
| Start time | OK / Back    | Save / cancel                               |

The tracker screen shows the current activity, how long you've been on it, when
it started, and the total for that activity today.

## Changing the start time

Switched late? Under **Edit** (Left) choose **Change start time** to set when
the current activity really started. The screen shows the new duration of the
current activity and, below it, the activity before it.

If the previous activity ended when the current one started (you switched
from it), its end time moves with the start time, so the timeline has no gaps
or overlaps. The last row of `log.csv` is updated. Otherwise the start time
can't be earlier than the end of the last logged interval. It can't be later
than now.

## NFC tags

You can stick NFC tags around the place (on your desk, by the door, on the
gym bag) and start an activity by scanning one.

1. On the tracker screen press **Down**, then hold a tag to the back of the
   Flipper.
2. The first time you scan a tag, pick the activity to link it to. Tracking
   switches to that activity straight away.
3. After that, scanning the tag switches to its activity. If you are already
   on that activity, the Flipper just vibrates.

Under **Edit** (Left) you can **Link NFC tag** to change a tag's activity, or
**Unlink NFC tag** to forget it. If you delete an activity, its tags ask for a
new activity the next time you scan them.

Any ISO14443-3A tag works: NTAG213/215/216 stickers, Mifare Ultralight,
Mifare Classic, DESFire, and most transit and access cards. The tag is
identified only by its UID, so nothing is written to it. The app can only scan
while it is open.

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
- `tags.txt`: linked NFC tags, one per line as `uid,activity`
  (e.g. `04A1B2C3D4E580,Work`).

Times come from the Flipper's clock (local time), so set the clock correctly.

## Building

```bash
pip install ufbt
ufbt          # builds dist/timeto.fap
ufbt launch   # builds, installs and runs on a connected Flipper
```

GitHub Actions builds the `.fap` against the release and dev SDKs on every push.
The builds are available as workflow artifacts.
