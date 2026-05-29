#ifndef _PLAYERBOT_PLAYERBOTGUILDMGR_H
#define _PLAYERBOT_PLAYERBOTGUILDMGR_H

#include "Guild.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include <unordered_map>
#include <utility>

struct GuildTheme
{
    std::string slug;
    uint8  faction = 2;
    uint32 classMask = 0;
    uint32 raceMask = 0;
    uint32 affinityClassMask = 0;
    uint32 affinityRaceMask = 0;
    uint8  minLevel = 1;
    uint16 targetSize = 15;
    int16  tabardEmblemStyle = -1;
    int16  tabardEmblemColor = -1;
    int16  tabardBorderStyle = -1;
    int16  tabardBorderColor = -1;
    int16  tabardBgColor = -1;
    bool   valid = false;
};

class PlayerbotGuildMgr
{
public:
    static PlayerbotGuildMgr& instance()
    {
        static PlayerbotGuildMgr instance;

        return instance;
    }

    void Init();
    std::string AssignToGuild(Player* player);
    void LoadGuildNames();
    void ValidateGuildCache();
    GuildTheme const& GetThemeByName(std::string const& guildName) const;
    uint8 PickRankForBot(GuildTheme const& theme, Player* bot) const;
    void ResetGuildCache();
    bool CreateGuild(Player* player, std::string guildName);
    void OnGuildUpdate  (Guild* guild);
    bool SetGuildEmblem(uint32 guildId);
    void DeleteBotGuilds();
    bool IsRealGuild(uint32 guildId);
    bool IsRealGuild(Player* bot);

private:
    PlayerbotGuildMgr() = default;
    ~PlayerbotGuildMgr() = default;

    PlayerbotGuildMgr(const PlayerbotGuildMgr&) = delete;
    PlayerbotGuildMgr& operator=(const PlayerbotGuildMgr&) = delete;

    PlayerbotGuildMgr(PlayerbotGuildMgr&&) = delete;
    PlayerbotGuildMgr& operator=(PlayerbotGuildMgr&&) = delete;

    std::unordered_map<std::string, bool> _guildNames;

    struct GuildCache
    {
        std::string name;
        uint8 status;
        uint32 maxMembers = 0;
        uint32 memberCount = 0;
        uint8 faction = 0;
        bool hasRealPlayer = false;
    };
    std::unordered_map<uint32 , GuildCache> _guildCache;
    std::vector<std::string> _shuffled_guild_keys;
    std::unordered_map<std::string, GuildTheme> _guildThemes;                                                  // keyed by guild name
    std::unordered_map<std::string, std::vector<std::pair<uint8, std::string>>> _guildRankNames;               // keyed by theme_slug
};

void PlayerBotsGuildValidationScript();

#endif
