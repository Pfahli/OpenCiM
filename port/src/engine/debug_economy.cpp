// Debugging: REGS_DEBUG_ECONOMY=1 logs the company's books after each month and year of the main clock, for the
// economy checks of port/docs/playable-todo.md §5 (formulas: docs/simulation/economy.md).
//
// After the scripts' onMonth handlers ran (player.script settles the month synchronously in its handler), it logs the
// money, every monthly book entry of MAIN.$gameData, loans, reputation and company value, and checks the books: the
// money must have moved since the last month's log by exactly
//   Σ monthlyTicketProfit + monthlyPenalties + salesIncome + purchaseExpenses
//   + vMaintenance + sMaintenance + energy + loan + wage expenses
// (tickets, penalties, sales and purchases are booked at once through earnMoney / spendMoney, the running costs at
// the turn of the month). A difference is logged as "books off by". After onYear it logs the annual report's inputs.
#include <cstdlib>
#include <string>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"

namespace regs::engine {

using namespace regs::script;

namespace {

Value member(VM& vm, const Value& o, const char16_t* name) {
  Value v = vm.resolve(o);
  if (!v.object()) return Value::null();
  try {
    return vm.resolve(vm.getMember(v, name));
  } catch (...) {
    return Value::null();
  }
}

std::string show(const Value& v) {
  std::string s = utf16ToUtf8(v.exportSource());
  if (s.size() > 400) s = s.substr(0, 400) + "...";
  return s;
}

double num(VM& vm, const Value& v) {
  try {
    return double(vm.toFloat(v));
  } catch (...) {
    return 0;
  }
}

}  // namespace

// what: 0 month, 1 year; called by gsClock after it fired onMonth / onYear
void debugEconomyClock(VM& vm, Object* clock, int what) {
  static const bool on = [] {
    const char* e = std::getenv("REGS_DEBUG_ECONOMY");
    return e && *e && *e != '0';
  }();
  if (!on) return;
  Value* mainp = vm.findGlobal(u"MAIN");
  if (!mainp) return;
  Value main = *mainp;
  if (member(vm, main, u"$mainClock").object() != clock) return;
  Value data = member(vm, main, u"$gameData");
  Value scen = member(vm, member(vm, main, u"$scenario"), u"$scenario");
  if (!data.object() || !scen.object()) return;
  int32_t year = vm.toInt(member(vm, Value::heap(VT::Object, clock), u"years"));
  int32_t month = vm.toInt(member(vm, Value::heap(VT::Object, clock), u"months"));
  // the clock already shows the new month (and year): log the one that just ended
  if (--month < 1) month = 12, --year;
  double money = num(vm, member(vm, scen, u"$money"));
  auto d = [&](const char16_t* n) { return member(vm, data, n); };
  if (what == 1) {
    LOG_INFO("economy: year %d ended: companyValue %s lastYearCompanyValue %s yearlyPassengersCarried %s "
             "lastYearPassengersCarried %s yearlyTasksCompleted %s lastYearTasksCompleted %s yearlyVehiclesSold %s "
             "lastYearVehiclesSold %s yearlyCustomerHappiness %s boardRating %s customerRating %s lastYearCoverage %s "
             "maxYearlyVehicles %s",
             year, show(d(u"$companyValue")).c_str(), show(d(u"$lastYearCompanyValue")).c_str(),
             show(d(u"$yearlyPassengersCarried")).c_str(), show(d(u"$lastYearPassengersCarried")).c_str(),
             show(d(u"$yearlyTasksCompleted")).c_str(), show(d(u"$lastYearTasksCompleted")).c_str(),
             show(d(u"$yearlyVehiclesSold")).c_str(), show(d(u"$lastYearVehiclesSold")).c_str(),
             show(d(u"$yearlyCustomerHappiness")).c_str(), show(d(u"$boardRating")).c_str(),
             show(d(u"$customerRating")).c_str(), show(d(u"$lastYearCoverage")).c_str(),
             show(d(u"$maxYearlyVehicles")).c_str());
    return;
  }
  // the books of the month just closed
  double tickets = 0;
  Value mtp = d(u"$monthlyTicketProfit");
  if (ArrayObj* a = mtp.array())
    for (const Value& x : a->items) tickets += num(vm, x);
  double penalties = num(vm, d(u"$monthlyPenalties")), sales = num(vm, d(u"$salesIncome")),
         purchases = num(vm, d(u"$purchaseExpenses")), vm_ = num(vm, d(u"$vMaintenanceExpenses")),
         sm = num(vm, d(u"$sMaintenanceExpenses")), energy = num(vm, d(u"$energyExpenses")),
         loan = num(vm, d(u"$loanExpenses")), wage = num(vm, d(u"$wageExpenses"));
  double expected = tickets + penalties + sales + purchases + vm_ + sm + energy + loan + wage;
  static double lastMoney = 0;
  static bool have = false;
  LOG_INFO("economy: %04d-%02d money %.0f | tickets %s penalties %.0f sales %.0f purchases %.0f | vMaint %.0f sMaint %.0f "
           "energy %.0f loans %.0f wages %.0f advertising %s | monthlyExpenses %s monthlyProfit %s totalMonthlyProfit %s",
           year, month, money, show(mtp).c_str(), penalties, sales, purchases, vm_, sm, energy, loan, wage,
           show(d(u"$advertisingExpenses")).c_str(), show(d(u"$monthlyExpenses")).c_str(),
           show(d(u"$monthlyProfit")).c_str(), show(d(u"$totalMonthlyProfit")).c_str());
  if (have)
    LOG_INFO("economy: %04d-%02d money moved %.0f, books say %.0f%s%.0f", year, month, money - lastMoney, expected,
             money - lastMoney == expected ? " (exact) " : " -> books off by ", money - lastMoney - expected);
  lastMoney = money;
  have = true;
  LOG_INFO("economy: %04d-%02d loans %s | fuel %s electricity %s ticketPrices %s | wageLevel %s maintenanceLevel %s "
           "maintenanceLevel2 %s vehicleEnergyMultiplier %s",
           year, month, show(d(u"$loanPayments")).c_str(), show(d(u"$fuelPrice")).c_str(),
           show(d(u"$electricityPrice")).c_str(), show(d(u"$ticketPrices")).c_str(), show(d(u"$wageLevel")).c_str(),
           show(d(u"$maintenanceLevel")).c_str(), show(d(u"$maintenanceLevel2")).c_str(),
           show(d(u"$vehicleEnergyMultiplier")).c_str());
  Value eh = d(u"$economyHistory"), ph = d(u"$passengersCarriedHistory");
  ArrayObj* eha = eh.array();
  ArrayObj* pha = ph.array();
  LOG_INFO("economy: %04d-%02d reputation %s companyReputation %s customerHappiness %s employeeHappiness %s "
           "extraHappiness %s | companyValue %s baseCompanyValue %s | economy %s (target %s) | coverage %s usage %s "
           "passengers %s population %d",
           year, month, show(d(u"$totalReputation")).c_str(), show(d(u"$companyReputation")).c_str(),
           show(d(u"$customerHappiness")).c_str(), show(d(u"$employeeHappiness")).c_str(),
           show(d(u"$extraHappiness")).c_str(), show(d(u"$companyValue")).c_str(),
           show(d(u"$baseCompanyValue")).c_str(),
           eha && !eha->items.empty() ? show(eha->items.back()).c_str() : "-", show(d(u"$economyTarget")).c_str(),
           show(d(u"$serviceCoverage")).c_str(), show(d(u"$serviceUsage")).c_str(),
           pha && !pha->items.empty() ? show(pha->items.back()).c_str() : "-",
           vm.toInt(member(vm, member(vm, member(vm, main, u"$grid"), u"citizens"), u"count")));
  // the annual report's running inputs (annualreport.script onYear reads them after this month's settlement)
  LOG_INFO("economy: %04d-%02d year so far: lastYearCompanyValue %s yearlyCustomerHappiness %s yearlyPassengersCarried %s "
           "yearlyTasksCompleted %s subventionCount %s | achievements %s",
           year, month, show(d(u"$lastYearCompanyValue")).c_str(), show(d(u"$yearlyCustomerHappiness")).c_str(),
           show(d(u"$yearlyPassengersCarried")).c_str(), show(d(u"$yearlyTasksCompleted")).c_str(),
           show(d(u"$subventionCount")).c_str(),
           show(member(vm, member(vm, main, u"$score"), u"$achivementsUnlocked")).c_str());
  // the scenario's objectives: title, status (0 open, 1 done, 2 failed), rewards
  Value objs = member(vm, scen, u"$objectives");
  if (MapObj* m = objs.map())
    for (const auto& [k, o] : m->entries) {
      Value ov = vm.resolve(o);
      auto f = [&](const char16_t* key) {
        if (MapObj* om = ov.map())
          if (Value* x = om->find(Value::string(std::u16string(key)))) return show(vm.resolve(*x));
        return std::string("-");
      };
      LOG_INFO("economy: %04d-%02d objective %s: task %s title %s status %s rewardCash %s rewardReputation %s", year,
               month, show(k).c_str(), f(u"task").c_str(), f(u"title").c_str(), f(u"status").c_str(),
               f(u"rewardCash").c_str(), f(u"rewardReputation").c_str());
    }
}

}  // namespace regs::engine
