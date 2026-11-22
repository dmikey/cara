// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "core/hle/service/ptm/step_history.h"

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path path = argv[1];
    using Service::PTM::StepHistory;
    struct Coin {
        uint16_t total_coins, total_coins_on_date;
        uint32_t step_count, last_step_count;
        uint16_t year;
        uint8_t month, day;
    } coin{42, 0, 0, 0, 2000, 1, 1};

    StepHistory::Record(path, 0, 99);
    assert(!StepHistory::Load(path).AwardCoins(coin));
    StepHistory::Record(path, 1, 1);
    assert(StepHistory::Load(path).AwardCoins(coin) && coin.total_coins == 43);
    coin.total_coins -= 3; // Spending coins in a game must survive relaunch.
    assert(!StepHistory::Load(path).AwardCoins(coin) && coin.total_coins == 40);
    StepHistory::Record(path, 23, 1200);
    assert(StepHistory::Load(path).AwardCoins(coin) && coin.total_coins == 49);
    StepHistory::Record(path, 24, 250);
    coin.total_coins = 299;
    auto history = StepHistory::Load(path);
    assert(history.total == 1550 && history.hours.at(1) == 1);
    assert(history.AwardCoins(coin) && coin.total_coins == 300);
    assert(coin.day == 2 && coin.total_coins_on_date == 2 && coin.step_count == 1500);
    assert(!history.AwardCoins(coin));
    bool refused = false;
    try { StepHistory::Record(path, 24, 65535); }
    catch (const std::exception&) { refused = true; }
    assert(refused && StepHistory::Load(path).total == 1550);
}
