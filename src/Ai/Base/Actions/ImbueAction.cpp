/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ImbueAction.h"

#include "Event.h"
#include "Playerbots.h"

ImbueWithPoisonAction::ImbueWithPoisonAction(PlayerbotAI* botAI) : Action(botAI, "apply poison") {}

bool ImbueWithPoisonAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat())
        return false;

    // remove stealth
    if (bot->HasAura(SPELL_AURA_MOD_STEALTH))
        bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    static const std::vector<uint32_t> prioritizedInstantPoisons = {
        INSTANT_POISON_IX, INSTANT_POISON_VIII, INSTANT_POISON_VII, INSTANT_POISON_VI, INSTANT_POISON_V, INSTANT_POISON_IV,
        INSTANT_POISON_III, INSTANT_POISON_II, INSTANT_POISON
    };

    static const std::vector<uint32_t> prioritizedDeadlyPoisons = {
        DEADLY_POISON_IX, DEADLY_POISON_VIII, DEADLY_POISON_VII, DEADLY_POISON_VI, DEADLY_POISON_V, DEADLY_POISON_IV,
        DEADLY_POISON_III, DEADLY_POISON_II, DEADLY_POISON
    };

    // Check if we have any deadly or instant poisons
    Item* deadlyPoison = nullptr;
    for (auto id : prioritizedDeadlyPoisons)
    {
        deadlyPoison = botAI->FindConsumable(id);
        if (deadlyPoison) break;
    }

    Item* instantPoison = nullptr;
    for (auto id : prioritizedInstantPoisons)
    {
        instantPoison = botAI->FindConsumable(id);
        if (instantPoison) break;
    }

    // Mainhand
    Item* poison = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        if (instantPoison && deadlyPoison)
        {
            poison = instantPoison;
        }
        else if (deadlyPoison)
        {
            poison = deadlyPoison;
        }
        else if (instantPoison)
        {
            poison = instantPoison;
        }

        if (poison)
        {
            botAI->ImbueItem(poison, EQUIPMENT_SLOT_MAINHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    // Offhand
    poison = nullptr;
    weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        if (deadlyPoison && instantPoison)
        {
            poison = deadlyPoison;
        }
        else if (instantPoison)
        {
            poison = instantPoison;
        }
        else if (deadlyPoison)
        {
            poison = deadlyPoison;
        }

        if (poison)
        {
            botAI->ImbueItem(poison, EQUIPMENT_SLOT_OFFHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    return true;
}

// Search and apply stone to weapons
ImbueWithStoneAction::ImbueWithStoneAction(PlayerbotAI* botAI) : Action(botAI, "apply stone") {}

bool ImbueWithStoneAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat())
        return false;

    // remove stealth
    if (bot->HasAura(SPELL_AURA_MOD_STEALTH))
        bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

    // hp check
    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // Search and apply stone to weapons
    // Mainhand ...
    Item* stone = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        stone = botAI->FindStoneFor(weapon);
        if (stone)
        {
            botAI->ImbueItem(stone, EQUIPMENT_SLOT_MAINHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    //... and offhand
    weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        stone = botAI->FindStoneFor(weapon);
        if (stone)
        {
            botAI->ImbueItem(stone, EQUIPMENT_SLOT_OFFHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    return true;
}

// Search and apply oil to weapons
ImbueWithOilAction::ImbueWithOilAction(PlayerbotAI* botAI) : Action(botAI, "apply oil") {}

bool ImbueWithOilAction::Execute(Event /*event*/)
{
    if (bot->IsInCombat())
        return false;

    // remove stealth
    if (bot->HasAura(SPELL_AURA_MOD_STEALTH))
        bot->RemoveAurasByType(SPELL_AURA_MOD_STEALTH);

    // hp check
    if (bot->getStandState() != UNIT_STAND_STATE_STAND)
        bot->SetStandState(UNIT_STAND_STATE_STAND);

    // Search and apply oil to weapons
    Item* oil = nullptr;
    Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == 0)
    {
        oil = botAI->FindOilFor(weapon);
        if (oil)
        {
            botAI->ImbueItem(oil, EQUIPMENT_SLOT_MAINHAND);
            botAI->SetNextCheckDelay(5);
        }
    }

    return true;
}

static const uint32 uPrioritizedHealingItemIds[19] = {
    HEALTHSTONE,
    FEL_REGENERATION_POTION,
    SUPER_HEALING_POTION,
    CRYSTAL_HEALING_POTION,
    MAJOR_DREAMLESS_SLEEP_POTION,
    VOLATILE_HEALING_POTION,
    MAJOR_HEALING_POTION,
    WHIPPER_ROOT_TUBER,
    NIGHT_DRAGON_BREATH,
    LIMITED_INVULNERABILITY_POTION,
    GREATER_DREAMLESS_SLEEP_POTION,
    SUPERIOR_HEALING_POTION,
    CRYSTAL_RESTORE,
    DREAMLESS_SLEEP_POTION,
    GREATER_HEALING_POTION,
    HEALING_POTION,
    LESSER_HEALING_POTION,
    DISCOLORED_HEALING_POTION,
    MINOR_HEALING_POTION,
};

TryEmergencyAction::TryEmergencyAction(PlayerbotAI* botAI) : Action(botAI, "try emergency") {}

bool TryEmergencyAction::Execute(Event /*event*/)
{
    // Do not use consumable if bot can heal self
    if ((botAI->IsHeal(bot)) && (bot->GetPowerPct(POWER_MANA) > 20))
        return false;

    // If bot does not have aggro: use bandage instead of potion/stone/crystal
    if ((!AI_VALUE(uint8, "my attacker count")) && !bot->HasAura(11196))  // Recently bandaged
    {
        if (Item* bandage = botAI->FindBandage())
        {
            botAI->ImbueItem(bandage, bot);
            botAI->SetNextCheckDelay(8);
            return true;            // bandaged — do not also chug potions
        }
    }

    // Else loop over the list of health consumable to pick one. Only USE (and report success)
    // for an item whose use-spell is actually OFF cooldown. Combat potions share a ~1 min,
    // one-per-combat category cooldown (Player::_AddSpellCooldown stamps every potion in the
    // category), so after the first one FindConsumable still returns the item but the server
    // silently drops the use. Returning true on that no-op would, at ACTION_EMERGENCY priority,
    // break the engine's action loop (Engine.cpp:219) and starve the whole combat rotation — the
    // bot freezes at low HP until passive regen clears the "low health" trigger. So skip
    // on-cooldown items and return false when nothing was usable, letting the rotation run.
    for (uint8 i = 0; i < std::size(uPrioritizedHealingItemIds); ++i)
    {
        Item* healthItem = botAI->FindConsumable(uPrioritizedHealingItemIds[i]);
        if (!healthItem)
            continue;

        uint32 spellId = 0;
        for (uint8 s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
        {
            if (healthItem->GetTemplate()->Spells[s].SpellId > 0)
            {
                spellId = healthItem->GetTemplate()->Spells[s].SpellId;
                break;
            }
        }
        if (!spellId || bot->HasSpellCooldown(spellId))
            continue;              // on cooldown / no use-spell — keep looking, do not claim success

        botAI->ImbueItem(healthItem);
        return true;               // used ONE usable consumable
    }

    return false;                  // nothing usable — let the combat rotation run instead of freezing
}
