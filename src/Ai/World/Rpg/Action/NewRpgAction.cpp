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

// rest-chair-sit-visual-hold: a seated bot has a fake session, so observers learn its sit ONLY from
// the UNIT_FIELD_BYTES_1 stand-state field, which re-broadcasts only on a real value change. A one-shot
// emote (REST's own cadence, or mod-ollama-chat's talk emote) renders the bot STANDING client-side
// without changing the byte, so SetStandState(same) sends nothing and it stays visually up. Force a
// fresh re-broadcast by flipping to an alternate SIT state and back: both are seated, so this never
// strips NOT_SEATED auras (food/drink) and never flickers (the field batches to one observer update per
// tick at the final value). Keeping the seat fresh also makes emotes auto-render their SEATED variant.
static void ForceResitBroadcast(Player* bot, uint8 seatState)
{
    uint8 alt = (seatState == UNIT_STAND_STATE_SIT) ? UNIT_STAND_STATE_SIT_MEDIUM_CHAIR
                                                    : UNIT_STAND_STATE_SIT;
    bot->SetStandState(alt);
    bot->SetStandState(seatState);
}

// rest-hub-unification: (beh,poi) -> EmotePalette row. Thin wrapper over the shared
// LookupPalette (NewRpgBaseAction.cpp), the single source of truth for the kPalette /
// kLoiterByPoi tables. The RPG_REST P2 phase reads only .sustainedPose from the result.
EmotePalette NewRpgStatusUpdateAction::PaletteOf(BotBehaviorId beh, BotCityPoi poi) const
{
    // BEH_LOITER resolves a per-POI row (variant = BotCityPoi 1..6); everything else uses
    // the default kPalette[beh] row. LookupPalette applies exactly this rule.
    return LookupPalette(beh, static_cast<uint8>(beh == BEH_LOITER ? poi : POI_NONE));
}

// rest-hub-unification: TAVERN/FIELD_REST seated hold — chair-seat-once then HOLD the pose by
// re-asserting the captured stand-state each tick (no re-Use / re-teleport), with a floor-sit
// fallback. Extracted VERBATIM (behavior-preserving) from the old RPG_REST Phase-3 block; now
// operates on the passed `rest`. NOTE the one behavioral nuance preserved exactly: when the chair
// is still being pathed to, MoveWorldObjectTo returns true and we RETURN OUT OF HoldSeat for the
// tick (the caller does no further work this tick) — this matches the old `return true` there.
void NewRpgStatusUpdateAction::HoldSeat(NewRpgInfo::Rest& rest)
{
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
                return;
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
            // Already seated: HOLD the pose. Re-broadcast each tick so a one-shot emote can't
            // leave the client rendering us standing (the byte never drifts, so a plain
            // re-assert is a no-op). See ForceResitBroadcast.
            if (sPlayerbotAIConfig.restSeatRebroadcast)
                ForceResitBroadcast(bot, rest.seatState);
            else if (bot->getStandState() != rest.seatState)
                bot->SetStandState(rest.seatState);
            chaired = true;
        }
    }

    // Floor fallback (no chair this tick): genuine ground-sit, re-asserted each tick so a
    // stray movement/reset can't leave the bot standing.
    if (!chaired)
    {
        if (sPlayerbotAIConfig.restSeatRebroadcast)
            ForceResitBroadcast(bot, UNIT_STAND_STATE_SIT);
        else if (bot->getStandState() != UNIT_STAND_STATE_SIT)
            bot->SetStandState(UNIT_STAND_STATE_SIT);
    }

    // Chaired: chair already holds the pose -> do one-shots ONLY (skip the EMOTE_STATE_SIT
    // re-assert that would fight the SIT_*_CHAIR stand-state). Floor: the BEH_REST palette
    // pose (EMOTE_STATE_SIT) is the held seated baseline alongside the stand-state sit.
    TickEmoteCadence(BEH_REST, 0, chaired);
}

// ───────────────────────────────────────────────────────────────────────────
// Task 8 — STROLL walk loop. Called from the RPG_REST hold phase (P3) each tick
// while rest.subtype == RS_STROLL. The episode's overall end is driven by P4's
// dwell check (GetMSTimeDiffToNow(rest.lastReach) >= rest.dwellMs); this function
// must NOT call ChangeTo* and must NOT index strollPts out of range.
//
// Pause convention: rest.strollPauseUntil stores the absolute getMSTime() value
// at which the pause ends. We test `getMSTime() < rest.strollPauseUntil` to
// decide "still pausing". This matches the pattern used elsewhere in the codebase
// (e.g. allowActiveCheckTimer comparisons in PlayerbotAI.cpp) — both sides of the
// comparison are uint32 getMSTime()-derived values and wrap identically, so the
// check is consistent.
//
// Arrival test: explicit bot->GetExactDist(x,y,z) < 10.0f, mirroring the
// TravelMount arrive threshold (NewRpgAction.cpp:363) and TravelToHubOrTeleport.
// MoveFarTo's return is NEVER used as an arrival predicate (R1).
// ───────────────────────────────────────────────────────────────────────────
void NewRpgStatusUpdateAction::TickStroll(NewRpgInfo::Rest& rest)
{
    if (rest.strollPts.empty())
        return;     // no waypoints — P4 dwell timer will end the episode naturally

    // Guard index against corruption (defensive; BuildStrollRoute sets idx=0 on init).
    if (rest.strollIdx >= rest.strollPts.size())
        rest.strollIdx = 0;

    // ── Pause phase: hold at the current POI until the pause expires ──
    if (rest.strollPauseUntil != 0 && getMSTime() < rest.strollPauseUntil)
        return;     // still pausing; do nothing this tick

    // Pause expired (or was never set) — reset the pause sentinel so we enter the
    // walk phase cleanly. If strollPauseUntil was just set last tick, clear it now.
    rest.strollPauseUntil = 0;

    // ── Walk phase: move toward the current waypoint ──
    WorldPosition& dest = rest.strollPts[rest.strollIdx];

    // Arrival check (explicit distance; NOT MoveFarTo's return).
    bool arrived = bot->GetExactDist(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()) <= 10.0f;
    if (!arrived)
    {
        MoveFarTo(dest);    // issue/continue movement — R1: return value ignored
        return;
    }

    // ── Arrived at this waypoint: begin a pause ──
    // Fire one-shot emote to signal the bot has "arrived and is looking around".
    FireOneShotEmote(BEH_WANDER_NPC, 0);

    // Set the pause end timestamp and advance to the next waypoint (wrapping).
    rest.strollPauseUntil = getMSTime() + sPlayerbotAIConfig.restHubStrollPausePerPoiSec * IN_MILLISECONDS;
    rest.strollIdx = static_cast<uint8>((rest.strollIdx + 1) % rest.strollPts.size());
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
            // GO_GRIND -> IDLE (WanderRandom removed in rest-hub-unification Task 9)
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // Deleted status — force IDLE immediately (rest-hub-unification Task 9).
            info.ChangeToIdle();
            return true;
        }
        case RPG_WANDER_NPC:
        {
            // Deleted status — force IDLE immediately (rest-hub-unification Task 9).
            info.ChangeToIdle();
            return true;
        }
        case RPG_PASTIME:
        {
            // Deleted status — force IDLE immediately (rest-hub-unification Task 9).
            EndSocialPastime(bot);
            info.ChangeToIdle();
            return true;
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
            // bot-rpg-bleed-suppression: bleed-guard. R2: ChangeToIdle reassigns the variant, so
            // return immediately and read no `rest&` afterward.
            if (ShouldSuppressRpg()) { info.ChangeToIdle(); return true; }
            // R2: take the substruct via get_if + null-guard, NEVER a throwing std::get<>.
            auto* restp = std::get_if<NewRpgInfo::Rest>(&info.data);
            if (!restp)
                return true;
            auto& rest = *restp;

            // P0 SELECT (once, when subtype unresolved)
            if (rest.subtype == RS_NONE)
            {
                WorldPosition hub = SelectRandomCampPos(bot);          // curated hub or empty
                bool hubReachable = (hub != WorldPosition());
                rest.hubPos = hub;
                if (!hubReachable)
                {
                    RestSubtype st = PickRestSubtype(false);           // anywhere subtype or FIELD_REST
                    rest.subtype = st;
                    if (st != RS_FIELD_REST && !AcquireSubtypeTarget(st)) rest.subtype = RS_FIELD_REST;
                }
                // hub path defers subtype pick to arrival (P2)
                return true;
            }

            // P1 TRAVEL + P2 ACQUIRE-ON-ARRIVAL (hub path; subtype stays RS_NONE until we actually arrive).
            if (rest.hubPos != WorldPosition() && rest.subtype == RS_NONE)
            {
                HubTravel t = TravelToHubOrTeleport(rest.hubPos);
                if (t == HUB_EN_ROUTE) return true;                    // still traveling -> stay in P1
                if (t == HUB_GIVE_UP)                                  // beyond budget -> field-rest in place
                { rest.hubPos = WorldPosition(); rest.subtype = RS_FIELD_REST; return true; }
                // HUB_ARRIVED -> P2 ACQUIRE
                RestSubtype st = PickRestSubtype(true);
                if (st != RS_FIELD_REST && !AcquireSubtypeTarget(st)) st = RS_FIELD_REST;
                rest.subtype = st;
                rest.sustainedPose = PaletteOf(kRestTable[st].palette, kRestTable[st].poiVariant).sustainedPose;
                return true;
            }

            // P3 ENGAGE + HOLD
            if (rest.lastReach == 0)
            {
                if (!EngageAndHold()) return true;   // still approaching OR fire-and-forget self-terminated (R2)
            }
            else
            {
                // Defensive: lastReach is only stamped (by EngageAndHold) for a resolved subtype, so
                // kRestTable[rest.subtype] below is always in range here — but guard anyway so a future
                // edit that stamps lastReach without resolving the subtype can't index with RS_NONE(0xFF).
                if (rest.subtype >= RS_COUNT)
                {
                    info.ChangeToIdle();
                    return true;                     // R2: nothing after this touches rest
                }

                if (rest.subtype == RS_TAVERN || rest.subtype == RS_FIELD_REST)
                    HoldSeat(rest);
                else if (rest.subtype == RS_STROLL)
                    TickStroll(rest);
                else
                {
                    bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, rest.sustainedPose);
                    TickEmoteCadence(kRestTable[rest.subtype].palette,
                                     static_cast<uint8>(kRestTable[rest.subtype].poiVariant));
                }

                // P4 EXIT
                if (GetMSTimeDiffToNow(rest.lastReach) >= rest.dwellMs)
                {
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                    bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
                    info.lastRestSubtype = rest.subtype;     // read rest BEFORE ChangeToIdle
                    info.ChangeToIdle();                     // R2: nothing after this touches rest
                }
            }
            return true;
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
