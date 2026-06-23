#ifndef _PLAYERBOT_NEWRPGACTION_H
#define _PLAYERBOT_NEWRPGACTION_H

#include "Duration.h"
#include "MovementActions.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

class TellRpgStatusAction : public Action
{
public:
    TellRpgStatusAction(PlayerbotAI* botAI) : Action(botAI, "rpg status") {}

    bool Execute(Event event) override;
};

class StartRpgDoQuestAction : public Action
{
public:
    StartRpgDoQuestAction(PlayerbotAI* botAI) : Action(botAI, "start rpg do quest") {}

    bool Execute(Event event) override;
};

class NewRpgStatusUpdateAction : public NewRpgBaseAction
{
public:
    NewRpgStatusUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg status update")
    {
        // int statusCount = RPG_STATUS_END - 1;

        // transitionMat.resize(statusCount, std::vector<int>(statusCount, 0));

        // transitionMat[RPG_IDLE][RPG_GO_GRIND] = 20;
        // transitionMat[RPG_IDLE][RPG_WANDER_NPC] = 30;
        // transitionMat[RPG_IDLE][RPG_DO_QUEST] = 35;
    }
    bool Execute(Event event) override;

protected:
    // ── RestHub helpers (rest-hub-unification) ──────────────────────────────
    // Witness-gated hub travel (R1: distance-driven, never treats a
    // MoveFarTo/MoveWorldObjectTo return as "arrived"; R2: target-acquire/engage take the
    // Rest substruct via std::get_if + null-guard and never read it after a ChangeTo*).
    // IsRealPlayerNear is inherited from NewRpgBaseAction.
    HubTravel   TravelToHubOrTeleport(WorldPosition const& hub);
    bool        AcquireSubtypeTarget(RestSubtype st);
    bool        EngageAndHold();
    RestSubtype PickRestSubtype(bool hubReachable);
    bool        IsSubtypeEligible(RestSubtype st) const;
    bool        IsAnywhereTargetPresent(RestSubtype st) const;

    // Fresh target selectors (mirror SelectVendorNpc's "nearest npcs"/"nearest game objects" idiom).
    ObjectGuid  SelectNearestNpcWithFlag(uint32 npcFlag) const;
    ObjectGuid  SelectNearestGoOfType(uint32 goType) const;
    ObjectGuid  SelectForgeOrProfTrainer() const;
    ObjectGuid  SelectSpectateTarget() const;

    // STROLL route — implemented in Task 8 (stub returns false for now so the link resolves).
    bool        BuildStrollRoute();

    // ── RPG_REST machine (Task 7) ───────────────────────────────────────────
    void HoldSeat(NewRpgInfo::Rest& rest);    // chair/floor seat re-broadcast (extracted)
    void TickStroll(NewRpgInfo::Rest& rest);  // STROLL walk loop — Task 8 fills; no-op stub for now
    EmotePalette PaletteOf(BotBehaviorId beh, BotCityPoi poi) const;  // (beh,poi) -> palette row

    // static NewRpgStatusTransitionProb transitionMat;
    const int32 statusWanderNpcDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusPastimeDuration = 10 * MINUTE * IN_MILLISECONDS;
    const int32 statusWanderRandomDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusDoQuestDuration = 30 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusOutDoorPvPDuration = HOUR * IN_MILLISECONDS ;
    const int32 statusTravelMountDuration = 15 * MINUTE * IN_MILLISECONDS;
    const int32 statusGatheringDuration = 20 * MINUTE * IN_MILLISECONDS;
};

class NewRpgGoGrindAction : public NewRpgBaseAction
{
public:
    NewRpgGoGrindAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go grind") {}
    bool Execute(Event event) override;
};


class NewRpgDoQuestAction : public NewRpgBaseAction
{
public:
    NewRpgDoQuestAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do quest") {}
    bool Execute(Event event) override;

protected:
    bool DoIncompleteQuest(NewRpgInfo::DoQuest& data);
    bool DoCompletedQuest(NewRpgInfo::DoQuest& data);

    const uint32 poiStayTime = 5 * 60 * 1000;
};

class NewRpgTravelFlightAction : public NewRpgBaseAction
{
public:
    NewRpgTravelFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel flight") {}
    bool Execute(Event event) override;
};

class NewRpgTravelMountAction : public NewRpgBaseAction
{
public:
    NewRpgTravelMountAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel mount") {}
    bool Execute(Event event) override;
};

class NewRpgGatheringCircuitAction : public NewRpgBaseAction
{
public:
    NewRpgGatheringCircuitAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg gathering circuit") {}
    bool Execute(Event event) override;
};

#endif
