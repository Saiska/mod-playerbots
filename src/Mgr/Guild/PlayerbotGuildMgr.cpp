#include "PlayerbotGuildMgr.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "DatabaseEnv.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "ScriptMgr.h"
#include "World.h"
#include "RandomPlayerbotMgr.h"
#include "RaidSimulationMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include <algorithm>
#include <array>
#include <cmath>

void PlayerbotGuildMgr::Init()
{
    _guildCache.clear();
    if (sPlayerbotAIConfig.deleteRandomBotGuilds)
        DeleteBotGuilds();

    LoadGuildNames();
    ValidateGuildCache();
}

bool PlayerbotGuildMgr::CreateGuild(Player* player, std::string guildName)
{
    Guild* guild = new Guild();
    if (!guild->Create(player, guildName))
    {
        LOG_ERROR("playerbots", "Error creating guild [ {} ] with leader [ {} ]", guildName, player->GetName());
        delete guild;
        return false;
    }
    sGuildMgr->AddGuild(guild);

    LOG_DEBUG("playerbots", "Guild created: id={} name='{}'", guild->GetId(), guildName);
    SetGuildEmblem(guild->GetId());

    // Apply themed rank labels.
    GuildTheme const& th = GetThemeByName(guildName);
    if (th.valid)
    {
        auto it = _guildRankNames.find(th.slug);
        if (it != _guildRankNames.end())
        {
            for (auto const& [rid, rname] : it->second)
            {
                std::string escaped = rname;
                CharacterDatabase.EscapeString(escaped);
                CharacterDatabase.Execute(
                    "UPDATE guild_rank SET rname='{}' WHERE guildid={} AND rid={}",
                    escaped, guild->GetId(), uint32(rid));
            }
            LOG_DEBUG("playerbots", "Applied {} themed rank labels to guild '{}'", it->second.size(), guildName);
        }
    }

    GuildCache entry;
    entry.name        = guildName;
    entry.memberCount = 1;
    entry.status      = 1;
    entry.maxMembers  = th.valid ? th.targetSize : sPlayerbotAIConfig.randomBotGuildSizeMax;
    entry.faction     = player->GetTeamId();
    _guildCache[guild->GetId()] = entry;
    _foundedAt[guild->GetId()] = getMSTime();
    return true;
}

bool PlayerbotGuildMgr::IsFreshlyFounded(uint32 guildId) const
{
    auto it = _foundedAt.find(guildId);
    return it != _foundedAt.end()
        && GetMSTimeDiffToNow(it->second) < sPlayerbotAIConfig.guildLifecyclePeriod * IN_MILLISECONDS;
}

bool PlayerbotGuildMgr::SetGuildEmblem(uint32 guildId)
{
    Guild* guild = sGuildMgr->GetGuildById(guildId);
    if (!guild)
        return false;

    GuildTheme const& th = GetThemeByName(guild->GetName());
    bool themed = th.valid;

    auto pickOrRand = [](int16 themed, uint32 hi) -> uint32 {
        return (themed >= 0) ? uint32(themed) : urand(0, hi);
    };

    uint32 st = themed ? pickOrRand(th.tabardEmblemStyle, 180) : urand(0, 180);
    uint32 cl = themed ? pickOrRand(th.tabardEmblemColor, 17)  : urand(0, 17);
    uint32 br = themed ? pickOrRand(th.tabardBorderStyle, 7)   : urand(0, 7);
    uint32 bc = themed ? pickOrRand(th.tabardBorderColor, 17)  : urand(0, 17);
    uint32 bg = themed ? pickOrRand(th.tabardBgColor,     51)  : urand(0, 51);

    LOG_DEBUG("playerbots",
        "[TABARD] new guild id={} name='{}' themed={} -> style={}, color={}, borderStyle={}, borderColor={}, bgColor={}",
        guild->GetId(), guild->GetName(), themed ? "yes" : "no", st, cl, br, bc, bg);

    CharacterDatabase.Execute(
        "UPDATE guild SET EmblemStyle={}, EmblemColor={}, BorderStyle={}, BorderColor={}, BackgroundColor={} "
        "WHERE guildid={}",
        st, cl, br, bc, bg, guildId);

    if (QueryResult qr = CharacterDatabase.Query(
            "SELECT EmblemStyle,EmblemColor,BorderStyle,BorderColor,BackgroundColor FROM guild WHERE guildid={}",
            guildId))
    {
        Field* f = qr->Fetch();
        LOG_DEBUG("playerbots",
            "[TABARD] DB check guild id={} => style={}, color={}, borderStyle={}, borderColor={}, bgColor={}",
            guildId, f[0].Get<uint8>(), f[1].Get<uint8>(), f[2].Get<uint8>(), f[3].Get<uint8>(), f[4].Get<uint8>());
    }
    return true;
}

std::string PlayerbotGuildMgr::AssignToGuild(Player* player)
{
    if (!player)
        return "";

    uint8  botFaction = player->GetTeamId();
    uint8  botClass   = player->getClass();
    uint8  botRace    = player->getRace();
    uint8  botLevel   = player->GetLevel();
    uint32 botClassBit = 1u << (botClass - 1);
    uint32 botRaceBit  = 1u << (botRace  - 1);

    struct Candidate
    {
        std::string name;
        bool        isGhost;
        double      weight;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(_guildThemes.size());

    for (auto const& kv : _guildThemes)
    {
        std::string const& gname = kv.first;
        GuildTheme  const& th    = kv.second;
        if (!th.valid)
            continue;

        // Hard gates.
        if (th.faction != 2 && th.faction != botFaction)
            continue;
        if (botLevel < th.minLevel)
            continue;

        Guild* live      = sGuildMgr->GetGuildByName(gname);
        uint32 memberCnt = live ? live->GetMemberCount() : 0;
        bool   isGhost   = !live;

        if (live)
        {
            auto it = _guildCache.find(live->GetId());
            if (it != _guildCache.end() && it->second.hasRealPlayer)
                continue;
            if (memberCnt >= th.targetSize)
                continue;
        }

        // Match score.
        int score = 0;
        if (th.classMask != 0)
        {
            if (th.classMask & botClassBit)                  score += 2;
            else if (th.affinityClassMask & botClassBit)     score += 1;
        }
        if (th.raceMask != 0)
        {
            if (th.raceMask & botRaceBit)                    score += 1;
            else if (th.affinityRaceMask & botRaceBit)       score += 1;
        }

        // Weight.
        double T = std::max(0.01f, sPlayerbotAIConfig.themedGuildTemperature);
        double w = std::exp(double(score) / T);
        double headroom = th.targetSize > 0
            ? double(th.targetSize - memberCnt) / double(th.targetSize)
            : 1.0;
        w *= (0.5 + 0.5 * headroom);
        if (isGhost)
            w *= 1.5;

        candidates.push_back({gname, isGhost, w});
    }

    if (candidates.empty())
    {
        LOG_INFO("playerbots",
            "No eligible themed guild for bot {} (class={}, race={}, lvl={}, faction={}).",
            player->GetName(), botClass, botRace, botLevel, botFaction);
        return "";
    }

    double total = 0.0;
    for (auto const& c : candidates) total += c.weight;
    double r   = double(urand(0, 1000000)) / 1000000.0 * total;
    double cum = 0.0;
    for (auto const& c : candidates)
    {
        cum += c.weight;
        if (r <= cum)
        {
            LOG_INFO("playerbots",
                "Themed assign: bot={} (cls={} race={} lvl={}) -> '{}' [{}], weight={:.3f}/{:.3f}",
                player->GetName(), botClass, botRace, botLevel, c.name,
                c.isGhost ? "founder" : "join", c.weight, total);
            return c.name;
        }
    }
    return candidates.back().name;
}

void PlayerbotGuildMgr::OnGuildUpdate(Guild* guild)
{
    auto it = _guildCache.find(guild->GetId());
    if (it == _guildCache.end())
        return;

    GuildCache& entry = it->second;
    entry.memberCount = guild->GetMemberCount();

    uint16 cap = entry.maxMembers;
    auto themeIt = _guildThemes.find(entry.name);
    if (themeIt != _guildThemes.end() && themeIt->second.valid)
        cap = themeIt->second.targetSize;

    entry.status = (entry.memberCount < cap) ? 1 : 2;

    std::string guildName = guild->GetName();
    for (auto& nm : _guildNames)
    {
        if (nm.first == guildName)
        {
            nm.second = false;
            break;
        }
    }
}

void PlayerbotGuildMgr::ResetGuildCache()
{
    _guildCache.clear();

    for (auto& nameEntry : _guildNames)
        nameEntry.second = true;
}

void PlayerbotGuildMgr::LoadGuildNames()
{
    LOG_INFO("playerbots", "Loading themed guild names from playerbots_guild_names...");

    _guildThemes.clear();
    _guildRankNames.clear();
    _guildNames.clear();
    _shuffled_guild_keys.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT name, theme_slug, faction, class_mask, race_mask, "
        "       affinity_class_mask, affinity_race_mask, min_level, target_size, "
        "       tabard_emblem_style, tabard_emblem_color, tabard_border_style, "
        "       tabard_border_color, tabard_bg_color, raid_offset, max_band "
        "FROM playerbots_guild_names");

    if (!result)
    {
        LOG_ERROR("playerbots", "No entries found in playerbots_guild_names. List is empty.");
        return;
    }

    do
    {
        Field* f = result->Fetch();
        std::string name = f[0].Get<std::string>();

        GuildTheme t;
        t.slug              = f[1].Get<std::string>();
        t.faction           = f[2].Get<uint8>();
        t.classMask         = f[3].Get<uint32>();
        t.raceMask          = f[4].Get<uint32>();
        t.affinityClassMask = f[5].Get<uint32>();
        t.affinityRaceMask  = f[6].Get<uint32>();
        t.minLevel          = f[7].Get<uint8>();
        t.targetSize        = f[8].Get<uint16>();
        t.tabardEmblemStyle = f[9].IsNull()  ? int16(-1) : f[9].Get<int16>();
        t.tabardEmblemColor = f[10].IsNull() ? int16(-1) : f[10].Get<int16>();
        t.tabardBorderStyle = f[11].IsNull() ? int16(-1) : f[11].Get<int16>();
        t.tabardBorderColor = f[12].IsNull() ? int16(-1) : f[12].Get<int16>();
        t.tabardBgColor     = f[13].IsNull() ? int16(-1) : f[13].Get<int16>();
        t.raidOffset        = f[14].Get<int8>();
        t.maxBand           = f[15].Get<uint8>();
        t.valid             = !t.slug.empty();

        _guildThemes[name] = t;
        _guildNames[name]  = true;                // keep legacy map populated
        _shuffled_guild_keys.push_back(name);     // keep field populated (any order)
    } while (result->NextRow());

    LOG_INFO("playerbots", "Loaded {} themed guild entries.", _guildThemes.size());

    QueryResult rr = CharacterDatabase.Query(
        "SELECT theme_slug, rid, rname FROM playerbots_guild_rank_names ORDER BY theme_slug, rid");
    if (rr)
    {
        do
        {
            Field* f = rr->Fetch();
            std::string slug = f[0].Get<std::string>();
            uint8 rid        = f[1].Get<uint8>();
            std::string rn   = f[2].Get<std::string>();
            _guildRankNames[slug].emplace_back(rid, rn);
        } while (rr->NextRow());
        LOG_INFO("playerbots", "Loaded rank-name overrides for {} themes.", _guildRankNames.size());
    }
}

void PlayerbotGuildMgr::ValidateGuildCache()
{
    QueryResult result = CharacterDatabase.Query("SELECT guildid, name FROM guild");
    if (!result)
    {
        LOG_ERROR("playerbots", "No guilds found in database, resetting guild cache");
        ResetGuildCache();
        return;
    }

    std::unordered_map<uint32, std::string> dbGuilds;
    do
    {
        Field* fields = result->Fetch();
        uint32 guildId = fields[0].Get<uint32>();
        std::string guildName = fields[1].Get<std::string>();
        dbGuilds[guildId] = guildName;
    } while (result->NextRow());

    _guildCache.clear();
    for (auto it = dbGuilds.begin(); it != dbGuilds.end(); it++)
    {
        uint32 guildId = it->first;
        GuildCache cache;
        cache.name = it->second;
        cache.maxMembers = sPlayerbotAIConfig.randomBotGuildSizeMax;

        Guild* guild = sGuildMgr ->GetGuildById(guildId);
        if (!guild)
            continue;

        cache.memberCount = guild->GetMemberCount();
        ObjectGuid leaderGuid = guild->GetLeaderGUID();
        CharacterCacheEntry const* leaderEntry = sCharacterCache->GetCharacterCacheByGuid(leaderGuid);
        uint32 leaderAccount = leaderEntry->AccountId;
        cache.hasRealPlayer = !(sPlayerbotAIConfig.IsInRandomAccountList(leaderAccount));
        cache.faction = Player::TeamIdForRace(leaderEntry->Race);
        if (cache.memberCount == 0)
            cache.status = 0; // empty
        else if (cache.memberCount < cache.maxMembers)
            cache.status = 1; // partially filled
        else
            cache.status = 2; // full

        _guildCache.insert_or_assign(guildId, cache);
        for (auto& it : _guildNames)
        {
            if (it.first == cache.name)
            {
                it.second = false;
                break;
            }
        }
    }

    // Themed-guilds: warn about live guilds that don't map to any theme.
    for (auto const& kv : _guildCache)
    {
        if (_guildThemes.find(kv.second.name) == _guildThemes.end())
        {
            LOG_WARN("playerbots",
                "Guild '{}' (id={}) has no matching theme in playerbots_guild_names. "
                "It will be treated as neutral with no filters.",
                kv.second.name, kv.first);
        }
    }
}

void PlayerbotGuildMgr::DeleteBotGuilds()
{
    LOG_INFO("playerbots", "Deleting random bot guilds...");
    std::vector<uint32> randomBots;

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BOT);
    stmt->SetData(0, "add");
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bot = fields[0].Get<uint32>();
            randomBots.push_back(bot);
        } while (result->NextRow());
    }

    for (std::vector<uint32>::iterator i = randomBots.begin(); i != randomBots.end(); ++i)
    {
        if (Guild* guild = sGuildMgr->GetGuildByLeader(ObjectGuid::Create<HighGuid::Player>(*i)))
            guild->Disband();
    }
    LOG_INFO("playerbots", "Random bot guilds deleted");
}

bool PlayerbotGuildMgr::IsRealGuild(Player* bot)
{
    if (!bot)
        return false;
    uint32 guildId = bot->GetGuildId();
    if (!guildId)
        return false;

    return IsRealGuild(guildId);
}

bool PlayerbotGuildMgr::IsRealGuild(uint32 guildId)
{
    if (!guildId)
        return false;

    auto it = _guildCache.find(guildId);
    if (it == _guildCache.end())
        return false;

    return it->second.hasRealPlayer;
}

bool PlayerbotGuildMgr::IsBotManagedGuild(uint32 guildId) const
{
    if (!guildId)
        return false;

    QueryResult r = CharacterDatabase.Query(
        "SELECT guid FROM guild_member WHERE guildid = {}", guildId);
    if (!r)
        return false;  // no members => not a guild we manage

    do
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(r->Fetch()[0].Get<uint32>());
        CharacterCacheEntry const* e = sCharacterCache->GetCharacterCacheByGuid(guid);
        if (!e)
            return false;  // unknown member => be safe, don't manage
        if (!sPlayerbotAIConfig.IsInRandomAccountList(e->AccountId))
            return false;  // a real player is a member => hands off
    } while (r->NextRow());

    return true;
}

uint8 PlayerbotGuildMgr::GoalLevel(GuildTheme const& th) const
{
    if (th.raidOffset >= 0)
        return uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));  // raid guild: max level
    return th.minLevel;                                              // casual: its floor
}

std::vector<Player*> PlayerbotGuildMgr::CollectEligibleUnguilded(GuildTheme const& th) const
{
    std::vector<Player*> out;
    uint8 goal = GoalLevel(th);

    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot || !bot->IsInWorld())
            continue;
        if (bot->GetGuildId() != 0)
            continue;                                   // already guilded
        if (th.faction != 2 && th.faction != bot->GetTeamId())
            continue;                                   // faction hard gate
        if (bot->GetLevel() < goal)
            continue;                                   // goal-level hard gate
        out.push_back(bot);
    }

    std::sort(out.begin(), out.end(), [&](Player* a, Player* b) {
        return PickRankForBot(th, a) < PickRankForBot(th, b);  // GR_OFFICER(1) < ... best-fit first
    });
    return out;
}

void PlayerbotGuildMgr::ReconcileGuilds()
{
    if (!sPlayerbotAIConfig.guildLifecycleEnable)
        return;

    uint32 actions = 0;
    uint32 const maxActions = sPlayerbotAIConfig.guildLifecycleMaxActionsPerCycle;
    auto budget = [&]() { return actions < maxActions; };

    // --- Guild density (GuildLifecycle.BotsPerGuild): per-faction cap, founding gate, and
    // smallest-first disband. BotsPerGuild==0 is fully inert -- every statement in this block
    // and every densityOn-gated statement below is skipped, leaving existing behavior untouched. ---
    uint32 const botsPer  = sPlayerbotAIConfig.guildLifecycleBotsPerGuild;
    bool const densityOn  = botsPer > 0;
    uint32 capA = 0, capH = 0;
    std::array<uint32, 2> guildCount{0, 0};  // index by TeamId: [TEAM_ALLIANCE], [TEAM_HORDE]
    if (densityOn)
    {
        // Standing target, ramp-safe (never the instantaneous online count, which collapses
        // during the post-restart ramp and would mass-disband at every boot). No per-faction
        // budget accessor is exposed by PopulationDynamicsMgr (GetTargets() is a combined
        // both-factions per-level total; its per-faction Census is computed internally but not
        // exposed) -- split the global "bot_count" target evenly; the pool is faction-fair by
        // construction (AssignToGuild/CollectEligibleUnguilded both hard-gate on faction).
        uint32 const target = sRandomPlayerbotMgr.GetMaxAllowedBotCount();
        capA = std::max<uint32>(1, (target / 2) / botsPer);
        capH = std::max<uint32>(1, (target / 2) / botsPer);

        // Census: live bot-managed guilds per faction, from the cache ValidateGuildCache()
        // just refreshed. Real-player guilds are never IsBotManagedGuild (any real member
        // fails the scan), so they're never counted here.
        for (auto const& [guildId, cache] : _guildCache)
        {
            if (cache.faction >= 2)
                continue;
            if (!IsBotManagedGuild(guildId))
                continue;
            ++guildCount[cache.faction];
        }
    }
    auto capFor = [&](uint8 faction) -> uint32 { return faction == TEAM_ALLIANCE ? capA : capH; };

    // Starvation flag: first bot guild found this cycle with count < target_size and an empty
    // eligible-unguilded pool. Recorded during the per-theme backfill branch below, consumed by
    // the density disband picker after the loop.
    uint32 starvingGuildId = 0;
    uint8  starvingFaction = 2;
    bool   haveStarving    = false;

    for (auto const& kv : _guildThemes)
    {
        if (!budget())
            break;

        std::string const& name = kv.first;
        GuildTheme const&  th   = kv.second;
        if (!th.valid)
            continue;

        Guild* guild = sGuildMgr->GetGuildByName(name);

        // --- Ghost theme: gated founding. ---
        if (!guild)
        {
            std::vector<Player*> pool = CollectEligibleUnguilded(th);
            if (pool.size() < sPlayerbotAIConfig.guildLifecycleFoundQuorum)
                continue;  // self-gates raid guilds until the frontier yields enough max-level bots

            Player* leader = pool.front();
            uint8 const leaderFaction = uint8(leader->GetTeamId());
            if (densityOn && guildCount[leaderFaction] >= capFor(leaderFaction))
                continue;  // [GuildDensity] at cap - no new guilds

            if (!CreateGuild(leader, name))
                continue;
            ++actions;
            if (densityOn)
                ++guildCount[leaderFaction];
            Guild* born = sGuildMgr->GetGuildByName(name);
            if (!born)
                continue;
            for (size_t i = 1; i < pool.size() && born->GetMemberCount() < th.targetSize && budget(); ++i)
            {
                if (born->AddMember(pool[i]->GetGUID(), PickRankForBot(th, pool[i])))
                    ++actions;
            }
            OnGuildUpdate(born);
            LOG_INFO("playerbots", "GuildLifecycle: founded '{}' ({} members)",
                     name, born->GetMemberCount());
            continue;
        }

        // --- Live guild. Skip if a real player is a member, or it isn't bot-led. ---
        if (IsRealGuild(guild->GetId()) || !IsBotManagedGuild(guild->GetId()))
            continue;

        uint32 count = guild->GetMemberCount();

        // Disband withered guilds that cannot refill.
        std::vector<Player*> candidates = CollectEligibleUnguilded(th);
        if (count < sPlayerbotAIConfig.guildLifecycleDisbandFloor && candidates.empty())
        {
            LOG_INFO("playerbots", "GuildLifecycle: disbanded '{}' (<{} members, no refill)",
                     name, sPlayerbotAIConfig.guildLifecycleDisbandFloor);
            uint32 const disbandedId = guild->GetId();
            guild->Disband();
            ++actions;
            if (densityOn)
            {
                auto cit = _guildCache.find(disbandedId);
                if (cit != _guildCache.end() && cit->second.faction < 2 && guildCount[cit->second.faction] > 0)
                    --guildCount[cit->second.faction];  // keep the census live for later themes this cycle
            }
            continue;
        }

        // Starvation flag: first under-target guild this cycle with a dry recruiting pool.
        if (densityOn && !haveStarving && count < th.targetSize && candidates.empty())
        {
            auto cit = _guildCache.find(guild->GetId());
            if (cit != _guildCache.end() && cit->second.faction < 2)
            {
                starvingGuildId = guild->GetId();
                starvingFaction = cit->second.faction;
                haveStarving    = true;
            }
        }

        // Backfill toward target_size.
        size_t ci = 0;
        while (count < th.targetSize && ci < candidates.size() && budget())
        {
            Player* recruit = candidates[ci++];
            if (guild->AddMember(recruit->GetGUID(), PickRankForBot(th, recruit)))
            {
                ++count;
                ++actions;
            }
        }
        OnGuildUpdate(guild);
        if (ci > 0)
            LOG_INFO("playerbots", "GuildLifecycle: recruited {} into '{}'", ci, name);

        // Re-rank online members (idempotent), never touching the GM seat.
        ObjectGuid leaderGuid = guild->GetLeaderGUID();
        for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            if (!bot || !bot->IsInWorld() || bot->GetGuildId() != guild->GetId())
                continue;
            if (bot->GetGUID() == leaderGuid)
                continue;
            uint8 want = PickRankForBot(th, bot);
            Guild::Member const* m = guild->GetMember(bot->GetGUID());
            if (m && m->GetRankId() != want && want != GR_GUILDMASTER)
                guild->ChangeMemberRank(bot->GetGUID(), want);
        }

        // Leader safety net: if the leader GUID no longer resolves to a member, remove it so the
        // core (Guild::DeleteMember) promotes a successor or disbands.
        if (leaderGuid && !guild->GetMember(leaderGuid))
            guild->DeleteMember(leaderGuid, false, false, true);
    }

    // --- Density disband: at most one, realm-wide, per cycle. "cap" (over the per-faction
    // cap) takes priority over "starve" (an under-target guild with a dry recruiting pool). ---
    bool densityActed = false;
    if (densityOn && budget())
    {
        uint8 pickedFaction = 2;
        std::string reason;

        if (guildCount[TEAM_ALLIANCE] > capA)
        {
            pickedFaction = TEAM_ALLIANCE;
            reason = "cap";
        }
        else if (guildCount[TEAM_HORDE] > capH)
        {
            pickedFaction = TEAM_HORDE;
            reason = "cap";
        }
        else if (haveStarving && guildCount[starvingFaction] > 1)
        {
            pickedFaction = starvingFaction;
            reason = "starve";
        }

        if (pickedFaction < 2)
        {
            // Eligibility: bot-managed, no active RaidSim run, not freshly founded, and (for
            // "starve") not the starving guild itself. "not the faction's last bot guild" is
            // implied: cap-overflow means guildCount>cap>=1 (so >=2 exist), and starve is
            // explicitly gated on guildCount>1 above.
            Guild* victim = nullptr;
            uint32 victimMembers = 0;
            bool   haveVictim = false;
            for (auto const& [guildId, cache] : _guildCache)
            {
                if (cache.faction != pickedFaction)
                    continue;
                if (reason == "starve" && guildId == starvingGuildId)
                    continue;
                if (!IsBotManagedGuild(guildId))
                    continue;
                if (RaidSimulationMgr::instance().HasActiveRun(guildId))
                    continue;
                if (IsFreshlyFounded(guildId))
                    continue;

                Guild* g = sGuildMgr->GetGuildById(guildId);
                if (!g)
                    continue;

                uint32 mc = g->GetMemberCount();
                if (!haveVictim || mc < victimMembers)
                {
                    victim        = g;
                    victimMembers = mc;
                    haveVictim    = true;
                }
            }

            if (victim)
            {
                uint32 const victimId = victim->GetId();
                LOG_INFO("playerbots",
                    "[GuildDensity] faction={} guilds={} cap={} -> disband '{}' ({} members, reason={})",
                    pickedFaction == TEAM_ALLIANCE ? "A" : "H",
                    guildCount[pickedFaction], capFor(pickedFaction),
                    victim->GetName(), victim->GetMemberCount(), reason);
                victim->Disband();
                CharacterDatabase.Execute("DELETE FROM custom_reagent_bank WHERE owner_guid = {}", victimId);
                ++actions;
                densityActed = true;
            }
        }
    }

    if (densityOn && !densityActed)
    {
        LOG_INFO("playerbots",
            "[GuildDensity] A: guilds={}/cap={} H: guilds={}/cap={} (no action)",
            guildCount[TEAM_ALLIANCE], capA, guildCount[TEAM_HORDE], capH);
    }
}

class BotGuildCacheWorldScript : public WorldScript
{
    public:

        BotGuildCacheWorldScript() : WorldScript("BotGuildCacheWorldScript"), _validateTimer(0){}

        void OnUpdate(uint32 diff) override
        {
            _validateTimer += diff;
            uint32 periodMs = sPlayerbotAIConfig.guildLifecyclePeriod * IN_MILLISECONDS;
            if (periodMs == 0)
                periodMs = 300 * IN_MILLISECONDS;
            if (_validateTimer >= periodMs)
            {
                _validateTimer = 0;
                PlayerbotGuildMgr::instance().ValidateGuildCache();
                PlayerbotGuildMgr::instance().ReconcileGuilds();
                LOG_DEBUG("playerbots", "GuildLifecycle: reconcile pass complete");
            }
        }

    private:
        uint32 _validateTimer;
};

void PlayerBotsGuildValidationScript()
{
    new BotGuildCacheWorldScript();
}

GuildTheme const& PlayerbotGuildMgr::GetThemeByName(std::string const& guildName) const
{
    static const GuildTheme empty;
    auto it = _guildThemes.find(guildName);
    return (it != _guildThemes.end()) ? it->second : empty;
}

uint8 PlayerbotGuildMgr::PickRankForBot(GuildTheme const& th, Player* bot) const
{
    if (!th.valid || !bot)
        return GR_INITIATE;

    // Neutral guild (no filters): everyone is a Member.
    if (th.classMask == 0 && th.raceMask == 0)
        return GR_MEMBER;

    uint32 classBit = 1u << (bot->getClass() - 1);
    uint32 raceBit  = 1u << (bot->getRace()  - 1);

    int score = 0;
    if (th.classMask != 0)
    {
        if (th.classMask & classBit)                  score += 2;
        else if (th.affinityClassMask & classBit)     score += 1;
    }
    if (th.raceMask != 0)
    {
        if (th.raceMask & raceBit)                    score += 1;
        else if (th.affinityRaceMask & raceBit)       score += 1;
    }

    if (score >= 3) return GR_OFFICER;
    if (score == 2) return GR_VETERAN;
    if (score == 1) return GR_MEMBER;
    return GR_INITIATE;
}
