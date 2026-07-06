#ifndef _PLAYERBOT_REDEEMCURRENCYACTION_H
#define _PLAYERBOT_REDEEMCURRENCYACTION_H
#include "Action.h"
class PlayerbotAI;
class RedeemCurrencyAction : public Action
{
public:
    RedeemCurrencyAction(PlayerbotAI* botAI) : Action(botAI, "redeem currency") {}
    bool Execute(Event event) override;   // implemented in Task 6
private:
    bool PayCost(uint32 extendedCostId);
    bool GrantAndEquip(uint32 gearId);
    uint32 _buys = 0;
};
#endif
