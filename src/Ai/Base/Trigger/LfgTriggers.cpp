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

    Group* group = bot->GetGroup();
    if (!group)
        return false;                       // LFG dungeon state implies a group; guard for GetGUID()

    // GetDungeonMapId is keyed on the GROUP guid (GroupsStore), not the player guid.
    uint32 dungeonMap = sLFGMgr->GetDungeonMapId(group->GetGUID());
    if (!dungeonMap)
        return false;                       // no assigned dungeon map yet

    if (bot->IsBeingTeleported())
        return false;                       // mid-transition, let it land

    if (bot->GetMapId() != dungeonMap)
        return true;                        // committed to a dungeon, still OUTSIDE -> teleport-in recovery

    // INSIDE the dungeon: only act if still roaming under NewRpg as a non-leader. LFG bots strip
    // "new rpg" only at accept-time (while still ungrouped, so it is immediately re-added) and
    // nothing re-strips it after the group forms -> they enter the instance and roam. Engage
    // dungeon mode. The HasStrategy gate makes this self-terminating: once ResetStrategies drops
    // "new rpg" for the now-grouped non-leader, this returns false.
    if (group->GetLeaderGUID() != bot->GetGUID() &&
        botAI->HasStrategy("new rpg", BOT_STATE_NON_COMBAT))
        return true;

    return false;
}
