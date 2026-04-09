/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_RANDOMPLAYERBOTFACTORY_H
#define _PLAYERBOT_RANDOMPLAYERBOTFACTORY_H

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "ArenaTeam.h"
#include "Common.h"
#include "DBCEnums.h"

class Player;
class WorldSession;

class RandomPlayerbotFactory
{
public:
    enum class NameRaceAndGender : uint8
    {
        // Generic is the category used for human & undead
        GenericMale = 0,
        GenericFemale,
        GnomeMale,
        GnomeFemale,
        DwarfMale,
        DwarfFemale,
        NightelfMale,
        NightelfFemale,
        DraeneiMale,
        DraeneiFemale,
        OrcMale,
        OrcFemale,
        TrollMale,
        TrollFemale,
        TaurenMale,
        TaurenFemale,
        BloodelfMale,
        BloodelfFemale
    };

    static constexpr NameRaceAndGender CombineRaceAndGender(uint8 race, uint8 gender);

    RandomPlayerbotFactory() {};
    virtual ~RandomPlayerbotFactory() {}

    Player* CreateRandomBot(WorldSession* session, uint8 cls, std::unordered_map<NameRaceAndGender, std::vector<std::string>>& names);
    static void CreateRandomBots();
    static std::string const CreateRandomGuildName();
    static uint32 CalculateTotalAccountCount();
    static uint32 CalculateAvailableCharsPerAccount();

    // Arena team utilities (static — no bot instance needed)
    static void DeleteBotArenaTeams();
    static uint32 GetBotArenaTeamCount(ArenaType type);
    static bool IsBotArenaTeam(ArenaTeam const* team);
    static void LoadArenaTeamNames();
    static std::string CreateRandomArenaTeamName();

private:
    static bool IsValidRaceClassCombination(uint8 race, uint8 class_, uint32 expansion);
    std::string const CreateRandomBotName(NameRaceAndGender raceAndGender);

    static inline std::vector<std::string> _availableArenaTeamNames;
};

#endif
