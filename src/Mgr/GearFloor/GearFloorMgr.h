/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 *
 * GearFloorMgr — rate-limits the maintenance gear-floor top-up (PlayerbotFactory::TopUpGear)
 * off the synchronous world-update tick. MaintenanceAction enqueues a bot; Update() drains the
 * queue at a bounded number of candidate-scans per tick, resuming each bot from a slot cursor.
 * See docs/superpowers/specs/2026-06-18-gearfloor-tick-throttle-design.md.
 */

#ifndef _PLAYERBOT_GEARFLOORMGR_H
#define _PLAYERBOT_GEARFLOORMGR_H

#include "Common.h"
#include "ObjectGuid.h"   // jobs store a bot GUID resolved at drain time
#include <deque>
#include <string>
#include <unordered_set>

class Player;

class GearFloorMgr
{
public:
    static GearFloorMgr& instance()
    {
        static GearFloorMgr instance;
        return instance;
    }

    void Enqueue(Player* bot);   // MaintenanceAction: queue a bot for a gear-floor top-up
    void Update(uint32 diff);    // ticked from PlayerbotsWorldScript::OnUpdate (world thread)

private:
    struct GearFloorJob
    {
        ObjectGuid guid;          // re-acquired at drain time; never a raw Player*
        size_t nextSlot = 0;      // resume cursor into initSlotsOrder
        uint32 filled = 0;        // running tally for the completion log line
        std::string slotsStr;     // running list for the completion log line
    };

    std::deque<GearFloorJob> _queue;
    std::unordered_set<ObjectGuid> _pending;   // dedup: a bot is queued at most once
    uint32 _logTimerMs = 0;                    // throttle the drip observability log
};

#define sGearFloorMgr GearFloorMgr::instance()

#endif
