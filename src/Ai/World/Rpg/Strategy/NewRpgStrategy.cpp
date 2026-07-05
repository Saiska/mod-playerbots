/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "NewRpgStrategy.h"

NewRpgStrategy::NewRpgStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

std::vector<NextAction> NewRpgStrategy::getDefaultActions()
{
    // the releavance should be greater than grind
    return {
        // upkeep-share-reduction fix: rel-100 so it runs in minimal mode (Engine.cpp:172) and can
        // rescue AI-throttled bots stuck in a rel-11-exit occupation; out-ranks the rel-11 machine
        // so queue.Peek() surfaces it first. isUseful() no-ops it for active bots.
        NextAction("new rpg minimal escape", 100.0f),
        NextAction("new rpg status update", 11.0f)
    };
}

void NewRpgStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "go grind status",
            {
                NextAction("new rpg go grind", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "go camp status",
            {
                NextAction("new rpg go camp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "do quest status",
            {
                NextAction("new rpg do quest", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "travel flight status",
            {
                NextAction("new rpg travel flight", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "outdoor pvp status",
            {
                NextAction("new rpg outdoor pvp", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "travel mount status",
            {
                NextAction("new rpg travel mount", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "explore landmark status",
            {
                NextAction("new rpg explore landmark", 3.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "gathering circuit status",
            {
                NextAction("new rpg gathering circuit", 3.0f)
            }
        )
    );
}

void NewRpgStrategy::InitMultipliers(std::vector<Multiplier*>&)
{
}
