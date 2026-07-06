#ifndef _PLAYERBOT_REDEEMCURRENCYACTION_H
#define _PLAYERBOT_REDEEMCURRENCYACTION_H
#include "Action.h"
class PlayerbotAI;

// Runs at the upkeep SELL step (on a MapUpdater worker). Execute() only ENQUEUES a RedeemCurrencyOperation
// onto the PlayerbotWorldThreadProcessor; the actual redemption (inventory reads + buy/equip/sink/convert)
// runs on the world thread, where core inventory mutation is safe. See RedeemCurrencyAction.cpp.
class RedeemCurrencyAction : public Action
{
public:
    RedeemCurrencyAction(PlayerbotAI* botAI) : Action(botAI, "redeem currency") {}
    bool Execute(Event event) override;
};
#endif
