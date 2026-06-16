#ifndef _PLAYERBOT_NEWRPGINFO_H
#define _PLAYERBOT_NEWRPGINFO_H

#include "Define.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "QuestDef.h"
#include "Strategy.h"
#include "Timer.h"
#include "TravelMgr.h"

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
    // RPG_WANDER_NPC
    struct WanderNpc
    {
        ObjectGuid npcOrGo{};
        uint32 lastReach{0};
    };
    // RPG_PASTIME
    struct Pastime
    {
        uint8  activityType{0};   // BotActivity
        ObjectGuid target{};      // companion / node / partner (guid-target activities)
        WorldPosition targetPos{};// position-target activities (e.g. fishing spot); target union
        uint32 lastReach{0};      // arrival timestamp (0 = en route)
        uint32 lastEmote{0};      // last social-emote timestamp
        uint32 dwellMs{0};        // dwell duration in ms (set on arrival; elapsed measured vs lastReach)
        uint8  poiType{POI_NONE}; // BotCityPoi — set by SelectLoiterPoi; used by themed-scene enactment
    };
    // RPG_WANDER_RANDOM
    struct WanderRandom
    {
        WanderRandom() = default;
        uint32 nextHaltMs{0};   // micro-halt schedule: 0 = seed on first tick; else next halt-eligible time
    };
    // RPG_DO_QUEST
    struct DoQuest
    {
        const Quest* quest{nullptr};
        uint32 questId{0};
        int32 objectiveIdx{0};
        WorldPosition pos{};
        uint32 lastReachPOI{0};
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
        WorldPosition pos{};    // innkeeper-hub destination (InnPull); empty = rest in place
        ObjectGuid chair{};     // seated chair GO (empty = floor-sit / not yet resolved)
        bool onChair{false};    // true once chair->Use() succeeded: gates against re-Use/re-teleport
        uint8 seatState{0};     // captured UNIT_STAND_STATE_SIT_*_CHAIR value, re-asserted each tick to hold the pose
        uint32 lastReach{0};    // 0 = en route / not yet resolved; set once on ARRIVAL at the rest pos
        uint32 dwellMs{0};      // jittered dwell duration in ms (set on arrival; elapsed measured vs lastReach)
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
        uint32 visited{0};
        uint32 maxNodes{0};
    };
    struct Idle
    {
    };

    uint32 startT{0};  // start timestamp of the current status

    BotBehaviorId lastEmittedBehaviorId{BEH_NONE};  // last behaviorId we emitted a lifecycle event for (central emitter)

    // --- emote cadence state (occupation-emote-palettes) — restarts each status change ---
    uint32 lastEmoteMs{0};
    uint32 nextEmoteGapMs{0};
    uint8  lastEmoteIdx{0xFF};
    uint32 heldSocialEmote{0};   // comedy-hold-dance: social pastime held EMOTE_STATE_* (0=none). Lives on
                                 // NewRpgInfo (not the Social variant) so it survives ChangeToIdle's variant
                                 // reset and the Execute-head sweep can clear it after an external yank.

    // --- occupation satiation (pipe 1) — in-memory only, no DB ---
    float  satiation[CAT_COUNT] = {0.0f};  // [0,1] per BotActivityCategory
    uint32 lastSatiationUpdateMs{0};       // last meter-integration tick (0 = uninitialised)

    // MOVE_FAR
    float nearestMoveFarDis{FLT_MAX};
    uint32 stuckTs{0};
    uint32 stuckAttempts{0};
    WorldPosition moveFarPos;
    // END MOVE_FAR

    using RpgData = std::variant<
        Idle,
        GoGrind,
        WanderNpc,
        Pastime,
        WanderRandom,
        DoQuest,
        Rest,
        TravelFlight,
        OutdoorPvP,
        TravelMount,
        GatheringCircuit
    >;
    RpgData data;

    NewRpgStatus GetStatus();
    bool HasStatusPersisted(uint32 maxDuration) { return GetMSTimeDiffToNow(startT) > maxDuration; }
    void ChangeToGoGrind(WorldPosition pos);
    void ChangeToWanderNpc();
    void ChangeToPastime(uint8 activityType, ObjectGuid target, WorldPosition targetPos = {}, uint8 poiType = POI_NONE);
    void ChangeToWanderRandom();
    void ChangeToDoQuest(uint32 questId, const Quest* quest);
    void ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path);
    void ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId = 0);
    void ChangeToTravelMount(WorldPosition pos);
    void ChangeToGatheringCircuit(uint32 maxNodes);
    void ChangeToRest();
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
