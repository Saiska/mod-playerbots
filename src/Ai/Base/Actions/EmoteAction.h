/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_EMOTEACTION_H
#define _PLAYERBOT_EMOTEACTION_H

#include <map>

#include "Action.h"
#include "NamedObjectContext.h"

class Player;
class PlayerbotAI;
class Unit;

enum TextEmotes : uint32;

class EmoteActionBase : public Action
{
public:
    EmoteActionBase(PlayerbotAI* botAI, std::string const name);

    static uint32 GetNumberOfEmoteVariants(TextEmotes emote, uint8 race, uint8 gender);

protected:
    bool Emote(Unit* target, uint32 type, bool textEmote = false);
    bool ReceiveEmote(Player* source, uint32 emote, bool verbal = false);
    Unit* GetTarget();
    void InitEmotes();
    // Ambient talk/emote is only allowed while the bot is in a stationary/social RPG state
    // (upkeep, rest, pastime). During active play (questing, gathering, grinding, travel, or
    // grouped with a real player) it stays silent — this keeps the "talk" facing-spline from
    // interrupting gather casts and stops the constant emote spam. See EmoteStrategy.
    bool AllowsSocialEmote();
    static std::map<std::string, uint32> emotes;
    static std::map<std::string, uint32> textEmotes;
};

class EmoteAction : public EmoteActionBase, public Qualified
{
public:
    EmoteAction(PlayerbotAI* botAI);

    bool Execute(Event event) override;
    bool isUseful() override;
};

class TalkAction : public EmoteActionBase
{
public:
    TalkAction(PlayerbotAI* botAI) : EmoteActionBase(botAI, "talk") {}

    bool Execute(Event event) override;
    bool isUseful() override;
    static uint32 GetRandomEmote(Unit* unit, bool textEmote = false);
};

#endif
