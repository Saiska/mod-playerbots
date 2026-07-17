/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DestroyItemAction.h"

#include "DatabaseEnv.h"
#include "Event.h"
#include "ItemCountValue.h"
#include "ItemUsageValue.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Playerbots.h"

bool DestroyItemAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemIds ids = chat->parseItems(text);

    for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
    {
        FindItemByIdVisitor visitor(*i);
        DestroyItem(&visitor);
    }

    return true;
}

void DestroyItemAction::DestroyItem(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    for (Item* item : items)
    {
        if (botAI->TryDepositLootToGuildBank(item))
            continue;

        std::ostringstream out;
        out << chat->FormatItem(item->GetTemplate()) << " destroyed";
        botAI->TellMaster(out);

        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    }
}

// Run for masterless bots AND bots grouped with a real player. Grouped bots were previously
// blocked (!HasActivePlayerMaster()), which disabled the whole 90% safety valve while grouped
// and orphaned the gentle real-player-master branch below as dead code -> grouped bots filled
// up and spammed "My inventory is full" with no disposal. The internal branches already do the
// right thing per case: real-player-master + real guild -> deposit surplus to guild bank /
// reagent vault then destroy greys only; everyone else -> cheapest-first flush. The bagSpace<90
// early-return keeps this a no-op until actually near-full.
bool SmartDestroyItemAction::isUseful() { return true; }

bool SmartDestroyItemAction::Execute(Event /*event*/)
{
    // Try to offload surplus to the guild bank BEFORE destroying anything (destroy = last resort).
    botAI->DepositSurplusToGuildBank();

    uint8 bagSpace = AI_VALUE(uint8, "bag space");

    if (bagSpace < 90)
        return false;

    // Disenchant-before-destroy: turn non-epic junk gear the bot won't equip into enchanting
    // materials (-> reagent vault) instead of destroying it, preserving value. Skill-free and
    // ungated on the player-master, so it also runs for a bot GROUPED with a real player — the
    // case the old "disenchant random item" branch skipped (it required !HasActivePlayerMaster()
    // AND the Enchanting skill AND cast spell 13262, so a grouped bot never disenchanted and its
    // bags jammed with un-offloadable gear -> "My inventory is full" spam). See DisenchantSurplusGear().
    if (sPlayerbotAIConfig.disenchantBeforeDestroy && !bot->IsInCombat() && DisenchantSurplusGear())
    {
        bagSpace = AI_VALUE(uint8, "bag space");
        if (bagSpace < 90)
            return true;
    }

    // only destoy grey items if with real player/guild
    if (botAI->HasRealPlayerMaster() && botAI->IsInRealGuild())
    {
        std::set<Item*> items;
        FindItemsToTradeByQualityVisitor visitor(ITEM_QUALITY_POOR, 5);
        IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);
        items.insert(visitor.GetResult().begin(), visitor.GetResult().end());

        for (auto& item : items)
        {
            FindItemByIdVisitor visitor(item->GetTemplate()->ItemId);
            DestroyItem(&visitor);

            bagSpace = AI_VALUE(uint8, "bag space");

            if (bagSpace < 90)
                return true;
        }
        return true;
    }

    std::vector<uint32> bestToDestroy = {ITEM_USAGE_NONE};  // First destroy anything useless.

    if (!AI_VALUE(bool, "can sell") &&
        AI_VALUE(
            bool,
            "should get money"))  // We need money so quest items are less important since they can't directly be sold.
        bestToDestroy.push_back(ITEM_USAGE_QUEST);
    else  // We don't need money so destroy the cheapest stuff.
    {
        bestToDestroy.push_back(ITEM_USAGE_VENDOR);
        bestToDestroy.push_back(ITEM_USAGE_AH);
    }

    // If we still need room
    bestToDestroy.push_back(
        ITEM_USAGE_SKILL);  // Items that might help tradeskill are more important than above but still expenable.
    bestToDestroy.push_back(ITEM_USAGE_USE);  // These are more likely to be usefull 'soon' but still expenable.

    for (auto& usage : bestToDestroy)
    {
        std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(usage));
        std::reverse(items.begin(), items.end());

        for (auto& item : items)
        {
            FindItemByIdVisitor visitor(item->GetTemplate()->ItemId);
            DestroyItem(&visitor);

            bagSpace = AI_VALUE(uint8, "bag space");

            if (bagSpace < 90)
                return true;
        }
    }

    return false;
}

// Skill-free disenchant of non-epic junk gear (any bot: no Enchanting skill, no spell cast). A bot
// grouped with a real player never runs the maintenance-strategy disenchant, and its mob-loot gear
// has no offload channel (BoP can't enter a guild bank; world-drops don't match the top-up-only bank;
// no vendor nearby while following) -> bags jam and it spams "My inventory is full". This rolls each
// item's disenchant loot and banks the materials STRAIGHT INTO THE REAGENT VAULT (uncapped, DB-backed
// custom_reagent_bank -- the same store the reagent deposit path uses), then destroys the source gear.
// Value is preserved as enchanting materials AND every disenchant nets a freed bag slot. Routing the
// materials through the bags instead (AutoStoreLoot) would swap gear for shards 1:1 and never free
// space while the bags are already full -- which is exactly the stuck state this fixes.
//
// The vault is guild-scoped, so this needs a real guild; masterless / non-guild bots fall through to
// the destroy/flush branches. Only gear the bot would NOT equip is eligible (we walk the
// NONE/AH/VENDOR/DISENCHANT usage buckets, so EQUIP/REPLACE/BAD_EQUIP upgrade candidates are never
// touched), capped at Disenchant.MaxQuality so epics are kept. Two-phase (snapshot ObjectGuids, then
// act) because DestroyItem invalidates Item* pointers -- re-fetching by GUID keeps the walk safe.
bool SmartDestroyItemAction::DisenchantSurplusGear()
{
    uint32 const guildId = bot->GetGuildId();
    if (!guildId || !botAI->IsInRealGuild())
        return false;

    std::vector<ObjectGuid> targets;
    for (uint32 usage : {(uint32)ITEM_USAGE_DISENCHANT, (uint32)ITEM_USAGE_AH,
                         (uint32)ITEM_USAGE_VENDOR, (uint32)ITEM_USAGE_NONE})
    {
        std::vector<Item*> items = AI_VALUE2(std::vector<Item*>, "inventory items", "usage " + std::to_string(usage));
        for (Item* item : items)
        {
            if (!item)
                continue;

            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON))
                continue;

            // No disenchant loot (jewellery, cloaks, tabards, off-hand holdables, ...) or epic+ -> keep.
            if (proto->DisenchantID == 0)
                continue;
            if (proto->Quality < ITEM_QUALITY_UNCOMMON || proto->Quality > sPlayerbotAIConfig.disenchantMaxQuality)
                continue;

            targets.push_back(item->GetGUID());
        }
    }

    uint32 count = 0;
    for (ObjectGuid const& guid : targets)
    {
        Item* item = bot->GetItemByGuid(guid);
        if (!item)
            continue;

        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->DisenchantID == 0)
            continue;

        // Roll this item's disenchant loot and deposit each rolled material straight into the guild
        // reagent vault (mirrors PlayerbotAI::TryDepositLootToGuildBank's vault INSERT). No bag round-trip.
        Loot loot;
        loot.FillLoot(proto->DisenchantID, LootTemplates_Disenchant, bot, true, true);
        for (LootItem const& li : loot.items)
        {
            if (!li.itemid || li.count == 0)
                continue;

            ItemTemplate const* mat = sObjectMgr->GetItemTemplate(li.itemid);
            if (!mat)
                continue;

            uint32 const subclass = (mat->Class == ITEM_CLASS_GEM) ? ITEM_SUBCLASS_JEWELCRAFTING : mat->SubClass;
            CharacterDatabase.Execute(
                "INSERT INTO custom_reagent_bank (owner_guid, item_entry, item_subclass, amount) "
                "VALUES ({}, {}, {}, {}) ON DUPLICATE KEY UPDATE amount = amount + VALUES(amount)",
                guildId, li.itemid, subclass, uint32(li.count));
        }
        loot.clear();

        uint8 const bag = item->GetBagSlot();
        uint8 const slot = item->GetSlot();
        bot->DestroyItem(bag, slot, true);
        ++count;

        // No early-out on bag space here: disenchanting to the vault is non-destructive (materials
        // are preserved), and stopping the moment bags dip below the fill threshold leaves only a
        // few free slots that an actively-grinding grouped bot refills in seconds -> "My inventory
        // is full" spam resumes. Clear ALL eligible junk gear in one pass so there's a real buffer.
    }

    if (count)
    {
        LOG_INFO("playerbots", "Bots disenchant-to-vault: {} disenchanted {} surplus item(s) (guild {})",
                 bot->GetName(), count, guildId);
        std::ostringstream out;
        out << "Disenchanted " << count << " surplus item(s)";
        botAI->TellMaster(out);
    }

    return count > 0;
}
