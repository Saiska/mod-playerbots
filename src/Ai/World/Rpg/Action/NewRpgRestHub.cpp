#include "NewRpgRestHub.h"
#include "Unit.h"               // UNIT_NPC_FLAG_*
#include "GameObject.h"         // GAMEOBJECT_TYPE_*

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
