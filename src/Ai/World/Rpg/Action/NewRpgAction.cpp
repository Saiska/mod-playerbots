#include "NewRpgAction.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

#include "AiObjectContext.h"
#include "AreaDefines.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "G3D/Vector2.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "LootObjectStack.h"
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
    bot->ClearEmoteState();   // zeroes UNIT_NPC_EMOTESTATE -> drops any held loiter pose
}

// Sustained, replicated pose per loiter POI. Driven via UNIT_NPC_EMOTESTATE (a UF_FLAG_PUBLIC field that
// renders on nearby clients — the same mechanism mod-ollama-chat uses for a held dance), NOT SetStandState
// or a one-shot HandleEmoteCommand, neither of which holds a visible pose on a bot. 0 = no pose (just stand).
static uint32 LoiterEmoteState(uint8 poiType)
{
    switch (static_cast<BotCityPoi>(poiType))
    {
        case POI_INNKEEPER:  return EMOTE_STATE_SIT;            // 13  — seated at the inn
        case POI_FORGE:      return EMOTE_STATE_USE_STANDING;   // 69  — smith/use pose at the anvil
        case POI_BANKER:
        case POI_AUCTIONEER:
        case POI_TRAINER:    return EMOTE_STATE_TALK;           // 378 — conversing / doing business
        default:             return 0;                          // mailbox / none — just stand
    }
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

    // --- occupation satiation: integrate meters (dt-based; runs every tick) ---
    // Current category rises; all others decay. IDLE -> CategoryOf == CAT_COUNT,
    // so no category matches and everything decays.
    if (sPlayerbotAIConfig.rpgSatiationEnable)
    {
        uint32 nowMs = getMSTime();
        if (info.lastSatiationUpdateMs == 0)
        {
            info.lastSatiationUpdateMs = nowMs;
        }
        else
        {
            uint32 elapsedMs = GetMSTimeDiffToNow(info.lastSatiationUpdateMs);  // wrap-safe
            info.lastSatiationUpdateMs = nowMs;
            if (elapsedMs > 60000)
                elapsedMs = 60000;  // clamp long idle gaps so meters don't jump
            float dt = elapsedMs / 1000.0f;
            BotActivityCategory cur = CategoryOf(info.GetStatus());
            for (uint8 c = 0; c < CAT_COUNT; ++c)
            {
                if (c == static_cast<uint8>(cur))
                    info.satiation[c] =
                        std::min(1.0f, info.satiation[c] + sPlayerbotAIConfig.rpgSatiationRiseRatePerSec * dt);
                else
                    info.satiation[c] =
                        std::max(0.0f, info.satiation[c] - sPlayerbotAIConfig.rpgSatiationDecayRatePerSec * dt);
            }
        }
    }

    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_PASTIME,
                                       RPG_DO_QUEST, RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP, RPG_TRAVEL_MOUNT,
                                       RPG_EXPLORE_LANDMARK, RPG_GATHERING_CIRCUIT});

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
        case RPG_TRAVEL_MOUNT:
        {
            auto& data = std::get<NewRpgInfo::TravelMount>(info.data);
            if (bot->GetExactDist(data.pos) < 10.0f || info.HasStatusPersisted(statusTravelMountDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_EXPLORE_LANDMARK:
        {
            if (info.HasStatusPersisted(statusExploreDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_GATHERING_CIRCUIT:
        {
            if (info.HasStatusPersisted(statusGatheringDuration))
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
            EndSocialPastime(bot);
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
                // Themed arrival: set sustained pose once on reaching the POI.
                if (sPlayerbotAIConfig.pastimeLoiterThemedScenes)
                {
                    // A mounted bot can't visibly pose (the mount model overrides it) — dismount on arrival.
                    bool wasMounted = bot->IsMounted();
                    if (wasMounted)
                        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                    // Hold the pose via the replicated UNIT_NPC_EMOTESTATE field (see LoiterEmoteState).
                    uint32 es = LoiterEmoteState(data.poiType);
                    if (es)
                        bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, es);
                }
                return true;
            }
            if (GetMSTimeDiffToNow(data.lastReach) < data.dwellMs)
            {
                if (sPlayerbotAIConfig.pastimeLoiterThemedScenes)
                {
                    // Stay dismounted + hold the sustained pose every tick (movement/idle AI resets both).
                    if (bot->IsMounted())
                        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                    uint32 es = LoiterEmoteState(data.poiType);
                    if (es && bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) != es)
                        bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, es);
                }
                else
                {
                    // Legacy path — byte-for-byte unchanged.
                    if (urand(0, 100) < 5)
                        bot->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
                }
                // Hold the bot in place for the dwell. Returning false here let the lower-priority
                // movement AI walk it off the POI within a tick or two, so it never actually lingered
                // (bots arrived but never dwelt — the pose flashed for one tick and was gone).
                return true;
            }
            EndSocialPastime(bot);
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

    if (data.activityType == ACTIVITY_GATHER)
    {
        if (data.target.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        WorldObject* node = ObjectAccessor::GetWorldObject(*bot, data.target);
        if (!node)
        {
            // harvested by someone else / despawned
            info.ChangeToIdle();
            return true;
        }
        if (!IsWithinInteractionDist(node))
        {
            if (MoveWorldObjectTo(data.target))
                return true;
            // can't reach -> give up
            info.ChangeToIdle();
            return true;
        }
        // Arrived: set the node as the loot target and delegate to the existing harvest action.
        // Construct a LootObject for the GO guid and set "loot target", mirroring LootAction.cpp.
        LootObject lootObj(bot, data.target);
        // Skip nodes this bot can't actually harvest (e.g. a miner with no pickaxe). The normal loot
        // pipeline filters these via IsLootPossible before reaching "open loot"; this delegating path
        // bypasses that filter, so apply it here to avoid a wasted cast -> re-roll next cycle instead.
        if (!lootObj.IsLootPossible(bot))
        {
            info.ChangeToIdle();
            return true;
        }
        context->GetValue<LootObject>("loot target")->Set(lootObj);
        // Fire-and-forget: the mining/herb gather cast completes independently of the RPG state, so
        // transitioning to Idle this tick does not abort it. One node per activation; re-roll next cycle.
        botAI->DoSpecificAction("open loot", Event(), true);
        info.ChangeToIdle();
        return true;
    }

    if (data.activityType == ACTIVITY_CRAFT)
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            data.dwellMs = urand(sPlayerbotAIConfig.pastimeCraftDwellMin,
                                 sPlayerbotAIConfig.pastimeCraftDwellMax) * IN_MILLISECONDS;
        }
        if (GetMSTimeDiffToNow(data.lastReach) >= data.dwellMs)
        {
            info.ChangeToIdle();
            return true;
        }
        bot->HandleEmoteCommand(EMOTE_STATE_USE_STANDING);
        return false;
    }

    if (data.activityType == ACTIVITY_DUEL)
    {
        Player* partner = ObjectAccessor::FindPlayer(data.target);
        if (!partner || !partner->IsInWorld() ||
            bot->GetExactDist(partner) > sPlayerbotAIConfig.pastimeDuelRadius)
        {
            info.ChangeToIdle();
            return true;
        }
        if (bot->GetExactDist(partner) > INTERACTION_DISTANCE)
            return MoveWorldObjectTo(data.target);
        // Initiate the friendly duel by casting spell 7266 (mirrors RpgDuelAction::Execute). The partner
        // auto-accepts (its default DuelStrategy) and both fight; entering combat switches the bot to the
        // combat engine, which suspends NewRpg until the duel resolves, then it resumes. Fire-and-forget.
        botAI->DoSpecificAction("cast custom spell",
            Event("rpg action", chat->FormatWorldobject(partner) + " 7266"), true);
        info.ChangeToIdle();
        return true;
    }

    if (data.activityType == ACTIVITY_EAT_DRINK)
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            data.dwellMs = urand(sPlayerbotAIConfig.pastimeEatDrinkDwellMin,
                                 sPlayerbotAIConfig.pastimeEatDrinkDwellMax) * IN_MILLISECONDS;
            bot->SetStandState(UNIT_STAND_STATE_SIT);
        }
        if (GetMSTimeDiffToNow(data.lastReach) >= data.dwellMs)
        {
            bot->SetStandState(UNIT_STAND_STATE_STAND);
            info.ChangeToIdle();
            return true;
        }
        if (!data.lastEmote || GetMSTimeDiffToNow(data.lastEmote) >= 5 * IN_MILLISECONDS)
        {
            bot->HandleEmoteCommand(EMOTE_ONESHOT_EAT);   // flavor only; no consumable
            data.lastEmote = getMSTime();
        }
        return false;
    }

    if (data.activityType == ACTIVITY_REST_EMOTE)
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            data.dwellMs = urand(sPlayerbotAIConfig.pastimeRestEmoteDwellMin,
                                 sPlayerbotAIConfig.pastimeRestEmoteDwellMax) * IN_MILLISECONDS;
            bot->SetStandState(UNIT_STAND_STATE_SIT);
        }
        if (GetMSTimeDiffToNow(data.lastReach) >= data.dwellMs)
        {
            bot->SetStandState(UNIT_STAND_STATE_STAND);
            info.ChangeToIdle();
            return true;
        }
        if (!data.lastEmote || GetMSTimeDiffToNow(data.lastEmote) >= 6 * IN_MILLISECONDS)
        {
            static const uint32 restEmotes[] = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_QUESTION };
            bot->HandleEmoteCommand(restEmotes[urand(0, 1)]);   // read/ponder flavor
            data.lastEmote = getMSTime();
        }
        return false;
    }

    if (data.activityType == ACTIVITY_REPAIR_SELL)
    {
        if (data.target.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        WorldObject* vendor = ObjectAccessor::GetWorldObject(*bot, data.target);
        if (!vendor)
        {
            info.ChangeToIdle();
            return true;
        }
        if (!IsWithinInteractionDist(vendor))
        {
            if (MoveWorldObjectTo(data.target))
                return true;
            return MoveRandomNear(15.0f);
        }
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            data.dwellMs = urand(sPlayerbotAIConfig.pastimeRepairSellDwellMin,
                                 sPlayerbotAIConfig.pastimeRepairSellDwellMax) * IN_MILLISECONDS;
            bot->SetFacingToObject(vendor);
            botAI->DoSpecificAction("sell", Event("rpg action", "vendor"), true);   // SellAction (vendor mode)
            botAI->DoSpecificAction("repair", Event(), true);                       // RepairAllAction (re-finds vendor)
            return true;
        }
        if (GetMSTimeDiffToNow(data.lastReach) < data.dwellMs)
            return false;
        info.ChangeToIdle();
        return true;
    }

    if (data.activityType == ACTIVITY_DUMMY)
    {
        Creature* dummy = ObjectAccessor::GetCreature(*bot, data.target);
        if (!dummy || !dummy->IsInWorld() || !dummy->IsAlive())
        {
            bot->AttackStop();
            info.ChangeToIdle();
            return true;
        }
        if (bot->GetExactDist(dummy) > INTERACTION_DISTANCE)
        {
            if (MoveWorldObjectTo(data.target))
                return true;
            return MoveRandomNear(10.0f);
        }
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            data.dwellMs = urand(sPlayerbotAIConfig.pastimeDummyDwellMin,
                                 sPlayerbotAIConfig.pastimeDummyDwellMax) * IN_MILLISECONDS;
            bot->SetFacingToObject(dummy);
            bot->Attack(dummy, true);   // melee auto-attack the practice target
            return true;
        }
        if (GetMSTimeDiffToNow(data.lastReach) >= data.dwellMs)
        {
            bot->AttackStop();
            info.ChangeToIdle();
            return true;
        }
        bot->Attack(dummy, true);   // keep swinging
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

bool NewRpgTravelMountAction::Execute(Event /*event*/)
{
    if (auto* data = std::get_if<NewRpgInfo::TravelMount>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }
    return false;
}

bool NewRpgExploreLandmarkAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* data = std::get_if<NewRpgInfo::ExploreLandmark>(&info.data);
    if (!data)
        return false;
    if (bot->GetExactDist(data->pos) > 10.0f)
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }
    // arrived: linger and look around
    if (!data->lastReach)
    {
        data->lastReach = getMSTime();
        data->dwellMs = urand(sPlayerbotAIConfig.exploreLandmarkDwellMin,
                              sPlayerbotAIConfig.exploreLandmarkDwellMax) * IN_MILLISECONDS;
    }
    if (GetMSTimeDiffToNow(data->lastReach) >= data->dwellMs)
    {
        info.ChangeToIdle();
        return true;
    }
    if (urand(0, 100) < 5)   // occasional look-around emote
    {
        static const uint32 lookEmotes[] = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_QUESTION };
        bot->HandleEmoteCommand(lookEmotes[urand(0, 2)]);
    }
    return false;
}

bool NewRpgGatheringCircuitAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* data = std::get_if<NewRpgInfo::GatheringCircuit>(&info.data);
    if (!data)
        return false;
    if (data->visited >= data->maxNodes)
    {
        info.ChangeToIdle();
        return true;
    }
    if (data->node.IsEmpty())
    {
        data->node = SelectGatherNode();
        if (data->node.IsEmpty())
        {
            info.ChangeToIdle();   // no node nearby -> done
            return true;
        }
    }
    GameObject* node = ObjectAccessor::GetGameObject(*bot, data->node);
    if (!node || !node->isSpawned())
    {
        data->node = ObjectGuid();   // harvested/despawned -> re-pick
        return true;
    }
    if (!IsWithinInteractionDist(node))
    {
        if (MoveWorldObjectTo(data->node))
            return true;
        data->node = ObjectGuid();   // unreachable -> try another
        return true;
    }
    // arrived: harvest — mirrors the existing ACTIVITY_GATHER harvest delegation
    LootObject lootObj(bot, data->node);
    if (lootObj.IsLootPossible(bot))
    {
        context->GetValue<LootObject>("loot target")->Set(lootObj);
        botAI->DoSpecificAction("open loot", Event(), true);
    }
    ++data->visited;
    data->node = ObjectGuid();   // advance to the next node
    return true;
}
