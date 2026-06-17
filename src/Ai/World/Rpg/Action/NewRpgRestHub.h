#ifndef _PLAYERBOT_NEWRPGRESTHUB_H
#define _PLAYERBOT_NEWRPGRESTHUB_H

#include "PlayerbotAIConfig.h"   // BotBehaviorId, BotCityPoi
#include "ObjectGuid.h"
#include "Position.h"            // WorldPosition
#include <cstdint>

class Player;
class NewRpgBaseAction;

enum RestSubtype : uint8
{
    RS_TAVERN = 0, RS_CLASS_TRAINER, RS_PROFESSION_CRAFT, RS_VENDOR, RS_QUEST_GIVER,
    RS_BANK, RS_AUCTION_HOUSE, RS_MAILBOX, RS_FLIGHT_MASTER, RS_SOCIAL, RS_STROLL,
    RS_SPECTATE, RS_DUMMY, RS_DUEL, RS_FISH, RS_FIELD_REST,
    RS_COUNT,
    RS_NONE = 0xFF
};

enum TargetKind : uint8
{
    TK_INN_CHAIR, TK_NPC_FLAG, TK_FORGE_OR_TRAINER, TK_VENDOR, TK_GO_TYPE,
    TK_SOCIAL, TK_STROLL, TK_SPECTATE, TK_DUMMY, TK_DUEL, TK_WATER, TK_IN_PLACE
};

// Tri-state result of the witness-gated hub travel: arrived (TP or on-foot),
// still walking/mounting, or give up (witnessed + beyond the foot/mount budget).
enum HubTravel : uint8 { HUB_ARRIVED, HUB_EN_ROUTE, HUB_GIVE_UP };

struct RestSubtypeDef
{
    RestSubtype  id;
    const char*  name;          // census/log label
    TargetKind   target;
    uint32       npcFlagOrGoType; // UNIT_NPC_FLAG_* or GAMEOBJECT_TYPE_* depending on target
    bool         needsHub;
    BotBehaviorId palette;       // emote palette/pose lookup row
    BotCityPoi   poiVariant;     // for BEH_LOITER per-POI palette (POI_NONE if N/A)
    bool         functional;     // true = fires sell/repair/craft/fish/duel/dummy/eat action
};

extern const RestSubtypeDef kRestTable[RS_COUNT];

// Pure weighted picker (testable, no bot state). Contract:
//   weight[]  base per-subtype weight. Rows whose avail[] is false are SKIPPED via the avail
//             gate (the fn does not re-zero weight[]); the caller may pass weight[] as-is.
//   avail[]   per-subtype availability gate (hub-reachable/eligible/present).
//   last      the previous episode's subtype (RS_NONE if none) — its effective weight is quartered
//             for anti-repeat (still selectable, just less likely).
//   rngRoll   a value the caller drew in [1, sum] where sum is the post-gate, post-anti-repeat total.
// Returns the picked RestSubtype; RS_FIELD_REST if nothing is available (sum == 0).
// Because the caller needs `sum` to draw rngRoll, compute it with RestSubtypeEffectiveSum() first.
RestSubtype PickRestSubtypePure(const uint16 weight[RS_COUNT], const bool avail[RS_COUNT],
                                RestSubtype last, uint32 rngRoll);

// Companion to PickRestSubtypePure: returns the cumulative weight sum used to draw rngRoll,
// applying the same gating (avail) and anti-repeat (quarter `last`) rules. 0 = nothing available.
uint32 RestSubtypeEffectiveSum(const uint16 weight[RS_COUNT], const bool avail[RS_COUNT],
                               RestSubtype last);

#endif
