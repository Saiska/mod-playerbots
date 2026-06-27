#include "NewRpgBaseAction.h"

#include <algorithm>
#include <cmath>

#include "AiObjectContext.h"
#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "ChatHelper.h"
#include "Creature.h"
#include "DBCStores.h"
#include "G3D/Vector2.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "Group.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "GridTerrainData.h"
#include "IVMapMgr.h"
#include "Map.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "OutdoorPvPMgr.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "QuestDef.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "RaidSimulationMgr.h"
#include "SharedDefines.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include "TravelMgr.h"

// Defined in FishingAction.cpp (only FindWaterRadial is header-declared).
WorldPosition FindFishingHole(PlayerbotAI* botAI);

// upkeep-sociability: per-POI held-pose set; file-scope so ResolveHeldPose (outside anon ns) can name it.
struct PoseSet { const uint32* poses; uint8 count; };

namespace
{
    // -----------------------------------------------------------------------
    // Emote palettes.
    //
    // One curated EmotePalette per BotBehaviorId (kPalette), plus per-POI loiter
    // rows (kLoiterByPoi) folding loiter-themed-scenes' 528bc948 pose switch into
    // the same data-driven table. LookupPalette resolves (behaviorId, variant);
    // loiter variants are BotCityPoi 1..6.
    //
    // Constants resolved against SharedDefines.h. Spec-listed emotes that have no
    // matching EMOTE_ONESHOT_* in 3.3.5a (SHRUG, WORK-oneshot, YAWN, DRINK) were
    // dropped to the nearest proven one-shot (the legacy social-emote set —
    // dance/cheer/laugh/applaud/point/talk/wave/bow/roar — plus EAT / QUESTION).
    // FISH/DUMMY/DUEL/TRAVEL_*/OUTDOOR_PVP carry no pool — the action
    // (channel / melee / cast / movement) is itself the animation.
    // -----------------------------------------------------------------------

    // --- one-shot pools (curated; missing constants resolved to the nearest proven one) ---
    const uint32 kOneShots_GoGrind[]   = { EMOTE_ONESHOT_ROAR, EMOTE_ONESHOT_FLEX, EMOTE_ONESHOT_POINT,
                                           EMOTE_ONESHOT_SALUTE, EMOTE_ONESHOT_EXCLAMATION, EMOTE_ONESHOT_TALK };
    const uint32 kOneShots_WanderRandom[] = { EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_QUESTION, EMOTE_ONESHOT_TALK,
                                              EMOTE_ONESHOT_EXCLAMATION };  // SHRUG -> dropped
    const uint32 kOneShots_DoQuest[]   = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_QUESTION, EMOTE_ONESHOT_EXCLAMATION,
                                           EMOTE_ONESHOT_BOW, EMOTE_ONESHOT_SALUTE, EMOTE_ONESHOT_YES };
    const uint32 kOneShots_Gather[]    = { EMOTE_ONESHOT_KNEEL, EMOTE_ONESHOT_EAT, EMOTE_ONESHOT_TALK };  // WORK -> KNEEL/TALK
    const uint32 kOneShots_Rest[]      = { EMOTE_ONESHOT_EAT, EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_QUESTION };  // DRINK/YAWN -> EAT/dropped
    const uint32 kOneShots_WanderNpc[] = { EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_BOW, EMOTE_ONESHOT_SALUTE,
                                           EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_QUESTION, EMOTE_ONESHOT_YES };
    const uint32 kOneShots_Social[]    = { EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_LAUGH, EMOTE_ONESHOT_APPLAUD,
                                           EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_BOW, EMOTE_ONESHOT_TALK };
    // upkeep-sociability: broadened loiter one-shots (was talk/question/point/laugh only).
    const uint32 kOneShots_LoiterTalk[] = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_QUESTION,
                                            EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_LAUGH,
                                            EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_APPLAUD,
                                            EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_BOW };
    // upkeep-sociability: interactive sub-pool used when a peer is co-located (cluster overlay).
    const uint32 kOneShots_LoiterInteractive[] = { EMOTE_ONESHOT_WAVE, EMOTE_ONESHOT_LAUGH,
                                                   EMOTE_ONESHOT_POINT, EMOTE_ONESHOT_BOW,
                                                   EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_APPLAUD };
    const uint32 kOneShots_Craft[]     = { EMOTE_ONESHOT_TALK };  // USE_STANDING is the held pose
    const uint32 kOneShots_RepairSell[] = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_BOW, EMOTE_ONESHOT_YES,
                                            EMOTE_ONESHOT_WAVE };
    static const uint32 kCheerPool[]   = { EMOTE_ONESHOT_CHEER, EMOTE_ONESHOT_APPLAUD, EMOTE_ONESHOT_LAUGH };

    #define POOL(a) (a), (uint8)(sizeof(a)/sizeof((a)[0]))
    // default rows, indexed by BotBehaviorId
    const EmotePalette kPalette[BEH_COUNT] = {
        /*BEH_NONE*/              { 0, nullptr, 0 },
        /*BEH_GO_GRIND*/         { 0, POOL(kOneShots_GoGrind) },
        /*BEH_WANDER_RANDOM*/    { 0, POOL(kOneShots_WanderRandom) },
        /*BEH_DO_QUEST*/         { 0, POOL(kOneShots_DoQuest) },
        /*BEH_GATHERING_CIRCUIT*/{ 0, POOL(kOneShots_Gather) },
        /*BEH_REST*/             { EMOTE_STATE_SIT, POOL(kOneShots_Rest) },          // floor-sit baseline pose
        /*BEH_WANDER_NPC*/       { 0, POOL(kOneShots_WanderNpc) },
        /*BEH_TRAVEL_FLIGHT*/    { 0, nullptr, 0 },                                  // mostly moving — no pool
        /*BEH_TRAVEL_MOUNT*/     { 0, nullptr, 0 },                                  // mount blocks pose — no pool
        /*BEH_OUTDOOR_PVP*/      { 0, nullptr, 0 },                                  // the fight is the action
        /*BEH_SOCIAL*/           { 0, POOL(kOneShots_Social) },
        /*BEH_LOITER*/           { 0, POOL(kOneShots_LoiterTalk) },                  // default; per-POI rows below
        /*BEH_FISH*/             { 0, nullptr, 0 },                                  // casting is the animation
        /*BEH_CRAFT*/            { EMOTE_STATE_USE_STANDING, POOL(kOneShots_Craft) },
        /*BEH_DUEL*/             { 0, nullptr, 0 },                                  // combat is the action
        /*BEH_REPAIR_SELL*/      { 0, POOL(kOneShots_RepairSell) },
        /*BEH_DUMMY*/            { 0, nullptr, 0 },                                  // melee is the action
        /*BEH_FLIGHT*/           { 0, POOL(kOneShots_LoiterTalk) },                 // chatting up the flight master (reuse talk one-shots)
        /*BEH_SPECTATE*/         { 0, POOL(kCheerPool) },                           // watching a duel/dancer
    };
    // Loiter per-POI sustained poses — PARITY with loiter-themed-scenes 528bc948.
    // Values copied verbatim from LoiterEmoteState (NewRpgAction.cpp): auctioneer/
    // banker/trainer = TALK(378), innkeeper = SIT(13), forge = USE_STANDING(69),
    // mailbox = 0 (just stand). Indexed by BotCityPoi 1..6 -> array 0..5.
    const EmotePalette kLoiterByPoi[6] = {
        /*POI_AUCTIONEER*/ { EMOTE_STATE_TALK,         POOL(kOneShots_LoiterTalk) },  // 378
        /*POI_BANKER*/     { EMOTE_STATE_TALK,         POOL(kOneShots_LoiterTalk) },  // 378
        /*POI_INNKEEPER*/  { EMOTE_STATE_SIT,          POOL(kOneShots_LoiterTalk) },  // 13
        /*POI_TRAINER*/    { EMOTE_STATE_TALK,         POOL(kOneShots_LoiterTalk) },  // 378
        /*POI_MAILBOX*/    { 0,                        POOL(kOneShots_LoiterTalk) },  // 0 (stand)
        /*POI_FORGE*/      { EMOTE_STATE_USE_STANDING, POOL(kOneShots_LoiterTalk) },  // 69
    };
    // upkeep-sociability: per-POI candidate HELD poses; a bot rolls ONE of these once per dwell
    // (NewRpgInfo::Upkeep/Rest.chosenDwellPose) so a crowd at one prop is not all EMOTE_STATE_TALK.
    // Indexed by BotCityPoi 1..6 -> array 0..5 (same mapping as kLoiterByPoi).
    // EMOTE_STATE_SIT_CHAIR absent in 3.3.5a SharedDefines.h -> substituted EMOTE_STATE_SIT.
    const uint32 kPose_Auctioneer[] = { 0, EMOTE_STATE_TALK, EMOTE_STATE_USE_STANDING };
    const uint32 kPose_Banker[]     = { 0, EMOTE_STATE_TALK, EMOTE_STATE_USE_STANDING };
    const uint32 kPose_Innkeeper[]  = { EMOTE_STATE_SIT, EMOTE_STATE_SIT, 0 };
    const uint32 kPose_Trainer[]    = { 0, EMOTE_STATE_TALK, EMOTE_STATE_USE_STANDING };
    const uint32 kPose_Mailbox[]    = { 0 };                       // mailbox reads wrong posed -> stand
    const uint32 kPose_Forge[]      = { EMOTE_STATE_USE_STANDING, 0 };

    const PoseSet kLoiterPoseSet[6] = {
        /*POI_AUCTIONEER*/ { kPose_Auctioneer, (uint8)(sizeof(kPose_Auctioneer)/sizeof(uint32)) },
        /*POI_BANKER*/     { kPose_Banker,     (uint8)(sizeof(kPose_Banker)/sizeof(uint32)) },
        /*POI_INNKEEPER*/  { kPose_Innkeeper,  (uint8)(sizeof(kPose_Innkeeper)/sizeof(uint32)) },
        /*POI_TRAINER*/    { kPose_Trainer,    (uint8)(sizeof(kPose_Trainer)/sizeof(uint32)) },
        /*POI_MAILBOX*/    { kPose_Mailbox,    (uint8)(sizeof(kPose_Mailbox)/sizeof(uint32)) },
        /*POI_FORGE*/      { kPose_Forge,      (uint8)(sizeof(kPose_Forge)/sizeof(uint32)) },
    };
    #undef POOL

} // anonymous namespace

// rest-hub-unification: LookupPalette resolves (behaviorId, variant) -> EmotePalette row.
// Promoted out of the anonymous namespace (declared in NewRpgBaseAction.h) so the RPG_REST
// machine's PaletteOf (NewRpgAction.cpp, a different TU) can resolve a row's sustained pose
// without duplicating the kPalette/kLoiterByPoi tables (single source of truth). Behavior is
// byte-identical to the prior file-static version.
const EmotePalette& LookupPalette(BotBehaviorId beh, uint8 variant)
{
    if (beh == BEH_LOITER && variant >= 1 && variant <= 6)
        return kLoiterByPoi[variant - 1];
    if (beh > BEH_NONE && beh < BEH_COUNT)
        return kPalette[beh];
    return kPalette[BEH_NONE];
}

// upkeep-sociability: resolve a per-bot held pose for (beh, variant).
// Returns the palette's sustainedPose for non-loiter behaviors; for BEH_LOITER
// variant 1..6 returns kLoiterPoseSet[variant-1].poses[rollIdx % count].
// Exported (declared in NewRpgBaseAction.h) so NewRpgRestHub.cpp (PoseAtProp) can call it.
uint32 ResolveHeldPose(BotBehaviorId beh, uint8 variant, uint8 rollIdx)
{
    if (beh == BEH_LOITER && variant >= 1 && variant <= 6)
    {
        PoseSet const& ps = kLoiterPoseSet[variant - 1];
        return ps.count ? ps.poses[rollIdx % ps.count] : 0;
    }
    return LookupPalette(beh, variant).sustainedPose;
}

void NewRpgBaseAction::TickEmoteCadence(BotBehaviorId beh, uint8 variant, bool skipSustainedPose)
{
    const EmotePalette& pal = LookupPalette(beh, variant);
    NewRpgInfo& info = botAI->rpgInfo;

    // 1. sustained pose (held, replicated) — re-assert each tick; dismount first (a mount hides the pose).
    // skipSustainedPose: caller already holds a real pose (e.g. RPG_REST seated on a chair, which sets a
    // SIT_*_CHAIR stand-state) — re-asserting UNIT_NPC_EMOTESTATE would fight it, so do one-shots only.
    if (pal.sustainedPose && !skipSustainedPose)
    {
        if (bot->IsMounted())
            bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
        if (bot->GetUInt32Value(UNIT_NPC_EMOTESTATE) != pal.sustainedPose)
            bot->SetUInt32Value(UNIT_NPC_EMOTESTATE, pal.sustainedPose);
    }

    // 2. timed, jittered, non-repeating one-shot
    if (!sPlayerbotAIConfig.emoteCadenceEnable || pal.oneShotCount == 0)
        return;
    uint32 mn = sPlayerbotAIConfig.emoteCadenceMin[beh];
    uint32 mx = sPlayerbotAIConfig.emoteCadenceMax[beh];
    if (mn == 0 && mx == 0) return;                       // off for this behavior
    uint32 now = getMSTime();
    if (info.lastEmoteMs == 0)
    {
        info.lastEmoteMs = now;
        info.nextEmoteGapMs = urand(mn, mx) * IN_MILLISECONDS;
        return;
    }
    if (GetMSTimeDiffToNow(info.lastEmoteMs) < info.nextEmoteGapMs)
        return;
    uint8 idx = (uint8)urand(0, pal.oneShotCount - 1);
    if (pal.oneShotCount > 1 && idx == info.lastEmoteIdx)  // non-repeat
        idx = (idx + 1) % pal.oneShotCount;
    bot->HandleEmoteCommand(pal.oneShots[idx]);
    info.lastEmoteIdx = idx;
    info.lastEmoteMs = now;
    info.nextEmoteGapMs = urand(mn, mx) * IN_MILLISECONDS;
}

void NewRpgBaseAction::FireOneShotEmote(BotBehaviorId beh, uint8 variant)
{
    const EmotePalette& pal = LookupPalette(beh, variant);
    if (pal.oneShotCount == 0)
        return;
    NewRpgInfo& info = botAI->rpgInfo;
    uint8 idx = (uint8)urand(0, pal.oneShotCount - 1);
    if (pal.oneShotCount > 1 && idx == info.lastEmoteIdx)   // non-repeat
        idx = (idx + 1) % pal.oneShotCount;
    bot->HandleEmoteCommand(pal.oneShots[idx]);
    info.lastEmoteIdx = idx;
}

bool NewRpgBaseAction::MoveFarTo(WorldPosition dest)
{
    if (dest == WorldPosition())
        return false;

    if (dest != botAI->rpgInfo.moveFarPos)
    {
        // clear stuck information if it's a new dest
        botAI->rpgInfo.SetMoveFarTo(dest);
    }

    // performance optimization
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    // Let previously committed movement finish before recomputing.
    //
    // MoveTo internally caps its stored delay at maxWaitForMove
    // (default 5s), but a long path (200+ yd routed around a
    // mountain) takes 30+ seconds to walk. After 5s
    // IsWaitingForLastMove returns false and MoveFarTo re-enters.
    // Without this gate, DoMovePoint would call mm->Clear() and
    // reissue MovePoint from the new bot position — and from a new
    // position mmap's partial-path endpoint often differs, so the
    // bot gets clobbered mid-walk and ends up oscillating (e.g.
    // cave entrance -> inside cave -> cave entrance -> mountain
    // base -> cave entrance...) around an unreachable destination.
    //
    // If the bot is still actively walking toward its last
    // committed point on the same map, just let the current spline
    // finish. The stuck counter below continues to track real
    // progress toward dest and triggers teleport recovery if the
    // committed paths genuinely aren't closing the gap.
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float remaining = bot->GetExactDist(lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // stuck check
    float disToDest = bot->GetDistance(dest);
    // Require a meaningful improvement (5yd) to reset the stuck counter.
    // The old 1yd threshold was small enough that bots oscillating back
    // and forth around an obstacle would keep "making progress" forever
    // and never trigger the teleport recovery below.
    if (disToDest + 5.0f < botAI->rpgInfo.nearestMoveFarDis)
    {
        botAI->rpgInfo.nearestMoveFarDis = disToDest;
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
    }
    else if (++botAI->rpgInfo.stuckAttempts >= 5 && GetMSTimeDiffToNow(botAI->rpgInfo.stuckTs) >= stuckTime)
    {
        // No meaningful progress toward dest for `stuckTime`: fall
        // back to teleporting directly so the bot can get on with
        // its RPG objective instead of oscillating indefinitely.
        botAI->rpgInfo.stuckTs = getMSTime();
        botAI->rpgInfo.stuckAttempts = 0;
        const AreaTableEntry* entry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        std::string zone_name = PlayerbotAI::GetLocalizedAreaName(entry);
        LOG_DEBUG(
            "playerbots",
            "[New RPG] Teleport {} from ({},{},{},{}) to ({},{},{},{}) as it stuck when moving far - Zone: {} ({})",
            bot->GetName(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), dest.GetMapId(), bot->GetZoneId(),
            zone_name);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        return bot->TeleportTo(dest);
    }

    float dis = bot->GetExactDist(dest);
    if (dis < pathFinderDis)
    {
        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), false, false,
                      false, true);
    }

    const uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    // Primary strategy: ask mmap for a route to the TRUE destination.
    // If mmap can reach it directly (PATHFIND_NORMAL) or partially
    // (PATHFIND_INCOMPLETE — destinations beyond the smooth-path cap
    // of ~296 yards, or where local geometry blocks the final step),
    // walk to the furthest reachable waypoint mmap computed. This
    // lets bots follow the real route around obstacles (mountains,
    // cave walls, cliffs) instead of trying to cut straight through.
    // The spline system walks the whole returned path smoothly, so
    // subsequent ticks early-out via IsWaitingForLastMove and no
    // further PathGenerator calls fire until the bot arrives.
    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));
        if (canReach)
        {
            const G3D::Vector3& endPos = path.GetActualEndPosition();
            // Only commit if the mmap endpoint actually makes progress
            // toward the destination. For pathological INCOMPLETE
            // results (e.g. disconnected polys that still report
            // INCOMPLETE) the endpoint can land right under the bot;
            // fall through to cone sampling in that case.
            float endDistToDest = dest.GetExactDist(endPos.x, endPos.y, endPos.z);
            if (endDistToDest + 5.0f < disToDest)
            {
                return MoveTo(bot->GetMapId(), endPos.x, endPos.y, endPos.z, false, false, false, true);
            }
        }
    }

    // Fallback: mmap couldn't route to the destination. Sample the
    // forward cone for a reachable stepping stone so the bot keeps
    // moving and can try again from a new vantage point. Cap at 2
    // samples — we already spent one PathGenerator call above and at
    // 3000 bots every extra CalculatePath matters.
    float minDelta = M_PI;
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    const float baseAngle = bot->GetAngle(&dest);
    float rx, ry, rz;
    bool found = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        float delta = (rand_norm() - 0.5f) * static_cast<float>(M_PI);  // ±π/2, forward cone
        float sampleDis = (0.5f + rand_norm() * 0.5f) * pathFinderDis;
        float angle = baseAngle + delta;
        float dx = x + cos(angle) * sampleDis;
        float dy = y + sin(angle) * sampleDis;
        float dz = z + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        bool canReach = !(type & (~typeOk));

        if (canReach && fabs(delta) <= minDelta)
        {
            found = true;
            const G3D::Vector3& endPos = path.GetActualEndPosition();
            rx = endPos.x;
            ry = endPos.y;
            rz = endPos.z;
            minDelta = fabs(delta);
        }
    }
    if (found)
    {
        return MoveTo(bot->GetMapId(), rx, ry, rz, false, false, false, true);
    }
    return false;
}

// ───────────────────────────────────────────────────────────────────────────
// Witness check: true if any real (non-bot) player is within `range` yards
// of `pos` on the bot's current map. Promoted to NewRpgBaseAction so both
// DriveTravel and the RPG_REST hub helpers can share the single definition.
// ───────────────────────────────────────────────────────────────────────────
bool NewRpgBaseAction::IsRealPlayerNear(WorldPosition const& pos, float range) const
{
    Map* map = bot->GetMap();
    if (!map)
        return false;
    for (auto const& ref : map->GetPlayers())
    {
        Player* p = ref.GetSource();
        if (!p || p == bot)
            continue;
        if (GET_PLAYERBOT_AI(p))
            continue;   // skip bots; only real players witness
        if (p->GetExactDist(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ()) <= range)
            return true;
    }
    return false;
}

// ───────────────────────────────────────────────────────────────────────────
// Witness-gated travel primitive used by T5 UPKEEP and T6 occupation travel.
// Mirrors the tri-state logic of TravelToHubOrTeleport, but target-parameterized.
//   ARRIVED   — within arrive radius (10y), OR teleported when unwitnessed.
//   EN_ROUTE  — witnessed + within budget: MoveFarTo issued (walk/mount handled internally).
//   GAVE_UP   — witnessed + beyond rpgTravelBudget: caller should choose a nearer goal.
// Ground-z safety of `target` is the CALLER's responsibility (as DoQuest already does).
// ───────────────────────────────────────────────────────────────────────────
TravelResult NewRpgBaseAction::DriveTravel(WorldPosition const& target)
{
    float const arrive = 10.0f;                       // cf. TravelMount :494
    float const dist = bot->GetExactDist(&target);
    if (dist <= arrive)
        return TravelResult::ARRIVED;
    bool const witnessed = IsRealPlayerNear(WorldPosition(bot), sPlayerbotAIConfig.rpgTravelWitnessRange)
                        || IsRealPlayerNear(target, sPlayerbotAIConfig.rpgTravelWitnessRange);
    if (!witnessed)                                   // empty world → cheap jump (ground-z by caller)
    {
        bot->TeleportTo(target.GetMapId(), target.GetPositionX(), target.GetPositionY(),
                        target.GetPositionZ(), bot->GetOrientation());
        return TravelResult::ARRIVED;
    }
    if (dist > sPlayerbotAIConfig.rpgTravelBudget)
        return TravelResult::GAVE_UP;
    MoveFarTo(target);                                // auto-mounts long hauls (existing behavior)
    if (sPlayerbotAIConfig.rpgMachineDebugLog)
        LOG_DEBUG("playerbots", "[RpgMachine] {} travel en-route d={:.0f}", bot->GetName(), dist);
    return TravelResult::EN_ROUTE;
}

bool NewRpgBaseAction::MoveWorldObjectTo(ObjectGuid guid, float distance)
{
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
    {
        return false;
    }

    WorldObject* object = botAI->GetWorldObject(guid);
    if (!object)
        return false;
    float x = object->GetPositionX();
    float y = object->GetPositionY();
    float z = object->GetPositionZ();
    float mapId = object->GetMapId();
    float angle = 0.f;

    if (!object->ToUnit() || !object->ToUnit()->isMoving())
        angle = object->GetAngle(bot) + (M_PI * irand(-25, 25) / 100.0);  // Closest 45 degrees towards the target
    else
        angle = object->GetOrientation() +
                (M_PI * irand(-25, 25) / 100.0);  // 45 degrees infront of target (leading it's movement)

    float rnd = rand_norm();
    x += cos(angle) * distance * rnd;
    y += sin(angle) * distance * rnd;
    if (!object->GetMap()->CheckCollisionAndGetValidCoords(object, object->GetPositionX(), object->GetPositionY(),
                                                           object->GetPositionZ(), x, y, z))
    {
        x = object->GetPositionX();
        y = object->GetPositionY();
        z = object->GetPositionZ();
    }
    return MoveTo(mapId, x, y, z, false, false, false, true);
}

bool NewRpgBaseAction::MoveRandomNear(float moveStep, MovementPriority priority, WorldObject* center)
{
    if (IsWaitingForLastMove(priority))
        return false;

    Map* map = bot->GetMap();
    const float x = bot->GetPositionX();
    const float y = bot->GetPositionY();
    const float z = bot->GetPositionZ();
    // Previously: attempts = 1. A single random sample often landed in
    // water / blocked geometry / unreachable poly, the function returned
    // false, and the caller had no fallback — bot stood still. Retry a
    // handful of times with a fresh distance each loop so a bad roll
    // doesn't lock the bot in place.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        float distance = (0.4f + rand_norm() * 0.6f) * moveStep;
        float angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float dx = x + distance * cos(angle);
        float dy = y + distance * sin(angle);
        float dz = z;

        PathGenerator path(bot);
        path.CalculatePath(dx, dy, dz);
        PathType type = path.GetPathType();
        uint32 typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;
        bool canReach = !(type & (~typeOk));

        if (!canReach)
            continue;

        if (!map->CanReachPositionAndGetValidCoords(bot, dx, dy, dz))
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), dx, dy, dz, bot->GetCollisionHeight()))
            continue;

        bool moved = MoveTo(bot->GetMapId(), dx, dy, dz, false, false, false, true, priority);
        if (moved)
            return true;
    }

    return false;
}

bool NewRpgBaseAction::ForceToWait(uint32 duration, MovementPriority priority)
{
    AI_VALUE(LastMovement&, "last movement")
        .Set(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation(),
             duration, priority);
    return true;
}

/// @TODO: Fix redundant code
/// Quest related method refer to TalkToQuestGiverAction.h
bool NewRpgBaseAction::InteractWithNpcOrGameObjectForQuest(ObjectGuid guid)
{
    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);
    if (!object || !bot->CanInteractWithQuestGiver(object))
        return false;

    // Creature* creature = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
    // if (creature)
    // {
    //     WorldPacket packet(CMSG_GOSSIP_HELLO);
    //     packet << guid;
    //     bot->GetSession()->HandleGossipHelloOpcode(packet);
    // }

    bot->PrepareQuestMenu(guid);
    const QuestMenu& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return true;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        const QuestStatus& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            AcceptQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_accepted",
                    "Quest accepted %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->rpgStatistic.questAccepted++;
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_rewarded",
                    "Quest rewarded %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            BroadcastHelper::BroadcastQuestTurnedIn(botAI, bot, quest);
            botAI->rpgStatistic.questRewarded++;
            LOG_DEBUG("playerbots", "[New RPG] {} turned in quest {}", bot->GetName(), quest->GetQuestId());
        }
    }
    return true;
}

bool NewRpgBaseAction::CanInteractWithQuestGiver(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that removes the distance check and keeps all other checks
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT: // Player::GetNPCIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            // unit checks
            if (!guid)
                return false;

            if (!bot->IsInWorld() || bot->IsDuringRemoveFromWorld())
                return false;

            if (bot->IsInFlight())
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            // Deathstate checks
            if (!bot->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_VISIBLE_TO_GHOSTS))
                return false;

            // alive or spirit healer
            if (!creature->IsAlive() &&
                !(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPE_FLAG_INTERACT_WHILE_DEAD))
                return false;

            // appropriate npc type
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                return false;

            // not allow interaction under control, but allow with own pets
            if (creature->GetCharmerGUID())
                return false;

            // xinef: perform better check
            if (creature->GetReactionTo(bot) <= REP_UNFRIENDLY)
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT: // Player::GetGameObjectIfCanInteractWith
        {
            ObjectGuid guid = questGiver->GetGUID();

            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                {
                    // Players cannot interact with gameobjects that use the "Point" icon
                    if (go->GetGOInfo()->IconName == "Point")
                        return false;

                    return true;
                }
            }

            return false;
        }
        // unused for now
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::IsWithinInteractionDist(Object* questGiver)
{
    // This is a variant of Player::CanInteractWithQuestGiver
    // that only keep the distance check
    switch (questGiver->GetTypeId())
    {
        case TYPEID_UNIT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            // unit checks
            if (!guid)
                return false;

            // exist (we need look pets also for some interaction (quest/etc)
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
            if (!creature)
                return false;

            if (!creature->IsWithinDistInMap(bot, INTERACTION_DISTANCE))
                return false;

            return true;
        }
        case TYPEID_GAMEOBJECT:
        {
            ObjectGuid guid = questGiver->GetGUID();
            if (GameObject* go = bot->GetMap()->GetGameObject(guid))
            {
                if (go->IsWithinDistInMap(bot))
                {
                    return true;
                }
            }
            return false;
        }
        // case TYPEID_PLAYER:
        //     return bot->IsAlive() && questGiver->ToPlayer()->IsAlive();
        // case TYPEID_ITEM:
        //     return bot->IsAlive();
        default:
            break;
    }
    return false;
}

bool NewRpgBaseAction::AcceptQuest(Quest const* quest, ObjectGuid guid)
{
    WorldPacket p(CMSG_QUESTGIVER_ACCEPT_QUEST);
    uint32 unk1 = 0;
    p << guid << quest->GetQuestId() << unk1;
    p.rpos(0);
    bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(p);

    return true;
}

bool NewRpgBaseAction::TurnInQuest(Quest const* quest, ObjectGuid guid)
{
    uint32 questID = quest->GetQuestId();

    if (bot->GetQuestRewardStatus(questID))
    {
        return false;
    }

    if (!bot->CanRewardQuest(quest, false))
    {
        return false;
    }

    bot->PlayDistanceSound(621);

    WorldPacket p(CMSG_QUESTGIVER_CHOOSE_REWARD);
    p << guid << quest->GetQuestId();
    if (quest->GetRewChoiceItemsCount() <= 1)
    {
        p << 0;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }
    else
    {
        uint32 bestId = BestRewardIndex(quest);
        p << bestId;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(p);
    }

    return true;
}

uint32 NewRpgBaseAction::BestRewardIndex(Quest const* quest)
{
    ItemIds returnIds;
    ItemUsage bestUsage = ITEM_USAGE_NONE;
    if (quest->GetRewChoiceItemsCount() <= 1)
        return 0;
    else
    {
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                bestUsage = ITEM_USAGE_EQUIP;
            else if (usage == ITEM_USAGE_BAD_EQUIP && bestUsage != ITEM_USAGE_EQUIP)
                bestUsage = usage;
            else if (usage != ITEM_USAGE_NONE && bestUsage == ITEM_USAGE_NONE)
                bestUsage = usage;
        }
        StatsWeightCalculator calc(bot);
        uint32 best = 0;
        float bestScore = 0;
        for (uint8 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
        {
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", quest->RewardChoiceItemId[i]);
            if (usage == bestUsage || usage == ITEM_USAGE_REPLACE)
            {
                float score = calc.CalculateItem(quest->RewardChoiceItemId[i]);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
        }
        return best;
    }
}

bool NewRpgBaseAction::IsQuestWorthDoing(Quest const* quest)
{
    bool isLowLevelQuest =
        bot->GetLevel() > (bot->GetQuestLevel(quest) + sWorld->getIntConfig(CONFIG_QUEST_LOW_LEVEL_HIDE_DIFF));

    if (isLowLevelQuest)
        return false;

    if (quest->IsRepeatable())
        return false;

    if (quest->IsSeasonal())
        return false;

    return true;
}

bool NewRpgBaseAction::IsQuestCapableDoing(Quest const* quest)
{
    bool highLevelQuest = bot->GetLevel() + 3 < bot->GetQuestLevel(quest);
    if (highLevelQuest)
        return false;

    // Elite quest and dungeon quest etc
    if (quest->GetType() != 0)
        return false;

    // now we only capable of doing solo quests
    if (quest->GetSuggestedPlayers() >= 2)
        return false;

    return true;
}

bool NewRpgBaseAction::OrganizeQuestLog()
{
    int32 freeSlotNum = 0;

    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            freeSlotNum++;
    }

    // it's ok if we have two more free slots
    if (freeSlotNum >= 2)
        return false;

    int32 dropped = 0;
    // remove quests that not worth doing or not capable of doing
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!IsQuestWorthDoing(quest) || !IsQuestCapableDoing(quest) ||
            bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
        {
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            bot->GetSession()->HandleQuestLogRemoveQuest(packet);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            dropped++;
        }
    }

    // drop more than 8 quests at once to avoid repeated accept and drop
    if (dropped >= 8)
        return true;

    // remove festival/class quests and quests in different zone
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
        const int64_t botZoneId = this->bot->GetZoneId();

        if (quest->GetZoneOrSort() < 0 || (quest->GetZoneOrSort() > 0 && quest->GetZoneOrSort() != botZoneId))
        {
            LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
            WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
            packet << (uint8)i;
            bot->GetSession()->HandleQuestLogRemoveQuest(packet);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                    "new_rpg_quest_dropped",
                    "Quest dropped %quest",
                    {{"%quest", ChatHelper::FormatQuest(quest)}}));
            botAI->rpgStatistic.questDropped++;
            dropped++;
        }
    }

    if (dropped >= 8)
        return true;

    // clear quests log
    for (uint16 i = 0; i < MAX_QUEST_LOG_SIZE; ++i)
    {
        uint32 questId = bot->GetQuestSlotQuestId(i);
        if (!questId)
            continue;

        const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
        LOG_DEBUG("playerbots", "[New RPG] {} drop quest {}", bot->GetName(), questId);
        WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
        packet << (uint8)i;
        bot->GetSession()->HandleQuestLogRemoveQuest(packet);
        if (botAI->GetMaster())
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "new_rpg_quest_dropped",
                "Quest dropped %quest",
                {{"%quest", ChatHelper::FormatQuest(quest)}}));
        botAI->rpgStatistic.questDropped++;
    }

    return true;
}

bool NewRpgBaseAction::SearchQuestGiverAndAcceptOrReward()
{
    OrganizeQuestLog();
    if (ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract(true, 80.0f))
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, npcOrGo);
        if (bot->CanInteractWithQuestGiver(object))
        {
            InteractWithNpcOrGameObjectForQuest(npcOrGo);
            ForceToWait(5000);
            return true;
        }
        return MoveWorldObjectTo(npcOrGo);
    }
    return false;
}

ObjectGuid NewRpgBaseAction::ChooseNpcOrGameObjectToInteract(bool questgiverOnly, float distanceLimit)
{
    GuidVector possibleTargets = AI_VALUE(GuidVector, "possible new rpg targets");
    GuidVector possibleGameObjects = AI_VALUE(GuidVector, "possible new rpg game objects");

    if (possibleTargets.empty() && possibleGameObjects.empty())
        return ObjectGuid();

    WorldObject* nearestObject = nullptr;
    for (ObjectGuid& guid : possibleTargets)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    for (ObjectGuid& guid : possibleGameObjects)
    {
        WorldObject* object = ObjectAccessor::GetWorldObject(*bot, guid);

        if (!object || !object->IsInWorld())
            continue;

        if (distanceLimit && bot->GetDistance(object) > distanceLimit)
            continue;

        if (CanInteractWithQuestGiver(object) && HasQuestToAcceptOrReward(object))
        {
            if (!nearestObject || bot->GetExactDist(nearestObject) > bot->GetExactDist(object))
                nearestObject = object;
            break;
        }
    }

    if (nearestObject)
        return nearestObject->GetGUID();

    // No questgiver to accept or reward
    if (questgiverOnly)
        return ObjectGuid();

    if (possibleTargets.empty())
        return ObjectGuid();

    int idx = urand(0, possibleTargets.size() - 1);
    ObjectGuid guid = possibleTargets[idx];
    WorldObject* object = ObjectAccessor::GetCreatureOrPetOrVehicle(*bot, guid);
    if (!object)
        object = ObjectAccessor::GetGameObject(*bot, guid);

    if (object && object->IsInWorld())
    {
        return object->GetGUID();
    }
    return ObjectGuid();
}

// Forward-declared here because SelectLoiterPoi uses it for the forge crafting-profession gate.
static bool BotHasCraftingProfession(Player* bot);

ObjectGuid NewRpgBaseAction::SelectLoiterPoi(uint8& outPoiType)
{
    outPoiType = POI_NONE;
    uint32 mask = sPlayerbotAIConfig.pastimeLoiterPoiTypeMask;

    // Weighted candidate list: each eligible POI gets weight = typeWeight / (1 + distanceFactor),
    // where distanceFactor = dist / scanRange.  When all type weights are equal this approximates
    // proximity-preference (legacy ordering), but allows clustering at preferred POI types.
    static constexpr float kScanRange = 150.0f;  // matches PossibleNewRpgTargetsValue default range

    struct PoiCand { WorldObject* object; uint8 type; float weight; };
    std::vector<PoiCand> cands;

    auto consider = [&](WorldObject* object, uint8 type)
    {
        if (!(mask & (1u << (type - 1))))
            return;
        float dist = bot->GetExactDist(object);
        float typeW = sPlayerbotAIConfig.pastimeLoiterTypeWeight[type];
        if (typeW <= 0.0f) return;  // zero-weight type: not a candidate (weight-zero = effectively disabled)
        float w = typeW / (1.0f + dist / kScanRange);
        cands.push_back({ object, type, w });
    };

    GuidVector targets = AI_VALUE(GuidVector, "possible new rpg targets");
    for (ObjectGuid& guid : targets)
    {
        Creature* c = ObjectAccessor::GetCreature(*bot, guid);
        if (!c || !c->IsInWorld())
            continue;
        if (c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))      consider(c, POI_AUCTIONEER);
        else if (c->HasNpcFlag(UNIT_NPC_FLAG_BANKER))     consider(c, POI_BANKER);
        else if (c->HasNpcFlag(UNIT_NPC_FLAG_INNKEEPER))  consider(c, POI_INNKEEPER);
        else if (c->HasNpcFlag(UNIT_NPC_FLAG_TRAINER))    consider(c, POI_TRAINER);
    }

    GuidVector gos = AI_VALUE(GuidVector, "possible new rpg game objects");
    for (ObjectGuid& guid : gos)
    {
        GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
        if (!go || !go->IsInWorld())
            continue;
        if (go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX)   consider(go, POI_MAILBOX);
    }

    // Forge/Anvil detection: GAMEOBJECT_TYPE_SPELL_FOCUS (= 8) with focusId == 1 (Anvil, SpellFocusObject.dbc)
    // or focusId == 3 (Forge).  Only considered when bot has a crafting profession.
    // Source: acore/src/server/game/Entities/GameObject/GameObjectData.h:140-150 (spellFocus.focusId field)
    //         SpellFocusObject.dbc ID=1 "Anvil", ID=3 "Forge"
    if (mask & (1u << (POI_FORGE - 1)) && BotHasCraftingProfession(bot))
    {
        constexpr uint32 FOCUS_ID_ANVIL = 1;  // SpellFocusObject.dbc ID=1 "Anvil"
        constexpr uint32 FOCUS_ID_FORGE = 3;  // SpellFocusObject.dbc ID=3 "Forge"
        GuidVector nearGos = context->GetValue<GuidVector>("nearest game objects")->Get();
        for (ObjectGuid const& guid : nearGos)
        {
            GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
            if (!go || !go->isSpawned())
                continue;
            if (go->GetGoType() != GAMEOBJECT_TYPE_SPELL_FOCUS)
                continue;
            uint32 fid = go->GetGOInfo()->spellFocus.focusId;
            if (fid != FOCUS_ID_ANVIL && fid != FOCUS_ID_FORGE)
                continue;
            consider(go, POI_FORGE);
        }
    }

    if (cands.empty())
        return ObjectGuid();

    // Weighted random draw over float weights using frand.
    float totalW = 0.0f;
    for (auto const& c : cands)
        totalW += c.weight;

    float r = frand(0.0f, totalW);
    float acc = 0.0f;
    for (auto const& c : cands)
    {
        acc += c.weight;
        if (acc >= r)
        {
            outPoiType = c.type;
            return c.object->GetGUID();
        }
    }
    // Floating-point rounding fallback: return last candidate.
    outPoiType = cands.back().type;
    return cands.back().object->GetGUID();
}

ObjectGuid NewRpgBaseAction::SelectVendorNpc()
{
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    Creature* best = nullptr;
    float bestDist = sPlayerbotAIConfig.pastimeRepairSellRadius;
    bool sawTarget = false;
    for (ObjectGuid& guid : npcs)
    {
        Creature* c = ObjectAccessor::GetCreature(*bot, guid);
        if (!c || !c->IsInWorld())
            continue;
        if (!c->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && !c->HasNpcFlag(UNIT_NPC_FLAG_REPAIR))
            continue;
        float d = bot->GetExactDist(c);
        if (d <= bestDist)
        {
            if (!sawTarget) { sawTarget = true; }
            bestDist = d;
            best = c;
        }
    }
    return best ? best->GetGUID() : ObjectGuid();
}

ObjectGuid NewRpgBaseAction::SelectTrainingDummy()
{
    auto const& entries = sPlayerbotAIConfig.pastimeDummyEntries;
    if (entries.empty())
        return ObjectGuid();

    float const radius = sPlayerbotAIConfig.pastimeDummyRadius;

    // Direct creature grid scan at the dummy radius (the real reach), instead of the
    // SightDistance(75y)-bounded "nearest npcs" value — capital dummies sit outside 75y.
    // Mirrors the searcher idiom in Ai/Base/Value/NearestNpcsValue.cpp.
    std::list<Unit*> targets;
    Acore::AnyUnitInObjectRangeCheck u_check(bot, radius);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitObjects(bot, searcher, radius);

    Creature* best = nullptr;
    float bestDist = radius;
    bool sawTarget = false;
    for (Unit* u : targets)
    {
        Creature* c = u ? u->ToCreature() : nullptr;
        if (!c || !c->IsInWorld())
            continue;
        if (std::find(entries.begin(), entries.end(), c->GetEntry()) == entries.end())
            continue;
        if (!c->IsFriendlyTo(bot))
            continue;   // only inert friendly dummies; hostile-faction targets would drag the bot into the combat engine
        float d = bot->GetExactDist(c);
        if (d <= bestDist)
        {
            if (!sawTarget) { sawTarget = true; }
            bestDist = d;
            best = c;
        }
    }
    return best ? best->GetGUID() : ObjectGuid();
}


ObjectGuid NewRpgBaseAction::SelectGatherNode()
{
    GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
    ObjectGuid best;
    float bestDist = sPlayerbotAIConfig.gatheringCircuitRadius;

    for (ObjectGuid const& guid : gos)
    {
        GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
        if (!go || !go->isSpawned())
            continue;

        if (go->GetGoType() != GAMEOBJECT_TYPE_CHEST)
            continue;

        LockEntry const* lockInfo = sLockStore.LookupEntry(go->GetGOInfo()->GetLockId());
        if (!lockInfo)
            continue;

        float dist = bot->GetExactDist(go);

        bool eligible = false;
        for (uint8 i = 0; i < 8; ++i)
        {
            if (lockInfo->Type[i] != LOCK_KEY_SKILL)
                continue;

            uint32 skillId = SkillByLockType(LockType(lockInfo->Index[i]));
            uint32 reqSkillValue = std::max(2u, lockInfo->Skill[i]);
            if ((skillId == SKILL_MINING || skillId == SKILL_HERBALISM) &&
                bot->HasSkill(skillId) && bot->GetSkillValue(skillId) >= reqSkillValue)
            {
                eligible = true;
                break;
            }
        }
        if (!eligible)
            continue;

        if (dist < bestDist)
        {
            bestDist = dist;
            best = guid;
        }
    }
    return best;
}

ObjectGuid NewRpgBaseAction::SelectInnChair(float radius)
{
    // Mirror SelectGatherNode's GO scan, filtered to GAMEOBJECT_TYPE_CHAIR (type 7).
    GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
    ObjectGuid best;
    float bestDist = radius;

    for (ObjectGuid const& guid : gos)
    {
        GameObject* go = ObjectAccessor::GetGameObject(*bot, guid);
        if (!go || !go->isSpawned())
            continue;

        if (go->GetGoType() != GAMEOBJECT_TYPE_CHAIR)
            continue;

        float dist = bot->GetExactDist(go);
        if (dist < bestDist)
        {
            bestDist = dist;
            best = guid;
        }
    }
    return best;
}

bool NewRpgBaseAction::SelectFarTaxiDest(WorldPosition& out)
{
    uint32 mapId = bot->GetMapId();
    std::vector<WorldPosition> cands;
    for (uint32 i = 0; i < sTaxiNodesStore.GetNumRows(); ++i)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
        if (!node || node->map_id != mapId)
            continue;
        WorldPosition p(mapId, node->x, node->y, node->z, 0.0f);
        float d = bot->GetExactDist(p);
        if (d >= sPlayerbotAIConfig.travelMountDistMin && d <= sPlayerbotAIConfig.travelMountDistMax)
            cands.push_back(p);
    }
    if (cands.empty())
        return false;
    out = cands[urand(0, cands.size() - 1)];
    return true;
}

static bool BotHasCraftingProfession(Player* bot)
{
    return bot->HasSkill(SKILL_BLACKSMITHING) ||
           bot->HasSkill(SKILL_TAILORING)     ||
           bot->HasSkill(SKILL_ENCHANTING)    ||
           bot->HasSkill(SKILL_ALCHEMY)       ||
           bot->HasSkill(SKILL_ENGINEERING)   ||
           bot->HasSkill(SKILL_LEATHERWORKING)||
           bot->HasSkill(SKILL_COOKING);
}

static bool BotHasGatheringProfession(Player* bot)
{
    return bot->HasSkill(SKILL_MINING) ||
           bot->HasSkill(SKILL_HERBALISM);
}

static bool BotInDuelAllowedArea(Player* bot)
{
    if (sPlayerbotAIConfig.IsInPvpProhibitedZone(bot->GetZoneId()))
        return false;
    AreaTableEntry const* casterAreaEntry = sAreaTableStore.LookupEntry(bot->GetAreaId());
    if (casterAreaEntry && !(casterAreaEntry->flags & AREA_FLAG_ALLOW_DUELS))
        return false;
    return true;
}

static ObjectGuid SelectDuelPartner(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();
    GuidVector friends = AI_VALUE(GuidVector, "nearest friendly players");
    Player* best = nullptr;
    float bestDist = sPlayerbotAIConfig.pastimeDuelRadius;
    bool sawTarget = false;
    for (ObjectGuid& guid : friends)
    {
        Player* other = ObjectAccessor::FindPlayer(guid);
        if (!other || other == bot || !other->IsInWorld())
            continue;
        if (other->isDead() || other->IsInCombat())
            continue;
        // AcceptDuelAction auto-declines when a non-master bot is below 90% HP, so a low-HP partner would
        // fizzle the request (sent, then cancelled). Skip them up front to match that accept gate.
        if (other->GetHealthPct() < 90.0f)
            continue;
        if (bot->GetExactDist(other) > sPlayerbotAIConfig.pastimeDuelRadius)
            continue;

        // In-radius base candidate confirmed — count once regardless of downstream filters.
        if (!sawTarget) { sawTarget = true; }

        bool isBot = sRandomPlayerbotMgr.IsRandomBot(other);
        if (!isBot)
        {
            if (!sPlayerbotAIConfig.pastimeDuelIncludePlayers)
                continue;   // real players only if opted in
        }
        else
        {
            // bot must be idle-ish (don't pester busy bots)
            // RPG_PASTIME/NewRpgInfo::Pastime removed (rest-hub-unification Task 9).
            PlayerbotAI* oai = GET_PLAYERBOT_AI(other);
            if (oai)
            {
                NewRpgStatus st = oai->rpgInfo.GetStatus();
                bool idleish = (st == RPG_IDLE || st == RPG_REST || st == RPG_WANDER_RANDOM);
                if (!idleish)
                    continue;
            }
        }

        float d = bot->GetExactDist(other);
        if (d < bestDist) { bestDist = d; best = other; }
    }
    return best ? best->GetGUID() : ObjectGuid();
}

bool NewRpgBaseAction::HasQuestToAcceptOrReward(WorldObject* object)
{
    ObjectGuid guid = object->GetGUID();
    bot->PrepareQuestMenu(guid);
    const QuestMenu& menu = bot->PlayerTalkClass->GetQuestMenu();
    if (menu.Empty())
        return false;

    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;
        const QuestStatus& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            return true;
        }
    }
    for (uint8 idx = 0; idx < menu.GetMenuItemCount(); idx++)
    {
        const QuestMenuItem& item = menu.GetItem(idx);
        const Quest* quest = sObjectMgr->GetQuestTemplate(item.QuestId);
        if (!quest)
            continue;

        const QuestStatus& status = bot->GetQuestStatus(item.QuestId);
        if (status == QUEST_STATUS_NONE && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false) &&
            IsQuestWorthDoing(quest) && IsQuestCapableDoing(quest))
        {
            return true;
        }
    }
    return false;
}

static std::vector<float> GenerateRandomWeights(int n)
{
    std::vector<float> weights(n);
    float sum = 0.0;

    for (int i = 0; i < n; ++i)
    {
        weights[i] = rand_norm();
        sum += weights[i];
    }
    for (int i = 0; i < n; ++i)
    {
        weights[i] /= sum;
    }
    return weights;
}

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete, bool requireInZone)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return false;

    const QuestPOIVector* poiVector = sObjectMgr->GetQuestPOIVector(questId);
    if (!poiVector)
    {
        return false;
    }

    const QuestStatusData& q_status = bot->getQuestStatusMap().at(questId);

    if (toComplete && q_status.Status == QUEST_STATUS_COMPLETE)
    {
        for (const QuestPOI& qPoi : *poiVector)
        {
            if (requireInZone && qPoi.MapId != bot->GetMapId())
                continue;

            // not the poi pos to reward quest
            if (qPoi.ObjectiveIndex != -1)
                continue;

            if (qPoi.points.size() == 0)
                continue;

            float dx = 0, dy = 0;
            std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
            for (size_t i = 0; i < qPoi.points.size(); i++)
            {
                const QuestPOIPoint& point = qPoi.points[i];
                dx += point.x * weights[i];
                dy += point.y * weights[i];
            }

            if (requireInZone && bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = 0.0f;
            if (qPoi.MapId == bot->GetMapId())
            {
                dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));
                if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                    continue;
                if (requireInZone && bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                    continue;
            }

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex, qPoi.MapId});
        }

        if (poiInfo.empty())
            return false;

        return true;
    }

    if (q_status.Status != QUEST_STATUS_INCOMPLETE)
        return false;

    // Get incomplete quest objective index
    std::vector<int32> incompleteObjectiveIdx;
    for (int i = 0; i < QUEST_OBJECTIVES_COUNT; i++)
    {
        int32 npcOrGo = quest->RequiredNpcOrGo[i];
        if (!npcOrGo)
            continue;

        if (q_status.CreatureOrGOCount[i] < quest->RequiredNpcOrGoCount[i])
            incompleteObjectiveIdx.push_back(i);
    }
    for (int i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; i++)
    {
        uint32 itemId = quest->RequiredItemId[i];
        if (!itemId)
            continue;

        if (q_status.ItemCount[i] < quest->RequiredItemCount[i])
            incompleteObjectiveIdx.push_back(QUEST_OBJECTIVES_COUNT + i);
    }

    // Get POIs to go
    for (const QuestPOI& qPoi : *poiVector)
    {
        if (requireInZone && qPoi.MapId != bot->GetMapId())
            continue;

        bool inComplete = false;
        for (uint32 objective : incompleteObjectiveIdx)
        {
            if (qPoi.ObjectiveIndex == objective)
            {
                inComplete = true;
                break;
            }
        }
        if (!inComplete)
            continue;
        if (qPoi.points.size() == 0)
            continue;
        float dx = 0, dy = 0;
        std::vector<float> weights = GenerateRandomWeights(qPoi.points.size());
        for (size_t i = 0; i < qPoi.points.size(); i++)
        {
            const QuestPOIPoint& point = qPoi.points[i];
            dx += point.x * weights[i];
            dy += point.y * weights[i];
        }

        if (requireInZone && bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = 0.0f;
        if (qPoi.MapId == bot->GetMapId())
        {
            dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));
            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;
            if (requireInZone && bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;
        }

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex, qPoi.MapId});
    }

    if (poiInfo.size() == 0)
    {
        // LOG_DEBUG("playerbots", "[New rpg] {}: No available poi can be found for quest {}", bot->GetName(), questId);
        return false;
    }

    return true;
}

WorldPosition NewRpgBaseAction::SelectRandomGrindPos(Player* bot)
{
    const std::vector<WorldLocation>& locs = sTravelMgr.GetLocsPerLevelCache(bot->GetLevel());
    float hiRange = 500.0f;
    float loRange = 2500.0f;
    if (bot->GetLevel() < 5)
    {
        hiRange /= 3;
        loRange /= 3;
    }
    std::vector<WorldLocation> lo_prepared_locs, hi_prepared_locs;

    bool inCity = false;
    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        if (bot->GetExactDist(loc) > 2500.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        if (bot->GetExactDist(loc) < hiRange)
        {
            hi_prepared_locs.push_back(loc);
        }

        if (bot->GetExactDist(loc) < loRange)
        {
            lo_prepared_locs.push_back(loc);
        }
    }
    WorldPosition dest{};
    if (urand(1, 100) <= 50 && !hi_prepared_locs.empty())
    {
        uint32 idx = urand(0, hi_prepared_locs.size() - 1);
        dest = hi_prepared_locs[idx];
    }
    else if (!lo_prepared_locs.empty())
    {
        uint32 idx = urand(0, lo_prepared_locs.size() - 1);
        dest = lo_prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random grind pos Map:{} X:{} Y:{} Z:{} ({}+{} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              hi_prepared_locs.size(), lo_prepared_locs.size() - hi_prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectRandomCampPos(Player* bot)
{
    const std::vector<WorldLocation> locs = sTravelMgr.GetTravelHubs(bot);

    bool inCity = false;

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
    {
        if (zone->flags & AREA_FLAG_CAPITAL)
            inCity = true;
    }

    std::vector<WorldLocation> prepared_locs;
    for (auto& loc : locs)
    {
        if (bot->GetMapId() != loc.GetMapId())
            continue;

        float range = bot->GetLevel() <= 5 ? 500.0f : 2500.0f;
        if (bot->GetExactDist(loc) > range)
            continue;

        if (bot->GetExactDist(loc) < 50.0f)
            continue;

        if (!inCity && bot->GetMap()->GetZoneId(bot->GetPhaseMask(), loc.GetPositionX(), loc.GetPositionY(),
                                                loc.GetPositionZ()) != bot->GetZoneId())
            continue;

        prepared_locs.push_back(loc);
    }
    WorldPosition dest{};
    if (!prepared_locs.empty())
    {
        uint32 idx = urand(0, prepared_locs.size() - 1);
        dest = prepared_locs[idx];
    }
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random inn keeper pos Map:{} X:{} Y:{} Z:{} ({} available in {})",
              bot->GetName(), dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(),
              prepared_locs.size(), locs.size());
    return dest;
}

WorldPosition NewRpgBaseAction::SelectCapitalHub(Player* bot)
{
    // Primary: the faction-appropriate capital BANKER coord (map-wide, not zone-filtered). When
    // EnableWeightTeleToCityBankers is on this is a weighted-capital pick; when off it is the
    // per-level banker fallback list. Either way it is non-empty whenever this level has bankers.
    std::vector<WorldLocation> cityLocs = sTravelMgr.GetCityLocations(bot);
    if (!cityLocs.empty())
        return WorldPosition(cityLocs[urand(0, cityLocs.size() - 1)]);

    // Fallback: guaranteed nearest faction/neutral capital, resolved directly from the capitals
    // banker cache (covers an empty per-level banker list). Empty ONLY if the capitals banker cache
    // failed to populate at boot -- a real boot bug, which is the can't-reach LOG_ERROR's true job.
    WorldPosition cap = sTravelMgr.GetNearestCapitalPos(bot);
    LOG_DEBUG("playerbots", "[New RPG] Bot {} SelectCapitalHub fell back to nearest capital Map:{}",
              bot->GetName(), cap.GetMapId());
    return cap;
}

WorldPosition NewRpgBaseAction::SelectCapitalHubAndZone(Player* bot, uint32& outZone)
{
    // upkeep-capital-pose-prop-resolve: single weighted roll that yields both the capital banker
    // anchor (hubPos) and the capital zoneId, so they always agree. Falls back to SelectCapitalHub
    // (outZone stays 0) when GetCityLocationAndZone returns an empty WorldLocation — the caller's
    // existing "up.hubPos == WorldPosition()" ERROR path then fires as before.
    outZone = 0;
    WorldLocation loc = sTravelMgr.GetCityLocationAndZone(bot, outZone);
    if (loc.GetMapId() != 0 || loc.GetPositionX() != 0.0f || loc.GetPositionY() != 0.0f)
        return WorldPosition(loc);
    // Fallback: nearest capital (zone unresolved → outZone stays 0; pose steps degrade to clean skips).
    return SelectCapitalHub(bot);
}

bool NewRpgBaseAction::SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path)
{
    TravelMgr::FlightMasterInfo const* info = sTravelMgr.GetNearestFlightMasterInfo(bot);
    if (!info)
        return false;

    std::vector<std::vector<uint32>> availablePaths = sTravelMgr.GetOptimalFlightDestinations(bot);
    if (availablePaths.empty())
        return false;

    flightMasterEntry = info->templateEntry;
    flightMasterPos = info->pos;
    path = availablePaths[urand(0, availablePaths.size() - 1)];
    LOG_DEBUG("playerbots", "[New RPG] Bot {} select random flight taxi node from:{} (node {}) to:{} ({} available)",
              bot->GetName(), flightMasterEntry, path[0], path[path.size() - 1], availablePaths.size());
    return true;
}

// A bot may run autonomous NewRpg behavior ONLY when ALL of these hold (allowlist — anything not
// provably "free" suppresses by default). Each axis is individually gated by its config toggle; an
// off toggle makes that axis non-constraining. Combat/dead are redundant with the COMBAT/DEAD engine
// switches but kept as cheap belt-and-suspenders against idle-window edges.
bool NewRpgBaseAction::IsFreeToIdle()
{
    PlayerbotAIConfig const& cfg = sPlayerbotAIConfig;

    // Always: a dead or mid-teleport bot is never free to idle.
    if (!bot->IsAlive() || bot->IsBeingTeleported())
        return false;

    if (cfg.rpgSuppressCombat && bot->IsInCombat())
        return false;

    // Instanced content: dungeon / raid / battleground / arena.
    if (cfg.rpgSuppressInstance)
    {
        Map* map = bot->GetMap();
        if (map && (map->IsDungeon() || map->IsRaid() || map->IsBattlegroundOrArena()))
            return false;
    }

    // On a moving platform or in a vehicle -> don't wander off it.
    if (cfg.rpgSuppressVehicle && (bot->GetTransport() || bot->GetVehicle()))
        return false;

    // Grouped with a human: a real player has no PlayerbotAI.
    if (cfg.rpgSuppressGroupedWithPlayer)
    {
        if (Group* group = bot->GetGroup())
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && member != bot && !GET_PLAYERBOT_AI(member))
                    return false;
            }
        }
    }

    // In a RaidSim run (the simulated-instance gear loop).
    if (cfg.rpgSuppressRaidSim && sRaidSimulationMgr.IsRaiding(bot->GetGUID()))
        return false;

    return true;
}

bool NewRpgBaseAction::ShouldSuppressRpg()
{
    return !IsFreeToIdle();
}

// --- occupation-rebalance Task 2: context-aware fallback helpers ---

bool NewRpgBaseAction::IsNearRestHub(float radius)
{
    for (WorldLocation const& hub : sTravelMgr.GetTravelHubs(bot))
    {
        if (hub.GetMapId() != bot->GetMapId())
            continue;
        if (bot->GetExactDist(hub) <= radius)
            return true;
    }
    return false;
}

// doquest-zone-travel: same-map planar distance to a POI, or a large constant for cross-map POIs
// so that same-map targets sort before cross-map targets (keeps teleports rarer).
float NewRpgBaseAction::DistToPoi(POIInfo const& poi)
{
    if (poi.mapId != bot->GetMapId())
        return 1000000.0f;
    return bot->GetDistance2d(poi.pos.x, poi.pos.y);
}

bool NewRpgBaseAction::FallToFarmOrRest()
{
    if (IsNearRestHub(sPlayerbotAIConfig.rpgNearHubRadius))
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    // Farm in place: anchor GoGrind to the bot's current position, which is always non-empty
    // so the commit cannot fail — this is the guarantee that breaks the Idle leak.
    botAI->rpgInfo.ChangeToGoGrind(WorldPosition(bot));
    return true;
}

// ── occupation-machine Task 2: predicate vocabulary ──────────────────────────
// All predicates are read-only, O(1)/cheap-proximity, valid for the current tick only.

bool NewRpgBaseAction::InOpenWorld()
{
    Map* map = bot->GetMap();
    return map && !map->IsDungeon() && !map->IsRaid() && !map->IsBattlegroundOrArena();
}

bool NewRpgBaseAction::NearHub(float r)
{
    return IsNearRestHub(r);
}

bool NewRpgBaseAction::InCityHub()
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId());
    return zone && (zone->flags & AREA_FLAG_CAPITAL);
}

bool NewRpgBaseAction::HealthLow()
{
    return !bot->IsInCombat() && bot->GetHealthPct() < sPlayerbotAIConfig.needHealthLowPct;
}

bool NewRpgBaseAction::ManaLow()
{
    // Only meaningful for classes that use mana as their primary power.
    // GetPower(POWER_MANA) == 0 on classes that have no mana bar (e.g. warrior, rogue).
    if (!bot->GetMaxPower(POWER_MANA))
        return false;
    return !bot->IsInCombat() && bot->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.needManaLowPct;
}

bool NewRpgBaseAction::DurabilityLow()
{
    // Scan equipped slots (EQUIPMENT_SLOT_START..EQUIPMENT_SLOT_END).
    // Mirror the RepairPercent idiom from StatsAction.cpp:169-183.
    for (uint32 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        uint16 pos = ((INVENTORY_SLOT_BAG_0 << 8) | i);
        Item* item = bot->GetItemByPos(pos);
        if (!item)
            continue;

        uint32 maxDurability = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if (!maxDurability)
            continue;

        uint32 curDurability = item->GetUInt32Value(ITEM_FIELD_DURABILITY);
        float pct = curDurability * 100.0f / maxDurability;
        if (pct < sPlayerbotAIConfig.needDurabilityLowPct)
            return true;
    }
    return false;
}

bool NewRpgBaseAction::BagsFull()
{
    // Count free slots across main backpack (16 slots) + equipped bags.
    // Mirror the free-slot accounting in StatsAction.cpp:39-63.
    uint32 totalFree = 0;

    // Main backpack: slots INVENTORY_SLOT_ITEM_START..INVENTORY_SLOT_ITEM_END (16 slots)
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
    {
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            ++totalFree;
    }

    // Equipped bag slots
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag const* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
        {
            ItemTemplate const* pBagProto = pBag->GetTemplate();
            if (pBagProto && pBagProto->Class == ITEM_CLASS_CONTAINER &&
                pBagProto->SubClass == ITEM_SUBCLASS_CONTAINER)
            {
                totalFree += pBag->GetFreeSlots();
            }
        }
    }

    return totalFree <= sPlayerbotAIConfig.needBagsFullSlots;
}

bool NewRpgBaseAction::MissingToolsOrReagents()
{
    // Mining: requires any mining pick in inventory.
    // item 2901 = Mining Pick (the canonical starter pick; bots are seeded with it by
    // PlayerbotFactory; use HasItemCount to cover all pick variants via the same pattern
    // used by ItemUsageValue.cpp:557 — but we check a representative set of known picks).
    if (bot->HasSkill(SKILL_MINING))
    {
        static uint32 const kMiningPicks[] = {2901, 1819, 1893, 1959, 9465, 20723, 40772, 40892, 40893};
        bool hasPick = false;
        for (uint32 pick : kMiningPicks)
        {
            if (bot->HasItemCount(pick, 1, true))
            {
                hasPick = true;
                break;
            }
        }
        if (!hasPick)
            return true;
    }

    // Fishing: requires a fishing pole equipped or in bags.
    if (AI_VALUE(bool, "can fish"))
    {
        // 6256 = Fishing Rod (FishingAction.cpp:23)
        if (!bot->HasItemCount(6256, 1, true))
            return true;
    }

    // Herbalism needs no physical tool; skinning knife is not checked here because
    // the gather-circuit only covers mining/herbalism (SelectGatherNode:1150).
    return false;
}

bool NewRpgBaseAction::MaintenanceOverdue()
{
    // lastUpkeepMs == 0 means never performed — treat as overdue.
    uint32 last = botAI->rpgInfo.lastUpkeepMs;
    if (last == 0)
        return true;
    return GetMSTimeDiffToNow(last) > sPlayerbotAIConfig.maintenanceOverdueMs;
}

bool NewRpgBaseAction::HasActionableQuest()
{
    // Mirror CheckRpgStatusAvailable(RPG_DO_QUEST) (line 1875) which is the canonical gate
    // for quest-able status: a quest log entry with a resolvable POI (complete-to-turn-in
    // OR incomplete-with-objective). requireInZone=false for cross-zone travel.
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        if (botAI->IsQuestLowPriority(questId))
            continue;

        std::vector<POIInfo> poiInfo;
        if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, /*toComplete=*/true, /*requireInZone=*/false))
            return true;
    }
    return false;
}

bool NewRpgBaseAction::HasGatherProfAndTool()
{
    // Reuses BotHasGatheringProfession pattern (NewRpgBaseAction.cpp:1226-1230).
    // Herbalism needs no tool; mining needs a pick (any variant).
    if (bot->HasSkill(SKILL_HERBALISM))
        return true;

    if (bot->HasSkill(SKILL_MINING))
    {
        static uint32 const kMiningPicks[] = {2901, 1819, 1893, 1959, 9465, 20723, 40772, 40892, 40893};
        for (uint32 pick : kMiningPicks)
        {
            if (bot->HasItemCount(pick, 1, true))
                return true;
        }
    }

    return false;
}

bool NewRpgBaseAction::NodeInRange(float r)
{
    // SelectGatherNode() uses sPlayerbotAIConfig.gatheringCircuitRadius as its hard cap.
    // When r <= gatheringCircuitRadius the existing scan naturally honours the tighter bound
    // (it tracks bestDist and only accepts nodes closer than its cap). When r > cap the result
    // is still bounded by the cap, which is the safe/cheap O(1) guarantee we need.
    // Either way: !IsEmpty() means at least one eligible node is within range.
    (void)r;   // r is the caller's intent; the scan already uses gatheringCircuitRadius
    return !SelectGatherNode().IsEmpty();
}

bool NewRpgBaseAction::VendorInRange()
{
    // Reuses SelectVendorNpc() (NewRpgBaseAction.cpp:1012-1034) which caps at
    // sPlayerbotAIConfig.pastimeRepairSellRadius via "nearest npcs" value.
    return !SelectVendorNpc().IsEmpty();
}

bool NewRpgBaseAction::EnemyNearForPvp()
{
    // Open-world PvP only (not in an instance/BG). Mirror CheckRpgStatusAvailable(RPG_OUTDOOR_PVP)
    // for the zone gate (lines 1899-1908), then probe "nearest enemy players" value.
    if (!InOpenWorld())
        return false;
    if (!bot->IsPvP())
        return false;
    uint32 zoneId = bot->GetZoneId();
    if (zoneId == AREA_NAGRAND)
        return false;
    OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
    if (!outdoorPvP)
        return false;

    // "nearest enemy players" value (ValueContext.h:430; NearestEnemyPlayersValue; sightDistance range).
    GuidVector enemies = AI_VALUE(GuidVector, "nearest enemy players");
    return !enemies.empty();
}

// ── end occupation-machine Task 2 predicates ─────────────────────────────────

// ── occupation-state-machine Task 4: NEEDS→DECIDE resolver ───────────────────
// Decide() runs ONLY at occupation boundaries (the IDLE case in NewRpgStatusUpdateAction::Execute
// and occupation exits). It is NOT a per-tick call. All predicates are O(1)/cheap-proximity.
//
// CRASH RULE (the #1 risk here): a bad_variant_access on a MapUpdater worker = world terminate.
// Every ChangeTo* call below is followed by an immediate `return` and we touch no variant data
// afterwards. Any rpgInfo.data read uses std::get_if + a null-guard, never throwing std::get<>.

bool NewRpgBaseAction::RecoverNeeded()
{
    // LAYER-1 survival need. HealthLow()/ManaLow() already qualify non-combat internally, so
    // a bot mid-fight never trips RECOVER. ManaLow() self-gates to mana-using classes
    // (GetMaxPower(POWER_MANA) > 0), so the "caster" qualifier is folded into the predicate.
    return HealthLow() || ManaLow();
}

bool NewRpgBaseAction::UpkeepNeeded()
{
    // LAYER-1 logistics need. Any one trigger sends the bot on a town errand (UPKEEP):
    // worn gear, full bags, missing gather tool, or the maintenance window elapsed.
    return DurabilityLow() || BagsFull() || MissingToolsOrReagents() || MaintenanceOverdue();
}

bool NewRpgBaseAction::OccupationFeasible(NewRpgStatus status)
{
    // Precondition ONLY (spec §5.1). Weight / cooldown shaping happens in Decide().
    switch (status)
    {
        case RPG_DO_QUEST:
            return HasActionableQuest();
        case RPG_GATHERING_CIRCUIT:
            return HasGatherProfAndTool() && NodeInRange(sPlayerbotAIConfig.gatheringCircuitRadius);
        case RPG_OUTDOOR_PVP:
            return EnemyNearForPvp();
        case RPG_REST:
            // HubLife is now hub-gated: a far bot never strands in FISH/FIELD_REST.
            return NearHub(sPlayerbotAIConfig.rpgNearHubRadius);
        case RPG_GO_GRIND:
            // Always-feasible wild default; EnterOccupation does the Grind-vs-RestAtHub split.
            return InOpenWorld();
        default:
            return false;
    }
}

void NewRpgBaseAction::EnterOccupation(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_DO_QUEST:
        {
            struct Cand { uint32 questId; const Quest* quest; POIInfo poi; bool complete; };
            std::vector<Cand> cands;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (!questId || botAI->IsQuestLowPriority(questId))
                    continue;
                std::vector<POIInfo> poiInfo;
                if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true, /*requireInZone=*/false))
                    continue;
                Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                if (!quest)
                    continue;
                bool complete = bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE;
                cands.push_back({questId, quest, poiInfo.front(), complete});
            }
            if (cands.empty())
            {
                FallToFarmOrRest();   // acquire-fail → always commits, never a silent Idle
                return;
            }
            std::sort(cands.begin(), cands.end(), [this](Cand const& a, Cand const& b)
            {
                if (a.complete != b.complete)
                    return a.complete;                          // turn-ins first
                return DistToPoi(a.poi) < DistToPoi(b.poi);    // then nearest
            });
            Cand const& pick = cands.front();
            botAI->rpgInfo.ChangeToDoQuest(pick.questId, pick.quest,
                WorldPosition(pick.poi.mapId, pick.poi.pos.x, pick.poi.pos.y, 0.0f));
            return;                                             // CRASH RULE: return now, touch nothing
        }
        case RPG_GATHERING_CIRCUIT:
        {
            uint32 maxNodes = urand(sPlayerbotAIConfig.gatheringCircuitMinNodes,
                                    sPlayerbotAIConfig.gatheringCircuitMaxNodes);
            botAI->rpgInfo.ChangeToGatheringCircuit(maxNodes);
            return;                                             // CRASH RULE
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return;                                             // CRASH RULE
        }
        case RPG_REST:
        {
            botAI->rpgInfo.ChangeToRest();
            return;                                             // CRASH RULE
        }
        case RPG_GO_GRIND:
        {
            // Grind-vs-RestAtHub default split (FallToFarmOrRest semantics): near a hub → rest in
            // place; else grind at a real grind pos, falling back to the current-pos anchor (always
            // non-empty so the commit cannot fail). Never dead-ends to Idle.
            if (NearHub(sPlayerbotAIConfig.rpgNearHubRadius))
            {
                botAI->rpgInfo.ChangeToRest();
                return;                                         // CRASH RULE
            }
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return;                                         // CRASH RULE
            }
            botAI->rpgInfo.ChangeToGoGrind(WorldPosition(bot));   // current-pos anchor, non-empty
            return;                                             // CRASH RULE
        }
        default:
        {
            // Unknown / unsupported pick: never dead-end to Idle — commit a safe occupation.
            FallToFarmOrRest();
            return;
        }
    }
}

void NewRpgBaseAction::Decide()
{
    // LAYER 1 — NEEDS (strict priority). Inert until Task 5 fills the bodies.
    if (RecoverNeeded())  { botAI->rpgInfo.ChangeToRecover(); return; }   // RECOVER first (survival)
    if (UpkeepNeeded())   { botAI->rpgInfo.ChangeToUpkeep();  return; }

    // LAYER 2 — DECIDE (weighted-random over the FEASIBLE productive set).
    struct Cand { NewRpgStatus s; uint32 w; };
    std::vector<Cand> feasible;
    uint32 sum = 0;
    for (NewRpgStatus s : {RPG_DO_QUEST, RPG_GATHERING_CIRCUIT, RPG_OUTDOOR_PVP,
                           RPG_REST /*HubLife*/, RPG_GO_GRIND /*+RestAtHub default*/})
    {
        if (!OccupationFeasible(s))
            continue;                                       // precondition ONLY (spec §5.1)
        uint32 w = sPlayerbotAIConfig.occupationWeight[s];
        if (w == 0)
            continue;
        uint32 cdMs = sPlayerbotAIConfig.occupationCooldownMs[s];
        if (cdMs && GetMSTimeDiffToNow(botAI->rpgInfo.lastFinished[s]) < cdMs)
            w = std::max<uint32>(1u, static_cast<uint32>(std::lround(w * sPlayerbotAIConfig.occupationCooldownFrac)));
        feasible.push_back({s, w});
        sum += w;
    }
    if (feasible.empty() || sum == 0)
    {
        // true last resort — should be near-impossible (Grind/RestAtHub are always-feasible defaults).
        LOG_ERROR("playerbots",
                  "[RpgMachine] {} #{} map={} zone={} — DECIDE found ZERO feasible "
                  "(openWorld={} nearHub={}); entering Idle",
                  bot->GetName(), bot->GetGUID().GetCounter(), bot->GetMapId(), bot->GetZoneId(),
                  InOpenWorld(), NearHub(sPlayerbotAIConfig.rpgNearHubRadius));
        botAI->rpgInfo.ChangeToIdle();   // short dwell set in ChangeToIdle
        return;                          // CRASH RULE
    }
    uint32 roll = urand(1, sum);
    uint32 acc = 0;
    NewRpgStatus pick = feasible.back().s;
    for (auto const& c : feasible)
    {
        acc += c.w;
        if (acc >= roll)
        {
            pick = c.s;
            break;
        }
    }
    if (sPlayerbotAIConfig.rpgMachineDebugLog)
        LOG_DEBUG("playerbots", "[RpgMachine] {} DECIDE pick={} (nFeasible={})",
                  bot->GetName(), static_cast<uint32>(pick), feasible.size());
    EnterOccupation(pick);   // ChangeTo* + ACQUIRE seed
}
