# Cheats in Motion (`opencim.cheats`)

A built-in OpenCiM mod: a cheat window for the game. It ships with the port in `data/mods/cheats` and is **disabled
by default**.

## Enable

Add its id to `mods.json` in the user mods folder (`Documents\OpenCiM\mods\mods.json`; create the file if missing):

```json
{ "enabled": ["opencim.cheats"] }
```

With the weather mod as well: `{ "enabled": ["opencim.weather", "opencim.cheats"] }`. It takes effect at the next
start. To turn it off, remove the id (or start with `--no-mods`).

## Use

In a game (campaign, scenario or sandbox; not in the editor) the left bar has one more button, a magic wand between
the roster and the settings wheel. It opens the cheat window where the HQ, vehicle and other left panels open; like
them it closes with its button, its close button, or when another left panel opens. Every button has a tooltip that
says what it does; the last action shows at the bottom of the window (and in the log as `cheats: ...`).

| Section | Button | What it does |
|---|---|---|
| Money | +10,000 / +100,000 / +1 million | adds the amount (`earnMoney(amount, false)`, as the original's debug key F3, but not counted as sales income) |
| | amount field and *Add* | any amount up to 20 million, digits only, Enter or the button |
| | *Unlimited money* | keeps the account at the original's cap of 20 million (2 000 000 000 cents), where spending takes nothing; topped up again after the monthly accounts. Saved with the game; switched off, the balance from before it was switched on comes back (at most 19 million: an account just under the cap would be at the cap again with the next fare) |
| Time | +1 day / +1 week / +1 month | skips ahead one day per frame (as the original's debug key F1), to the first of the next month for *+1 month*. Every day's events run: dated tasks, the map's timeline, the monthly accounts, the annual report. Nobody travels on the skipped days, so there are costs but no fare income |
| | -1 day / -1 week / *1st of month* | turns the clock back, **within the current month only** (see below) |
| Company | *Complete* | completes the first open task or scenario objective with its reward (the original's debug key F2); the line beside it names it |
| | *Max reputation* | every passenger group fully satisfied: the bonus a task reward gives (`extraHappiness`) is raised so each group is at the top with a year's margin; it fades by 0.02 a month like any reward |
| | *Pay off loans* | removes all running loans without paying them |
| | *All vehicles* | every vehicle model in the shop at once, future and retired ones too (models a scenario locks with `"disabled"` stay locked) |

Achievements the original unlocks on money or reputation thresholds unlock as they would without the mod.

## Turning the clock back

The game's clock fires `onMonth` and `onYear` when the month or year of its date changes, in either direction, and
the monthly accounts (wages, loan payments, histories for the graphs) run in `onMonth`. Going back across a month
would run them again, so the rewind stops at the first of the current month; within the month only `onDay` fires.

What already happened on the days between the new date and the latest date reached does not happen again:

* the campaign scenarios start their tasks on exact dates (`dateChanged`: `if ($date == mkdate(12,2,1923)) ...`);
  the mod's extension of `metro/scripts/scenario.script` does not pass `dateChanged` on for those days;
* the map's timeline (historic buildings and roads by date) keeps its day: game.script's `onDay` sets
  `$timeline.currentDay` from the date and would replay the days again; the mod's `onDay` handler (bound after
  game.script's) puts it back.

The latest date is kept in the scenario's `customData` (`opencimCheatsLatestDate`, saved with the game, unknown to
the scenario scripts) until the clock passes it again; a save made in between keeps all of this. Threads that wait
for a date (`waitForDate`, task deadlines, petitions that expire) simply wait the extra days.

## Files

| File | Purpose |
|---|---|
| `mod.json` | the manifest |
| `extend/metro/scripts/game.script` | adds `scripts/cheats.script` to the game's `$gameScripts` (loaded and unloaded with the game) |
| `extend/metro/ui/levelbar/levelbar.script` | the wand button in the left bar (the bar grows by one button), the cheat window as one of the left panels |
| `extend/metro/scripts/scenario.script` | `dateChanged` held back for the days already passed before a rewind |
| `scripts/cheats.script` | the window and every cheat; an empty `updateERA` (game.script calls it on every game script at each new year) |
| `strings/de.strings`, `strings/en_US.strings` | the texts (English for every other language) |
| `textures/cheats*.dds` | the button (normal, selected, hover), drawn by `port/tools/make-cheats-icon.py` |

Test switch: `REGS_TEST_CHEATS=log` logs where the button and the window are on the desktop (for scripted clicks),
`REGS_TEST_CHEATS=open` also opens the window at the start.
