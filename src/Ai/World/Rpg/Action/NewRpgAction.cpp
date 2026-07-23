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
#include "MapMgr.h"
#include "RandomPlayerbotMgr.h"
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

// rest-sit-render-diagnostic (TEMP, gated default-off): true at most once per ~3 s per bot when the
// diag is on and (no name filter OR this bot's name matches). Stamps the throttle. Read-only.
static bool SitDiagDue(Player* bot, NewRpgInfo& info)
{
    if (!sPlayerbotAIConfig.restSitDiagLog)
        return false;
    std::string const& want = sPlayerbotAIConfig.restSitDiagName;
    if (!want.empty() && bot->GetName() != want)
        return false;
    if (info.sitDiagLastMs != 0 && GetMSTimeDiffToNow(info.sitDiagLastMs) < 3000)
        return false;
    info.sitDiagLastMs = getMSTime();
    return true;
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
void NewRpgStatusUpdateAction::HoldSeat(NewRpgInfo::Rest& rest, bool diag)
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
            {
                if (diag)
                    LOG_INFO("playerbots", "[SitDiag] {} HoldSeat branch=PATHING ss-after={}",
                             bot->GetName(), uint32(bot->getStandState()));
                return;
            }
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

    // rest-sit-standstate-render: skip the BEH_REST sustained pose (EMOTE_STATE_SIT) on BOTH the
    // chaired AND floor paths. EMOTE_STATE_SIT has no player-model seated animation, so a non-zero
    // UNIT_NPC_EMOTESTATE masks the real seat render (the UNIT_STAND_STATE_SIT byte) -> the bot
    // renders standing. The seat is held purely via the stand-state (re-asserted above /
    // ForceResitBroadcast); the floor branch needs only the rest one-shots, not the
    // never-rendering, strand-leaving sustained pose. (chaired already skipped; now floor does too.)
    if (diag)
        LOG_INFO("playerbots", "[SitDiag] {} HoldSeat branch={} ss-after={} em-after={}",
                 bot->GetName(),
                 chaired ? (rest.onChair ? "CHAIR_HOLD" : "USE") : "FLOOR",
                 uint32(bot->getStandState()),
                 bot->GetUInt32Value(UNIT_NPC_EMOTESTATE));
    TickEmoteCadence(BEH_REST, 0, /*skipSustainedPose=*/true);
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

    // zone-change clear: wipe the lowPriorityQuest blacklist when the bot moves to a new zone so
    // stalls in the old zone don't permanently haunt quests that may be completable in the new area.
    uint32 const curZone = bot->GetZoneId();
    if (info.lastZoneId != curZone)
    {
        if (info.lastZoneId != 0)
            botAI->ClearLowPriorityQuests();
        info.lastZoneId = curZone;
    }

    // social-dance-resthub-revive: general stale rest-pose sweep. The rest engine holds
    // UNIT_NPC_EMOTESTATE for posed subtypes (SOCIAL dance/talk, vendor/trainer talk, craft
    // use-standing). The normal P4 exit clears it, but an external yank — the bleed-suppression
    // guard below calls ChangeToIdle (pure-data, no unit access) — leaves status != RPG_REST with
    // the pose still on the unit. Clear it (equality-gated so we never stomp a foreign pose) so a
    // suppressed/idling bot doesn't keep dancing/talking while it follows. heldRestPose lives on
    // NewRpgInfo so it survives ChangeToIdle's variant reset. ABOVE the bleed guard so it fires
    // while the bot stays suppressed (status == RPG_IDLE).
    if (info.heldRestPose && info.GetStatus() != RPG_REST)
    {
        if (bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) == info.heldRestPose)
            bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
        info.heldRestPose = 0;
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
    // bot-guild-activity-status: surface occupation (+sub-state, +zone) in the guild note / debug chat.
    // OUTSIDE the behavior-edge guard so sub-state resolution and zone changes also update. Self-gated.
    botAI->UpdateGuildActivityStatus();

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

    // upkeep-on-group-leave: consume the flag the GroupScript stamped (world thread) when this bot left a
    // group. We are on the bot's OWN worker tick here — the only place ChangeToUpkeep may safely reassign
    // the RPG variant. Sits AFTER the suppression guard, so a still-busy bot (in instance / grouped-with-
    // human / RaidSim) keeps the flag and enters upkeep only once it's free. Deferred while in combat.
    if (info.pendingUpkeepMs && !bot->IsInCombat())
    {
        info.pendingUpkeepMs = 0;
        if (info.GetStatus() != RPG_UPKEEP)
        {
            LOG_INFO("playerbots", "[UpkeepOnGroupLeave] {} left a group in zone {} — entering upkeep run",
                     bot->GetName(), bot->GetZoneId());
            info.ChangeToUpkeep();   // CRASH RULE: last statement touching info before return
            return true;
        }
    }

    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
        {
            if (NewRpgInfo::Idle* idle = std::get_if<NewRpgInfo::Idle>(&info.data))
                if (idle->dwellMs && GetMSTimeDiffToNow(info.startT) < idle->dwellMs)
                    return true;   // still dwelling — skip the occupation-availability sweep
            // NEEDS→DECIDE resolver (occupation-state-machine Task 4) replaces the satiation roulette.
            Decide();
            return true;
        }

        case RPG_GO_GRIND:
        {
            // CRASH RULE: get_if + null-guard, NEVER a throwing std::get<> — a bad_variant_access
            // on a MapUpdater worker terminates the world.
            auto* gp = std::get_if<NewRpgInfo::GoGrind>(&info.data);
            if (!gp)
                return true;
            WorldPosition& originalPos = gp->pos;
            assert(gp->pos != WorldPosition());
            // GO_GRIND finish — occupation-state-machine Task 6: stamp lastFinished + re-Decide,
            // not ChangeToIdle. lastFinished is a flat array field (safe to write pre-Decide);
            // Decide() reassigns the variant, so return immediately and read no gp->/data after.
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.lastFinished[RPG_GO_GRIND] = getMSTime();
                Decide();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST finish — Task 6: stamp lastFinished + re-Decide (flat field pre-Decide; CRASH RULE).
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.lastFinished[RPG_DO_QUEST] = getMSTime();
                Decide();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            // Task 6: TRAVEL_FLIGHT is no longer a DECIDE pick — only a travel sub-leg. On arrival
            // re-Decide (not ChangeToIdle) so the spine immediately picks an occupation. No
            // lastFinished stamp — a travel leg is not an occupation. CRASH RULE: get_if + null-guard.
            auto* fp = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
            if (!fp)
                return true;
            if (fp->inFlight && !bot->IsInFlight())
            {
                if (sPlayerbotAIConfig.rpgSuppressWhenBusy && ShouldSuppressRpg())
                {
                    info.ChangeToIdle();
                    return true;   // suppressed: freeze at IDLE; the IDLE boundary re-Decides when free
                }
                Decide();
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

            // rest-sit-render-diagnostic (TEMP): one read-only line per throttled tick, all phases.
            bool sitDiag = SitDiagDue(bot, info);
            if (sitDiag)
            {
                char const* phase = (rest.hubArriveT != 0 && rest.subtype == RS_NONE) ? "SETTLE"
                                  : (rest.lastReach == 0)                              ? "APPROACH"
                                  : (rest.subtype == RS_TAVERN || rest.subtype == RS_FIELD_REST) ? "HOLD"
                                  : (rest.subtype == RS_STROLL)                        ? "STROLL"
                                  : "OTHER";
                char const* sub = (rest.subtype < RS_COUNT) ? kRestTable[rest.subtype].name : "NONE";
                LOG_INFO("playerbots",
                    "[SitDiag] {} status=REST subtype={} phase={} lastReach={} dwellMs={} onChair={} chair={} ss={} em={}",
                    bot->GetName(), sub, phase,
                    rest.lastReach == 0 ? std::string("enroute") : std::to_string(GetMSTimeDiffToNow(rest.lastReach)),
                    rest.dwellMs, uint32(rest.onChair ? 1 : 0), uint32(rest.chair ? 1 : 0),
                    uint32(bot->getStandState()), bot->GetUInt32Value(UNIT_NPC_EMOTESTATE));
            }

            // P2 ACQUIRE-ON-ARRIVAL — after a short settle so the post-teleport grid + the
            // NearestUnitsValue caches (checkInterval ~1s) have refreshed for the new location.
            if (rest.hubArriveT != 0 && rest.subtype == RS_NONE)
            {
                if (GetMSTimeDiffToNow(rest.hubArriveT) < 2500)        // settle window (~grid load + cache refresh)
                    return true;
                RestSubtype picked = PickRestSubtype(true);
                bool acq = (picked == RS_FIELD_REST) ? true : AcquireSubtypeTarget(picked);
                RestSubtype st = acq ? picked : RS_FIELD_REST;
                rest.subtype = st;
                rest.sustainedPose = PaletteOf(kRestTable[st].palette, kRestTable[st].poiVariant).sustainedPose;
                // upkeep-sociability: per-bot held-pose roll for non-seated posed REST subtypes
                // (TAVERN/FIELD_REST sit via HoldSeat+skipSustainedPose — leave them on their seat logic).
                if (st != RS_TAVERN && st != RS_FIELD_REST)
                    rest.chosenDwellPose = ResolveHeldPose(kRestTable[st].palette,
                                                           static_cast<uint8>(kRestTable[st].poiVariant),
                                                           (uint8)urand(0, 5));
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
                    HoldSeat(rest, sitDiag);
                else if (rest.subtype == RS_STROLL)
                    TickStroll(rest);
                else if (rest.subtype == RS_FISH)
                    TickFish();
                else
                {
                    // upkeep-sociability: use the per-bot rolled pose if available, else fall back to sustainedPose.
                    uint32 pose = (rest.chosenDwellPose != 0xFFFFFFFF) ? rest.chosenDwellPose : rest.sustainedPose;
                    // A mount hides a held pose; dismount before asserting a non-zero one
                    // (mirrors comedy-hold-dance + TickEmoteCadence's sustained-pose guard).
                    if (pose && bot->IsMounted())
                        bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                    // upkeep-sociability fix: pose 0 means "plain stand" — restore the old unconditional
                    // scrub so a stale UNIT_NPC_EMOTESTATE from a prior pose doesn't persist through the dwell.
                    if (!pose && bot->GetUInt32Value(UNIT_NPC_EMOTESTATE))
                        bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
                    info.heldRestPose = pose;   // track for the head sweep (0 = none, harmless)
                    TickEmoteCadence(kRestTable[rest.subtype].palette,
                                     static_cast<uint8>(kRestTable[rest.subtype].poiVariant),
                                     /*skipSustainedPose=*/false, pose);
                }

                // P4 EXIT — Task 6: REST is an occupation, so stamp lastFinished[RPG_REST] + re-Decide
                // (not ChangeToIdle). Read every rest field BEFORE Decide(); lastFinished is a flat
                // array (safe to write pre-Decide). CRASH RULE: nothing reads rest after Decide().
                if (GetMSTimeDiffToNow(rest.lastReach) >= rest.dwellMs)
                {
                    bot->SetStandState(UNIT_STAND_STATE_STAND);
                    bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
                    info.heldRestPose = 0;                   // clean exit cleared the pose; sweep has nothing to do
                    info.lastRestSubtype = rest.subtype;     // read rest BEFORE Decide()
                    info.lastFinished[RPG_REST] = getMSTime();
                    Decide();                                // R2: nothing after this touches rest
                    return true;
                }
            }
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            // OutdoorPvp finish — Task 6: stamp lastFinished + re-Decide (flat field; CRASH RULE).
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.lastFinished[RPG_OUTDOOR_PVP] = getMSTime();
                Decide();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_MOUNT:
        {
            // Task 6: TRAVEL_MOUNT is no longer a DECIDE pick — only a travel sub-leg. On arrival
            // (or duration) re-Decide (not ChangeToIdle). No lastFinished stamp — it's a travel leg,
            // not an occupation. CRASH RULE: get_if + null-guard.
            auto* mp = std::get_if<NewRpgInfo::TravelMount>(&info.data);
            if (!mp)
                return true;
            if (bot->GetExactDist(mp->pos) < 10.0f || info.HasStatusPersisted(statusTravelMountDuration))
            {
                if (sPlayerbotAIConfig.rpgSuppressWhenBusy && ShouldSuppressRpg())
                {
                    info.ChangeToIdle();
                    return true;   // suppressed: freeze at IDLE; the IDLE boundary re-Decides when free
                }
                Decide();
                return true;
            }
            break;
        }
        case RPG_GATHERING_CIRCUIT:
        {
            // GatheringCircuit finish — Task 6: stamp lastFinished + re-Decide (flat field; CRASH RULE).
            if (info.HasStatusPersisted(sPlayerbotAIConfig.gatheringCircuitDurationSec * IN_MILLISECONDS))
            {
                info.lastFinished[RPG_GATHERING_CIRCUIT] = getMSTime();
                Decide();
                return true;
            }
            break;
        }
        case RPG_RECOVER:
        {
            // occupation-state-machine Task 5 — LAYER-1 RECOVER (in place, no travel).
            // CRASH RULE: get_if + null-guard; every Decide()/ChangeTo* is followed by an
            // immediate return and touches no variant data afterward.
            auto* recp = std::get_if<NewRpgInfo::Recover>(&info.data);
            if (!recp)
                return true;
            auto& rec = *recp;

            // EXIT — recovered (or a non-mana class with HP restored). ManaLow() self-gates to
            // mana-using classes, so this naturally ignores mana on warriors/rogues/etc.
            if (!HealthLow() && !ManaLow())
            {
                Decide();
                return true;   // CRASH RULE: nothing reads rec after Decide()
            }

            // Bounded dwell so a bot that cannot recover (no food/drink/bandage) does not spin
            // forever in RECOVER — after the window, hand back to Decide() regardless.
            if (rec.dwellMs == 0)
                rec.dwellMs = 30000;   // ~30s recovery window
            if (GetMSTimeDiffToNow(info.startT) >= rec.dwellMs)
            {
                Decide();
                return true;   // CRASH RULE
            }

            // Perform ONE recovery step this tick (reuse existing arms — invent no healing logic):
            //   "try emergency" → bandage (out of combat) or a healing consumable (ImbueAction.cpp:202).
            //   "food"  → eat to regen HP out of combat (ActionContext.h:120).
            //   "drink" → drink to regen mana (ActionContext.h:121).
            if (HealthLow())
            {
                botAI->DoSpecificAction("try emergency");
                botAI->DoSpecificAction("food");
            }
            if (ManaLow())
                botAI->DoSpecificAction("drink");
            return true;
        }
        case RPG_UPKEEP:
        {
            // occupation-upkeep-two-tier — UPKEEP routes by tier (rolled in ChangeToUpkeep):
            //   LOCAL  : zone hub -> sell -> maintenance -> inn rest.
            //   CAPITAL: capital anchor -> errand chain + city poses (Task 7).
            // CRASH RULE: get_if + null-guard; every Decide()/ChangeTo* immediately returns.
            auto* upkp = std::get_if<NewRpgInfo::Upkeep>(&info.data);
            if (!upkp)
                return true;
            auto& up = *upkp;

            // upkeep-share-reduction fix: active-bot stall watchdog (parity with DO_QUEST/PVP/GATHER;
            // UPKEEP previously had none). Inactive bots are rescued by the rel-100 escape action; this
            // catches a genuinely-stuck ACTIVE bot. Long ceiling so it never aborts a legit capital
            // episode (~15-30m). CRASH RULE: Decide() is the last statement; nothing reads up/data after.
            if (info.HasStatusPersisted(sPlayerbotAIConfig.upkeepActiveWatchdogSec * IN_MILLISECONDS))
            {
                info.lastUpkeepMs = getMSTime();
                Decide();
                return true;
            }

            // ACQUIRE the hub once (step 0), honoring the tier + the LOCAL->CAPITAL fallthrough.
            if (up.hubPos == WorldPosition())
            {
                // Floating-capital in-place: if the bot is ALREADY standing in a no-ground-egress capital
                // (Dalaran), do the CAPITAL upkeep right here. Selecting a different/weighted hub would make
                // it travel out, tripping the MoveFarTo no-ground-egress teleport (NewRpgBaseAction.cpp:321)
                // that blinks the bot to a hostile Northrend zone where it dies before ever reaching the
                // sell/redeem step (observed: ungroup-in-Dalaran -> Icecrown -> death). Staying put runs the
                // errand chain + token redeem in place.
                // 4395 = Dalaran, the only no-ground-egress capital (canonical:
                // IsNoGroundEgressCapitalZone in NewRpgBaseAction.cpp — file-static, so inlined here).
                if (bot->GetZoneId() == 4395)
                {
                    up.tier = NewRpgInfo::UPKEEP_TIER_CAPITAL;
                    up.hubPos = WorldPosition(bot);     // current position — no travel, no egress
                    up.capitalZone = bot->GetZoneId();  // Dalaran (4395) — pose props resolve here
                }
                else if (up.tier == NewRpgInfo::UPKEEP_TIER_LOCAL)
                {
                    up.hubPos = SelectRandomCampPos(bot);
                    if (up.hubPos == WorldPosition())
                    {
                        // No reachable zone hub — fall through to the capital tier (universal
                        // backstop). This is the path that kills the old "NO reachable hub" trickle.
                        up.tier = NewRpgInfo::UPKEEP_TIER_CAPITAL;
                        up.hubPos = SelectCapitalHubAndZone(bot, up.capitalZone);
                    }
                }
                else
                    up.hubPos = SelectCapitalHubAndZone(bot, up.capitalZone);

                if (up.hubPos == WorldPosition())
                {
                    // True should-never-fire: not even a capital is reachable (capital-cache boot
                    // bug, not a runtime condition). Do the maintenance in place so the bot is not
                    // stranded, then hand back to Decide().
                    LOG_ERROR("playerbots",
                              "[RpgMachine] {} #{} map={} zone={} — UPKEEP no capital reachable; "
                              "in-place maintenance + Decide()",
                              bot->GetName(), bot->GetGUID().GetCounter(), bot->GetMapId(),
                              bot->GetZoneId());
                    botAI->DoSpecificAction("maintenance");
                    info.lastUpkeepMs = getMSTime();   // flat field — safe to stamp pre-Decide
                    Decide();
                    return true;   // CRASH RULE
                }

                // upkeep-workload-scaling: snapshot bag-fullness ONCE now (bot/botAI live here;
                // ChangeToUpkeep has no bot*). Roll each cosmetic step's keep/skip once so the
                // episode is deterministic. Enable=0 leaves the defaults (100 / all-keep) => no-op.
                if (sPlayerbotAIConfig.upkeepWorkloadScaleEnable)
                {
                    up.workloadPct = AI_VALUE(uint8, "bag space");
                    up.cosmeticKeep = 0;
                    bool const cap = (up.tier == NewRpgInfo::UPKEEP_TIER_CAPITAL);
                    // capital cosmetic steps {2,3,4,5,8}; local cosmetic step {3}
                    uint8 const steps[] = { 2, 3, 4, 5, 8 };
                    for (uint8 s : steps)
                    {
                        if (!cap && s != 3)
                            continue;   // LOCAL: only step 3 is cosmetic
                        if (roll_chance_f(up.workloadPct))
                            up.cosmeticKeep |= (uint16)(1u << s);
                    }
                    LOG_INFO("playerbots",
                             "[UpkeepWorkload] {} bagSpace={} keptCosmetics={:#06x} tier={}",
                             bot->GetName(), uint32(up.workloadPct), up.cosmeticKeep,
                             cap ? "CAPITAL" : "LOCAL");
                }

                up.step = 1;   // 0 = acquire done; pipelines start at step 1
                return true;
            }

            if (up.tier == NewRpgInfo::UPKEEP_TIER_CAPITAL)
                return TickUpkeepCapital(up);   // Task 7
            return TickUpkeepLocal(up);
        }
        default:
            break;
    }
    return false;
}

// Shared dwell + one-shot-action primitive for BOTH upkeep tiers (occupation-upkeep-two-tier).
// On the step's ENTRY tick (up.stepStartMs == 0): stamp the clock, set the randomized dwell, fire
// `action` EXACTLY ONCE (so sell/maintenance are not re-issued every tick), then return false (hold).
// On later ticks: issue nothing, return true once the dwell has elapsed.
bool NewRpgStatusUpdateAction::UpkeepDwell(NewRpgInfo::Upkeep& up, uint32 secs, std::string const& action,
                                           bool vendorEvent)
{
    if (up.stepStartMs == 0)
    {
        up.stepStartMs = getMSTime();
        // upkeep-workload-scaling: shrink the dwell by bag fullness (floor keeps it visible).
        float const wf = sPlayerbotAIConfig.upkeepWorkloadScaleEnable
                             ? std::max(up.workloadPct / 100.0f, sPlayerbotAIConfig.upkeepWorkloadDwellFloor)
                             : 1.0f;
        up.dwellMs = (uint32)(secs * IN_MILLISECONDS * wf);
        if (!action.empty())
        {
            // "sell" must carry the vendor event so SellAction takes the RS_VENDOR path (which also
            // auto-fires the guild-bank DEPOSIT via TryDepositLootToGuildBank). Mirrors the form used
            // by the rest engine's RS_VENDOR arm (NewRpgRestHub.cpp:377-380).
            if (vendorEvent)
                botAI->DoSpecificAction(action, Event("rpg action", "vendor"), true);
            else
                botAI->DoSpecificAction(action);
        }
        return false;
    }
    // upkeep-sociability: errand dwell shouldn't be a frozen statue — light loiter cadence.
    TickEmoteCadence(BEH_LOITER, (uint8)POI_NONE);
    return GetMSTimeDiffToNow(up.stepStartMs) >= up.dwellMs;
}

// LOCAL upkeep: travel to the zone hub, sell (+guild deposit) -> maintenance -> the shared inn rest.
// Steps 1..3 (step 0 = acquire, handled in the RPG_UPKEEP case). CRASH RULE: every Decide() is the
// last statement before return; nothing reads `up` after a Decide().
bool NewRpgStatusUpdateAction::TickUpkeepLocal(NewRpgInfo::Upkeep& up)
{
    // Only the SELL/MAINT errands need the bot anchored at the hub; the questgiver pose and the
    // finish/coda drive their own movement (or none).
    if (up.step <= 2 && bot->GetExactDist(up.hubPos) > 10.0f)
    {
        if (DriveTravel(up.hubPos) == TravelResult::EN_ROUTE)
            return true;
    }

    switch (up.step)
    {
        case 1:   // SELL (+guild-bank deposit) — fire once, hold a randomized dwell.
        {
            if (up.stepStartMs == 0 && sPlayerbotAIConfig.tokenRedeemEnable)
                botAI->DoSpecificAction("redeem currency");   // synthetic, in-place; once, before the sell/deposit dwell
            if (!UpkeepDwell(up, urand(sPlayerbotAIConfig.upkeepSellMinSec, sPlayerbotAIConfig.upkeepSellMaxSec),
                             "sell", true))
                return true;
            up.step = 2;
            up.stepStartMs = 0;
            return true;
        }
        case 2:   // MAINTENANCE (NPC-less restock + guild-bank withdraw + gear floor) — fire once, hold.
        {
            if (!UpkeepDwell(up, urand(sPlayerbotAIConfig.upkeepMaintMinSec, sPlayerbotAIConfig.upkeepMaintMaxSec),
                             "maintenance"))
                return true;
            up.step = 3;
            up.stepStartMs = 0;
            return true;
        }
        case 3:   // QUESTGIVER — cosmetic loiter pose (skip if none nearby).
        {
            if (PoseAtNearbyNpc(UNIT_NPC_FLAG_QUESTGIVER,
                                urand(sPlayerbotAIConfig.upkeepQuestGiverMinSec,
                                      sPlayerbotAIConfig.upkeepQuestGiverMaxSec) * IN_MILLISECONDS, up))
                return true;
            up.step = 4;
            up.target.Clear();
            up.stepStartMs = 0;
            return true;
        }
        case 4:   // TERMINAL — post-upkeep rest coda.
        default:
            return TickUpkeepFinish(up);
    }
}


// CAPITAL upkeep: sell, cosmetic city poses (bank/AH/mail/trainer), maintenance, a gated combat-dummy
// tail, then the post-upkeep rest coda. Steps 1..8 (step 0 = acquire, handled in the RPG_UPKEEP case).
// PoseAtProp drives its own intra-city hop to each prop; only the leading SELL/MAINTENANCE errands
// need the bot anchored at the capital hub, so the initial hub travel is gated on step 1. CRASH RULE:
// `up` is the caller's already-get_if'd reference; no Decide()/ChangeTo* lives here (the terminal
// rest coda, which owns those ops, is reached via TickUpkeepFinish at step 8).
bool NewRpgStatusUpdateAction::TickUpkeepCapital(NewRpgInfo::Upkeep& up)
{
    // Travel to the capital anchor before the errand chain. The pose steps resolve + hop to their own
    // prop, and the inn step owns its own movement, so only gate the leading SELL on the hub distance.
    if (up.step == 1 && bot->GetExactDist(up.hubPos) > 30.0f)
    {
        if (DriveTravel(up.hubPos) == TravelResult::EN_ROUTE)
            return true;
    }

    switch (up.step)
    {
        case 1:   // SELL (+guild-bank deposit) — fire once, hold a randomized dwell.
        {
            if (up.stepStartMs == 0 && sPlayerbotAIConfig.tokenRedeemEnable)
                botAI->DoSpecificAction("redeem currency");   // synthetic, in-place; once, before the sell/deposit dwell
            if (!UpkeepDwell(up, urand(sPlayerbotAIConfig.upkeepSellMinSec, sPlayerbotAIConfig.upkeepSellMaxSec),
                             "sell", true))
                return true;
            up.step = 2;
            up.target.Clear();
            up.stepStartMs = 0;
            return true;
        }
        case 2:   // BANK pose (cosmetic — no transaction).
        {
            if (PoseAtProp(RS_BANK,
                           urand(sPlayerbotAIConfig.upkeepBankMinSec, sPlayerbotAIConfig.upkeepBankMaxSec) * IN_MILLISECONDS,
                           up))
                return true;
            up.step = 3;
            up.target.Clear();
            up.poseArriveT = 0;
            up.posePos = WorldPosition();   // next pose re-acquires its own coord
            up.stepStartMs = 0;
            return true;
        }
        case 3:   // AUCTION HOUSE pose.
        {
            if (PoseAtProp(RS_AUCTION_HOUSE,
                           urand(sPlayerbotAIConfig.upkeepAHMinSec, sPlayerbotAIConfig.upkeepAHMaxSec) * IN_MILLISECONDS,
                           up))
                return true;
            up.step = 4;
            up.target.Clear();
            up.poseArriveT = 0;
            up.posePos = WorldPosition();   // next pose re-acquires its own coord
            up.stepStartMs = 0;
            return true;
        }
        case 4:   // MAILBOX pose.
        {
            if (PoseAtProp(RS_MAILBOX,
                           urand(sPlayerbotAIConfig.upkeepMailMinSec, sPlayerbotAIConfig.upkeepMailMaxSec) * IN_MILLISECONDS,
                           up))
                return true;
            up.step = 5;
            up.target.Clear();
            up.poseArriveT = 0;
            up.posePos = WorldPosition();   // next pose re-acquires its own coord
            up.stepStartMs = 0;
            return true;
        }
        case 5:   // CLASS TRAINER pose.
        {
            if (PoseAtProp(RS_CLASS_TRAINER,
                           urand(sPlayerbotAIConfig.upkeepTrainerMinSec, sPlayerbotAIConfig.upkeepTrainerMaxSec) * IN_MILLISECONDS,
                           up))
                return true;
            up.step = 6;
            up.target.Clear();
            up.poseArriveT = 0;
            up.posePos = WorldPosition();   // next pose re-acquires its own coord
            up.stepStartMs = 0;
            return true;
        }
        case 6:   // MAINTENANCE (NPC-less restock + guild-bank withdraw + gear floor + train) — fire once.
        {
            // The maintenance action sets botAI->lastMaintenanceLearnedNew (TrainerAction.cpp:277).
            // Reset it on the entry tick so a stale prior-episode value can't false-gate the dummy.
            if (up.stepStartMs == 0)
                botAI->lastMaintenanceLearnedNew = false;
            if (!UpkeepDwell(up, urand(sPlayerbotAIConfig.upkeepMaintMinSec, sPlayerbotAIConfig.upkeepMaintMaxSec),
                             "maintenance"))
                return true;
            // Capture the learned-new-spell signal AFTER maintenance completed (gates the dummy tail).
            up.learnedNew = botAI->lastMaintenanceLearnedNew;
            up.step = 7;
            up.target.Clear();
            up.stepStartMs = 0;
            return true;
        }
        case 7:   // DUMMY — only a freshly-trained bot tests its new ability on a target dummy.
        {
            if (!sPlayerbotAIConfig.upkeepDummyTestEnable || !up.learnedNew)
            {
                up.step = 8;
                up.target.Clear();
                up.poseArriveT = 0;
                up.posePos = WorldPosition();   // next pose re-acquires its own coord
                up.stepStartMs = 0;
                return true;
            }
            if (PoseAtProp(RS_DUMMY,
                           urand(sPlayerbotAIConfig.upkeepDummyTestMinMin, sPlayerbotAIConfig.upkeepDummyTestMaxMin)
                               * MINUTE * IN_MILLISECONDS,
                           up))
                return true;
            up.step = 8;
            up.target.Clear();
            up.poseArriveT = 0;
            up.posePos = WorldPosition();   // next pose re-acquires its own coord
            up.stepStartMs = 0;
            return true;
        }
        case 8:   // QUESTGIVER — cosmetic loiter pose (skip if none nearby).
        {
            if (PoseAtNearbyNpc(UNIT_NPC_FLAG_QUESTGIVER,
                                urand(sPlayerbotAIConfig.upkeepQuestGiverMinSec,
                                      sPlayerbotAIConfig.upkeepQuestGiverMaxSec) * IN_MILLISECONDS, up))
                return true;
            up.step = 9;
            up.target.Clear();
            up.stepStartMs = 0;
            return true;
        }
        case 9:   // TERMINAL — post-upkeep rest coda.
        default:
            return TickUpkeepFinish(up);
    }
}

// rest-upkeep-consolidation — cosmetic loiter pose at a nearby NPC of `npcFlag`, for both upkeep
// tiers. The bot is already at its hub, so a local grid-scan resolves the prop (no capital-coord
// lookup). No transaction — pure ambiance. Returns true = still posing (caller returns this tick),
// false = done OR none nearby (caller advances the step). CRASH RULE: no ChangeTo*/Decide() here.
bool NewRpgStatusUpdateAction::PoseAtNearbyNpc(uint32 npcFlag, uint32 dwellMs, NewRpgInfo::Upkeep& up)
{
    // ACQUIRE (once): nearest matching NPC in the loaded grid.
    if (up.target.IsEmpty())
    {
        ObjectGuid npc = SelectNearestNpcWithFlag(npcFlag);
        if (npc.IsEmpty())
            return false;   // none nearby → clean skip
        up.target          = npc;
        up.stepStartMs     = 0;
        up.chosenDwellPose = ResolveHeldPose(BEH_WANDER_NPC, (uint8)POI_NONE, (uint8)urand(0, 5));
    }

    WorldObject* npcObj = ObjectAccessor::GetWorldObject(*bot, up.target);
    if (!npcObj)
        return false;   // despawned → skip

    // TRAVEL: walk within interaction range (witness-gated; teleports unwitnessed).
    if (bot->GetExactDist(npcObj) > INTERACTION_DISTANCE)
    {
        TravelResult const tr = DriveTravel(WorldPosition(npcObj));
        if (tr == TravelResult::GAVE_UP)
            return false;
        if (tr != TravelResult::ARRIVED)
            return true;
    }

    // PERFORM: face + hold the loiter pose for the dwell.
    if (up.stepStartMs == 0)
    {
        bot->SetFacingToObject(npcObj);
        up.stepStartMs = getMSTime();
        float const wf = sPlayerbotAIConfig.upkeepWorkloadScaleEnable
                             ? std::max(up.workloadPct / 100.0f, sPlayerbotAIConfig.upkeepWorkloadDwellFloor)
                             : 1.0f;
        up.dwellMs     = (uint32)(dwellMs * wf);
    }
    TickEmoteCadence(BEH_WANDER_NPC, (uint8)POI_NONE, /*skipSustainedPose=*/false, up.chosenDwellPose);
    return GetMSTimeDiffToNow(up.stepStartMs) < up.dwellMs;
}

// rest-upkeep-consolidation — terminal step of BOTH tiers. Replaces the old always-on inn sit.
// Roll AfterUpkeepChance: yes → enter REST in place (bot is already in the hub; TAVERN reproduces
// the old inn rest); no → back to work. CRASH RULE: ChangeTo*/Decide() is the LAST statement.
bool NewRpgStatusUpdateAction::TickUpkeepFinish(NewRpgInfo::Upkeep& up)
{
    NewRpgInfo& info = botAI->rpgInfo;
    info.lastUpkeepMs = getMSTime();   // flat field — safe to stamp before any ChangeTo*/Decide()

    if (roll_chance_f(sPlayerbotAIConfig.restAfterUpkeepChance * 100.0f))
    {
        info.ChangeToRest();
        // Seed REST in place so it skips the hub-travel path (removed in the next task) and lands
        // straight in the P2 acquire: bot is already at the hub, no teleport/settle needed.
        if (auto* rp = std::get_if<NewRpgInfo::Rest>(&info.data))
        {
            rp->hubPos     = WorldPosition(bot);
            rp->hubArriveT = getMSTime();   // settle window elapses harmlessly (grid already loaded)
            rp->subtype    = RS_NONE;       // P2 picks an ambient subtype next tick
        }
        return true;   // CRASH RULE — touch nothing after ChangeTo*
    }

    Decide();
    return true;       // CRASH RULE
}

// upkeep-share-reduction fix — rescue an AI-throttled bot stranded in an occupation whose only exit
// is a rel-11 duration/dwell check it can never reach. Rescuable set = the duration-watchdog
// occupations + UPKEEP; travel legs / REST / IDLE / RECOVER are deliberately excluded (see spec §3.1).
bool NewRpgMinimalEscapeAction::isUseful()
{
    if (!sPlayerbotAIConfig.minimalEscapeEnable)
        return false;
    // Active bots run the full rel-11 machine; only rescue the throttled (minimal-mode) pool.
    // Same derivation as Engine's `minimal = !AllowActivity()` (PlayerbotAI.cpp:1597), so they agree.
    if (botAI->AllowActivity())
        return false;

    NewRpgInfo& info = botAI->rpgInfo;
    switch (info.GetStatus())
    {
        case RPG_UPKEEP:
            return info.HasStatusPersisted(sPlayerbotAIConfig.upkeepEscapeCeilingSec * IN_MILLISECONDS);
        case RPG_DO_QUEST:
            return info.HasStatusPersisted(30 * MINUTE * IN_MILLISECONDS);   // mirrors statusDoQuestDuration
        case RPG_OUTDOOR_PVP:
            return info.HasStatusPersisted(HOUR * IN_MILLISECONDS);          // mirrors statusOutDoorPvPDuration
        case RPG_GATHERING_CIRCUIT:
            return info.HasStatusPersisted(sPlayerbotAIConfig.gatheringCircuitDurationSec * IN_MILLISECONDS);
        default:
            return false;
    }
}

bool NewRpgMinimalEscapeAction::Execute(Event /*event*/)
{
    // One self-contained tick: essential side-effect -> stamp completion -> drop to IDLE.
    // CRASH RULE: every ChangeToIdle() is the last statement; nothing reads info.data afterward.
    NewRpgInfo& info = botAI->rpgInfo;
    switch (info.GetStatus())
    {
        case RPG_UPKEEP:
            botAI->DoSpecificAction("maintenance");   // real one-tick work (mirrors NewRpgAction.cpp:611)
            info.lastUpkeepMs = getMSTime();           // resets the 2h MaintenanceOverdue timer
            info.ChangeToIdle();
            return true;
        case RPG_DO_QUEST:
            info.lastFinished[RPG_DO_QUEST] = getMSTime();
            info.ChangeToIdle();
            return true;
        case RPG_OUTDOOR_PVP:
            info.lastFinished[RPG_OUTDOOR_PVP] = getMSTime();
            info.ChangeToIdle();
            return true;
        case RPG_GATHERING_CIRCUIT:
            info.lastFinished[RPG_GATHERING_CIRCUIT] = getMSTime();
            info.ChangeToIdle();
            return true;
        default:
            return false;   // isUseful() already filtered; defensive
    }
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

    // doquest-zone-travel: if a cross-zone/map target is set, travel there first (Task 6: now via
    // the shared, witness-gated DriveTravel primitive), then let the in-zone questing loop take over.
    if (data.targetPos != WorldPosition())
    {
        uint32 const tMap = data.targetPos.GetMapId();
        float const tx = data.targetPos.GetPositionX();
        float const ty = data.targetPos.GetPositionY();
        if (bot->GetMapId() != tMap || bot->GetDistance2d(tx, ty) >= 1500.0f)
        {
            // Ground-z safety (parity with the old bespoke block): resolve a valid ground height
            // BEFORE handing the target to DriveTravel, so an unwitnessed cheap-jump teleport can
            // never drop the bot into the ground/air. FindMap-null and invalid-height both give up,
            // exactly as before. CRASH RULE: return now, touch no `data` after a ChangeToIdle.
            Map* tmap = sMapMgr->FindMap(tMap, 0);
            if (!tmap)
            {
                FallToFarmOrRest();   // destination map not loaded; give up, let the spine re-roll
                return true;
            }
            float const ground = tmap->GetHeight(bot->GetPhaseMask(), tx, ty, MAX_HEIGHT);
            if (ground <= INVALID_HEIGHT)
            {
                FallToFarmOrRest();
                return true;
            }
            WorldPosition const dest(tMap, tx, ty, ground + 0.05f);

            // Burst cap (parity): bracket the travel attempt with the atomic counter. Over budget →
            // retry next tick with the target retained.
            if (!sRandomPlayerbotMgr.TryBeginQuestTravel(bot->GetGUID()))
                return true;
            TravelResult const tr = DriveTravel(dest);
            sRandomPlayerbotMgr.EndQuestTravel(bot->GetGUID());

            if (tr == TravelResult::EN_ROUTE)
                return true;           // still traveling; target retained for the next tick
            if (tr == TravelResult::GAVE_UP)
            {
                FallToFarmOrRest();   // beyond travel budget; give up, let the spine re-roll
                return true;           // CRASH RULE: touch no `data` after this
            }
            // ARRIVED — clear the travel target so next tick the in-zone questing loop runs. Update
            // through the live variant only if it is still DoQuest (DriveTravel does not change status):
            if (NewRpgInfo::DoQuest* dq = std::get_if<NewRpgInfo::DoQuest>(&info.data))
                dq->targetPos = WorldPosition();
            return true;
        }
        // already on/near the target map+zone — clear the travel target and quest in-zone
        data.targetPos = WorldPosition();
    }

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
    FallToFarmOrRest();
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
            FallToFarmOrRest();
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
            botAI->lowPriorityQuest[questId] = getMSTime();
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            FallToFarmOrRest();
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
            FallToFarmOrRest();
            return true;
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
        botAI->lowPriorityQuest[questId] = getMSTime();
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        FallToFarmOrRest();
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
        FallToFarmOrRest();
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
        FallToFarmOrRest();
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

// gather-travel-to-node: bounded recent-visited ring. Remember up to this many just-finished
// nodes so the next NearestGatherNode seed skips them (prevents A<->B ping-pong between the two
// closest nodes). Oldest guid drops when the ring is full.
static constexpr size_t GATHER_RECENT_VISITED_CAP = 16;
static void PushGatherRecentVisited(NewRpgInfo::GatheringCircuit& gc, ObjectGuid::LowType spawnId)
{
    if (spawnId == 0)
        return;
    gc.recentVisited.push_back(spawnId);
    if (gc.recentVisited.size() > GATHER_RECENT_VISITED_CAP)
        gc.recentVisited.erase(gc.recentVisited.begin());
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
        return true;                    // CRASH RULE — ChangeToIdle is the last statement
    }
    // Seed the next target node from the boot-built index (reachable up to the travel radius),
    // skipping every recently visited/abandoned node so the circuit advances instead of looping.
    if (data->nodeSpawnId == 0)
    {
        TravelMgr::GatherNodeHit hit;
        if (!sTravelMgr.NearestGatherNode(bot, sPlayerbotAIConfig.gatheringCircuitTravelRadius,
                                          data->recentVisited, hit))
        {
            FallToFarmOrRest();          // no reachable node left -> done
            return true;                // CRASH RULE — FallToFarmOrRest is the last statement
        }
        data->nodeSpawnId = hit.spawnId;
        data->nodeEntry = hit.entry;
        data->nodePos = hit.pos;
        data->harvesting = false;       // fresh node — no harvest in progress
        data->harvestStartMs = 0;
    }
    // Resolve the LIVE GameObject by its DB spawnId — the runtime guid is generated at LoadFromDB
    // and is NOT the spawnId, so a fabricated guid never matches. The store carries the object only
    // once the grid is loaded (present after we travel into range). `node` is a per-tick pointer,
    // never stored across ticks (CRASH RULE). `bot->GetMap()` is valid for the member Player* here.
    GameObject* node = nullptr;
    auto bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(data->nodeSpawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
    {
        GameObject* g = it->second;
        if (g && g->isSpawned() && (data->nodeEntry == 0 || g->GetEntry() == data->nodeEntry))
        {
            node = g;
            break;
        }
    }
    if (!node)
    {
        // Either the grid isn't loaded yet (far target) OR we arrived and the object is gone
        // (harvested/despawned). Far -> travel leg toward the indexed position; near -> resolve.
        // R1: never treat MoveFarTo's return as an arrival test — use an explicit distance.
        if (bot->GetExactDist(data->nodePos.GetPositionX(), data->nodePos.GetPositionY(),
                              data->nodePos.GetPositionZ()) > 30.0f)
        {
            MoveFarTo(data->nodePos);
            return true;
        }
        // We ARE at the position but there is no live object -> already harvested/despawned.
        if (data->harvesting)           // we were harvesting this one -> success
            ++data->visited;
        PushGatherRecentVisited(*data, data->nodeSpawnId);
        data->nodeSpawnId = 0;
        data->nodeEntry = 0;
        data->nodePos = WorldPosition();
        data->harvesting = false;
        data->harvestStartMs = 0;
        return true;
    }
    ObjectGuid const nodeGuid = node->GetGUID();   // the REAL runtime guid for loot/movement
    if (!IsWithinInteractionDist(node))
    {
        if (MoveWorldObjectTo(nodeGuid))
            return true;
        PushGatherRecentVisited(*data, data->nodeSpawnId);
        data->nodeSpawnId = 0;          // unreachable -> try another
        data->nodeEntry = 0;
        data->nodePos = WorldPosition();
        data->harvesting = false;
        data->harvestStartMs = 0;
        return true;
    }
    // arrived & within interaction distance
    if (!data->harvesting)
    {
        // Begin the harvest: DoLoot stops movement, then casts the mining/herb gather spell.
        LootObject lootObj(bot, nodeGuid);
        if (lootObj.IsLootPossible(bot))
        {
            context->GetValue<LootObject>("loot target")->Set(lootObj);
            botAI->DoSpecificAction("open loot", Event(), true);
        }
        data->harvesting = true;
        data->harvestStartMs = getMSTime();
        TickEmoteCadence(BEH_GATHERING_CIRCUIT, 0);   // between-node pause at the node
        return true;                                   // HOLD — do not advance, do not move
    }

    // Harvest in progress: hold here. The node despawning (caught above) is success; otherwise give up
    // after gatherHarvestHoldMs. This window is ABSOLUTE from the first cast — we may re-issue the cast
    // (transient nudge recovery) but MUST NOT reset the clock, or a node whose loot can't clear (e.g.
    // FULL BAGS -> mined ore can't be stored -> IsLootPossible stays true) loops forever and the bot
    // never exits gathering to go sell/upkeep. (Bug: the old code reset harvestStartMs on every re-cast.)
    if (GetMSTimeDiffToNow(data->harvestStartMs) >= sPlayerbotAIConfig.gatherHarvestHoldMs)
    {
        // If bags are full the ore can't be stored, so NO node in this circuit can complete. End the
        // whole circuit now -> Decide() sees BagsFull() (LAYER-1) -> UPKEEP (sell), instead of burning
        // the give-up window on every remaining node.
        if (BagsFull())
        {
            info.ChangeToIdle();
            return true;                 // CRASH RULE — ChangeToIdle is the last statement
        }
        ++data->visited;                 // gave up on this node -> count it and move on
        PushGatherRecentVisited(*data, data->nodeSpawnId);
        data->nodeSpawnId = 0;
        data->nodeEntry = 0;
        data->nodePos = WorldPosition();
        data->harvesting = false;
        data->harvestStartMs = 0;
        return true;
    }
    if (!bot->HasUnitState(UNIT_STATE_CASTING))
    {
        LootObject lootObj(bot, nodeGuid);
        if (lootObj.IsLootPossible(bot))
        {
            context->GetValue<LootObject>("loot target")->Set(lootObj);
            botAI->DoSpecificAction("open loot", Event(), true);   // retry, but do NOT extend the window
        }
    }
    return true;   // still holding
}
