#ifndef _PLAYERBOT_REDEEMCURRENCYACTION_H
#define _PLAYERBOT_REDEEMCURRENCYACTION_H
#include "Action.h"
class PlayerbotAI;

// Runs INLINE on the bot's own upkeep tick (its MapUpdater-worker context), where the bot's inventory
// reads + buy/equip/sink/convert mutations are serialized with its other inventory ops on that same
// thread. It is NOT deferred to the world thread (that raced the worker). See RedeemCurrencyAction.cpp.
// (Stale prior comment claimed an enqueue-to-world-thread design — that was reverted in 42260fd7.)
class RedeemCurrencyAction : public Action
{
public:
    RedeemCurrencyAction(PlayerbotAI* botAI) : Action(botAI, "redeem currency") {}
    bool Execute(Event event) override;
};
#endif
