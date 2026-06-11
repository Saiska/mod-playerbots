/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_PLAYERbotAICONFIG_H
#define _PLAYERBOT_PLAYERbotAICONFIG_H

#include <cmath>
#include <mutex>
#include <array>
#include <unordered_map>
#include <set>
#include <vector>
#include <map>
#include <algorithm>
#include <string>

#include "DBCEnums.h"
#include "SharedDefines.h"

enum class BotCheatMask : uint32
{
    none = 0,
    taxi = 1,
    gold = 2,
    health = 4,
    mana = 8,
    power = 16,
    raid = 32,
    food = 64,
    maxMask = 128
};

enum class HealingManaEfficiency : uint8
{
    VERY_LOW = 1,
    LOW = 2,
    MEDIUM = 4,
    HIGH = 8,
    VERY_HIGH = 16,
    SUPERIOR = 32
};

enum NewRpgStatus : int
{
    //Initial Status
    RPG_IDLE = 0,
    RPG_GO_GRIND = 1,
    // Exploring nearby
    RPG_WANDER_RANDOM = 3,
    RPG_WANDER_NPC = 4,
    // Do Quest (based on quest status)
    RPG_DO_QUEST = 5,
    // Travel

    RPG_TRAVEL_FLIGHT = 6,
    // Taking a break
    RPG_REST = 7,
    RPG_OUTDOOR_PVP = 8,
    RPG_PASTIME,      // leisure/social activities (framework + social starter)
    RPG_TRAVEL_MOUNT,       // ride overland to a far hub
    RPG_GATHERING_CIRCUIT,  // multi-node profession-gathering loop
    RPG_STATUS_END
};

// Thematic activity categories for occupation satiation (pipe 1).
// Data-driven: a future occupation just adds one case to CategoryOf and
// inherits a satiation axis for free. CAT_COUNT doubles as "no category".
enum BotActivityCategory : uint8
{
    CAT_ADVENTURE = 0,
    CAT_SOCIAL,
    CAT_WORK,      // was CAT_REST
    CAT_TRAVEL,
    CAT_PVP,
    CAT_COUNT          // element count AND the "none" sentinel (e.g. RPG_IDLE)
};

inline BotActivityCategory CategoryOf(NewRpgStatus s)
{
    switch (s)
    {
        case RPG_GO_GRIND:
        case RPG_WANDER_RANDOM:        return CAT_ADVENTURE;
        case RPG_DO_QUEST:
        case RPG_GATHERING_CIRCUIT:    return CAT_WORK;
        case RPG_REST:
        case RPG_WANDER_NPC:
        case RPG_PASTIME:              return CAT_SOCIAL;
        case RPG_TRAVEL_FLIGHT:
        case RPG_TRAVEL_MOUNT:         return CAT_TRAVEL;
        case RPG_OUTDOOR_PVP:          return CAT_PVP;
        default:                       return CAT_COUNT;  // RPG_IDLE / none
    }
}

// Pure appeal multiplier: 1.0 at empty meter, 0.0 at full. Higher exponent =
// sharper "had enough". Invariants (relied on, no unit harness in this module):
//   sat<=0 -> 1.0 ; sat>=1 -> 0.0 ; strictly decreasing in sat for exponent>0.
inline float RpgSatiationSuppress(float sat, float exponent)
{
    if (sat <= 0.0f)
        return 1.0f;
    if (sat >= 1.0f)
        return 0.0f;
    return std::pow(1.0f - sat, exponent);
}

enum BotCityPoi : uint8
{
    POI_NONE = 0,
    POI_AUCTIONEER,
    POI_BANKER,
    POI_INNKEEPER,
    POI_TRAINER,
    POI_MAILBOX,
    POI_FORGE
};

enum BotActivity : uint8 { ACTIVITY_SOCIAL = 0, ACTIVITY_LOITER, ACTIVITY_FISH, ACTIVITY_CRAFT, ACTIVITY_DUEL, ACTIVITY_REPAIR_SELL, ACTIVITY_DUMMY, ACTIVITY_NONE = 0xFF };

// Unified flat id over the 16 rationalized behaviors (9 top-level statuses + 7 pastimes).
// The shared spine: emote palettes, cadence, and lifecycle chat all key off this.
enum BotBehaviorId : uint8
{
    BEH_NONE = 0,
    // top-level occupations (NewRpgStatus)
    BEH_GO_GRIND, BEH_WANDER_RANDOM, BEH_DO_QUEST, BEH_GATHERING_CIRCUIT,
    BEH_REST, BEH_WANDER_NPC, BEH_TRAVEL_FLIGHT, BEH_TRAVEL_MOUNT, BEH_OUTDOOR_PVP,
    // pastimes (BotActivity, under RPG_PASTIME)
    BEH_SOCIAL, BEH_LOITER, BEH_FISH, BEH_CRAFT, BEH_DUEL, BEH_REPAIR_SELL, BEH_DUMMY,
    BEH_COUNT
};

// Curated emote set for a behavior (or a behavior+variant). Held pose + a pool
// of one-shots picked from at random by the cadence tick. See the kPalette table
// in NewRpgBaseAction.cpp (data-driven, one row per BotBehaviorId).
struct EmotePalette
{
    uint32        sustainedPose;   // UNIT_NPC_EMOTESTATE value; 0 = none/stand
    const uint32* oneShots;        // static pool; nullptr/0 count = no one-shots
    uint8         oneShotCount;
};

#define MAX_SPECNO 20

class PlayerbotAIConfig
{
public:
    static PlayerbotAIConfig& instance()
    {
        static PlayerbotAIConfig instance;

        return instance;
    }

    bool Initialize();
    bool IsInRandomAccountList(uint32 id);
    bool IsInRandomQuestItemList(uint32 id);
    bool IsPvpProhibited(uint32 zoneId, uint32 areaId);
    bool IsInPvpProhibitedZone(uint32 id);
    bool IsInPvpProhibitedArea(uint32 id);

    bool enabled;
    bool disabledWithoutRealPlayer;
    bool EnableICCBuffs;
    bool allowAccountBots, allowGuildBots, allowTrustedAccountBots;
    bool randomBotGuildNearby, randomBotInvitePlayer, inviteChat;
    uint32 globalCoolDown, reactDelay, maxWaitForMove, disableMoveSplinePath, maxMovementSearchTime, expireActionTime,
        dispelAuraDuration, passiveDelay, repeatDelay, errorDelay, rpgDelay, sitDelay, returnDelay, lootDelay;
    bool dynamicReactDelay;
    float sightDistance, spellDistance, reactDistance, grindDistance, lootDistance, shootDistance, fleeDistance,
        tooCloseDistance, meleeDistance, followDistance, whisperDistance, contactDistance, aoeRadius, rpgDistance,
        targetPosRecalcDistance, farDistance, healDistance, aggroDistance;
    uint32 criticalHealth, lowHealth, mediumHealth, almostFullHealth;
    uint32 lowMana, mediumMana, highMana;
    bool autoSaveMana;
    uint32 saveManaThreshold;
    bool autoAvoidAoe;
    float maxAoeAvoidRadius;
    std::set<uint32> aoeAvoidSpellWhitelist;
    bool tellWhenAvoidAoe;
    std::set<uint32> disallowedGameObjects;
    std::set<uint32> attunementQuests;
    std::set<uint32> unobtainableItems;

    uint32 openGoSpell;
    bool randomBotAutologin;
    bool botAutologin;
    std::string randomBotMapsAsString;
    float probTeleToBankers;
    bool enableWeightTeleToCityBankers;
    int weightTeleToStormwind;
    int weightTeleToIronforge;
    int weightTeleToDarnassus;
    int weightTeleToExodar;
    int weightTeleToOrgrimmar;
    int weightTeleToUndercity;
    int weightTeleToThunderBluff;
    int weightTeleToSilvermoonCity;
    int weightTeleToShattrathCity;
    int weightTeleToDalaran;
    std::vector<uint32> randomBotMaps;
    std::vector<uint32> randomBotQuestItems;
    std::vector<uint32> randomBotAccounts;
    std::vector<uint32> randomBotSpellIds;
    std::vector<uint32> randomBotQuestIds;
    uint32 randomBotTeleportDistance;
    float randomGearLoweringChance;
    bool incrementalGearInit;
    int32 randomGearQualityLimit;
    int32 randomGearScoreLimit;
    bool preferClassArmorType;
    bool preferredSpecWeapons;
    float randomBotMinLevelChance, randomBotMaxLevelChance;
    float randomBotRpgChance;
    uint32 minRandomBots, maxRandomBots;
    uint32 randomBotUpdateInterval, randomBotCountChangeMinInterval, randomBotCountChangeMaxInterval;
    uint32 minRandomBotInWorldTime, maxRandomBotInWorldTime;
    uint32 minRandomBotRandomizeTime, maxRandomBotRandomizeTime;
    uint32 minRandomBotChangeStrategyTime, maxRandomBotChangeStrategyTime;
    uint32 minRandomBotReviveTime, maxRandomBotReviveTime;
    uint32 minRandomBotTeleportInterval, maxRandomBotTeleportInterval;
    uint32 permanentlyInWorldTime;
    uint32 minRandomBotPvpTime, maxRandomBotPvpTime;
    uint32 randomBotsPerInterval;
    uint32 minRandomBotsPriceChangeInterval, maxRandomBotsPriceChangeInterval;
    uint32 disabledWithoutRealPlayerLoginDelay, disabledWithoutRealPlayerLogoutDelay;
    bool randomBotJoinLfg;

    // Buff system
    // Min group size to use Greater buffs (Paladin, Mage, Druid). Default: 3
    int32 minBotsForGreaterBuff;
    // Cooldown (seconds) between reagent-missing RP warnings, per bot & per buff. Default: 30
    int32 rpWarningCooldown;

    // Professions
    bool enableFishingWithMaster;
    uint32 classMatchingProfessionChance;
    float fishingDistanceFromMaster, fishingDistance, endFishingWithMaster;

    // chat
    bool randomBotTalk;
    bool randomBotEmote;
    bool randomBotSuggestDungeons;
    bool enableBroadcasts;
    bool enableGreet;
    bool randomBotSayWithoutMaster;

    uint32 broadcastChanceMaxValue;

    uint32 broadcastToGuildGlobalChance;
    uint32 broadcastToWorldGlobalChance;
    uint32 broadcastToGeneralGlobalChance;
    uint32 broadcastToTradeGlobalChance;
    uint32 broadcastToLFGGlobalChance;
    uint32 broadcastToLocalDefenseGlobalChance;
    uint32 broadcastToWorldDefenseGlobalChance;
    uint32 broadcastToGuildRecruitmentGlobalChance;

    uint32 broadcastChanceLootingItemPoor;
    uint32 broadcastChanceLootingItemNormal;
    uint32 broadcastChanceLootingItemUncommon;
    uint32 broadcastChanceLootingItemRare;
    uint32 broadcastChanceLootingItemEpic;
    uint32 broadcastChanceLootingItemLegendary;
    uint32 broadcastChanceLootingItemArtifact;

    uint32 broadcastChanceQuestAccepted;
    uint32 broadcastChanceQuestUpdateObjectiveCompleted;
    uint32 broadcastChanceQuestUpdateObjectiveProgress;
    uint32 broadcastChanceQuestUpdateFailedTimer;
    uint32 broadcastChanceQuestUpdateComplete;
    uint32 broadcastChanceQuestTurnedIn;

    uint32 broadcastChanceKillNormal;
    uint32 broadcastChanceKillElite;
    uint32 broadcastChanceKillRareelite;
    uint32 broadcastChanceKillWorldboss;
    uint32 broadcastChanceKillRare;
    uint32 broadcastChanceKillUnknown;
    uint32 broadcastChanceKillPet;
    uint32 broadcastChanceKillPlayer;

    uint32 broadcastChanceLevelupGeneric;
    uint32 broadcastChanceLevelupTenX;
    uint32 broadcastChanceLevelupMaxLevel;

    uint32 broadcastChanceSuggestInstance;
    uint32 broadcastChanceSuggestQuest;
    uint32 broadcastChanceSuggestGrindMaterials;
    uint32 broadcastChanceSuggestGrindReputation;
    uint32 broadcastChanceSuggestSell;
    uint32 broadcastChanceSuggestSomething;

    uint32 broadcastChanceSuggestSomethingToxic;

    uint32 broadcastChanceSuggestToxicLinks;
    std::string toxicLinksPrefix;
    uint32 toxicLinksRepliesChance;

    uint32 broadcastChanceSuggestThunderfury;
    uint32 thunderfuryRepliesChance;

    uint32 broadcastChanceGuildManagement;

    uint32 guildRepliesRate;

    bool suggestDungeonsInLowerCaseRandomly;

    // --

    bool randomBotJoinBG;
    bool randomBotAutoJoinBG;

    std::string randomBotAutoJoinICBrackets;
    std::string randomBotAutoJoinEYBrackets;
    std::string randomBotAutoJoinAVBrackets;
    std::string randomBotAutoJoinABBrackets;
    std::string randomBotAutoJoinWSBrackets;

    uint32 randomBotAutoJoinBGICCount;
    uint32 randomBotAutoJoinBGEYCount;
    uint32 randomBotAutoJoinBGAVCount;
    uint32 randomBotAutoJoinBGABCount;
    uint32 randomBotAutoJoinBGWSCount;

    uint32 randomBotAutoJoinArenaBracket;

    uint32 randomBotAutoJoinBGRatedArena2v2Count;
    uint32 randomBotAutoJoinBGRatedArena3v3Count;
    uint32 randomBotAutoJoinBGRatedArena5v5Count;

    bool randomBotLoginAtStartup;
    uint32 randomBotTeleLowerLevel, randomBotTeleHigherLevel;
    std::map<uint32, std::pair<uint32, uint32>> zoneBrackets;
    bool logInGroupOnly, logValuesPerTick;
    bool fleeingEnabled;
    bool summonAtInnkeepersEnabled;
    std::string combatStrategies, nonCombatStrategies;
    std::string randomBotCombatStrategies, randomBotNonCombatStrategies;
    bool applyInstanceStrategies;
    uint32 randomBotMinLevel, randomBotMaxLevel;
    float randomChangeMultiplier;

    // std::string premadeLevelSpec[MAX_CLASSES][10][91]; //lvl 10 - 100
    // ClassSpecs classSpecs[MAX_CLASSES];

    std::string premadeSpecName[MAX_CLASSES][MAX_SPECNO];
    std::string premadeSpecGlyph[MAX_CLASSES][MAX_SPECNO];
    std::vector<uint32> parsedSpecGlyph[MAX_CLASSES][MAX_SPECNO];
    std::string premadeSpecLink[MAX_CLASSES][MAX_SPECNO][MAX_LEVEL];
    std::string premadeHunterPetLink[3][21];
    std::vector<std::vector<uint32>> parsedSpecLinkOrder[MAX_CLASSES][MAX_SPECNO][MAX_LEVEL];
    std::vector<std::vector<uint32>> parsedHunterPetLinkOrder[3][21];
    uint32 randomClassSpecProb[MAX_CLASSES][MAX_SPECNO];
    uint32 randomClassSpecIndex[MAX_CLASSES][MAX_SPECNO];

    std::string commandPrefix, commandSeparator;
    std::string randomBotAccountPrefix;
    uint32 randomBotAccountCount;
    bool randomBotRandomPassword;
    bool deleteRandomBotAccounts;
    uint32 randomBotGuildCount, randomBotGuildSizeMax;
    bool guildLifecycleEnable;
    uint32 guildLifecyclePeriod;        // seconds
    uint32 guildLifecycleFoundQuorum;
    uint32 guildLifecycleDisbandFloor;
    uint32 guildLifecycleMaxActionsPerCycle;
    float themedGuildTemperature;
    bool deleteRandomBotGuilds;
    std::vector<uint32> pvpProhibitedZoneIds;
    std::vector<uint32> pvpProhibitedAreaIds;
    bool fastReactInBG;

    bool randombotsWalkingRPG;
    bool randombotsWalkingRPGInDoors;
    uint32 minEnchantingBotLevel;
    uint32 limitEnchantExpansion;
    uint32 limitGearExpansion;
    uint32 randombotStartingLevel;
    bool enablePeriodicOnlineOffline;
    float periodicOnlineOfflineRatio;
    bool gearscorecheck;
    bool randomBotPreQuests;

    bool guildTaskEnabled;
    uint32 minGuildTaskChangeTime, maxGuildTaskChangeTime;
    uint32 minGuildTaskAdvertisementTime, maxGuildTaskAdvertisementTime;
    uint32 minGuildTaskRewardTime, maxGuildTaskRewardTime;
    uint32 guildTaskAdvertCleanupTime;
    uint32 guildTaskKillTaskDistance;

    uint32 iterationsPerTick;

    std::mutex m_logMtx;
    bool enableAutoTradeOnItemMention;
    std::vector<std::string> tradeActionExcludedPrefixes;
    std::vector<std::string> allowedLogFiles;
    std::unordered_map<std::string, std::pair<FILE*, bool>> logFiles;

    std::vector<std::string> botCheats;
    uint32 botCheatMask = 0;

    struct worldBuff
    {
        uint32 spellId;
        uint32 factionId;
        uint32 classId;
        uint32 specId;
        uint32 minLevel;
        uint32 maxLevel;
    };

    std::vector<worldBuff> worldBuffs;

    uint32 commandServerPort;
    bool perfMonEnabled;
    bool summonWhenGroup;
    bool randomBotShowHelmet;
    bool randomBotShowCloak;
    bool randomBotFixedLevel;
    bool disableRandomLevels;
    float randomBotXPRate;
    uint32 randomBotAllianceRatio;
    uint32 randomBotHordeRatio;
    bool disableDeathKnightLogin;
    bool limitTalentsExpansion;
    uint32 botActiveAlone;
    uint32 BotActiveAloneDurationSeconds;
    uint32 BotActiveAloneForceWhenInRadius;
    bool BotActiveAloneForceWhenInZone;
    bool BotActiveAloneForceWhenInMap;
    bool BotActiveAloneForceWhenIsFriend;
    bool BotActiveAloneForceWhenInGuild;
    bool botActiveAloneSmartScale;
    uint32 botActiveAloneSmartScaleDiffLimitfloor;
    uint32 botActiveAloneSmartScaleDiffLimitCeiling;
    uint32 botActiveAloneSmartScaleWhenMinLevel;
    uint32 botActiveAloneSmartScaleWhenMaxLevel;

    bool freeMethodLoot;
    int32 lootNeedRollLevel;
    bool lootGreedRollLevel;
    bool lootRollRecipe;
    bool lootRollDisenchant;
    std::string autoPickReward;
    bool autoEquipUpgradeLoot;
    float equipUpgradeThreshold;
    bool twoRoundsGearInit;
    bool syncQuestWithPlayer;
    bool syncQuestForPlayer;
    bool dropObsoleteQuests;
    bool allowLearnTrainerSpells;
    bool autoPickTalents;
    bool autoUpgradeEquip;
    int32 hunterWolfPet;
    int32 defaultPetStance;
    int32 petChatCommandDebug;
    bool autoLearnTrainerSpells;
    bool autoDoQuests;
    bool enableNewRpgStrategy;
    std::unordered_map<NewRpgStatus, uint32> RpgStatusProbWeight;
    // --- occupation satiation (pipe 1: bot-occupation-satiation) ---
    bool  rpgSatiationEnable{true};
    float rpgSatiationRiseRatePerSec{0.0025f};   // meter gain/sec while in-category (~400s to fill)
    float rpgSatiationDecayRatePerSec{0.0015f};  // meter loss/sec otherwise (~670s to empty)
    float rpgSatiationSuppressExponent{1.5f};    // steepness of (1-meter)^k
    float rpgSatiationMinAppealFrac{0.05f};      // floor as fraction of base weight
    // --- more-activities-occupations (pipe 2b) ---
    float  travelMountDistMin{300.0f};
    float  travelMountDistMax{2000.0f};
    uint32 gatheringCircuitMinNodes{3};
    uint32 gatheringCircuitMaxNodes{6};
    float  gatheringCircuitRadius{60.0f};
    uint32 pastimeSocialWeight;
    float  pastimeSocialRadius;
    float  pastimeSocialClusterDist;
    uint32 pastimeSocialDwellMin;
    uint32 pastimeSocialDwellMax;
    uint32 pastimeSocialEmoteInterval;
    bool   pastimeSocialIncludePlayers;
    std::vector<std::string> pastimeSocialEmotes;
    uint32 pastimeLoiterWeight;
    uint32 pastimeLoiterDwellMin;
    uint32 pastimeLoiterDwellMax;
    uint32 pastimeLoiterPoiTypeMask;
    bool   pastimeLoiterThemedScenes;
    float  pastimeLoiterTypeWeight[POI_FORGE + 1];
    uint32 pastimeFishWeight;
    uint32 pastimeFishDwellMin;
    uint32 pastimeFishDwellMax;
    uint32 pastimeCraftWeight;
    uint32 pastimeCraftDwellMin;
    uint32 pastimeCraftDwellMax;
    uint32 pastimeDuelWeight;
    float  pastimeDuelRadius;
    bool   pastimeDuelIncludePlayers;
    // --- occupation-hub-saturation (pipe 3: Rest InnPull) ---
    bool   restInnPullEnable{true};   // RPG_REST routes to a nearby innkeeper hub, then sits
    uint32 restDwellMin{120};         // seconds — min seated dwell at the inn (measured from arrival)
    uint32 restDwellMax{300};         // seconds — max seated dwell
    // --- more-activities-pastimes (pipe 2a) ---
    uint32 pastimeRepairSellWeight{25};
    uint32 pastimeRepairSellDwellMin{5};
    uint32 pastimeRepairSellDwellMax{15};
    float  pastimeRepairSellRadius{60.0f};
    uint32 pastimeDummyWeight{20};
    uint32 pastimeDummyDwellMin{30};
    uint32 pastimeDummyDwellMax{90};
    float  pastimeDummyRadius{60.0f};
    std::vector<uint32> pastimeDummyEntries;  // training-dummy creature entries (CSV in conf)
    // --- per-behavior emote cadence (occupation-emote-palettes) ---
    bool   emoteCadenceEnable{true};
    uint32 emoteCadenceMin[BEH_COUNT]{};   // seconds, per BotBehaviorId
    uint32 emoteCadenceMax[BEH_COUNT]{};
    bool syncLevelWithPlayers;
    bool autoLearnQuestSpells;
    bool autoTeleportForLevel;
    bool randomBotGroupNearby;
    int32 enableRandomBotTrading;
    uint32 tweakValue;  // Debugging config

    uint32 randomBotArenaTeamCount;
    uint32 randomBotArenaTeamMaxRating;
    uint32 randomBotArenaTeamMinRating;
    uint32 randomBotArenaTeam2v2Count;
    uint32 randomBotArenaTeam3v3Count;
    uint32 randomBotArenaTeam5v5Count;
    bool deleteRandomBotArenaTeams;
    std::vector<uint32> randomBotArenaTeams;

    uint32 selfBotLevel;
    bool downgradeMaxLevelBot;
    bool equipmentPersistence;
    int32 equipmentPersistenceLevel;
    int32 groupInvitationPermission;
    bool keepAltsInGroup = false;
    bool KeepAltsInGroup() const { return keepAltsInGroup; }
    bool allowSummonInCombat;
    bool allowSummonWhenMasterIsDead;
    bool allowSummonWhenBotIsDead;
    int reviveBotWhenSummoned;
    bool botRepairWhenSummon;
    bool autoInitOnly;
    float autoInitEquipLevelLimitRatio;
    int32 maxAddedBots;
    int32 addClassCommand;
    int32 addClassAccountPoolSize;
    int32 maintenanceCommand;
    bool altMaintenanceAttunementQs,
            altMaintenanceBags,
            altMaintenanceAmmo,
            altMaintenanceFood,
            altMaintenanceReagents,
            altMaintenanceConsumables,
            altMaintenancePotions,
            altMaintenanceTalentTree,
            altMaintenancePet,
            altMaintenancePetTalents,
            altMaintenanceClassSpells,
            altMaintenanceAvailableSpells,
            altMaintenanceSkills,
            altMaintenanceReputation,
            altMaintenanceSpecialSpells,
            altMaintenanceMounts,
            altMaintenanceGlyphs,
            altMaintenanceKeyring,
            altMaintenanceGemsEnchants;
    // Autonomous (timer-driven) maintenance — runs the maintenance set on a
    // staggered per-bot cadence, independent of the manual MaintenanceCommand.
    bool autoMaintenance;
    uint32 minAutoMaintenanceInterval;
    uint32 maxAutoMaintenanceInterval;
    int32 autoGearCommand, autoGearCommandAltBots, autoGearQualityLimit, autoGearScoreLimit;

    uint32 useGroundMountAtMinLevel;
    uint32 useFastGroundMountAtMinLevel;
    uint32 useFlyMountAtMinLevel;
    uint32 useFastFlyMountAtMinLevel;

    // stagger flightpath takeoff
    uint32 botTaxiDelayMin;
    uint32 botTaxiDelayMax;
    uint32 botTaxiGapMs;
    uint32 botTaxiGapJitterMs;

    // --- Autonomous Instance Simulation (RaidSim) ---
    bool   raidSimEnable;
    uint32 raidSimDuration;        // minutes a run lasts
    uint32 raidSimLootInterval;    // minutes between loot awards
    uint32 raidSimRollsPerInterval;// members rolled per interval (1 = trickle)
    uint32 raidSimDungeonPeriod;   // minutes between a guild's dungeon runs (start-to-start target)
    uint32 raidSimRaidPeriod;      // minutes between a guild's raid runs (start-to-start target)
    uint32 raidSimJitterPct;       // +/- percent jitter on the period, redrawn each cycle
    uint32 raidSimDefaultOffset;   // tier offset applied to guilds with raid_offset < 0 (unassigned)
    uint32 raidSimMinDungeon;      // min online L80 to field a 5-man
    uint32 raidSimMinRaid10;       // min online L80 to field a 10-man
    uint32 raidSimMinRaid25;       // min online L80 to field a 25-man
    bool   raidSimOnlyUpgrades;    // only equip rolled items that beat the current slot
    // Guild-chat broadcast toggles (in-character immersion). Master gates the per-category ones.
    bool   raidSimBroadcast;           // master switch for ALL guild-chat broadcasts
    bool   raidSimBroadcastStartStop;  // "sets out for X" / "returns from X" lines
    bool   raidSimBroadcastLoot;       // "<bot> receives <item>" lines
    bool   raidSimAnnounce;            // server-wide SendWorldText announce (debug/flavor; orthogonal)

    // Population dynamics (server-population-dynamics). Per-LEVEL targeting: each 10-level band has one
    // "bots per level" knob (populationBracket[b]); level 80 is the remainder (MaxPopulation - sum of bands).
    bool   populationDynamicsEnable;
    uint32 populationMaxPopulation;      // total population at full endgame (cap = 80)
    uint32 populationHeadroom;
    uint32 populationMinCap;
    uint32 populationPeriod;             // seconds between conveyor ticks
    uint32 populationMaxPromotionsPerCycle;
    uint32 populationSinkPeriod;         // seconds between level-80 sink-gate ticks
    uint32 populationSinkBatch;          // bots promoted 79->80 per faction per sink tick
    std::array<uint32, 8> populationBracket;   // bots-per-level for bands 0..7 (levels 1-9 .. 70-79)
    bool   populationClassFavor;               // favor most under-represented class when promoting

    std::string const GetTimestampStr();
    bool hasLog(std::string const fileName)
    {
        return std::find(allowedLogFiles.begin(), allowedLogFiles.end(), fileName) != allowedLogFiles.end();
    };
    bool openLog(std::string const fileName, char const* mode = "a");
    bool isLogOpen(std::string const fileName)
    {
        auto it = logFiles.find(fileName);
        return it != logFiles.end() && it->second.second;
    }
    void log(std::string const fileName, const char* str, ...);

    void loadWorldBuff();

    static std::vector<std::vector<uint32>> ParseTempTalentsOrder(uint32 cls, std::string temp_talents_order);
    static std::vector<std::vector<uint32>> ParseTempPetTalentsOrder(uint32 spec, std::string temp_talents_order);

    bool restrictHealerDPS = false;
    std::vector<uint32> restrictedHealerDPSMaps;
    bool IsRestrictedHealerDPSMap(uint32 mapId) const;

    std::vector<uint32> excludedHunterPetFamilies;

private:
    PlayerbotAIConfig() = default;
    ~PlayerbotAIConfig() = default;

    PlayerbotAIConfig(const PlayerbotAIConfig&) = delete;
    PlayerbotAIConfig& operator=(const PlayerbotAIConfig&) = delete;

    PlayerbotAIConfig(PlayerbotAIConfig&&) = delete;
    PlayerbotAIConfig& operator=(PlayerbotAIConfig&&) = delete;
};

#define sPlayerbotAIConfig PlayerbotAIConfig::instance()

#endif
