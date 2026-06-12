/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_LFGACTIONS_H
#define _PLAYERBOT_LFGACTIONS_H

#include "InventoryAction.h"

class PlayerbotAI;

class LfgJoinAction : public InventoryAction
{
public:
    LfgJoinAction(PlayerbotAI* botAI, std::string const name = "lfg join") : InventoryAction(botAI, name) {}

    bool Execute(Event event) override;
    bool isUseful() override;

protected:
    bool JoinLFG();
    uint32 GetRoles();
};

class LfgAcceptAction : public LfgJoinAction
{
public:
    LfgAcceptAction(PlayerbotAI* botAI) : LfgJoinAction(botAI, "lfg accept") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgRoleCheckAction : public LfgJoinAction
{
public:
    LfgRoleCheckAction(PlayerbotAI* botAI) : LfgJoinAction(botAI, "lfg role check") {}

    bool Execute(Event event) override;
    bool isUseful() override { return true; }
};

class LfgLeaveAction : public Action
{
public:
    LfgLeaveAction(PlayerbotAI* botAI) : Action(botAI, "lfg leave") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

class LfgTeleportAction : public Action
{
public:
    LfgTeleportAction(PlayerbotAI* botAI) : Action(botAI, "lfg teleport") {}

    bool Execute(Event event) override;
};

class LfgTeleportRecoveryAction : public Action
{
public:
    LfgTeleportRecoveryAction(PlayerbotAI* botAI) : Action(botAI, "lfg teleport recovery") {}

    bool Execute(Event event) override;

private:
    uint32 m_trackedMapId   = 0;     // dungeon map this episode is tracking
    uint32 m_attempts       = 0;
    uint32 m_firstAttemptMs = 0;
    bool   m_didReset       = false; // one-shot ResetStrategies+Reset per episode
    bool   m_gaveUp         = false; // logged give-up, stop retrying this episode

    static constexpr uint32 MAX_ATTEMPTS  = 45;      // ~90s at the 2s trigger cadence
    static constexpr uint32 MAX_WINDOW_MS = 120000;  // hard 2-min wall-clock cap
};

#endif
