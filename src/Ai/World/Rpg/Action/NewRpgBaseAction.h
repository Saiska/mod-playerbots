#ifndef _PLAYERBOT_NEWRPGBASEACTION_H
#define _PLAYERBOT_NEWRPGBASEACTION_H

#include "Duration.h"
#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
    uint32 mapId{0};
};

// Tri-state result of the witness-gated travel primitive DriveTravel().
//   EN_ROUTE  — bot is moving toward the target (MoveFarTo issued).
//   ARRIVED   — bot is within arrive radius OR teleported when unwitnessed.
//   GAVE_UP   — witnessed + beyond rpgTravelBudget (caller should pick a nearer goal).
enum class TravelResult : uint8 { EN_ROUTE, ARRIVED, GAVE_UP };

// Reason why a bot's open-world RPG idling is blocked, or IB_NONE when free.
// Shared by IsFreeToIdle() (engine gate) and the census (label).
enum RpgIdleBlock : uint8
{
    IB_NONE = 0,    // free to idle (IsFreeToIdle == true)
    IB_DEAD_OR_TP,  // dead or being teleported
    IB_RAIDSIM,     // in a RaidSim run (checked before INSTANCE for clearer attribution)
    IB_COMBAT,      // in combat
    IB_INSTANCE,    // dungeon/raid/bg/arena (non-RaidSim)
    IB_VEHICLE,     // transport/vehicle
    IB_GROUPED      // grouped with a human
};

// First reason this bot's open-world RPG idling is blocked, or IB_NONE. Shared by the engine
// (IsFreeToIdle) and the census. Player* + config + sRaidSimulationMgr only.
RpgIdleBlock GetRpgIdleBlock(Player* bot);

// dalaran-quarter-npc-exclusion: Dalaran's two faction embassies are first-class AreaTable
// sub-areas of zone 4395. Their resident NPCs are NEUTRAL (not hostile) to the opposing
// faction, so the ordinary "!IsHostileTo ⇒ valid target" tests wrongly accept them — the bot
// then walks/teleports into the wrong quarter and trips the trespasser eviction (stun + forced
// teleport out), wrecking whatever NewRpg episode it was running.
constexpr uint32 AREA_SUNREAVERS_SANCTUARY = 4616;  // Horde quarter — forbidden to Alliance bots
constexpr uint32 AREA_SILVER_ENCLAVE       = 4740;  // Alliance quarter — forbidden to Horde bots

// True if `obj` sits in the Dalaran quarter opposite `bot`'s faction. Cheap zone gate first, so
// it is a no-op everywhere but Dalaran. A bot standing in its OWN quarter, or at a neutral
// Dalaran NPC (Eventide, Magus Commerce Exchange, ...), is unaffected — only the OPPOSITE
// quarter is excluded. Free function (not a NewRpgBaseAction member) so non-member callers
// (PossibleRpgTargetsValue.cpp) can use it too.
bool IsInForbiddenFactionQuarter(Player* bot, WorldObject const* obj);

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    bool MoveFarTo(WorldPosition dest);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    // Witness-gated travel primitive.  Teleports when unwitnessed; walks/mounts
    // via MoveFarTo when witnessed + within budget; gives up if too far.
    // Ground-z safety of the target is the CALLER's responsibility.
    TravelResult DriveTravel(WorldPosition const& target);

    // True if any non-bot player is within `range` yards of `pos` on the bot's map.
    bool IsRealPlayerNear(WorldPosition const& pos, float range) const;

    /* EMOTE CADENCE (occupation-emote-palettes) */
    // Re-assert the behavior's sustained pose + fire a timed, jittered, non-repeating
    // one-shot from the (beh,variant) EmotePalette. Call from a stationary dwell point.
    // skipSustainedPose=true does the one-shots ONLY (the caller already holds a pose,
    // e.g. RPG_REST seated on a real chair, so re-asserting EMOTE_STATE_SIT would fight
    // the chair's SIT_*_CHAIR stand-state).
    void TickEmoteCadence(BotBehaviorId beh, uint8 variant, bool skipSustainedPose = false,
                          uint32 overridePose = 0xFFFFFFFF);
    void FireOneShotEmote(BotBehaviorId beh, uint8 variant);   // immediate, non-repeating one-shot pick
    // upkeep-sociability: nearest co-located idle/dwelling friendly bot within `radius` yards, by GUID.
    // Crash rule: returns ObjectGuid; caller re-resolves at use, never stores a Player*.
    ObjectGuid FindDwellPeer(float radius);

    /* QUEST RELATED CHECK */
    ObjectGuid ChooseNpcOrGameObjectToInteract(bool questgiverOnly = false, float distanceLimit = 0.0f);
    ObjectGuid SelectLoiterPoi(uint8& outPoiType);
    ObjectGuid SelectVendorNpc();
    ObjectGuid SelectTrainingDummy();
    bool       SelectFarTaxiDest(WorldPosition& out);
    // Town-sized grid-scan prop resolvers (promoted from NewRpgStatusUpdateAction so both the rest
    // engine AND the upkeep capital poses (PoseAtProp) resolve props identically — no drift).
    // SelectNearestNpcWithFlag: nearest friendly creature carrying `npcFlag` (banker/auctioneer/
    // trainer/...). SelectNearestGoOfType: nearest spawned GameObject of `goType` (mailbox).
    // Both scan at restHubPoiRadius; empty ObjectGuid if none in range.
    ObjectGuid SelectNearestNpcWithFlag(uint32 npcFlag) const;
    ObjectGuid SelectNearestGoOfType(uint32 goType) const;
    // occupation-upkeep-two-tier Task 4 — cosmetic city pose at a capital prop (banker/auctioneer/
    // mailbox/trainer). Pure RP: NO transaction. Reuses the rest engine's per-subtype resolver
    // mapping (RS_BANK/RS_AUCTION_HOUSE/RS_CLASS_TRAINER via NPC flag, RS_MAILBOX via GO type),
    // DriveTravel (witness-gated short hop), SetFacingToObject, and TickEmoteCadence pose dwell.
    // Returns true while still posing (caller returns this tick); false when done OR the prop is
    // unresolvable (caller advances up.step and clears up.target). `up` is a live reference into the
    // variant — caller already did the get_if; never re-enters throwing variant access.
    bool PoseAtProp(uint8 restSubtype, uint32 dwellMs, NewRpgInfo::Upkeep& up);
    // Nearest GAMEOBJECT_TYPE_CHAIR within `radius` (nearest-spawned-GO grid scan).
    // Empty if none — RPG_REST then floor-sits in place.
    ObjectGuid SelectInnChair(float radius);
    bool HasQuestToAcceptOrReward(WorldObject* object);
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool CanInteractWithQuestGiver(Object* questGiver);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);
    bool IsQuestWorthDoing(Quest const* quest);
    bool IsQuestCapableDoing(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false, bool requireInZone = true);
    // doquest-zone-travel: same-map planar distance to a POI, or a large constant for cross-map POIs.
    float DistToPoi(POIInfo const& poi);
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    // occupation-upkeep-two-tier: map-wide faction-appropriate capital hub for the CAPITAL tier
    // and the LOCAL->CAPITAL fallthrough. Non-empty for a free bot of any level (Task 6 caller).
    static WorldPosition SelectCapitalHub(Player* bot);
    // upkeep-capital-pose-prop-resolve: same weighted pick as SelectCapitalHub but also fills
    // outZone with the chosen capital's zoneId so hubPos and capitalZone agree (single roll).
    // Falls back to SelectCapitalHub (outZone stays 0) when GetCityLocationAndZone returns empty.
    static WorldPosition SelectCapitalHubAndZone(Player* bot, uint32& outZone);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    // ── occupation-state-machine Task 4: NEEDS→DECIDE resolver ───────────────
    // Run only at occupation boundaries (from the IDLE case / occupation exits), never every tick.
    // LAYER 1 NEEDS (strict priority RECOVER>UPKEEP), then LAYER 2 weighted-random over the
    // FEASIBLE productive set. Replaces the deleted satiation-roulette RandomChangeStatus.
    void Decide();
    // Layer-0 stranded guard: relocate a bot displaced off its content band. Returns true = handled.
    bool TryRelocateStranded();
    // Maps a candidate status to its Task-2 precondition (precondition ONLY — weights/cooldowns
    // are applied by Decide()). Deliberately changes behaviour vs the old CheckRpgStatusAvailable
    // (e.g. RPG_REST is now hub-gated, so a far bot never strands in field-rest).
    bool OccupationFeasible(NewRpgStatus status);
    // Wraps the per-status ChangeTo* + ACQUIRE seed. CRASH RULE: every ChangeTo* is followed by an
    // immediate return; acquire-fail paths route to FallToFarmOrRest(), never a silent ChangeToIdle.
    void EnterOccupation(NewRpgStatus status);
    // LAYER 1 NEEDS predicates. Bodies land in Task 5; for now they are inert (return false) so
    // NEEDS never fire and the build stays green.
    bool RecoverNeeded();
    bool UpkeepNeeded();
    // bot-rpg-bleed-suppression: allowlist guard — a bot may run autonomous NewRpg ONLY when free.
    bool IsFreeToIdle();
    bool ShouldSuppressRpg();
    // --- occupation-rebalance: context-aware fallback (Task 2) ---
    // Returns true if the bot is within `radius` yards of any travel hub on its current map.
    bool IsNearRestHub(float radius);
    // Last-resort fallback: near a hub -> rest in place; in the wild -> farm in place (GoGrind
    // anchored to current pos, which is always non-empty so the commit cannot fail). Never Idle.
    bool FallToFarmOrRest();

    // ── occupation-machine Task 2: predicate vocabulary ──────────────────────
    // All predicates are O(1)/cheap-proximity; read-only; valid for the current tick only.
    // Callers: Task 4 Decide()/OccupationFeasible()/RecoverNeeded()/UpkeepNeeded(), Task 5 NEEDS.
    bool InOpenWorld();                 // not in dungeon/raid/battleground/arena
    bool NearHub(float r);              // within r yards of any travel hub (wraps IsNearRestHub)
    bool InCityHub();                   // current zone has AREA_FLAG_CAPITAL
    bool HealthLow();                   // hp% < needHealthLowPct, non-combat
    bool ManaLow();                     // mana% < needManaLowPct (power-using classes only)
    bool DurabilityLow();               // any equipped item below needDurabilityLowPct%
    bool BagsFull();                    // free inventory slots <= needBagsFullSlots
    bool MissingToolsOrReagents();      // mining-pick absent (miner) or fishing-pole absent (fisher)
    bool MaintenanceOverdue();          // time since lastUpkeepMs > maintenanceOverdueMs
    bool HasActionableQuest();          // held quest with resolvable POI, or complete-unturned
    bool HasGatherProfAndTool();        // gathering profession (mining/herb) + required tool present
    bool VendorInRange();               // vendor/repair NPC within pastimeRepairSellRadius
    bool EnemyNearForPvp();             // open-world PvP zone AND nearest hostile player present
    // farm-lean-by-class: soft per-bot weight tilt of the gather-vs-grind occupation choice
    // by class + active talent spec. Returns (gatherMult, grindMult). Never a hard 0;
    // caller floors with max(1u, ...) before passing to the weighted draw.
    std::pair<float, float> FarmLean() const;

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * 1000;
    const uint32 nopathTeleportAttempts = 3;   // teleport after N consecutive NOPATH dest pathfinds
};

// rest-hub-unification: (behaviorId, variant) -> curated EmotePalette row. Defined in
// NewRpgBaseAction.cpp over the file-static kPalette/kLoiterByPoi tables (single source of
// truth). Exposed so the RPG_REST machine (NewRpgAction.cpp) can read a row's sustained pose
// without re-declaring those tables. variant is a BotCityPoi for BEH_LOITER (1..6), else 0.
const EmotePalette& LookupPalette(BotBehaviorId beh, uint8 variant);
// upkeep-sociability: returns kLoiterPoseSet[variant-1][rollIdx%count] for BEH_LOITER; else sustainedPose.
uint32 ResolveHeldPose(BotBehaviorId beh, uint8 variant, uint8 rollIdx);
// upkeep-sociability: accessor over the file-static kOneShots_LoiterInteractive pool.
// Fills `count` and returns a pointer to the array; both are valid for the lifetime of the TU.
uint32 const* GetLoiterInteractivePool(uint8& count);

#endif
