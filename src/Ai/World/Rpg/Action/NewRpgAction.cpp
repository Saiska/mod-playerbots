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
#include "ScriptMgr.h"
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

    // comedy-hold-dance: stale held-pose sweep. ANY exit from the social pastime (dwell end, partner
    // lost, OR an external yank — the bot-rpg-bleed-suppression guard below calls ChangeToIdle, which
    // does NOT clear emote-state) leaves status != RPG_PASTIME with our social emote-state still on the
    // unit. Clear it so a suppressed bot doesn't keep dancing/talking while it idles/follows. Sits ABOVE
    // the bleed guard so it still fires while the bot stays suppressed (status == RPG_IDLE). heldSocialEmote
    // lives on NewRpgInfo (not the Social variant) so it survives ChangeToIdle's reset and we can clean up.
    if (info.heldSocialEmote && info.GetStatus() != RPG_PASTIME)
    {
        if (bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) == info.heldSocialEmote)
            bot->ClearEmoteState();
        info.heldSocialEmote = 0;
    }

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

    // --- occupation lifecycle events: emit on any behaviorId edge (once-only by construction) ---
    // GetCurrentBehaviorId() is BEH_NONE while a pastime is still converging (start-only-when-real)
    // and the real BEH_* only once engaged; statuses report at intent. One diff per tick catches every edge.
    {
        BotBehaviorId curBeh = botAI->GetCurrentBehaviorId();
        if (curBeh != info.lastEmittedBehaviorId)
        {
            if (info.lastEmittedBehaviorId != BEH_NONE)
                sScriptMgr->OnPlayerbotActivityFinish(bot, static_cast<uint32>(info.lastEmittedBehaviorId));
            if (curBeh != BEH_NONE)
                sScriptMgr->OnPlayerbotActivityStart(bot, static_cast<uint32>(curBeh));
            info.lastEmittedBehaviorId = curBeh;
        }
    }

    // bot-rpg-bleed-suppression: on-task guard. Suppress autonomous NewRpg when the bot is not free
    // to idle (dungeon/raid/BG, transport/vehicle, grouped-with-human, RaidSim, combat, dead, mid-port).
    // This sits HERE, at the machine head — NOT inside CheckRpgStatusAvailable: vetoing every status
    // there hits RandomChangeStatus's "nothing available" branch, which falls back to ChangeToRest()
    // (sit) — i.e. a busy bot would sit down. Forcing RPG_IDLE is the correct suppression.
    if (sPlayerbotAIConfig.rpgSuppressWhenBusy && ShouldSuppressRpg())
    {
        NewRpgStatus cur = info.GetStatus();
        // In-progress travel legs (taxi flight / mount travel) are allowed to FINISH — yanking a
        // flight mid-air can strand the bot; on arrival it returns to IDLE and is held below.
        if (cur != RPG_TRAVEL_FLIGHT && cur != RPG_TRAVEL_MOUNT)
        {
            if (cur != RPG_IDLE)
                info.ChangeToIdle();   // interrupt ground comedy/idle/errand cleanly
            return true;               // never enter a new status while on-task
        }
    }

    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_PASTIME,
                                       RPG_DO_QUEST, RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP, RPG_TRAVEL_MOUNT,
                                       RPG_GATHERING_CIRCUIT});

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
            auto& rest = std::get<NewRpgInfo::Rest>(info.data);

            // Phase 1 — travel to the inn, then arrive once. rest.lastReach==0 => en route / unresolved.
            if (!rest.lastReach)
            {
                // InnPull: walk to the innkeeper hub chosen on REST entry. MoveFarTo returns true while
                // still moving toward it (mirrors NewRpgGoGrindAction). No dest / pull off => arrive here.
                if (sPlayerbotAIConfig.restInnPullEnable && rest.pos != WorldPosition() && MoveFarTo(rest.pos))
                    return true;

                // Arrived (or resting in place): sit, resolve a nearby chair once, set the dwell clock.
                rest.lastReach = getMSTime();
                if (bot->getStandState() == UNIT_STAND_STATE_STAND)
                    bot->SetStandState(UNIT_STAND_STATE_SIT);
                rest.chair = SelectInnChair(15.0f);
                if (!rest.chair)
                    LOG_DEBUG("playerbots", "[New RPG] {} rest: seated floor (no chair in range)", bot->GetName());
                uint32 mn = sPlayerbotAIConfig.restDwellMin;
                uint32 mx = sPlayerbotAIConfig.restDwellMax;
                if (mx < mn) std::swap(mn, mx);
                rest.dwellMs = urand(mn, mx) * IN_MILLISECONDS;
            }

            // Phase 2 — dwell expiry, measured FROM ARRIVAL (so travel time doesn't eat the dwell).
            if (GetMSTimeDiffToNow(rest.lastReach) >= rest.dwellMs)
            {
                // Leave REST in a neutral pose: the chair Use set a SIT_*_CHAIR stand-state
                // and/or TickEmoteCadence held EMOTE_STATE_SIT — clear both so the bot stands.
                if (bot->getStandState() != UNIT_STAND_STATE_STAND)
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                bot->ClearEmoteState();
                rest.chair = ObjectGuid();
                rest.onChair = false;
                rest.seatState = 0;
                info.ChangeToIdle();
                return true;
            }

            // Phase 3 — seated hold: seat on a chair ONCE, then HOLD the pose by re-asserting the
            // captured stand-state each tick (no re-Use / re-teleport). The old code re-Used every
            // tick whenever standState read < SIT_LOW_CHAIR; the chair Use TeleportTo's the bot, so
            // any reset re-fired the teleport -> flicker -> bot stands on the chair. Floor-sit fallback.
            bool chaired = false;
            if (rest.chair)
            {
                GameObject* chair = ObjectAccessor::GetGameObject(*bot, rest.chair);
                if (!chair || !chair->isSpawned())
                {
                    rest.chair = ObjectGuid();   // chair despawned -> floor fallback
                    rest.onChair = false;
                    LOG_DEBUG("playerbots", "[New RPG] {} rest: seated floor (chair despawned)", bot->GetName());
                }
                else if (!rest.onChair && bot->GetExactDist(chair) > INTERACTION_DISTANCE)
                {
                    // Not seated yet: path into seating range; keep moving toward the chair this tick.
                    if (MoveWorldObjectTo(rest.chair))
                        return true;
                    // Pathing failed (offset in geometry); abandon the chair and floor-sit.
                    rest.chair = ObjectGuid();
                    LOG_DEBUG("playerbots", "[New RPG] {} rest: seated floor (chair pathing failed)", bot->GetName());
                }
                else if (!rest.onChair)
                {
                    // In range, not yet seated: perform the chair Use EXACTLY ONCE. A successful Use
                    // teleports the bot onto a free slot and sets UNIT_STAND_STATE_SIT_LOW_CHAIR(4)..
                    // SIT_HIGH_CHAIR(6); capture that value so we re-assert it later without re-Use.
                    chair->Use(bot);
                    if (bot->getStandState() >= UNIT_STAND_STATE_SIT_LOW_CHAIR)
                    {
                        rest.onChair = true;
                        rest.seatState = bot->getStandState();
                        chaired = true;
                        LOG_DEBUG("playerbots", "[New RPG] {} rest: seated chair (seatState={})",
                                  bot->GetName(), uint32(rest.seatState));
                    }
                    else
                    {
                        rest.chair = ObjectGuid();   // slot full / Use failed -> floor fallback
                        LOG_DEBUG("playerbots", "[New RPG] {} rest: seated floor (chair Use failed)", bot->GetName());
                    }
                }
                else
                {
                    // Already seated: HOLD the pose. Re-assert the captured chair stand-state if
                    // anything cleared it. Cheap replicated field set -> NO teleport, no flicker.
                    if (bot->getStandState() != rest.seatState)
                        bot->SetStandState(rest.seatState);
                    chaired = true;
                }
            }

            // Floor fallback (no chair this tick): genuine ground-sit, re-asserted each tick so a
            // stray movement/reset can't leave the bot standing.
            if (!chaired)
            {
                if (bot->getStandState() != UNIT_STAND_STATE_SIT)
                    bot->SetStandState(UNIT_STAND_STATE_SIT);
            }

            // Chaired: chair already holds the pose -> do one-shots ONLY (skip the EMOTE_STATE_SIT
            // re-assert that would fight the SIT_*_CHAIR stand-state). Floor: the BEH_REST palette
            // pose (EMOTE_STATE_SIT) is the held seated baseline alongside the stand-state sit.
            TickEmoteCadence(BEH_REST, 0, chaired);
            return true;   // HOLD so movement AI doesn't walk the resting bot off
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

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    // Micro-halt: only at the arrival seam (not already moving) so we never force-stop a moving bot
    // (bug #2). ForceToWait records a movement-wait state -> the bot holds; the stroll resumes when it
    // expires. While ForceToWait is active, IsWaitingForLastMove() is true so we fall through to
    // MoveRandomNear() which early-outs -> the bot stays put for the halt duration.
    // rpgInfo.data may not hold the WanderRandom alternative at execute time (a status/data
    // transition window), so use the checked accessor like every other action in this file. A raw
    // std::get<> here throws std::bad_variant_access, which is uncaught on the MapUpdater worker
    // thread and terminates the process (crash 2026-06-11 18:32).
    if (sPlayerbotAIConfig.wanderMicroHaltEnable && !IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        if (auto* data = std::get_if<NewRpgInfo::WanderRandom>(&botAI->rpgInfo.data))
        {
            uint32 now = getMSTime();
            if (data->nextHaltMs == 0)
            {
                // first arrival this wander spell: stroll first, schedule the first halt
                data->nextHaltMs = now + urand(sPlayerbotAIConfig.wanderMicroHaltGapMin,
                                               sPlayerbotAIConfig.wanderMicroHaltGapMax) * IN_MILLISECONDS;
            }
            else if (now >= data->nextHaltMs)
            {
                uint32 durMs = urand(sPlayerbotAIConfig.wanderMicroHaltDurationMin,
                                     sPlayerbotAIConfig.wanderMicroHaltDurationMax) * IN_MILLISECONDS;
                ForceToWait(durMs, MovementPriority::MOVEMENT_NORMAL);   // bug-#2-safe hold
                FireOneShotEmote(BEH_WANDER_RANDOM, 0);                  // one immediate look-around emote
                data->nextHaltMs = now + durMs + urand(sPlayerbotAIConfig.wanderMicroHaltGapMin,
                                                       sPlayerbotAIConfig.wanderMicroHaltGapMax) * IN_MILLISECONDS;
                return true;
            }
        }
    }

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
        {
            TickEmoteCadence(BEH_WANDER_NPC, 0);   // dwell-facing-NPC hold
            return false;
        }

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
                // Per-POI sustained pose + cadence one-shots now live in the EmotePalette
                // table (kLoiterByPoi, keyed by BotCityPoi via data.poiType). The helper
                // dismounts and sets the table pose; one-shots are gated by EmoteCadence.Enable.
                TickEmoteCadence(BEH_LOITER, data.poiType);
                return true;
            }
            if (GetMSTimeDiffToNow(data.lastReach) < data.dwellMs)
            {
                // Re-assert the table pose (movement/idle AI resets it) + emit cadence
                // one-shots. Dismount + pose hold are handled inside TickEmoteCadence.
                TickEmoteCadence(BEH_LOITER, data.poiType);
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
        TickEmoteCadence(BEH_CRAFT, 0);   // table holds USE_STANDING as the sustained pose; cadence layers one-shots
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
        {
            TickEmoteCadence(BEH_REPAIR_SELL, 0);   // post-arrival dwell hold at the vendor
            return false;
        }
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
        // comedy-hold-dance: roll this session's held pose once — mostly converse, sometimes dance.
        info.heldSocialEmote = (urand(0, 99) < sPlayerbotAIConfig.pastimeSocialDancePct)
                               ? EMOTE_STATE_DANCE : EMOTE_STATE_TALK;
    }
    if (GetMSTimeDiffToNow(data.lastReach) >= data.dwellMs)
    {
        EndSocialPastime(bot);
        info.ChangeToIdle();
        return true;
    }
    bot->SetFacingToObject(target);
    // comedy-hold-dance: HOLD a sustained social pose so the bot reads as socializing for the whole
    // dwell. The old return-false + no held pose let the movement AI fidget the bot between the 3-6s
    // one-shots ("doesn't stick"). A mount hides the pose, so dismount first (mirrors TickEmoteCadence).
    // Re-assert the held emote-state every tick; skipSustainedPose=true => the cadence helper does
    // one-shots ONLY (we own the pose), exactly like RPG_REST's chaired case.
    if (bot->IsMounted())
        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
    if (bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) != info.heldSocialEmote)
        bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, info.heldSocialEmote);
    TickEmoteCadence(BEH_SOCIAL, 0, /*skipSustainedPose=*/true);
    return true;   // HOLD: block the movement AI from walking the socializing bot off its pose
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
        // Arrival/holding nudge: pathing couldn't advance, so the bot is effectively
        // parked. BEH_TRAVEL_MOUNT's palette is {nullptr,0} (a mount hides any pose),
        // so this is a no-op today but keeps travel on the shared cadence call site.
        TickEmoteCadence(BEH_TRAVEL_MOUNT, 0);
        return MoveRandomNear(10.0f);
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
    // arrived: harvest — mirrors the loot-harvest delegation
    LootObject lootObj(bot, data->node);
    if (lootObj.IsLootPossible(bot))
    {
        context->GetValue<LootObject>("loot target")->Set(lootObj);
        botAI->DoSpecificAction("open loot", Event(), true);
    }
    TickEmoteCadence(BEH_GATHERING_CIRCUIT, 0);   // between-node pause at the harvested node
    ++data->visited;
    data->node = ObjectGuid();   // advance to the next node
    return true;
}
