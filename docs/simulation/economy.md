# Economy, scenarios and the annual report (script side)

The company's books are kept entirely by the game's scripts; the engine only supplies counters and events. This file
writes down the formulas as the scripts compute them, the order they run in each month and year, the natives they
rely on, and what was checked in the port (2026-11-06, `port/docs/playable-todo.md` §5). Money is in cents
everywhere (`$money = 18000*100` is 18 000,00); 2 000 000 000 is the "unlimited" sentinel.

Sources: `metro/scripts/{economy,player,scenario,campaign,game}.script`, `metro/ui/linepanel/linepanel.script`
(fares, breakdowns), `metro/ui/hqpanel/{hqpanel,banks,advertising}.script`, `metro/ui/vehiclepanel/vehiclepanel.script`,
`metro/ui/annualreport/annualreport.script`, `metro/scenarios/*.scenario`, `metro/campaigns/*.campaign`.

## 1. Money flows

| When | What | Booked as | Script |
|---|---|---|---|
| a passenger boards without having paid this line type | fare `ticketPrices[type]`, or with probability `(1.2 − 0.1·h[−7])·cheat[class]` a cheater, caught when `random(0,10) < employeeHappiness[1]` → penalty `15·fare` | `currentProfit[type]` / `currentPenalties`, money at once (`earnMoney(x, false)`) | `linepanel.script:345-406` |
| a vehicle breaks down (`onBroken`) | repair `getPrice(int(price·priceMultiplier/100·vehiclePriceMultiplier[type]·condition·0.1))` | `currentPurchases`, money at once | `linepanel.script:408-421` |
| buying / selling a vehicle | `getPrice(int(price·priceMultiplier/100·vehiclePriceMultiplier[type]))`; sale `getPrice(int(… ·condition·0.9))` | purchases / sales (`currentSales`) | `vehiclepanel.script:192-233` |
| construction (hover tool) | the tool's price | purchases | `hoverinfo.script:480` |
| taking a loan | `+amount` | sales (`earnMoney` default) | `banks.script:138-147` |
| task reward, subvention | `rewardCash`, 3000,00 | sales | `scenario.script:75`, `default.scenario:110` |
| advertising | `(months+2)·monthlyCost`, not a purchase | money only | `advertising.script:185` |
| turn of the month | `vMaintenance + sMaintenance + energy + loans + wages` | money | `player.script:357` |

So between two month ends the money moves by exactly
`Σ monthlyTicketProfit + monthlyPenalties + salesIncome + purchaseExpenses + vMaint + sMaint + energy + loans + wages`
(all values as they stand after the month's `onMonth`), plus advertising.

## 2. The month (`MAIN.$mainClock.onMonth`, `player.script:305-450`)

Order inside the handler:
1. Per line vehicle: condition `c = 0.8·c + 0.2/(1 + travelDistance/(200000·(0.1+0.9·reliability)·(0.1+0.009·maint)))`,
   written to the native `condition`; `attractiveness = vehicle.$attractiveness·c`; monthly income bookkeeping.
2. `updateMonthlyProfit()`:
   * energy per vehicle `−int(int(fuelConsumption·energyMult[type])·fuelPrice) − int(int(electricityConsumption·energyMult[type])·electricityPrice)`
   * vehicle maintenance `−Σ int(maintenanceLevel[type]·price·0.0005)`
   * wages `−n·w0 − ((n+4)/5)·w1 − ((n+2)/3)·w2 − ((n+3)/4)·w3` (integer division; metro trains count twice in n)
   * stop maintenance `−Σ int(maintenanceLevel2[stop.type]·model.$maintenanceCost·0.01)` over non-temporary stops
   * loans `−Σ int(calculateMonthlyPayment(amount, interest, months))`
   * `monthlyExpenses = vMaint + sMaint + energy + loans + wages + purchases + advertising`
   * `monthlyProfit = sales + monthlyPenalties + Σ monthlyTicketProfit`, `totalMonthlyProfit = monthlyProfit + monthlyExpenses`
3. Loans: `amount −= payment − int(amount·interest/1200)`; `months−−`, removed at 0.
4. Stop attractiveness `model.$attractiveness·(1.167 − 0.2/(maintenanceLevel2·0.01 + 0.2))`.
5. `money += monthlyExpenses − purchaseExpenses − advertisingExpenses` (the running costs only).
6. Only now `monthlyTicketProfit = currentProfit` and `monthlyPenalties = currentPenalties`.
7. Statistics (coverage, usage, car usage, histories), `economy.updateEconomy()` (new fuel and electricity prices,
   population, unemployment), employee happiness, reputation, company value.

**Script quirk (same in the original):** step 2 runs before step 6, so `monthlyProfit` and `totalMonthlyProfit` —
the budget panel's "Gesamter Monatsgewinn", the profit history graph and the profit achievements 9 and 10 — use
the *previous* month's fares and penalties together with this month's costs and sales. Seen in the port's Berlin
run: 1933-02 `monthlyProfit 632940` = January's fares 536 040 + January's penalties 96 900. Energy likewise uses
the fuel price set at the end of the month before.

Loan annuity (`hqpanel.script:59-63`): `i = interest/1200`, `payment = (1+i)^n·i/((1+i)^n − 1)·amount`.
Interest offered (`economy.script:41-44`): `int(margin·(1 + 0.1·h[−1])·4·interestMultiplier[bankLevel])·0.25` with
`interestMultiplier = [1.3, 1.3, 1.3, 1.0, 0.5, 0.5, 0.5]`; rewards `int(price·rewardMultiplier[bankLevel])`,
`rewardMultiplier = [0.75, 0.75, 0.75, 1.0, 1.5, 1.5, 1.5]`. Prices follow the economy index:
`getPrice(p) = int(min(2e9, p·(1 + 0.05·h[−1])))` rounded down to 50 000 / 5 000 / 500 / 50 / 10 by size; wages
`getWage(w) = int(w·(1 + 0.05·h[−4]))`.

Economy index `h` (`economy.script:5-31`): a random walk toward `economyTarget`; up-steps `r·|r|` with
`r = random(−0.4, 0.8)`, down-steps with `r = random(−1.333, 0.4)`; reaching the target draws a new one
(`random(−3, 0)` or `random(1, 5)`). Fuel `int(120·(1 + h·random(.03,.08)))`, electricity `int(70·(1 + h·random(.02,.04)))`.

## 3. Reputation and company value

* Employee happiness `clamp(0.5 + (wageLevel[i] − getWage(base[i]))·(i+2)·0.00025, 0, 1)`, base `[1100, 1200, 1100, 1600]`.
* Customer happiness per class: on each fare `h·0.98 + 0.02/(1 + wait·0.01)·employeeHappiness[0]`
  (`linepanel.script:406`); a citizen giving up at a stop (mood 3, `onCitizenMood`) pulls it toward `0.0909`
  (`ui.script:572-576`).
* Reputation per class `clamp(Σ (employeeHappiness[i] − 0.5)·[0.12, 0.12, 0.12, 0.40][i] + customer[c] + extra[c], 0, 1)`;
  total = `Σ·0.14285`; lines get `attractiveness = total`. `extraHappiness` comes from task rewards
  (`rewardReputation·0.01`) and advertising, and decays by 0.02 a month.
* Company value `max(0, money + baseCompanyValue + Σ int(vehicle price·condition) + Σ ads·months·cost·citizens − Σ loan amounts)`.

## 4. The year (`onYear`)

`annualreport.script:234-286`: most / least profitable vehicle (`yearlyProfit`), then
`boardRating = clamp(int((companyValue − lastYearCompanyValue)/(100000 + lastYearCompanyValue^0.6)) + 2, 0, 4)`,
`customerRating = clamp(int(5·yearlyCustomerHappiness), 0, 4)` where `yearlyCustomerHappiness` gains
`Σ customer[c]·0.0119047619` each month (≈ the yearly mean of the 7 classes), and the yearly counters move to the
`lastYear…` fields. `game.script:612` unlocks era stops by year. The report opens from the petition bar.

## 5. Scenarios and objectives

Three layers (scenario, location, `default.scenario`) are called by name (`scenario.script:280-295`). Tasks start
from `dateChanged(date)` (`game.script:695`, called from `onDay`) or threads, appear as petitions
(`addTask`), and on *accept* become objectives (`petitionAccepted` → `addObjective`) whose `checkTaskCondition`
thread tests `condition_<task>` every 1.13 s. On success: `completed_<task>`, `objectiveCompleted` (cash and
reputation reward, ticker petition with the task's `_passed` text, `yearlyTasksCompleted`, `MAIN.$score`).
The scenario ends when its last task calls `MAIN.$scenario.scenarioCompleted()` → `end_scenario` petition →
`MAIN.$campaign.scenarioCompleted()` (next scenario, `unlockScenario`). Sandbox games and every campaign level
draw random tasks (`mainloop`: every 9.51 s a 93/10 000 chance per task id); 18 of them
(`start_sandbox_task76`…`93`) take the petitioner's portrait from `MAIN.$grid.citizens.getCharacter(class)`.
Negative money only brings a subvention offer (`default.scenario:86-110`, bank level ≥ 3); there is no lose
condition. Achievements are unlocked in `MAIN.$score` (`ui/achievement/achievement.script:205-222`) and saved in
`userdata/state.gs`; `awardAchievement` / `awardSteamAchievement` only forward them to the online services.

## 6. Natives the economy and scenario scripts use

Listed by `port/run/campaign/economy-natives.mjs`-style scan of the files above (UI-only natives left out). Port
status as of 2026-11-06:

| Native | Decompile | Port |
|---|---|---|
| `random`, `int`, `float`, `min`, `max`, `clamp`, `abs`, `pow`, `mkdate`, `mktime`, `current_date`, `expandPattern`, `escapePattern`, `array_splice`, `array_erase`, `map_erase`, `vector_length` | `docs/script-runtime/` | implemented (`engine/natives_core.cpp`); `array_splice` in place since 2026-11-05 |
| `gsClock` `date`, `days`, `months`, `years`, `time`, `onDay`, `onMonth`, `onYear` | `0x10073c40` | implemented; events fire synchronously in that order |
| `gsGridCitizens.count`, `getHomeCount`, `getWorkplaceCount`, `getUnemployment`, `get{Walk,Drive,Travel,CanTravel,PriceTooHigh}Count(-1/class)`, `resetTripCounts`, `getRandomCitizen`, `findCitizen` | `docs/simulation/citizens.md` | implemented (`engine/sim_citizens.cpp`) |
| `gsGridCitizens.setPriceSignificance`, `setReasonProbability`, `setUtilProbability`, `joblessRate`, `carMultiplier`, `modelMask`, `addCharacter`, `addCar` | idem | implemented (feed the simulation) |
| `gsGridCitizens.getCharacter(class[, gender])` | `0x100ce7f0`, L:`0x08157710` | **implemented 2026-11-06** (was a stub returning unset → `$portraitNames` error) |
| `gsGridCitizens.clearGameData` | `0x100cfdc0` → `0x100cfce0` | **implemented 2026-11-06** (empties character and car pools) |
| `gsGridCitizens.setClusterData` (scenario 4 only) | `0x100ce8d0` | not checked |
| `gsGridCitizenController.getState`, `getClass`, `usedTransport`, `getStart`, `getTarget`, `getWorkplace`, `getHome`, `getMood`, `getCharacter`, `getCharModel`, `getVehicle`, `getID` | citizens.md §11 | implemented |
| `gsTransportLine.getVehicles`, `attractiveness`, `ticketPrice`, `passengersTransported`, `calculateLength`, `countConnectedStops`, `isClosed`, `stopCount`, `getStop`, `hasTempStops`, `calculateCoverage`, `active`, `type` | transport.md | implemented |
| `gsTransportVehicle.travelDistance` (+6 per path node per leg), `condition`, `attractiveness`, `passengersTransported`, `stopTime`; `onGetPassenger`, `onBroken` | transport.md §4.6, §6 | implemented |
| `gsTransportLines.routeExists`, `countLines`, `countStops`, `getStopCount`, `getStop`; stop `getFlags`, `getModel`, `type`, `attractiveness` | pathfinding.md | implemented |
| `gsGrid.getGridObject`, `findItem`, `getRandomItem` | — | implemented |
| `gsGridItemController.activate` / `deactivate` (demonstrations and parades, `default.scenario` `startAnimationEvent`) | `0x100ecca0` / `0x100ecce0`: item `+0x58` = 0 / 0x15, `updateMesh`, `updateGraph` | **stub**: the event's ticker text shows, the crowd items stay as they are |
| `gsAchievements.awardAchievement`, `awardSteamAchievement` | `0x1011bf70`, `0x1011c2b0` | stubs on purpose (no Paradox / Steam services); unlocking itself is script-side and works |
| `gsGrid.addCamera` | `0x100bd510` | stub (not economy) |

## 7. Checked in the port (2026-11-06)

`REGS_DEBUG_ECONOMY=1` logs the books after every month and year and checks the money against §1; all runs below
closed every month **exactly** ("books say … (exact)"), 0 script errors:

* **Berlin save** (`samples/saves/berlin`, a campaign game in 1932, bank level 3), 38 months to 1936-03: vehicle
  maintenance −1 444,25, stop maintenance −907,00 and wages −2 818,00 a month are identical to the original's last
  month in the save; energy −2 733,96 / −2 742,56 / −2 753,31 at fuel 125 / 129 / 134 and electricity 73 solve to
  integer consumptions (fuel 215, electricity 3 377), as the formula requires; coverage 29–30 % and 1 000–1 100
  passengers a month match the save's history (29.2–30.3 %, 983–1 061). The company keeps losing 1 000–4 600 a
  month (the save's own history is mostly negative too). Board rating at the end of 1932: Δvalue −63 475 /
  (100 000 + 16 159 933^0.6 = 121 130) → int −0.52 = 0, +2 = **2** (logged 2); customer rating int(5·0.360) = **1**
  (logged 1); 1933: Δ −2 842 057 → clamp → 0 (logged 0).
* **Campaign scenario 1** ("Berliner Fundamente", started from the menu, Berlin 1923, 20 000,00), 19 months: the
  first task (Verbindungsarbeit) accepted and open; "Mehr Geld" (have 15 000,00) accepted, completed in
  December 1923 with reward `getReward(1700·100)` = 1 700,00 at bank level 3 (×1.0) and +20 % reputation
  (`extraHappiness` +0.2); that month the money moved +1 547,00 = 1 700,00 reward + 171,00 fares − 324,00 running costs. Achievements 6
  (reputation ≥ 70 %) and 10 (monthly profit ≥ 1 500,00) unlocked. Annual report 1923
  (`port/run/campaign/s1-20200.png`): 870 passengers, company value 22 043,00, board "Gut" (int(204 300/106 034)+2 = 3),
  customers "Neutral" (int(5·0.406) = 2), 1 task.
* **Sandbox Wien 1980** (from the menu, Normal): 12 500,00 start money; a loan of 10 000,00 at Alpengeld (margin 10,
  60 months; offered 12.250 % = int(10·(1+0.1·h)·4)·0.25 with h ≈ 2.4): monthly payment shown 223,70 = annuity
  formula (22 370,4 → 22 370); money −223,70 each month exactly; balance 1 000 000 → 987 838 → 975 552 → 963 140 →
  950 602 → 937 936 = `amount − (22 370 − int(amount·12.25/1200))`.
* **Vienna and Cim2 saves** (sandbox games with networks, 2010): months closed exactly; port saves of Berlin,
  Vienna and Cim2 reload and keep closing exactly.
