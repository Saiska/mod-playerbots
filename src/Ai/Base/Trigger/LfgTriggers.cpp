/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LfgTriggers.h"

#include "Playerbots.h"
#include "LFGMgr.h"

bool LfgProposalActiveTrigger::IsActive() { return AI_VALUE(uint32, "lfg proposal"); }

bool UnknownDungeonTrigger::IsActive()
{
    return botAI->HasActivePlayerMaster() && botAI->GetMaster() && botAI->GetMaster()->IsInWorld() &&
           botAI->GetMaster()->GetMap()->IsDungeon() && bot->GetMapId() == botAI->GetMaster()->GetMapId();
}

bool LfgTeleportRecoveryTrigger::IsActive()
{
    if (sLFGMgr->GetState(bot->GetGUID()) != lfg::LFG_STATE_DUNGEON)
        return false;

    uint32 dungeonMap = sLFGMgr->GetDungeonMapId(bot->GetGUID());
    if (!dungeonMap || dungeonMap == uint32(-1))
        return false;                       // no assigned map yet

    if (bot->GetMapId() == dungeonMap)
        return false;                       // already inside

    if (bot->IsBeingTeleported())
        return false;                       // mid-transition, let it land

    return true;                            // committed to a dungeon, still outside
}
