#include "NewRpgInfo.h"

#include <algorithm>
#include <cmath>

#include "Random.h"
#include "Timer.h"

void NewRpgInfo::ChangeToGoGrind(WorldPosition pos)
{
    startT = getMSTime();
    data = GoGrind{pos};
}

void NewRpgInfo::ChangeToDoQuest(uint32 questId, const Quest* quest, WorldPosition targetPos)
{
    startT = getMSTime();
    DoQuest do_quest;
    do_quest.questId = questId;
    do_quest.quest = quest;
    do_quest.targetPos = targetPos;
    data = do_quest;
}

void NewRpgInfo::ChangeToTravelFlight(uint32 flightMasterEntry, WorldPosition flightMasterPos, std::vector<uint32> path)
{
    startT = getMSTime();
    TravelFlight flight;
    flight.flightMasterEntry = flightMasterEntry;
    flight.flightMasterPos = flightMasterPos;
    flight.path = std::move(path);
    flight.inFlight = false;
    data = flight;
}

void NewRpgInfo::ChangeToOutdoorPvp(ObjectGuid::LowType capturePointSpawnId)
{
    startT = getMSTime();
    OutdoorPvP pvp;
    pvp.capturePointSpawnId = capturePointSpawnId;
    data = pvp;
}

void NewRpgInfo::ChangeToTravelMount(WorldPosition pos)
{
    startT = getMSTime();
    data = TravelMount{pos};
}

void NewRpgInfo::ChangeToGatheringCircuit(uint32 maxNodes)
{
    startT = getMSTime();
    GatheringCircuit g;
    g.maxNodes = maxNodes;
    data = g;
}

void NewRpgInfo::ChangeToRest()
{
    startT = getMSTime();
    data = Rest{};
}

void NewRpgInfo::ChangeToRecover()
{
    startT = getMSTime();
    data = Recover{};
    lastEmoteMs = 0;
    nextEmoteGapMs = 0;
    lastEmoteIdx = 0xFF;
}

void NewRpgInfo::ChangeToUpkeep()
{
    // occupation-upkeep-two-tier: roll the tier ONCE on entry (no bot* here — only the config is
    // needed). hubPos stays empty; the hub is resolved on the first ACQUIRE tick (Step 2) where
    // SelectRandomCampPos(bot)/SelectCapitalHub(bot) can run. Mirrors how ChangeToRest defers the
    // arrival-time acquire to the tick path.
    Upkeep up{};
    bool capital = roll_chance_f(sPlayerbotAIConfig.upkeepCapitalChance * 100.0f);
    up.tier = capital ? UPKEEP_TIER_CAPITAL : UPKEEP_TIER_LOCAL;
    startT = getMSTime();
    data = up;
    lastEmoteMs = 0;
    nextEmoteGapMs = 0;
    lastEmoteIdx = 0xFF;
}

void NewRpgInfo::ChangeToIdle()
{
    startT = getMSTime();
    data = Idle{ sPlayerbotAIConfig.rpgIdleDwellMs };
}

bool NewRpgInfo::CanChangeTo(NewRpgStatus)
{
    return true;
}

void NewRpgInfo::Reset()
{
    data = Idle{};
    startT = getMSTime();
    std::fill(std::begin(lastFinished), std::end(lastFinished), 0u);
    lastUpkeepMs = 0;
    lastEmoteMs = 0;
    nextEmoteGapMs = 0;
    lastEmoteIdx = 0xFF;
    lastEmittedBehaviorId = BEH_NONE;
}

void NewRpgInfo::SetMoveFarTo(WorldPosition pos)
{
    nearestMoveFarDis = FLT_MAX;
    stuckTs = 0;
    stuckAttempts = 0;
    moveFarPos = pos;
}

NewRpgStatus NewRpgInfo::GetStatus()
{
    return std::visit([](auto&& arg) -> NewRpgStatus {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, Idle>) return RPG_IDLE;
        if constexpr (std::is_same_v<T, GoGrind>) return RPG_GO_GRIND;
        if constexpr (std::is_same_v<T, Rest>) return RPG_REST;
        if constexpr (std::is_same_v<T, DoQuest>) return RPG_DO_QUEST;
        if constexpr (std::is_same_v<T, TravelFlight>) return RPG_TRAVEL_FLIGHT;
        if constexpr (std::is_same_v<T, OutdoorPvP>) return RPG_OUTDOOR_PVP;
        if constexpr (std::is_same_v<T, TravelMount>) return RPG_TRAVEL_MOUNT;
        if constexpr (std::is_same_v<T, GatheringCircuit>) return RPG_GATHERING_CIRCUIT;
        if constexpr (std::is_same_v<T, Recover>) return RPG_RECOVER;
        if constexpr (std::is_same_v<T, Upkeep>) return RPG_UPKEEP;
        return RPG_IDLE;
    }, data);
}

std::string NewRpgInfo::ToString()
{
    std::stringstream out;
    out << "Status: ";
    std::visit([&out, this](auto&& arg)
    {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, GoGrind>)
        {
            out << "GO_GRIND";
            out << "\nGrindPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastGoGrind: " << startT;
        }
        else if constexpr (std::is_same_v<T, Idle>)
        {
            out << "IDLE";
        }
        else if constexpr (std::is_same_v<T, Rest>)
        {
            out << "REST";
            out << "\nlastRest: " << startT;
        }
        else if constexpr (std::is_same_v<T, DoQuest>)
        {
            out << "DO_QUEST";
            out << "\nquestId: " << arg.questId;
            out << "\nobjectiveIdx: " << arg.objectiveIdx;
            out << "\npoiPos: " << arg.pos.GetMapId() << " " << arg.pos.GetPositionX() << " "
                << arg.pos.GetPositionY() << " " << arg.pos.GetPositionZ();
            out << "\nlastReachPOI: " << (arg.lastReachPOI ? GetMSTimeDiffToNow(arg.lastReachPOI) : 0);
        }
        else if constexpr (std::is_same_v<T, TravelFlight>)
        {
            out << "TRAVEL_FLIGHT";
            out << "\nflightMasterEntry: " << arg.flightMasterEntry;
            out << "\nfromNode: " << arg.path[0];
            out << "\ntoNode: " << arg.path[arg.path.size() - 1];
            out << "\ninFlight: " << arg.inFlight;
        }
        else if constexpr (std::is_same_v<T, OutdoorPvP>)
        {
            out << "OUTDOOR_PVP";
            if (!arg.capturePointSpawnId)
                out << "\nNo capture point assigned.";
            else
                out << "\ncapturePointSpawnId: " << arg.capturePointSpawnId;
        }
        else if constexpr (std::is_same_v<T, TravelMount>)
        {
            out << "TRAVEL_MOUNT";
        }
        else if constexpr (std::is_same_v<T, GatheringCircuit>)
        {
            out << "GATHERING_CIRCUIT";
            out << "\nvisited: " << arg.visited << "/" << arg.maxNodes;
        }
        else if constexpr (std::is_same_v<T, Recover>)
        {
            out << "RECOVER";
            out << "\ndwellMs: " << arg.dwellMs;
        }
        else if constexpr (std::is_same_v<T, Upkeep>)
        {
            out << "UPKEEP";
            out << "\ntier: " << static_cast<uint32>(arg.tier) << " step: " << static_cast<uint32>(arg.step)
                << " dwellMs: " << arg.dwellMs << " learnedNew: " << arg.learnedNew;
        }
        else
            out << "UNKNOWN";
    }, data);
    return out.str();
}
