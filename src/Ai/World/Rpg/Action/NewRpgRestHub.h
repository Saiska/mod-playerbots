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

#endif
