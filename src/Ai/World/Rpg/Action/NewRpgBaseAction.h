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

    /* EMOTE CADENCE (occupation-emote-palettes) */
    // Re-assert the behavior's sustained pose + fire a timed, jittered, non-repeating
    // one-shot from the (beh,variant) EmotePalette. Call from a stationary dwell point.
    // skipSustainedPose=true does the one-shots ONLY (the caller already holds a pose,
    // e.g. RPG_REST seated on a real chair, so re-asserting EMOTE_STATE_SIT would fight
    // the chair's SIT_*_CHAIR stand-state).
    void TickEmoteCadence(BotBehaviorId beh, uint8 variant, bool skipSustainedPose = false);
    void FireOneShotEmote(BotBehaviorId beh, uint8 variant);   // immediate, non-repeating one-shot pick

    /* QUEST RELATED CHECK */
    ObjectGuid ChooseNpcOrGameObjectToInteract(bool questgiverOnly = false, float distanceLimit = 0.0f);
    ObjectGuid SelectLoiterPoi(uint8& outPoiType);
    ObjectGuid SelectVendorNpc();
    ObjectGuid SelectTrainingDummy();
    bool       SelectFarTaxiDest(WorldPosition& out);
    ObjectGuid SelectGatherNode();   // promoted from file-static
    // Nearest GAMEOBJECT_TYPE_CHAIR within `radius` (mirrors SelectGatherNode's scan).
    // Empty if none — RPG_REST then floor-sits in place.
    ObjectGuid SelectInnChair(float radius);
    // Nearest random bot within Pastime.Social.Radius that is idle-ish OR already socializing
    // (or a player if Pastime.Social.IncludePlayers). Empty if none.
    ObjectGuid SelectSocialPartner();
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
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    bool CheckRpgStatusAvailable(NewRpgStatus status);
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
    bool NodeInRange(float r);          // a gather node within r yards (uses SelectGatherNode cap)
    bool VendorInRange();               // vendor/repair NPC within pastimeRepairSellRadius
    bool EnemyNearForPvp();             // open-world PvP zone AND nearest hostile player present

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
};

// rest-hub-unification: (behaviorId, variant) -> curated EmotePalette row. Defined in
// NewRpgBaseAction.cpp over the file-static kPalette/kLoiterByPoi tables (single source of
// truth). Exposed so the RPG_REST machine (NewRpgAction.cpp) can read a row's sustained pose
// without re-declaring those tables. variant is a BotCityPoi for BEH_LOITER (1..6), else 0.
const EmotePalette& LookupPalette(BotBehaviorId beh, uint8 variant);

#endif
