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
    const uint32 kOneShots_LoiterTalk[] = { EMOTE_ONESHOT_TALK, EMOTE_ONESHOT_QUESTION, EMOTE_ONESHOT_POINT,
                                            EMOTE_ONESHOT_LAUGH };
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
                botAI->TellMasterNoFacing("Quest accepted " + ChatHelper::FormatQuest(quest));
            BroadcastHelper::BroadcastQuestAccepted(botAI, bot, quest);
            botAI->rpgStatistic.questAccepted++;
            LOG_DEBUG("playerbots", "[New RPG] {} accept quest {}", bot->GetName(), quest->GetQuestId());
        }
        if (status == QUEST_STATUS_COMPLETE && bot->CanRewardQuest(quest, 0, false))
        {
            TurnInQuest(quest, guid);
            if (botAI->GetMaster())
                botAI->TellMasterNoFacing("Quest rewarded " + ChatHelper::FormatQuest(quest));
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
                botAI->TellMasterNoFacing("Quest dropped " + ChatHelper::FormatQuest(quest));
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
                botAI->TellMasterNoFacing("Quest dropped " + ChatHelper::FormatQuest(quest));
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
            botAI->TellMasterNoFacing("Quest dropped " + ChatHelper::FormatQuest(quest));
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

ObjectGuid NewRpgBaseAction::SelectSocialPartner()
{
    GuidVector friends = AI_VALUE(GuidVector, "nearest friendly players");
    Player* best = nullptr;
    float bestDist = sPlayerbotAIConfig.pastimeSocialRadius;
    bool sawTarget = false;
    for (ObjectGuid& guid : friends)
    {
        Player* other = ObjectAccessor::FindPlayer(guid);
        if (!other || other == bot || !other->IsInWorld())
            continue;
        if (other->isDead() || other->IsInCombat())
            continue;
        if (bot->GetExactDist(other) > sPlayerbotAIConfig.pastimeSocialRadius)
            continue;

        // In-radius base candidate confirmed — count once regardless of downstream filters.
        if (!sawTarget) { sawTarget = true; }

        bool isBot = sRandomPlayerbotMgr.IsRandomBot(other);
        if (!isBot)
        {
            if (!sPlayerbotAIConfig.pastimeSocialIncludePlayers)
                continue;   // real players only if opted in
        }
        else
        {
            // bot must be idle-ish OR already socializing (don't pester busy bots)
            PlayerbotAI* oai = GET_PLAYERBOT_AI(other);
            if (oai)
            {
                NewRpgStatus st = oai->rpgInfo.GetStatus();
                // RPG_PASTIME/NewRpgInfo::Pastime removed (rest-hub-unification Task 9);
                // socializing arm deleted. Check idleish only.
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

bool NewRpgBaseAction::GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete)
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
            if (qPoi.MapId != bot->GetMapId())
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

            if (bot->GetDistance2d(dx, dy) >= 1500.0f)
                continue;

            float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

            if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
                continue;

            if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
                continue;

            poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
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
        if (qPoi.MapId != bot->GetMapId())
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

        if (bot->GetDistance2d(dx, dy) >= 1500.0f)
            continue;

        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            continue;

        if (bot->GetZoneId() != bot->GetMap()->GetZoneId(bot->GetPhaseMask(), dx, dy, dz))
            continue;

        poiInfo.push_back({{dx, dy}, qPoi.ObjectiveIndex});
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

bool NewRpgBaseAction::RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus)
{
    std::vector<NewRpgStatus> availableStatus;
    std::vector<uint32> statusWeight;
    uint32 probSum = 0;
    for (NewRpgStatus status : candidateStatus)
    {
        uint32 base = sPlayerbotAIConfig.RpgStatusProbWeight[status];
        if (base == 0)
            continue;
        if (!CheckRpgStatusAvailable(status))
            continue;

        uint32 weight = base;  // Enable=0 -> weight==base -> legacy roulette byte-for-byte
        if (sPlayerbotAIConfig.rpgSatiationEnable)
        {
            BotActivityCategory cat = CategoryOf(status);
            float sat = (cat < CAT_COUNT) ? botAI->rpgInfo.satiation[cat] : 0.0f;
            float appeal = base * RpgSatiationSuppress(sat, sPlayerbotAIConfig.rpgSatiationSuppressExponent);
            float floorAppeal = base * sPlayerbotAIConfig.rpgSatiationMinAppealFrac;
            if (appeal < floorAppeal)
                appeal = floorAppeal;
            // scale to integer so we keep the existing urand roulette; never 0 for an eligible status
            weight = std::max<uint32>(1u, static_cast<uint32>(std::lround(appeal * 1000.0f)));
        }

        availableStatus.push_back(status);
        statusWeight.push_back(weight);
        probSum += weight;
    }
    // Safety check. Default to "rest" if nothing is eligible.
    if (availableStatus.empty() || probSum == 0)
    {
        botAI->rpgInfo.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        return true;
    }
    uint32 rand = urand(1, probSum);
    uint32 accumulate = 0;
    NewRpgStatus chosenStatus = RPG_STATUS_END;
    for (size_t i = 0; i < availableStatus.size(); ++i)
    {
        accumulate += statusWeight[i];
        if (accumulate >= rand)
        {
            chosenStatus = availableStatus[i];
            break;
        }
    }

    if (sPlayerbotAIConfig.rpgSatiationEnable && chosenStatus != RPG_STATUS_END)
    {
        NewRpgInfo const& ri = botAI->rpgInfo;
        LOG_DEBUG("playerbots",
                  "[RpgSatiation] Bot #{} sat[ADV,SOC,WORK,TRV,PVP]=[{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}] chose={}",
                  bot->GetGUID().GetCounter(),
                  ri.satiation[CAT_ADVENTURE], ri.satiation[CAT_SOCIAL], ri.satiation[CAT_WORK],
                  ri.satiation[CAT_TRAVEL], ri.satiation[CAT_PVP],
                  static_cast<uint32>(chosenStatus));
    }

    switch (chosenStatus)
    {
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_PASTIME:
            // Deleted statuses (rest-hub-unification Task 9): types/ChangeTo* removed.
            // These enum values remain so the weight-load lines compile; roulette will
            // never pick them once the IDLE candidate list is pruned in Task 10.
            return false;
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            if (pos != WorldPosition())
            {
                botAI->rpgInfo.ChangeToGoGrind(pos);
                return true;
            }
            return false;
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    availableQuests.push_back(questId);
                }
            }
            if (availableQuests.size())
            {
                uint32 questId = availableQuests[urand(0, availableQuests.size() - 1)];
                const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
                if (quest)
                {
                    botAI->rpgInfo.ChangeToDoQuest(questId, quest);
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            if (SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
            {
                botAI->rpgInfo.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, path);
                return true;
            }
            return false;
        }
        case RPG_IDLE:
        {
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        case RPG_REST:
        {
            // InnPull retired (rest-hub-unification Task 9): hub selection + travel now handled
            // by the five-phase state machine (P0/P1 in NewRpgStatusUpdateAction::Execute).
            botAI->rpgInfo.ChangeToRest();
            return true;
        }
        case RPG_OUTDOOR_PVP:
        {
            botAI->rpgInfo.ChangeToOutdoorPvp();
            return true;
        }
        case RPG_TRAVEL_MOUNT:
        {
            WorldPosition pos;
            if (SelectFarTaxiDest(pos))
            {
                botAI->rpgInfo.ChangeToTravelMount(pos);
                return true;
            }
            return false;
        }
        case RPG_GATHERING_CIRCUIT:
        {
            uint32 maxNodes = urand(sPlayerbotAIConfig.gatheringCircuitMinNodes,
                                    sPlayerbotAIConfig.gatheringCircuitMaxNodes);
            botAI->rpgInfo.ChangeToGatheringCircuit(maxNodes);
            return true;
        }
        default:
        {
            botAI->rpgInfo.ChangeToRest();
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
    }
    return false;
}

bool NewRpgBaseAction::CheckRpgStatusAvailable(NewRpgStatus status)
{
    switch (status)
    {
        case RPG_IDLE:
        case RPG_REST:
            return true;
        case RPG_WANDER_RANDOM:
        case RPG_WANDER_NPC:
        case RPG_PASTIME:
            return false; // deleted in rest-hub-unification
        case RPG_GO_GRIND:
        {
            WorldPosition pos = SelectRandomGrindPos(bot);
            return pos != WorldPosition();
        }
        case RPG_DO_QUEST:
        {
            std::vector<uint32> availableQuests;
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 questId = bot->GetQuestSlotQuestId(slot);
                if (botAI->lowPriorityQuest.find(questId) != botAI->lowPriorityQuest.end())
                    continue;

                std::vector<POIInfo> poiInfo;
                if (GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
                {
                    return true;
                }
            }
            return false;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            uint32 flightMasterEntry = 0;
            WorldPosition flightMasterPos;
            std::vector<uint32> path;
            return SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path);
        }
        case RPG_OUTDOOR_PVP:
        {
            if (!bot->IsPvP())
                return false;
            uint32 zoneId = bot->GetZoneId();
            if (zoneId == AREA_NAGRAND)
                return false;

            OutdoorPvP* outdoorPvP = sOutdoorPvPMgr->GetOutdoorPvPToZoneId(zoneId);
            return outdoorPvP != nullptr;
        }
        case RPG_TRAVEL_MOUNT:
        {
            WorldPosition pos;
            return SelectFarTaxiDest(pos);
        }
        case RPG_GATHERING_CIRCUIT:
            return BotHasGatheringProfession(bot);
        default:
            return false;
    }
    return false;
}
