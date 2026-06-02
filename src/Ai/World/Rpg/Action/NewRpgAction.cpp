#include "NewRpgAction.h"

#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "G3D/Vector2.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"

// Restore a socializing bot to a neutral pose when it leaves the knot: stand up and
// clear any held looping emote state. Used on every RPG_PASTIME exit path (including the
// status-timeout path, which would otherwise leave a sitting/dancing bot frozen mid-pose).
static void EndSocialPastime(Player* bot)
{
    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    bot->ClearEmoteState();
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;
    std::string out = botAI->rpgInfo.ToString();
    bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
    return true;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_PASTIME,
                                       RPG_DO_QUEST, RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_PASTIME:
        {
            if (info.HasStatusPersisted(statusPastimeDuration))
            {
                EndSocialPastime(bot);
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

static void PerformSocialEmote(Player* bot)
{
    auto const& names = sPlayerbotAIConfig.pastimeSocialEmotes;
    if (names.empty())
        return;
    std::string const& name = names[urand(0, names.size() - 1)];
    if (name == "sit")
    {
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return;
    }
    static const std::unordered_map<std::string, uint32> m = {
        {"dance", EMOTE_STATE_DANCE}, {"cheer", EMOTE_ONESHOT_CHEER}, {"laugh", EMOTE_ONESHOT_LAUGH},
        {"applaud", EMOTE_ONESHOT_APPLAUD}, {"point", EMOTE_ONESHOT_POINT}, {"talk", EMOTE_ONESHOT_TALK},
        {"wave", EMOTE_ONESHOT_WAVE}, {"bow", EMOTE_ONESHOT_BOW}, {"roar", EMOTE_ONESHOT_ROAR}
    };
    auto it = m.find(name);
    if (it != m.end())
    {
        if (bot->getStandState() != UNIT_STAND_STATE_STAND)
            bot->SetStandState(UNIT_STAND_STATE_STAND);
        bot->HandleEmoteCommand(it->second);
    }
}

bool NewRpgPastimeAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::Pastime>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;

    if (data.activityType == ACTIVITY_LOITER)
    {
        if (data.target.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.target);
        if (object && IsWithinInteractionDist(object))
        {
            if (!data.lastReach)
            {
                data.lastReach = getMSTime();
                uint32 dwellSec = urand(sPlayerbotAIConfig.pastimeLoiterDwellMin,
                                        sPlayerbotAIConfig.pastimeLoiterDwellMax);
                data.dwellMs = dwellSec * IN_MILLISECONDS;
                bot->SetFacingToObject(object);
                return true;
            }
            if (GetMSTimeDiffToNow(data.lastReach) < data.dwellMs)
            {
                // dwelling: occasional idle emote keeps it lively
                if (urand(0, 100) < 5)
                    bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                return false;
            }
            info.ChangeToIdle();
            return true;
        }
        if (MoveWorldObjectTo(data.target))
            return true;
        return MoveRandomNear(15.0f);
    }

    if (data.activityType == ACTIVITY_FISH)
    {
        // Dwell window elapsed (timer starts on the first successful cast) -> stop fishing.
        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) >= data.dwellMs)
        {
            info.ChangeToIdle();
            return true;
        }

        // Delegate the existing, self-gating fishing chain. DoSpecificAction returns true only when the
        // action was useful + possible + ran this tick; the active +loot strategy auto-uses the bobber + loots.
        if (botAI->DoSpecificAction("equip fishing pole", Event(), true))
            return true;                                   // acquiring/equipping a pole
        if (botAI->DoSpecificAction("move near water", Event(), true))
            return true;                                   // walking to the resolved fishing spot
        if (botAI->DoSpecificAction("go fishing", Event(), true))
        {
            if (!data.lastReach)                           // first successful cast = arrived & fishing
            {
                data.lastReach = getMSTime();
                data.dwellMs = urand(sPlayerbotAIConfig.pastimeFishDwellMin,
                                     sPlayerbotAIConfig.pastimeFishDwellMax) * IN_MILLISECONDS;
            }
            return true;
        }

        // Nothing in the chain could act. If we never started fishing (no pole / no water reachable),
        // give up; otherwise we're channeling the cast -> wait.
        if (!data.lastReach)
        {
            info.ChangeToIdle();
            return true;
        }
        return false;
    }

    // ACTIVITY_SOCIAL (default): converge on a friendly player and emote.
    Player* target = ObjectAccessor::FindPlayer(data.target);
    bool targetOk = target && target->IsInWorld() &&
                    bot->GetExactDist(target) <= sPlayerbotAIConfig.pastimeSocialRadius;
    if (!targetOk)
    {
        ObjectGuid t = SelectSocialPartner();
        if (t.IsEmpty())
        {
            EndSocialPastime(bot);
            info.ChangeToIdle();
            return true;
        }
        data.target = t; data.lastReach = 0; data.lastEmote = 0; data.dwellMs = 0;
        return true;
    }

    if (bot->GetExactDist(target) > sPlayerbotAIConfig.pastimeSocialClusterDist)
    {
        if (MoveWorldObjectTo(data.target))
            return true;
        return MoveRandomNear(10.0f);
    }

    // within cluster range
    if (!data.lastReach)
    {
        data.lastReach = getMSTime();
        data.dwellMs = urand(sPlayerbotAIConfig.pastimeSocialDwellMin,
                             sPlayerbotAIConfig.pastimeSocialDwellMax) * IN_MILLISECONDS;
    }
    if (GetMSTimeDiffToNow(data.lastReach) >= data.dwellMs)
    {
        EndSocialPastime(bot);
        info.ChangeToIdle();
        return true;
    }
    bot->SetFacingToObject(target);
    if (!data.lastEmote ||
        GetMSTimeDiffToNow(data.lastEmote) >= sPlayerbotAIConfig.pastimeSocialEmoteInterval * IN_MILLISECONDS)
    {
        PerformSocialEmote(bot);
        data.lastEmote = getMSTime();
    }
    return false;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const QuestStatusData& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        uint32 rndIdx = urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const QuestStatusData& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    const Quest* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->rpgStatistic.questCompleted++;
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes[0], nodes[nodes.size() - 1]);
        info.ChangeToIdle();
        return true;
    }
    return true;
}
