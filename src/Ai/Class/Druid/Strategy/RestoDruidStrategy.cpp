/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "RestoDruidStrategy.h"

#include "Playerbots.h"

RestoDruidStrategy::RestoDruidStrategy(PlayerbotAI* botAI) : GenericDruidStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

void RestoDruidStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericDruidStrategy::InitTriggers(triggers);

    triggers.push_back(new TriggerNode("no healer dps strategy",
                                       { NextAction("tree form", 5.0f) }));

    triggers.push_back(new TriggerNode(
        "party member to heal out of spell range",
        { NextAction("reach party member to heal", 39.0f) }));

    triggers.push_back(
        new TriggerNode("party member critical health",
                        {
                            NextAction("tree form",              34.1f),
                            NextAction("swiftmend on party",     34.0f),
                            NextAction("wild growth on party",   33.0f),
                            NextAction("nourish on party",       32.0f),
                            NextAction("regrowth on party",      31.0f),
                            NextAction("healing touch on party", 30.0f),
                        }));

    triggers.push_back(
        new TriggerNode("party member critical health",
                        { NextAction("nature's swiftness", 58.0f) }));

    triggers.push_back(new TriggerNode(
        "nature's swiftness active",
        { NextAction("healing touch on party", 55.0f) }));

    triggers.push_back(new TriggerNode("clearcasting",
        { NextAction("lifebloom on main tank", 13.0f) }));

    // LOW
    triggers.push_back(
        new TriggerNode("party member low health",
                        {
                            NextAction("tree form",              21.5f),
                            NextAction("swiftmend on party",     21.4f),
                            NextAction("wild growth on party",   21.3f),
                            NextAction("nourish on party",       21.2f),
                            NextAction("regrowth on party",      21.1f),
                            NextAction("healing touch on party", 21.0f),
                        }));

    // MEDIUM
    triggers.push_back(
        new TriggerNode("party member medium health",
                        {
                            NextAction("tree form",              20.5f),
                            NextAction("swiftmend on party",     20.4f),
                            NextAction("wild growth on party",   20.3f),
                            NextAction("nourish on party",       20.2f),
                            NextAction("regrowth on party",      20.1f),
                            NextAction("healing touch on party", 20.0f),
                        }));

    // ALMOST FULL
    triggers.push_back(
        new TriggerNode("party member almost full health",
                        {
                            NextAction("wild growth on party",  10.3f),
                            NextAction("rejuvenation on party", 10.2f),
                            NextAction("regrowth on party",     10.1f),
                        }));

    triggers.push_back(
        new TriggerNode("medium mana", { NextAction("innervate", 25.0f) }));

    triggers.push_back(new TriggerNode("enemy too close for spell",
                                       { NextAction("flee", 39.0f) }));
}

void DruidTranquilityStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("medium group heal setting",
                                       { NextAction("tree form", 30.6f), NextAction("tranquility", 30.5f) }));
}

void DruidBlanketStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Proactive HoT maintenance must NOT force Tree of Life. Rejuvenation/Wild Growth cast fine in
    // caster form, so demanding tree here fought the "healer dps" strategy's "cancel tree form" every
    // time a blanket HoT expired (BuffOnPartyTrigger fires on the missing buff regardless of health),
    // shifting a nothing-to-heal druid tree<->caster endlessly. The big heals (party critical/low/medium
    // health) still demand tree form, so the bot shapeshifts only for real healing, not upkeep.
    triggers.push_back(new TriggerNode(
        "wild growth blanket",
        { NextAction("wild growth blanket", 8.0f) }));

    triggers.push_back(new TriggerNode(
        "rejuvenation blanket",
        { NextAction("rejuvenation blanket", 6.0f) }));
}
