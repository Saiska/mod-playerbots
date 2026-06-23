/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_PLAYERBOTAICONFIG_H
#define _PLAYERBOT_PLAYERBOTAICONFIG_H

#include <cmath>
#include <mutex>
#include <array>
#include <unordered_map>
#include <set>
#include <vector>
#include <map>
#include <algorithm>
#include <string>
#include <chrono>

#include "DBCEnums.h"
#include "SharedDefines.h"

// slow-ai-tick-instrument (7th tick-spike instrument): per-bot UpdateAI outlier capture.
// One thread_local accumulator per bot-tick: a bot's whole UpdateAI runs to completion on a
// single MapUpdater worker thread before the next bot, so all reads/writes are same-thread.
// PlayerbotAI::UpdateAI resets it at entry and emits at exit; Engine fills the action/trigger
// laps. Microseconds (a single action's Execute can be sub-ms; ms truncation loses the signal).
// Zero cost when AiPlayerbot.SlowAiTickLogMs == 0 (every write is behind that gate).
struct SlowAiTickAccum
{
    uint64 internalUs = 0;   // UpdateAIInternal call (bracketed in UpdateAI)
    uint64 engineUs   = 0;   // Engine::DoNextAction
    uint64 trigSumUs  = 0;   // sum of all trigger->Check this tick
    uint64 actSumUs   = 0;   // sum of all action->Execute this tick
    uint32 nAct       = 0;   // actions executed this tick
    uint64 maxActUs   = 0;   // heaviest single action
    uint64 maxTrigUs  = 0;   // heaviest single trigger
    std::string maxActName;
    std::string maxTrigName;
};

inline thread_local SlowAiTickAccum g_slowAiTick;

inline void SlowAiTickReset() { g_slowAiTick = SlowAiTickAccum{}; }

inline uint64 SlowAiTickNowUs()
{
    return static_cast<uint64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void SlowAiTickNoteAct(std::string const& name, uint64 us)
{
    g_slowAiTick.actSumUs += us;
    ++g_slowAiTick.nAct;
    if (us > g_slowAiTick.maxActUs)
    {
        g_slowAiTick.maxActUs = us;
        g_slowAiTick.maxActName = name;
    }
}

inline void SlowAiTickNoteTrig(std::string const& name, uint64 us)
{
    g_slowAiTick.trigSumUs += us;
    if (us > g_slowAiTick.maxTrigUs)
    {
        g_slowAiTick.maxTrigUs = us;
        g_slowAiTick.maxTrigName = name;
    }
}

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

enum class AutoPartyBuffMode : uint8
{
    DISABLED = 0,
    RAID_ONLY = 1,
    GROUP_OR_RAID = 2
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
    RPG_RECOVER,            // bot heals up / regens before next occupation
    RPG_UPKEEP,             // bot performs maintenance (repair, restock, train)
    RPG_STATUS_END
};


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
    BEH_FLIGHT,
    BEH_SPECTATE,
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
    AutoPartyBuffMode autoGreaterBlessings;
    AutoPartyBuffMode autoPartyBuffs;
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
    float raceHomeCapitalWeightMult;
    std::vector<uint32> randomBotMaps;
    std::vector<uint32> randomBotQuestItems;
    std::vector<uint32> randomBotAccounts;
    std::vector<uint32> randomBotSpellIds;
    std::vector<uint32> randomBotQuestIds;
    uint32 randomBotTeleportDistance;
    float randomGearLoweringChance;
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

    uint32 randomBotTeleLowerLevel, randomBotTeleHigherLevel;
    std::map<uint32, std::pair<uint32, uint32>> zoneBrackets;
    bool logInGroupOnly, logValuesPerTick;
    bool fleeingEnabled;
    bool summonAtInnkeepersEnabled;
    std::string combatStrategies, nonCombatStrategies;
    std::string randomBotCombatStrategies, randomBotNonCombatStrategies;
    std::string randomBotCombatStrategiesHealer, randomBotCombatStrategiesTank;
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
    bool botSendMailEnabled;

    bool guildTaskEnabled;
    uint32 minGuildTaskChangeTime, maxGuildTaskChangeTime;
    uint32 minGuildTaskAdvertisementTime, maxGuildTaskAdvertisementTime;
    uint32 minGuildTaskRewardTime, maxGuildTaskRewardTime;
    uint32 guildTaskAdvertCleanupTime;
    uint32 guildTaskKillTaskDistance;

    uint32 iterationsPerTick;
    uint32 slowAiTickLogMs;  // slow-ai-tick-instrument threshold in ms; 0 = off (default)

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
    // --- occupation-machine (state-machine rewrite) ---
    // Per-occupation DECIDE weights (0 = never chosen); indexed by NewRpgStatus.
    uint32 occupationWeight[RPG_STATUS_END]{};
    // Per-occupation cooldown in ms (0 = no cooldown); indexed by NewRpgStatus.
    uint32 occupationCooldownMs[RPG_STATUS_END]{};
    // Fraction of dwell time that must elapse before cooldown starts counting.
    float  occupationCooldownFrac{0.25f};
    // How long (ms) since the last maintenance before the bot is considered overdue.
    uint32 maintenanceOverdueMs{7200000};
    // Max yards the bot may travel to reach the next occupation destination.
    float  rpgTravelBudget{2500.0f};
    // Witness range reused from rest-hub: how far a bot can "see" hubs/NPCs.
    float  rpgTravelWitnessRange{120.0f};
    // Enable verbose per-bot DECIDE/state-machine logging.
    bool   rpgMachineDebugLog{false};
    // NEEDS thresholds
    float  needHealthLowPct{35.0f};
    float  needManaLowPct{20.0f};
    float  needDurabilityLowPct{25.0f};
    uint32 needBagsFullSlots{2};
    // short idle dwell — bot waits this many ms before re-running the occupation-availability sweep
    uint32 rpgIdleDwellMs{2000};
    // bot-rpg-bleed-suppression: gate autonomous NewRpg off when a bot is on-task
    bool rpgSuppressWhenBusy{true};
    bool rpgSuppressInstance{true};
    bool rpgSuppressGroupedWithPlayer{true};
    bool rpgSuppressVehicle{true};
    bool rpgSuppressRaidSim{true};
    bool rpgSuppressCombat{true};
    uint32 gatherHarvestHoldMs{4000};  // gathering_circuit: hold at a node this long for the gather cast
                                       // to complete before giving up and moving to the next node
    // low-health self-preservation knobs
    bool lowHealthSelfPreservation{true};  // low-band (45%) potion-in-combat / bandage-out-of-combat
    bool rpgSuppressWhenHurt{true};        // stop NewRpg wandering while below lowHealth so the bot recovers
    // --- occupation-rebalance: context-aware fallback (Task 2) ---
    // Radius (yards) within which a bot is considered "at a rest hub" and rests in place instead of
    // farming in place. Beyond this the bot farms (kill in place) rather than sitting in the open world.
    float rpgNearHubRadius{60.0f};
    // --- occupation-rebalance: lowPriorityQuest decay (Task 8) ---
    uint32 lowPriorityQuestDecayMs{1800000};  // ms a stalled quest stays skipped (also clears on zone change); 0 = disabled
    // --- occupation-rebalance: doquest zone-travel guards (Task 9) ---
    bool   doQuestSuppressScatter{true};      // suppress random scatter-teleport while a bot is in RPG_DO_QUEST
    // --- occupation-state-machine Task 6: retire the periodic 1-5h scatter teleport ---
    bool   randomTeleportEnable{false};       // false (default): the periodic random relocation is OFF (occupations replace it)
    uint32 doQuestMaxConcurrentTravel{50};    // max bots performing a cross-zone quest teleport in the same tick
    bool healSayOncePerEpisode{true};      // announce "need heal" once per low-health descent, not in a row
    uint32 healSayMinIntervalSec{240};     // min seconds between heal-says (low health / critical health only)
    // --- more-activities-occupations (pipe 2b) ---
    float  travelMountDistMin{300.0f};
    float  travelMountDistMax{2000.0f};
    uint32 gatheringCircuitMinNodes{3};
    uint32 gatheringCircuitMaxNodes{6};
    float  gatheringCircuitRadius{120.0f};
    uint32 pastimeSocialWeight;
    float  pastimeSocialRadius;
    float  pastimeSocialClusterDist;
    uint32 pastimeSocialDwellMin;
    uint32 pastimeSocialDwellMax;
    uint32 pastimeSocialEmoteInterval;
    bool   pastimeSocialIncludePlayers;
    uint32 pastimeSocialDancePct{25};
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
    // --- occupation-hub-saturation (InnPull retired: hub selection now in RestHub state machine) ---
    uint32 restDwellMin{120};         // seconds — min seated dwell at the inn (measured from arrival)
    uint32 restDwellMax{300};         // seconds — max seated dwell
    bool   restSeatRebroadcast{true};  // RPG_REST: re-broadcast the seat stand-state each tick so a
                                       // one-shot emote can't leave the bot rendering standing
    // --- rest-hub-unification config ---
    // Note: restHubWeight is sized by RS_COUNT (RestSubtype enum in NewRpgRestHub.h).
    // NewRpgRestHub.h includes PlayerbotAIConfig.h, so we cannot include it here (circular).
    // RS_COUNT = 16 (RS_TAVERN..RS_FIELD_REST); update literal if enum grows.
    bool     restHubEnable{true};
    uint16   restHubWeight[16]{};          // per-subtype base weight (indexed by RestSubtype / RS_COUNT)
    uint16   restHubDwellMinSec{300};
    uint16   restHubDwellMaxSec{600};
    float    restHubHubRange{2500.0f};         // curated-hub in-range radius
    float  upkeepCapitalChance{0.45f};
    uint32 upkeepSellMinSec{90},     upkeepSellMaxSec{180};
    uint32 upkeepMaintMinSec{60},    upkeepMaintMaxSec{120};
    uint32 upkeepBankMinSec{300},    upkeepBankMaxSec{600};
    uint32 upkeepAHMinSec{300},      upkeepAHMaxSec{600};
    uint32 upkeepMailMinSec{90},     upkeepMailMaxSec{180};
    uint32 upkeepTrainerMinSec{120}, upkeepTrainerMaxSec{240};
    bool   upkeepDummyTestEnable{true};
    uint32 upkeepDummyTestMinMin{10}, upkeepDummyTestMaxMin{20};
    float    restHubWitnessRange{120.0f};      // real-player witness radius for TP gate
    float    restHubTravelBudget{4000.0f};     // max foot/mount distance before field-rest (witnessed)
    uint8    restHubStrollPoiCount{3};
    uint16   restHubStrollPausePerPoiSec{45};
    bool     restHubTrainerTypeFidelity{true};
    float    restHubPoiRadius{150.0f};         // grid-scan radius for hub NPC/GO POI resolution (town-sized)
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
    // --- WANDER_RANDOM micro-halts (occupation-travel-micro-halts) ---
    bool   wanderMicroHaltEnable{true};
    uint32 wanderMicroHaltGapMin{25};       // seconds between halts
    uint32 wanderMicroHaltGapMax{60};
    uint32 wanderMicroHaltDurationMin{2};   // seconds held per halt
    uint32 wanderMicroHaltDurationMax{4};
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
    bool equipAndSpecPersistence;
    int32 equipAndSpecPersistenceLevel;
    bool maintenanceGearFloor;
    int32 maintenanceGearFloorLevelGap;
    int32 maintenanceGearFloorScansPerTick;
    bool guildBankWithdraw;
    int32 guildBankWithdrawMinQuality;
    int32 guildBankWithdrawIndexTtlSeconds;
    bool guildBankDepositEnable;
    uint32 guildBankDepositMinQuality;
    int32 groupInvitationPermission;
    bool keepAltsInGroup = false;
    bool KeepAltsInGroup() const { return keepAltsInGroup; }
    bool allowSummonInCombat;
    bool allowSummonWhenMasterIsDead;
    bool allowSummonWhenBotIsDead;
    int reviveBotWhenSummoned;
    bool botRepairWhenSummon;
    bool autoInitOnly;
    bool resetInstanceIdForAltBots;
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
    int32 autoGearBisCommand;

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
    bool   raidSimBroadcastRealmWide;      // realm-wide [System] lines (new wording) vs legacy guild chat
    uint32 raidSimBroadcastLootMinQuality; // min item Quality for a realm-wide loot line (spam gate)
    bool   raidSimAnnounce;            // server-wide SendWorldText announce (debug/flavor; orthogonal)
    // Orphan reaper (raidsim-orphan-reaper): leader-independent teardown of all-random-bot groups
    // left over from a restart (orphans not backed by a live run). Budgeted drip; off the world tick.
    bool   raidSimOrphanReaper;    // master switch for the reaper
    uint32 raidSimReaperInterval;  // seconds between reaper passes
    uint32 raidSimReaperBatch;     // max orphan groups disbanded per pass (drip budget)

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

    // Provision First-Aid bandages (and the fishing pole) to bots that have the matching skill.
    bool botProvisionConsumables{true};

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
