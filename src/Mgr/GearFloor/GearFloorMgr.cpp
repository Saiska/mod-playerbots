/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "GearFloorMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Log.h"
#include <limits>

void GearFloorMgr::Enqueue(Player* bot)
{
    if (!bot)
        return;

    ObjectGuid guid = bot->GetGUID();
    if (_pending.count(guid))
        return;   // already queued/in-progress

    _pending.insert(guid);
    _queue.push_back(GearFloorJob{guid});
}

void GearFloorMgr::Update(uint32 diff)
{
    if (!sPlayerbotAIConfig.maintenanceGearFloor)
        return;

    if (_queue.empty())
    {
        _logTimerMs = 0;
        return;
    }

    int32 perTick = sPlayerbotAIConfig.maintenanceGearFloorScansPerTick;
    uint32 remaining = perTick <= 0 ? std::numeric_limits<uint32>::max() : (uint32)perTick;
    uint32 scannedThisTick = 0;

    while (!_queue.empty() && remaining > 0)
    {
        GearFloorJob& job = _queue.front();

        // Re-acquire by GUID; the bot may have logged out / despawned since enqueue.
        Player* bot = ObjectAccessor::FindPlayer(job.guid);
        if (!bot)
        {
            _pending.erase(job.guid);
            _queue.pop_front();
            continue;
        }

        // Construct the factory the same way MaintenanceAction does (gear-score from config).
        uint32 gs = sPlayerbotAIConfig.autoGearScoreLimit == 0
                        ? 0
                        : PlayerbotFactory::CalcMixedGearScore(sPlayerbotAIConfig.autoGearScoreLimit,
                                                               sPlayerbotAIConfig.autoGearQualityLimit);
        PlayerbotFactory factory(bot, bot->GetLevel(), sPlayerbotAIConfig.autoGearQualityLimit, gs);

        bool done = false;
        uint32 used = factory.TopUpGearStep(job.nextSlot, remaining, job.filled, job.slotsStr, done);
        remaining -= used;
        scannedThisTick += used;

        if (done)
        {
            if (job.filled > 0)
                LOG_INFO("server.loading", "[GearFloor] Bot #{} {} lvl{}: filled {} slot(s) ({})",
                         bot->GetGUID().GetCounter(), bot->GetName().c_str(), bot->GetLevel(),
                         job.filled, job.slotsStr.c_str());
            _pending.erase(job.guid);
            _queue.pop_front();
        }
        else
        {
            break;   // budget exhausted mid-job; resume this bot next tick
        }
    }

    // Throttled drip observability (~once/5s) for live verification.
    _logTimerMs += diff;
    if (_logTimerMs >= 5000)
    {
        _logTimerMs = 0;
        LOG_INFO("playerbots", "GearFloor drip: scanned={} queued={}", scannedThisTick, (uint32)_queue.size());
    }
}
