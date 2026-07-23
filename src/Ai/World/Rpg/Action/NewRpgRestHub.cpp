#include "NewRpgRestHub.h"
#include "NewRpgAction.h"       // NewRpgStatusUpdateAction (the helpers live on it)
#include "Unit.h"               // UNIT_NPC_FLAG_*
#include "GameObject.h"         // GAMEOBJECT_TYPE_*
#include "Creature.h"
#include "Player.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Playerbots.h"         // GET_PLAYERBOT_AI, sPlayerbotAIConfig, AI_VALUE, sPlayerbotsMgr
#include "SharedDefines.h"      // EMOTE_STATE_DANCE, UNIT_NPC_FLAG_*
#include "DBCStores.h"          // sAreaTableStore, AreaTableEntry, AREA_FLAG_ALLOW_DUELS
#include "ChatHelper.h"         // chat->FormatWorldobject
#include "RandomPlayerbotMgr.h" // sRandomPlayerbotMgr
#include "Random.h"             // urand
#include "Timer.h"              // getMSTime
#include "CellImpl.h"           // Cell::VisitObjects — direct grid scan for SelectNearestNpcWithFlag/Go
#include "GridNotifiers.h"      // Acore::UnitListSearcher, Acore::AnyUnitInObjectRangeCheck
#include "GridNotifiersImpl.h"  // searcher template impls
#include "NearestGameObjects.h" // AnyGameObjectInObjectRangeCheck, Acore::GameObjectListSearcher
#include "Ai/Base/Actions/FishingAction.h"   // FindWaterRadial (water presence gate for RS_FISH)
#include <algorithm>

// PlayerbotAIConfig.h cannot include NewRpgRestHub.h (circular), so its
// restHubWeight[] is hard-sized to a literal 16. Guard that literal here.
static_assert(RS_COUNT == 16, "PlayerbotAIConfig::restHubWeight[16] must match RS_COUNT");

const RestSubtypeDef kRestTable[RS_COUNT] =
{
  // id,                 name,             target,             npcFlagOrGoType,            needsHub, palette,         poiVariant,      functional
  { RS_TAVERN,           "TAVERN",         TK_INN_CHAIR,       0,                          true,  BEH_REST,        POI_NONE,        true  },
  { RS_CLASS_TRAINER,    "CLASS_TRAINER",  TK_NPC_FLAG,        UNIT_NPC_FLAG_TRAINER,      true,  BEH_LOITER,      POI_TRAINER,     false },
  { RS_PROFESSION_CRAFT, "PROFESSION_CRAFT",TK_FORGE_OR_TRAINER,0,                         true,  BEH_CRAFT,       POI_FORGE,       true  },
  { RS_VENDOR,           "VENDOR",         TK_VENDOR,          0,                          true,  BEH_REPAIR_SELL, POI_NONE,        true  },
  { RS_QUEST_GIVER,      "QUEST_GIVER",    TK_NPC_FLAG,        UNIT_NPC_FLAG_QUESTGIVER,   true,  BEH_WANDER_NPC,  POI_NONE,        false },
  { RS_BANK,             "BANK",           TK_NPC_FLAG,        UNIT_NPC_FLAG_BANKER,       true,  BEH_LOITER,      POI_BANKER,      false },
  { RS_AUCTION_HOUSE,    "AH",             TK_NPC_FLAG,        UNIT_NPC_FLAG_AUCTIONEER,   true,  BEH_LOITER,      POI_AUCTIONEER,  false },
  { RS_MAILBOX,          "MAIL",           TK_GO_TYPE,         GAMEOBJECT_TYPE_MAILBOX,    true,  BEH_LOITER,      POI_MAILBOX,     false },
  { RS_FLIGHT_MASTER,    "FLIGHT",         TK_NPC_FLAG,        UNIT_NPC_FLAG_FLIGHTMASTER, true,  BEH_FLIGHT,      POI_NONE,        false },
  { RS_SOCIAL,           "SOCIAL",         TK_SOCIAL,          0,                          true,  BEH_SOCIAL,      POI_NONE,        false },
  { RS_STROLL,           "STROLL",         TK_STROLL,          0,                          true,  BEH_WANDER_NPC,  POI_NONE,        false },
  { RS_SPECTATE,         "SPECTATE",       TK_SPECTATE,        0,                          true,  BEH_SPECTATE,    POI_NONE,        false },
  { RS_DUMMY,            "DUMMY",          TK_DUMMY,           0,                          true,  BEH_DUMMY,       POI_NONE,        true  },
  { RS_DUEL,             "DUEL",           TK_DUEL,            0,                          false, BEH_NONE,        POI_NONE,        true  },
  { RS_FISH,             "FISH",           TK_WATER,           0,                          false, BEH_FISH,        POI_NONE,        true  },
  { RS_FIELD_REST,       "FIELD_REST",     TK_IN_PLACE,        0,                          false, BEH_REST,        POI_NONE,        true  },
};

PropKind kindOf(uint8 restSubtype)
{
    switch (restSubtype)
    {
        case RS_BANK:          return PK_BANKER;
        case RS_AUCTION_HOUSE: return PK_AUCTIONEER;
        case RS_CLASS_TRAINER: return PK_CLASS_TRAINER;
        case RS_MAILBOX:       return PK_MAILBOX;
        case RS_DUMMY:         return PK_DUMMY;
        case RS_VENDOR:        return PK_VENDOR;
        default:               return PROPKIND_COUNT;
    }
}

// ───────────────────────────────────────────────────────────────────────────
// C1/R1 — witness-gated hub travel, tri-state. NEVER uses MoveFarTo's return as
// an arrival predicate: arrival is driven purely by an explicit distance check
// (10y, mirroring the TravelMount arrive test at NewRpgAction.cpp:363).
//   - no real witness near the bot OR the hub -> instant TeleportTo, HUB_ARRIVED
//   - witnessed + beyond restHubTravelBudget                 -> HUB_GIVE_UP
//   - witnessed + within 10y                                 -> HUB_ARRIVED
//   - otherwise issue/continue movement (auto-mounts long hauls) -> HUB_EN_ROUTE
// ───────────────────────────────────────────────────────────────────────────
HubTravel NewRpgStatusUpdateAction::TravelToHubOrTeleport(WorldPosition const& hub)
{
    bool witnessed = IsRealPlayerNear(WorldPosition(bot), sPlayerbotAIConfig.restHubWitnessRange)
                  || IsRealPlayerNear(hub, sPlayerbotAIConfig.restHubWitnessRange);
    if (!witnessed)
    {
        bot->TeleportTo(hub.GetMapId(), hub.GetPositionX(), hub.GetPositionY(),
                        hub.GetPositionZ(), bot->GetOrientation());
        return HUB_ARRIVED;
    }
    if (bot->GetExactDist(&hub) > sPlayerbotAIConfig.restHubTravelBudget)
        return HUB_GIVE_UP;
    if (bot->GetExactDist(&hub) <= 10.0f /* arrive threshold (cf. TravelMount :363) */)
        return HUB_ARRIVED;
    MoveFarTo(hub);   // issue/continue movement; auto-mounts long hauls. NOT an arrival predicate (R1).
    return HUB_EN_ROUTE;
}

// ───────────────────────────────────────────────────────────────────────────
// C4 — direct grid-scan selectors for hub POI resolution.
//
// The old implementation iterated AI_VALUE(GuidVector,"nearest npcs/game objects"),
// which is bounded by SightDistance (~75y). Banker, auctioneer, trainer,
// flightmaster, and mailbox all routinely sit >75y from the inn → the list was
// empty → every TK_NPC_FLAG/TK_GO_TYPE subtype fell back to RS_FIELD_REST.
//
// Fix (mirrors SelectTrainingDummy in NewRpgBaseAction.cpp): use Cell::VisitObjects
// at restHubPoiRadius (default 150y, town-sized, tunable). This runs at acquire-time
// only (once per rest episode via AcquireSubtypeTarget → P2 on arrival), NOT per tick,
// so the bounded grid scan cost is acceptable — same as SelectTrainingDummy's pattern.
// ───────────────────────────────────────────────────────────────────────────
ObjectGuid NewRpgBaseAction::SelectNearestNpcWithFlag(uint32 npcFlag) const
{
    float const radius = sPlayerbotAIConfig.restHubPoiRadius;

    // Direct creature grid scan at town-sized radius; SightDistance (75y) is too narrow
    // for hub NPCs (banker/auctioneer/trainer/flightmaster). Pattern from SelectTrainingDummy.
    std::list<Unit*> targets;
    Acore::AnyUnitInObjectRangeCheck u_check(bot, radius);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitObjects(bot, searcher, radius);

    Creature* best = nullptr;
    float bestDist = radius;
    for (Unit* u : targets)
    {
        Creature* c = u ? u->ToCreature() : nullptr;
        if (!c || !c->IsInWorld())
            continue;
        if (!c->HasNpcFlag((NPCFlags)npcFlag))
            continue;
        if (c->IsHostileTo(bot))
            continue;   // skip only genuinely hostile NPCs. Capital service NPCs (banker/auctioneer/
                        // trainer) carry NEUTRAL-reaction faction templates, not >= REP_FRIENDLY, so the
                        // old IsFriendlyTo gate filtered every one out (100% NO-prop at confirm); !IsHostileTo
                        // resolves neutral props while still never dragging the bot toward a hostile guard.
        if (IsInForbiddenFactionQuarter(bot, c))
            continue;   // dalaran-quarter-npc-exclusion: neutral but in the opposing embassy
        float d = bot->GetExactDist(c);
        if (d <= bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best ? best->GetGUID() : ObjectGuid();
}

ObjectGuid NewRpgBaseAction::SelectNearestGoOfType(uint32 goType) const
{
    float const radius = sPlayerbotAIConfig.restHubPoiRadius;

    // Direct GO grid scan at town-sized radius; mailbox can sit >75y from the inn.
    // Pattern mirrors SelectNearestNpcWithFlag above; AnyGameObjectInObjectRangeCheck
    // already checks isSpawned() + GetGOInfo() inside its operator().
    std::list<GameObject*> goTargets;
    AnyGameObjectInObjectRangeCheck go_check(bot, radius);
    Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> goSearcher(bot, goTargets, go_check);
    Cell::VisitObjects(bot, goSearcher, radius);

    GameObject* best = nullptr;
    float bestDist = radius;
    for (GameObject* go : goTargets)
    {
        if (!go->isSpawned())
            continue;
        if (go->GetGoType() != goType)
            continue;
        if (IsInForbiddenFactionQuarter(bot, go))
            continue;   // dalaran-quarter-npc-exclusion: prop in the opposing embassy
        float d = bot->GetExactDist(go);
        if (d <= bestDist)
        {
            bestDist = d;
            best = go;
        }
    }
    return best ? best->GetGUID() : ObjectGuid();
}

// ───────────────────────────────────────────────────────────────────────────
// occupation-upkeep-two-tier Task 4 — PoseAtProp.
//
// Cosmetic capital city pose: stand at a prop (banker/auctioneer/mailbox/trainer)
// and emote for `dwellMs`. Pure RP — NO transaction (the bank/AH/mail/trainer steps
// are poses only). Reuses the SAME per-subtype prop resolution the rest engine uses:
// kRestTable[restSubtype].target picks the resolver (TK_NPC_FLAG → SelectNearestNpcWithFlag,
// TK_GO_TYPE → SelectNearestGoOfType) and .npcFlagOrGoType supplies the flag/type. So
// RS_BANK/RS_AUCTION_HOUSE/RS_CLASS_TRAINER resolve to a creature guid via NPC flag, and
// RS_MAILBOX resolves to a GameObject guid via GO type — exactly as EngageAndHold does.
//
// The mailbox wrinkle: RS_MAILBOX's target is a GameObject, not a creature. ObjectAccessor::
// GetUnit will NOT resolve it; we re-resolve the prop each tick as a WorldObject via
// ObjectAccessor::GetWorldObject (a GameObject IS-A WorldObject) and SetFacingToObject takes a
// WorldObject*. So one facing/position path covers both NPC props and the mailbox GO.
//
// Crash discipline: `up` is a live reference into the caller's variant (the caller already did
// the get_if) — we never re-enter throwing variant access here. We store only the ObjectGuid
// (up.target) and re-resolve the WorldObject each tick (no cached raw pointers across ticks).
//
// Return contract:
//   true  = still posing (caller `return`s this tick)
//   false = done OR prop unresolvable (caller advances up.step and clears up.target)
// ───────────────────────────────────────────────────────────────────────────
bool NewRpgBaseAction::PoseAtProp(uint8 restSubtype, uint32 dwellMs, NewRpgInfo::Upkeep& up)
{
    // Guard the kRestTable index up front (both ACQUIRE and PERFORM index it). RS_NONE(0xFF) or any
    // out-of-range subtype → skip the step rather than read past the table.
    if (restSubtype >= RS_COUNT)
    {
        LOG_WARN("playerbots",
                 "[RpgMachine] {} #{} map={} — UPKEEP capital pose: bad subtype={}; skipping",
                 bot->GetName(), bot->GetGUID().GetCounter(), bot->GetMapId(), uint32(restSubtype));
        return false;   // caller advances step
    }

    PropKind const kind = kindOf(restSubtype);

    // ── ACQUIRE the prop's capital coordinate (once), then travel + settle before the local scan ──
    // Gate on posePos (written once here, untouched by TRAVEL/SETTLE/CONFIRM) so ACQUIRE runs EXACTLY
    // ONCE per pose step. Gating on up.target instead livelocked: target is set only later, in CONFIRM,
    // which runs only after SETTLE — so ACQUIRE re-ran every tick, re-zeroing poseArriveT and forcing
    // TRAVEL to re-return, never reaching SETTLE/CONFIRM (the step never terminated).
    if (up.posePos == WorldPosition())
    {
        // Resolve this prop's coordinate map-wide within the chosen capital (NOT a 150y blind scan
        // from a single banker anchor — that left 100% NO prop). capitalZone==0 (fallback acquire)
        // or a city lacking this kind → empty → clean skip.
        WorldPosition dest = (kind < PROPKIND_COUNT && up.capitalZone != 0)
            ? sTravelMgr.SelectCapitalPropPos(up.capitalZone, kind)
            : WorldPosition();

        if (dest == WorldPosition())
        {
            LOG_WARN("playerbots",
                     "[RpgMachine] {} #{} map={} — UPKEEP capital pose subtype={} NO prop; skipping",
                     bot->GetName(), bot->GetGUID().GetCounter(), bot->GetMapId(), uint32(restSubtype));
            return false;   // caller advances step
        }

        // dalaran-quarter-npc-exclusion (audit finding): capitalPropLocations is keyed by ZONE
        // (TravelMgr.cpp resolves area->zone at build time), so Sunreaver's Sanctuary and Silver
        // Enclave props are pooled into the SAME Dalaran(4395) bucket with no faction split.
        // SelectCapitalPropPos above can therefore hand back a raw coordinate inside the bot's
        // OPPOSING quarter — and TRAVEL below would walk/teleport the bot there BEFORE the
        // CONFIRM-time SelectNearestNpcWithFlag/SelectNearestGoOfType guards ever run, tripping
        // the trespasser eviction. Reject here so it never leaves ACQUIRE.
        if (up.capitalZone == 4395)
        {
            uint32 const destArea = dest.getAreaId();
            bool const destForbidden = bot->GetTeamId() == TEAM_ALLIANCE
                ? (destArea == AREA_SUNREAVERS_SANCTUARY)
                : (destArea == AREA_SILVER_ENCLAVE);
            if (destForbidden)
            {
                LOG_DEBUG("playerbots",
                          "[QuarterGuard] {} rejected capital-pose destination in forbidden quarter (area {})",
                          bot->GetName(), destArea);
                return false;   // caller advances step; a later cycle may roll a valid prop
            }
        }

        // Per-bot horizontal scatter: SelectCapitalPropPos hands every bot the SAME cached point, so
        // multiple bots routing to one prop pile up on the identical spot. A few yards of jitter spreads
        // them. Keep the prop's z (the cache no longer lifts it by +2) — small scatter on a flat capital
        // service floor stays on the ground, and witnessed (walking) bots ground-snap via pathfinding.
        up.posePos     = WorldPosition(dest.GetMapId(),
                                       dest.GetPositionX() + frand(-4.0f, 4.0f),
                                       dest.GetPositionY() + frand(-4.0f, 4.0f),
                                       dest.GetPositionZ(),
                                       dest.GetOrientation());
        up.stepStartMs = 0;          // dwell starts only after settle + confirm
        float const wf = sPlayerbotAIConfig.upkeepWorkloadScaleEnable
                             ? std::max(up.workloadPct / 100.0f, sPlayerbotAIConfig.upkeepWorkloadDwellFloor)
                             : 1.0f;
        up.dwellMs     = (uint32)(dwellMs * wf);
        // up.target stays empty until the post-settle confirm scan resolves the live object.
    }

    // ── TRAVEL: hop to the prop's capital coordinate (witness-gated; teleports when unwitnessed) ──
    if (up.poseArriveT == 0)
    {
        if (bot->GetExactDist(&up.posePos) > INTERACTION_DISTANCE)
        {
            TravelResult const tr = DriveTravel(up.posePos);
            if (tr == TravelResult::GAVE_UP)
                return false;        // witnessed + beyond budget → skip this pose, advance step
            if (tr != TravelResult::ARRIVED)
                return true;         // still EN_ROUTE
        }
        up.poseArriveT = getMSTime();   // arrived → start the settle
        return true;
    }

    // ── SETTLE: let the post-teleport grid + nearest caches load before the confirm scan ──
    // (mirrors the rest engine, NewRpgAction.cpp:399-404; without it the scan reads the STALE grid.)
    if (GetMSTimeDiffToNow(up.poseArriveT) < 2500)
        return true;

    // ── CONFIRM: resolve the live object now that we're on top of it (150y scan is ample here) ──
    if (up.target.IsEmpty())
    {
        RestSubtypeDef const& d = kRestTable[restSubtype];
        ObjectGuid prop;
        if (d.target == TK_DUMMY)
            prop = SelectTrainingDummy();                         // training dummy (Creature, no npcFlag)
        else if (d.target == TK_GO_TYPE)
            prop = SelectNearestGoOfType(d.npcFlagOrGoType);     // mailbox (GameObject)
        else
            prop = SelectNearestNpcWithFlag(d.npcFlagOrGoType);  // banker/auctioneer/trainer (NPC)

        if (prop.IsEmpty())
        {
            LOG_WARN("playerbots",
                     "[RpgMachine] {} #{} map={} — UPKEEP capital pose subtype={} NO prop (confirm); skipping",
                     bot->GetName(), bot->GetGUID().GetCounter(), bot->GetMapId(), uint32(restSubtype));
            return false;   // caller advances step
        }
        up.target      = prop;
        up.stepStartMs = getMSTime();   // dwell clock starts now
        // upkeep-sociability: roll one held pose for this bot's dwell so the capital crowd isn't all EMOTE_STATE_TALK.
        RestSubtypeDef const& pdRoll = kRestTable[restSubtype];
        up.chosenDwellPose = ResolveHeldPose(pdRoll.palette,
                                             static_cast<uint8>(pdRoll.poiVariant),
                                             (uint8)urand(0, 5));   // rollIdx wrapped by ResolveHeldPose
    }

    // ── PERFORM: face the prop (Unit OR GameObject) + hold the emote pose for the dwell ──
    if (WorldObject* propObj = ObjectAccessor::GetWorldObject(*bot, up.target))
        bot->SetFacingToObject(propObj);
    // Mirror the rest hold phase verbatim (NewRpgAction.cpp:436-437): the (palette, poiVariant)
    // pair comes from the subtype's kRestTable row, NOT the raw subtype. For the capital props
    // these rows are BEH_LOITER with POI_BANKER/POI_AUCTIONEER/POI_MAILBOX/POI_TRAINER, so the
    // per-POI loiter palette is selected exactly as the rest engine selects it.
    RestSubtypeDef const& pd = kRestTable[restSubtype];
    TickEmoteCadence(pd.palette, static_cast<uint8>(pd.poiVariant),
                     /*skipSustainedPose=*/false, up.chosenDwellPose);

    if (GetMSTimeDiffToNow(up.stepStartMs) < up.dwellMs)
        return true;   // still posing
    return false;      // dwell elapsed → caller advances step (and clears up.target)
}

// PROFESSION_CRAFT target. NOTE (deviation): a creature's trainer_type is owned by the
// Trainer subsystem (keyed by Trainer::Type, reachable only through sObjectMgr's trainer
// store, not a plain Creature accessor), so the restHubTrainerTypeFidelity "prefer a
// tradeskill (type 2) trainer" preference is DEGRADED to "any trainer NPC". We still try a
// forge GO (anvil = GAMEOBJECT_TYPE_BARBER_CHAIR is wrong; the craft pose is in-place anyway)
// and fall back to any trainer. Flagged for the controller — wire trainer_type fidelity later.
ObjectGuid NewRpgStatusUpdateAction::SelectForgeOrProfTrainer() const
{
    // Best-effort: a profession/skill trainer NPC. (Fidelity to tradeskill-only trainers is
    // degraded — see note above.) The craft animation is a held in-place pose, so an exact
    // forge object is not required; a trainer NPC anchors the bot at a plausible craft spot.
    return SelectNearestNpcWithFlag(UNIT_NPC_FLAG_TRAINER);
}

// SPECTATE target — a nearby player currently dueling or dancing. Best-effort; empty if none.
ObjectGuid NewRpgStatusUpdateAction::SelectSpectateTarget() const
{
    GuidVector players = AI_VALUE(GuidVector, "nearest friendly players");
    Player* best = nullptr;
    float bestDist = sPlayerbotAIConfig.pastimeSocialRadius;
    for (ObjectGuid& guid : players)
    {
        Player* p = ObjectAccessor::FindPlayer(guid);
        if (!p || p == bot || !p->IsInWorld())
            continue;
        bool dueling = p->duel && p->duel->State == DUEL_STATE_IN_PROGRESS;
        bool dancing = p->GetUInt32Value(UNIT_NPC_EMOTESTATE) == EMOTE_STATE_DANCE;
        if (!dueling && !dancing)
            continue;
        float d = bot->GetExactDist(p);
        if (d <= bestDist)
        {
            bestDist = d;
            best = p;
        }
    }
    return best ? best->GetGUID() : ObjectGuid();
}

// ───────────────────────────────────────────────────────────────────────────
// Task 8 — STROLL waypoint route builder.
// Collects up to restHubStrollPoiCount nearby NPC positions from the
// "possible new rpg targets" GuidVector (the same source ChooseNpcOrGameObjectToInteract
// uses) and writes them into rest.strollPts. Falls back to the "nearest npcs" vector
// if the rpg-target list comes up short. Returns true if at least 1 point was stored
// (caller will accept the subtype); false → caller falls back to RS_FIELD_REST.
// ───────────────────────────────────────────────────────────────────────────
bool NewRpgStatusUpdateAction::BuildStrollRoute()
{
    auto* restp = std::get_if<NewRpgInfo::Rest>(&botAI->rpgInfo.data);
    if (!restp)
        return false;
    auto& rest = *restp;

    rest.strollPts.clear();
    rest.strollIdx = 0;
    rest.strollPauseUntil = 0;

    uint8 wantCount = sPlayerbotAIConfig.restHubStrollPoiCount;
    if (wantCount == 0)
        return false;

    // Primary source: "possible new rpg targets" (curated NPC list used by RPG_WANDER_NPC).
    GuidVector rpgTargets = AI_VALUE(GuidVector, "possible new rpg targets");
    for (ObjectGuid& guid : rpgTargets)
    {
        if (rest.strollPts.size() >= wantCount)
            break;
        Creature* c = ObjectAccessor::GetCreature(*bot, guid);
        if (!c || !c->IsInWorld())
            continue;
        rest.strollPts.push_back(WorldPosition(c));
    }

    // Fallback / top-up: "nearest npcs" (broader list).
    if (rest.strollPts.size() < wantCount)
    {
        GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
        for (ObjectGuid& guid : npcs)
        {
            if (rest.strollPts.size() >= wantCount)
                break;
            Creature* c = ObjectAccessor::GetCreature(*bot, guid);
            if (!c || !c->IsInWorld())
                continue;
            // Cheap de-dup: skip if a point very close to this one is already stored.
            WorldPosition pt(c);
            bool duplicate = false;
            for (WorldPosition& existing : rest.strollPts)
            {
                if (existing.distance(pt) < 5.0f)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                rest.strollPts.push_back(pt);
        }
    }

    return !rest.strollPts.empty();
}

// Local duel-partner selector. DEVIATION: NewRpgBaseAction.cpp's SelectDuelPartner is a
// file-static free function (internal linkage) and so is NOT linkable from this TU. Rather
// than touch NewRpgBaseAction.{h,cpp} (out of scope for this task / would risk a merge
// conflict), we mirror its selection logic here, minus the file-static g_pastimeSawTarget
// census counters (also internal to that TU).
// Two intentional changes vs the original, both consequences of this rewrite:
//   1) the original ALSO accepted a partner in RPG_PASTIME+ACTIVITY_SOCIAL; that branch is
//      dropped because RPG_PASTIME / NewRpgInfo::Pastime no longer exist (deleted in Task 5),
//      so std::get_if<Pastime> would not even compile.
//   2) socializing bots now live under the RPG_REST umbrella (RS_SOCIAL), and RPG_REST is
//      already in the `idleish` set below — so the old socializing case is COVERED, not lost.
static ObjectGuid RestHubSelectDuelPartner(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    AiObjectContext* context = botAI->GetAiObjectContext();
    GuidVector friends = AI_VALUE(GuidVector, "nearest friendly players");
    Player* best = nullptr;
    float bestDist = sPlayerbotAIConfig.pastimeDuelRadius;
    for (ObjectGuid& guid : friends)
    {
        Player* other = ObjectAccessor::FindPlayer(guid);
        if (!other || other == bot || !other->IsInWorld())
            continue;
        if (other->isDead() || other->IsInCombat())
            continue;
        if (other->GetHealthPct() < 90.0f)
            continue;   // mirrors the AcceptDuelAction <90% auto-decline gate
        if (bot->GetExactDist(other) > sPlayerbotAIConfig.pastimeDuelRadius)
            continue;

        bool isBot = sRandomPlayerbotMgr.IsRandomBot(other);
        if (!isBot)
        {
            if (!sPlayerbotAIConfig.pastimeDuelIncludePlayers)
                continue;
        }
        else
        {
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

// ───────────────────────────────────────────────────────────────────────────
// R2 — AcquireSubtypeTarget. Reachable during a status-transition window, so the
// Rest substruct is taken via std::get_if + null-guard (a throwing std::get<> would
// crash the MapUpdater worker on bad_variant_access). Returns true when the subtype
// has a usable target (or needs none); false when no target could be acquired.
// ───────────────────────────────────────────────────────────────────────────
bool NewRpgStatusUpdateAction::AcquireSubtypeTarget(RestSubtype st)
{
    auto* restp = std::get_if<NewRpgInfo::Rest>(&botAI->rpgInfo.data);
    if (!restp)
        return false;   // active alternative no longer Rest (R2)
    auto& rest = *restp;
    RestSubtypeDef const& d = kRestTable[st];
    switch (d.target)
    {
        case TK_INN_CHAIR:        rest.chair = SelectInnChair(15.0f); return true; // chair optional -> floor-sit fallback
        case TK_VENDOR:           rest.target = SelectVendorNpc(); return !rest.target.IsEmpty();
        case TK_DUMMY:            rest.target = SelectTrainingDummy(); return !rest.target.IsEmpty();
        case TK_DUEL:             rest.target = RestHubSelectDuelPartner(botAI); return !rest.target.IsEmpty();
        case TK_NPC_FLAG:         rest.target = SelectNearestNpcWithFlag(d.npcFlagOrGoType); return !rest.target.IsEmpty();
        case TK_GO_TYPE:          rest.target = SelectNearestGoOfType(d.npcFlagOrGoType); return !rest.target.IsEmpty();
        case TK_FORGE_OR_TRAINER: rest.target = SelectForgeOrProfTrainer(); return !rest.target.IsEmpty();
        case TK_WATER:            return true;  // fishing chain handled in EngageAndHold
        case TK_STROLL:           return BuildStrollRoute();  // Task 8 fills BuildStrollRoute
        case TK_SPECTATE:         rest.target = SelectSpectateTarget(); return !rest.target.IsEmpty();
        case TK_IN_PLACE:         return true;
    }
    return false;
}

// ───────────────────────────────────────────────────────────────────────────
// R1/R2 — EngageAndHold. Drives approach->engage off an EXPLICIT distance check
// (mirrors the REPAIR_SELL arm at NewRpgAction.cpp:632-650), never off a move
// call's return. Takes the Rest substruct via std::get_if + guard. After the
// fire-and-forget DUEL self-terminates (ChangeToIdle), it returns immediately
// touching no `rest` (R2). Contract:
//   true  = arrived & engaged this tick (lastReach just stamped)
//   false = still approaching, OR a fire-and-forget subtype self-terminated
//           (caller treats false as "return true, touch nothing more this tick")
// ───────────────────────────────────────────────────────────────────────────
bool NewRpgStatusUpdateAction::EngageAndHold()
{
    auto* restp = std::get_if<NewRpgInfo::Rest>(&botAI->rpgInfo.data);
    if (!restp)
        return false;   // active alternative no longer Rest (R2)
    auto& rest = *restp;
    if (rest.subtype == RS_NONE || rest.subtype >= RS_COUNT)
        return false;   // subtype not resolved yet — never index kRestTable with RS_NONE(0xFF)
    RestSubtypeDef const& d = kRestTable[rest.subtype];

    // ── Approach phase (explicit distance check; never trust the move-issue return) ──
    if (rest.target && !rest.target.IsEmpty())
    {
        WorldObject* targetObj = ObjectAccessor::GetWorldObject(*bot, rest.target);
        if (!targetObj)
        {
            // Target despawned mid-approach. Do NOT spin (lastReach is still 0, so the caller
            // would re-enter forever and never reach P4): end the episode and re-roll.
            botAI->rpgInfo.ChangeToIdle();   // R2: return now, touch no `rest` after this
            return false;
        }
        if (bot->GetExactDist(targetObj) > INTERACTION_DISTANCE)
        {
            MoveWorldObjectTo(rest.target);   // R1: issue movement, do NOT treat as arrival
            return false;
        }
    }
    else if (d.target == TK_IN_PLACE || d.target == TK_WATER || d.target == TK_STROLL || d.target == TK_INN_CHAIR)
    {
        // No single object to approach via rest.target: engage in place.
        //  - FIELD_REST / FISH: rest where standing.
        //  - STROLL: per-POI walking driven by TickStroll during the hold phase.
        //  - TAVERN (TK_INN_CHAIR): the chair lives in rest.chair, not rest.target; HoldSeat
        //    (P3 hold phase) moves the bot to the chair and sits it. So engage in place here.
    }
    else
    {
        // Subtype expected a target but none is held. P2 already falls back to FIELD_REST when
        // AcquireSubtypeTarget fails, so this is defensive — but never spin here: end + re-roll.
        botAI->rpgInfo.ChangeToIdle();   // R2: return now, touch no `rest` after this
        return false;
    }

    // ── Engage phase — runs once, on the tick we first arrive (lastReach unset) ──
    if (rest.lastReach != 0)
        return true;   // already engaged on a prior tick — let the RPG_REST hold phase proceed

    WorldObject* targetObj = (rest.target && !rest.target.IsEmpty())
                                 ? ObjectAccessor::GetWorldObject(*bot, rest.target)
                                 : nullptr;
    if (targetObj)
        bot->SetFacingToObject(targetObj);

    rest.lastReach = getMSTime();
    rest.dwellMs = urand(sPlayerbotAIConfig.restHubDwellMinSec,
                         sPlayerbotAIConfig.restHubDwellMaxSec) * IN_MILLISECONDS;

    if (d.functional)
    {
        switch (rest.subtype)
        {
            case RS_VENDOR:
                // Verbatim from the deleted REPAIR_SELL arm (NewRpgAction.cpp:646-647).
                botAI->DoSpecificAction("sell", Event("rpg action", "vendor"), true);
                botAI->DoSpecificAction("repair", Event(), true);
                break;
            case RS_PROFESSION_CRAFT:
                // USE_STANDING pose only; the craft anim is the held dwell pose (Task 7).
                break;
            case RS_DUMMY:
                if (Unit* dummy = ObjectAccessor::GetUnit(*bot, rest.target))
                    bot->Attack(dummy, true);
                break;
            case RS_DUEL:
            {
                // Verbatim from the deleted DUEL arm (NewRpgAction.cpp:615-617). Fire-and-forget:
                // cast 7266 then drop straight to Idle. R2 — return immediately, touch no `rest` after.
                WorldObject* partner = ObjectAccessor::GetWorldObject(*bot, rest.target);
                if (partner)
                    botAI->DoSpecificAction("cast custom spell",
                        Event("rpg action", chat->FormatWorldobject(partner) + " 7266"), true);
                botAI->rpgInfo.ChangeToIdle();
                return false;   // R2: caller returns true; do not touch `rest` after a ChangeTo*
            }
            case RS_FISH:
                // Fishing is a self-gating chain ("move near water"/"go fishing"/"use fishing bobber");
                // the hold tick drives it via TickFish(). The dwell is stamped above.
                break;
            case RS_TAVERN:
            case RS_FIELD_REST:
                // eat/drink + chair re-broadcast are handled by the seated HoldSeat pose (Task 7).
                break;
            default:
                break;
        }
    }
    return true;
}

// ───────────────────────────────────────────────────────────────────────────
// RS_FISH hold driver. Reuses the existing fishing chain: "move near water"
// (resolves the "fishing spot" WorldPosition + walks there), "go fishing"
// (auto-acquires pole 6256 + casts 7620), "use fishing bobber" (reels on a bite).
// Attempt in priority order each tick; each action self-gates (isUseful/isPossible),
// so exactly the right step runs. No raw pointers held across ticks; no ChangeTo*.
// ───────────────────────────────────────────────────────────────────────────
void NewRpgStatusUpdateAction::TickFish()
{
    // Reel first if a bobber is biting; else cast if not already fishing; else walk to water.
    if (botAI->DoSpecificAction("use fishing bobber", Event(), true))
        return;
    if (botAI->DoSpecificAction("go fishing", Event(), true))
        return;
    botAI->DoSpecificAction("move near water", Event(), true);
}

// Mirrored from FishingAction.cpp (file-static there). Values copied verbatim — keep in sync.
static constexpr float REST_FISH_MIN_DIST = 10.0f;   // MIN_DISTANCE_TO_WATER
static constexpr float REST_FISH_MAX_DIST = 20.0f;   // MAX_DISTANCE_TO_WATER
static constexpr float REST_FISH_SEARCH_INC = 2.5f;  // SEARCH_INCREMENT

// ───────────────────────────────────────────────────────────────────────────
// Per-subtype eligibility (skill/area gates). Verified helpers:
//   BotHasCraftingProfession(Player*)  — NewRpgBaseAction.cpp:1211 (file-static there;
//       re-declared file-static below since it isn't exported in a header).
//   AI_VALUE(bool,"can fish")          — CanFishValue, ValueContext.h:327.
//   BotInDuelAllowedArea(Player*)      — NewRpgBaseAction.cpp:1228 (file-static; re-declared).
// ───────────────────────────────────────────────────────────────────────────
static bool RestHubHasCraftingProfession(Player* bot)
{
    return bot->HasSkill(SKILL_BLACKSMITHING) ||
           bot->HasSkill(SKILL_TAILORING)     ||
           bot->HasSkill(SKILL_ENCHANTING)    ||
           bot->HasSkill(SKILL_ALCHEMY)       ||
           bot->HasSkill(SKILL_ENGINEERING)   ||
           bot->HasSkill(SKILL_LEATHERWORKING)||
           bot->HasSkill(SKILL_COOKING);
}

static bool RestHubInDuelAllowedArea(Player* bot)
{
    if (sPlayerbotAIConfig.IsInPvpProhibitedZone(bot->GetZoneId()))
        return false;
    AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(bot->GetAreaId());
    if (areaEntry && !(areaEntry->flags & AREA_FLAG_ALLOW_DUELS))
        return false;
    return true;
}

bool NewRpgStatusUpdateAction::IsSubtypeEligible(RestSubtype st) const
{
    switch (st)
    {
        case RS_PROFESSION_CRAFT: return RestHubHasCraftingProfession(bot);
        case RS_FISH:             return AI_VALUE(bool, "can fish");
        case RS_DUEL:             return RestHubInDuelAllowedArea(bot);
        default:                  return true;
    }
}

// Cheap presence probe for the anywhere (no-hub) subtypes. Eligibility already covers
// skill/area; this only checks whether a target is plausibly around right now.
bool NewRpgStatusUpdateAction::IsAnywhereTargetPresent(RestSubtype st) const
{
    switch (st)
    {
        case RS_DUEL:       return !RestHubSelectDuelPartner(botAI).IsEmpty();
        case RS_FISH:
        {
            // Real water gate (was unconditional `true`, falsely assuming "can fish" implies
            // water). FISH rests where the bot already is, so require fishable water within
            // fishingDistance of the current position; the hold's "move near water" does the
            // authoritative resolution. Runs once per rest-episode selection (not per tick) —
            // bounded cost, like the other anywhere probes (see :97-98).
            WorldPosition water =
                FindWaterRadial(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
                                bot->GetMap(), bot->GetPhaseMask(),
                                REST_FISH_MIN_DIST,
                                sPlayerbotAIConfig.fishingDistance + REST_FISH_MAX_DIST,
                                REST_FISH_SEARCH_INC, false);
            return water.IsValid();
        }
        case RS_FIELD_REST: return true;   // always possible in place
        default:            return true;
    }
}

// ───────────────────────────────────────────────────────────────────────────
// Weighted picker. Math kept pure (PickRestSubtypePure / RestSubtypeEffectiveSum)
// so it is unit-testable; the member only assembles the weight/avail arrays and
// draws the roll.
// ───────────────────────────────────────────────────────────────────────────
uint32 RestSubtypeEffectiveSum(const uint16 weight[RS_COUNT], const bool avail[RS_COUNT],
                               RestSubtype last)
{
    uint32 sum = 0;
    for (uint8 i = 0; i < RS_COUNT; ++i)
    {
        if (!avail[i])
            continue;
        uint32 w = weight[i];
        if ((RestSubtype)i == last)
            w /= 4;   // anti-repeat: quarter the previous episode's subtype
        sum += w;
    }
    return sum;
}

RestSubtype PickRestSubtypePure(const uint16 weight[RS_COUNT], const bool avail[RS_COUNT],
                                RestSubtype last, uint32 rngRoll)
{
    uint32 acc = 0;
    for (uint8 i = 0; i < RS_COUNT; ++i)
    {
        if (!avail[i])
            continue;
        uint32 w = weight[i];
        if ((RestSubtype)i == last)
            w /= 4;
        if (w == 0)
            continue;
        acc += w;
        if (rngRoll <= acc)
            return (RestSubtype)i;
    }
    return RS_FIELD_REST;   // nothing available, or roll fell through rounding
}

bool RestSubtypePickerEligible(RestSubtype st)
{
    switch (st)
    {
        case RS_TAVERN:
        case RS_FIELD_REST:
        case RS_FISH:
        case RS_STROLL:
        case RS_DUEL:
        case RS_SPECTATE:
        case RS_PROFESSION_CRAFT:
            return true;
        default:
            // VENDOR/BANK/AH/MAIL/CLASS_TRAINER/DUMMY (owned by UPKEEP), FLIGHT, QUEST_GIVER
            // (moved to UPKEEP), SOCIAL (retired).
            return false;
    }
}

RestSubtype NewRpgStatusUpdateAction::PickRestSubtype(bool hubReachable)
{
    uint16 w[RS_COUNT];
    bool   avail[RS_COUNT];
    for (uint8 i = 0; i < RS_COUNT; ++i)
    {
        RestSubtype st = (RestSubtype)i;
        if (!RestSubtypePickerEligible(st))
        {
            avail[i] = false;
            w[i] = 0;
            continue;
        }
        avail[i] = (kRestTable[st].needsHub ? hubReachable : IsAnywhereTargetPresent(st))
                   && IsSubtypeEligible(st);
        w[i] = avail[i] ? sPlayerbotAIConfig.restHubWeight[i] : 0;
    }
    RestSubtype last = (RestSubtype)botAI->rpgInfo.lastRestSubtype;
    uint32 sum = RestSubtypeEffectiveSum(w, avail, last);
    if (sum == 0)
        return RS_FIELD_REST;
    return PickRestSubtypePure(w, avail, last, urand(1, sum));
}
