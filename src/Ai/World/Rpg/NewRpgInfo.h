#ifndef _PLAYERBOT_NEWRPGINFO_H
#define _PLAYERBOT_NEWRPGINFO_H

#include "Action/NewRpgRestHub.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelMgr.h"
#include <vector>

using NewRpgStatusTransitionProb = std::vector<std::vector<int>>;

struct NewRpgInfo
{
    NewRpgInfo() : data(Idle{}) {}
    ~NewRpgInfo() = default;

    // RPG_GO_GRIND
    struct GoGrind
    {
        WorldPosition pos{};
    };
    // RPG_DO_QUEST
    struct DoQuest
    {
        const Quest* quest{nullptr};
        uint32 questId{0};
        int32 objectiveIdx{0};
        WorldPosition pos{};
        uint32 lastReachPOI{0};
        WorldPosition targetPos{};   // doquest-zone-travel: cross-zone/map travel target (empty = in-zone, no travel)
    };
    // RPG_TRAVEL_FLIGHT
    struct TravelFlight
    {
        uint32 flightMasterEntry{0};
        WorldPosition flightMasterPos{};
        std::vector<uint32> path;
        bool inFlight{false};
    };
    // RPG_REST
    struct Rest
    {
        Rest() = default;
        RestSubtype   subtype{RS_NONE};   // resolved on arrival (or pre-resolved for anywhere subtypes)
        WorldPosition hubPos{};           // chosen curated hub destination (empty = field-rest)
        uint32        hubArriveT{0};       // ms ts the bot reached the hub (0=en route); gates a settle before P2 acquire
        ObjectGuid    target{};           // resolved subtype target (NPC/GO); empty for in-place/social-cluster
        WorldPosition targetPos{};        // for social cluster / stroll waypoints
        ObjectGuid    chair{};            // TAVERN/FIELD_REST chair GO (empty = floor-sit)
        bool          onChair{false};
        uint8         seatState{0};       // captured SIT_*_CHAIR, re-asserted each tick
        uint32        sustainedPose{0};   // EMOTE_STATE_* held during dwell (from the table row)
        uint32        chosenDwellPose{0xFFFFFFFF};   // upkeep-sociability: per-bot held pose, rolled once on dwell entry (0xFFFFFFFF = unrolled)
        uint32        lastReach{0};       // 0 = en route; set once on arrival
        uint32        dwellMs{0};
        uint8         strollIdx{0};       // STROLL: current waypoint index
        uint32        strollPauseUntil{0};// STROLL: ms timestamp the current pause ends
        std::vector<WorldPosition> strollPts; // STROLL: per-episode waypoint route
    };
    // RPG_OUTDOOR_PVP
    struct OutdoorPvP
    {
        ObjectGuid::LowType capturePointSpawnId{0};
    };
    // RPG_TRAVEL_MOUNT
    struct TravelMount
    {
        WorldPosition pos{};
    };
    // RPG_GATHERING_CIRCUIT
    struct GatheringCircuit
    {
        ObjectGuid node{};
        WorldPosition nodePos{};    // world position of the current target node (from the boot index);
                                    // drives the far-travel leg before the node's grid/live object loads
        uint32 visited{0};
        uint32 maxNodes{0};
        bool   harvesting{false};   // true while holding at a node for the gather cast to finish
        uint32 harvestStartMs{0};   // GetMSTime() when the gather cast was (re)issued; wrap-safe elapsed
        // Bounded recent-visited ring (oldest dropped when full): guids of nodes just
        // harvested/abandoned, excluded from the next seed so the circuit doesn't ping-pong
        // between the two closest nodes. Capped in the action (GATHER_RECENT_VISITED_CAP).
        std::vector<ObjectGuid> recentVisited{};
    };
    struct Idle
    {
        uint32 dwellMs{0};
    };
    // RPG_RECOVER — bot heals/regens before picking the next occupation
    struct Recover
    {
        uint32 dwellMs{0};
    };
    // RPG_UPKEEP — bot performs maintenance + (capital tier) cosmetic city poses
    enum { UPKEEP_TIER_LOCAL = 0, UPKEEP_TIER_CAPITAL = 1 };
    struct Upkeep
    {
        uint8         tier{UPKEEP_TIER_LOCAL};  // LOCAL or CAPITAL (rolled once on entry)
        uint8         step{0};                  // index into the tier's pipeline
        WorldPosition hubPos{};                 // chosen hub / capital anchor position
        WorldPosition posePos{};                // current prop NPC position (capital poses)
        ObjectGuid    target{};                 // resolved vendor / prop NPC guid
        uint32        dwellMs{0};               // current step's randomized dwell
        uint32        stepStartMs{0};           // getMSTime() the current step began
        bool          learnedNew{false};        // maintenance granted a new spell/rank (gates DUMMY)
        uint32        capitalZone{0};            // chosen capital zoneId (CAPITAL tier; resolved once at acquire)
        uint32        poseArriveT{0};            // ms ts the bot reached the current prop (0=en route); gates the settle
        uint32        chosenDwellPose{0xFFFFFFFF};   // upkeep-sociability: per-bot held pose, rolled once on dwell entry (0xFFFFFFFF = unrolled)
    };

    uint32 startT{0};  // start timestamp of the current status
    uint8  lastRestSubtype{RS_NONE};  // cross-episode: subtype of the previous Rest episode (survives variant reset)
    uint32 lastZoneId{0};            // zone at last Execute tick; clears lowPriorityQuest on zone change

    BotBehaviorId lastEmittedBehaviorId{BEH_NONE};  // last behaviorId we emitted a lifecycle event for (central emitter)

    // --- emote cadence state (occupation-emote-palettes) — restarts each status change ---
    uint32 lastEmoteMs{0};
    uint32 nextEmoteGapMs{0};
    uint8  lastEmoteIdx{0xFF};
    uint32 heldRestPose{0};      // social-dance-resthub-revive: the UNIT_NPC_EMOTESTATE the rest engine is
                                 // holding for a posed subtype (0=none). On top-level NewRpgInfo (not the Rest
                                 // variant) so it survives ChangeToIdle's reset → the Execute-head sweep can
                                 // clear a stuck pose after an external yank (bleed-suppression).
    uint32 sitDiagLastMs{0};     // rest-sit-render-diagnostic (TEMP): ms ts of this bot's last [SitDiag] line

    // --- occupation-machine tracking ---
    uint32 lastFinished[RPG_STATUS_END] = {0};  // getMSTime() when each occupation last ended; 0=never
    uint32 lastUpkeepMs{0};                     // getMSTime() of the last completed UPKEEP episode; 0=never

    // MOVE_FAR
    float nearestMoveFarDis{FLT_MAX};
    uint32 stuckTs{0};
    uint32 stuckAttempts{0};
    WorldPosition moveFarPos;
    // END MOVE_FAR

    using RpgData = std::variant<Idle, GoGrind, DoQuest, Rest, TravelFlight, OutdoorPvP, TravelMount, GatheringCircuit, Recover, Upkeep>;
    RpgData data;

    NewRpgStatus GetStatus();
    bool HasStatusPersisted(uint32 maxDuration) { return GetMSTimeDiffToNow(startT) > maxDuration; }
    void ChangeToGoGrind(WorldPosition pos);
    void ChangeToDoQuest(uint32 questId, const Quest* quest, WorldPosition targetPos = WorldPosition());
    void ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path);
    void ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId = 0);
    void ChangeToTravelMount(WorldPosition pos);
    void ChangeToGatheringCircuit(uint32 maxNodes);
    void ChangeToRest();
    void ChangeToRecover();
    void ChangeToUpkeep();
    void ChangeToIdle();
    bool CanChangeTo(NewRpgStatus status);
    void Reset();
    void SetMoveFarTo(WorldPosition pos);
    std::string ToString();
};

struct NewRpgStatistic
{
    uint32 questAccepted{0};
    uint32 questCompleted{0};
    uint32 questAbandoned{0};
    uint32 questRewarded{0};
    uint32 questDropped{0};
    NewRpgStatistic operator+(const NewRpgStatistic& other) const
    {
        NewRpgStatistic result;
        result.questAccepted = this->questAccepted + other.questAccepted;
        result.questCompleted = this->questCompleted + other.questCompleted;
        result.questAbandoned = this->questAbandoned + other.questAbandoned;
        result.questRewarded = this->questRewarded + other.questRewarded;
        result.questDropped = this->questDropped + other.questDropped;
        return result;
    }
    NewRpgStatistic& operator+=(const NewRpgStatistic& other)
    {
        this->questAccepted += other.questAccepted;
        this->questCompleted += other.questCompleted;
        this->questAbandoned += other.questAbandoned;
        this->questRewarded += other.questRewarded;
        this->questDropped += other.questDropped;
        return *this;
    }
};

#endif
