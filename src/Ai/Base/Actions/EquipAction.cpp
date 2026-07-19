/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "EquipAction.h"
#include <utility>

#include "Event.h"
#include "ItemCountValue.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"
#include "ItemPackets.h"

bool EquipAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemIds ids = chat->parseItems(text);
    EquipItems(ids);
    return true;
}

void EquipAction::EquipItems(ItemIds ids, bool disposeDisplaced)
{
    for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
    {
        FindItemByIdVisitor visitor(*i);
        EquipItem(&visitor, disposeDisplaced);
    }
}

// Return bagslot with smalest bag.
uint8 EquipAction::GetSmallestBagSlot()
{
    int8 curBag = 0;
    uint32 curSlots = 0;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        const Bag* const pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (pBag)
        {
            if (curBag > 0 && curSlots < pBag->GetBagSize())
                continue;

            curBag = bag;
            curSlots = pBag->GetBagSize();
        }
        else
            return bag;
    }

    return curBag;
}

void EquipAction::EquipItem(FindItemVisitor* visitor, bool disposeDisplaced)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    if (!items.empty())
        EquipItem(*items.begin(), disposeDisplaced);
}

void EquipAction::EquipItem(Item* item, bool disposeDisplaced)
{
    uint8 bagIndex = item->GetBagSlot();
    uint8 slot = item->GetSlot();
    const ItemTemplate* itemProto = item->GetTemplate();
    uint32 itemId = itemProto->ItemId;
    uint8 invType = itemProto->InventoryType;

    // Handle ammunition separately
    if (invType == INVTYPE_AMMO)
    {
        bot->SetAmmo(itemId);
        std::ostringstream out;
        out << "equipping " << chat->FormatItem(itemProto);
        botAI->TellMaster(out);
        return;
    }

    // Handle bags: swap the loose bag into a bag slot. Only when the swap can actually
    // succeed — an empty bag slot, or a slot holding an EMPTY bag. The core rejects moving
    // a NON-EMPTY equipped bag out to a regular inventory slot (EQUIP_ERR_ITEMS_CANT_BE_
    // SWAPPED), and since the item-usage gate keeps re-selecting a bigger loose bag every
    // tick, a blind swap spams an endless "Equipping [bag]" / "Cannot swap these items"
    // retry loop (e.g. a 26-slot Traveler's Backpack against four full 24-slot bags).
    // Containers are handled here in full — never fall through to the gear-equip path.
    if (itemProto->Class == ITEM_CLASS_CONTAINER)
    {
        uint8 newBagSlot = GetSmallestBagSlot();
        if (newBagSlot > 0)
        {
            Bag* dstBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, newBagSlot);
            if (!dstBag || dstBag->IsEmpty())
            {
                uint16 src = ((bagIndex << 8) | slot);
                uint16 dst = ((INVENTORY_SLOT_BAG_0 << 8) | newBagSlot);
                bot->SwapItem(src, dst);

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto);
                botAI->TellMaster(out);
            }
        }
        return;
    }

    // Equip as gear
    {
        // Ranged weapons aren't handled by the rest of the weapon equip logic
        // Handle them early here to avoid issues.
        if (invType == INVTYPE_RANGED || invType == INVTYPE_THROWN || invType == INVTYPE_RANGEDRIGHT)
        {
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << uint8(EQUIPMENT_SLOT_RANGED);

            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

            std::ostringstream out;
            out << "Equipping " << chat->FormatItem(itemProto) << " in ranged slot";
            botAI->TellMaster(out);
            return;
        }

        uint8 dstSlot = botAI->FindEquipSlot(itemProto, NULL_SLOT, true);

        // Check if the item is a weapon and whether the bot can dual wield or use Titan Grip
        bool isWeapon = (itemProto->Class == ITEM_CLASS_WEAPON);
        bool canTitanGrip = bot->CanTitanGrip();
        bool canDualWield = bot->CanDualWield();

        bool isTwoHander = (invType == INVTYPE_2HWEAPON);
        bool isValidTGWeapon = false;
        if (canTitanGrip && isTwoHander)
        {
            // Titan Grip-valid 2H weapon subclasses: Axe2, Mace2, Sword2
            isValidTGWeapon = (itemProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
        }

        // Check if the main hand currently has a 2H weapon equipped
        Item* currentMHItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool have2HWeaponEquipped = (currentMHItem && currentMHItem->GetTemplate()->InventoryType == INVTYPE_2HWEAPON);

        // bool canDualWieldOrTG = (canDualWield || (canTitanGrip && isTwoHander));
        bool canDualWieldOrTG = (canDualWield || isTwoHander);

        // If this is a weapon and we can dual wield or Titan Grip, check if we can improve main/off-hand setup
        if (isWeapon && canDualWieldOrTG)
        {
            // Fetch current main hand and offhand items
            Item* mainHandItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item* offHandItem  = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

            // Set up the stats calculator once and reuse results for performance
            StatsWeightCalculator calculator(bot);
            calculator.SetItemSetBonus(false);
            calculator.SetOverflowPenalty(false);

            // Calculate item scores once and store them
            float newItemScore = calculator.CalculateItem(itemId, item->GetItemRandomPropertyId());
            float mainHandScore = mainHandItem
                ? calculator.CalculateItem(mainHandItem->GetTemplate()->ItemId, mainHandItem->GetItemRandomPropertyId()) : 0.0f;
            float offHandScore = offHandItem
                ? calculator.CalculateItem(offHandItem->GetTemplate()->ItemId, offHandItem->GetItemRandomPropertyId()) : 0.0f;

            // Determine where this weapon can go
            bool canGoMain = (invType == INVTYPE_WEAPON ||
                              invType == INVTYPE_WEAPONMAINHAND ||
                              isTwoHander);

            bool canTGOff = false;
            if (canTitanGrip && isTwoHander && isValidTGWeapon)
                canTGOff = true;

            bool canGoOff = (invType == INVTYPE_WEAPON ||
                             invType == INVTYPE_WEAPONOFFHAND ||
                             canTGOff);

            // Check if the main hand item can go to offhand if needed
            bool mainHandCanGoOff = false;
            if (mainHandItem)
            {
                const ItemTemplate* mhProto = mainHandItem->GetTemplate();
                bool mhIsValidTG = false;
                if (canTitanGrip && mhProto->InventoryType == INVTYPE_2HWEAPON)
                {
                    mhIsValidTG = (mhProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
                }

                mainHandCanGoOff = (mhProto->InventoryType == INVTYPE_WEAPON ||
                                    mhProto->InventoryType == INVTYPE_WEAPONOFFHAND ||
                                    (mhProto->InventoryType == INVTYPE_2HWEAPON && mhIsValidTG));
            }

            // Priority 1: Replace main hand if the new weapon is strictly better
            // and if conditions allow (e.g. no conflicting 2H logic)
            bool betterThanMH = (newItemScore > mainHandScore);
            // If a one-handed weapon is better, we can still use it instead of a two-handed weapon
            bool mhConditionOK = (invType != INVTYPE_2HWEAPON ||
                      (isTwoHander && !canTitanGrip) ||
                      (canTitanGrip && isValidTGWeapon));

            if (canGoMain && betterThanMH && mhConditionOK)
            {
                // Equip new weapon in main hand
                {
                    WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid newItemGuid = item->GetGUID();
                    eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_MAINHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
                }

                // Try moving old main hand weapon to offhand if beneficial
                if (mainHandItem && mainHandCanGoOff && (!offHandItem || mainHandScore > offHandScore))
                {
                    const ItemTemplate* oldMHProto = mainHandItem->GetTemplate();

                    WorldPacket offhandPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid oldMHGuid = mainHandItem->GetGUID();
                    offhandPacket << oldMHGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(offhandPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

                    std::ostringstream moveMsg;
                    moveMsg << "Main hand upgrade found. Moving " << chat->FormatItem(oldMHProto) << " to offhand";
                    botAI->TellMaster(moveMsg);
                }

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in main hand";
                botAI->TellMaster(out);
                return;
            }

            // Priority 2: If not better than main hand, check if better than offhand
            else if (canGoOff && newItemScore > offHandScore)
            {
                // Equip in offhand
                WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                ObjectGuid newItemGuid = item->GetGUID();
                eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                nicePacket.Read();
                bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in offhand";
                botAI->TellMaster(out);
                return;
            }
            else
            {
                // No improvement, do nothing
                return;
            }
        }

        // Caster combo path: a 1H main-hand candidate while a 2H is worn and the bot
        // cannot dual-wield. The dual-wield/TG weapon block above is skipped for this
        // case, and the single-slot guard below would compare the 1H alone vs the 2H
        // (which the 2H wins), so a caster never assembles a better 1H + off-hand.
        // Re-check the loadout (same helper + threshold as the gate), then equip the
        // 1H (the core auto-moves the 2H to bags, freeing the off-hand slot) and the
        // best owned off-hand. Non-destructive, so it runs on the player .equip path
        // too (fixing today's silent no-op where a 1H won't replace a 2H for a caster).
        if (sPlayerbotAIConfig.casterWeaponComboEval && isWeapon && !canDualWield &&
            have2HWeaponEquipped && currentMHItem &&
            (invType == INVTYPE_WEAPON || invType == INVTYPE_WEAPONMAINHAND))
        {
            StatsWeightCalculator calc(bot);
            calc.SetItemSetBonus(false);
            calc.SetOverflowPenalty(false);

            bool isPvp = sRandomPlayerbotMgr.IsSpecPvp(bot->GetGUID().GetCounter(), bot->getClass());
            if (isPvp)
                calc.SetPvpSpec(true);

            Item* bestOff = ItemUsageValue::FindBestUsableOffHand(bot, calc);

            float newLoadout = calc.CalculateItem(itemId, item->GetItemRandomPropertyId());
            if (bestOff)
                newLoadout += calc.CalculateItem(bestOff->GetTemplate()->ItemId,
                    bestOff->GetItemRandomPropertyId());
            float const curLoadout = calc.CalculateItem(currentMHItem->GetTemplate()->ItemId,
                currentMHItem->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));

            if (newLoadout <= curLoadout * sPlayerbotAIConfig.equipUpgradeThreshold)
                return;  // pair does not beat the 2H by the margin — keep the 2H

            // Equip the 1H in main hand (core moves the displaced 2H to bags).
            {
                WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                ObjectGuid newItemGuid = item->GetGUID();
                eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_MAINHAND);
                WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                nicePacket.Read();
                bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
            }

            // Equip the best owned off-hand into the now-free off-hand slot.
            if (bestOff && !bot->IsTwoHandUsed())
            {
                WorldPacket offPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                ObjectGuid offGuid = bestOff->GetGUID();
                offPacket << offGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                WorldPackets::Item::AutoEquipItemSlot niceOff(std::move(offPacket));
                niceOff.Read();
                bot->GetSession()->HandleAutoEquipItemSlotOpcode(niceOff);
            }

            std::ostringstream out;
            out << "Equipping " << chat->FormatItem(itemProto) << " in main hand with off-hand";
            botAI->TellMaster(out);
            return;
        }

        // If not a special dual-wield/TG scenario or no improvement found, fall back to original logic
        if (dstSlot == EQUIPMENT_SLOT_FINGER1 ||
            dstSlot == EQUIPMENT_SLOT_TRINKET1 ||
            (dstSlot == EQUIPMENT_SLOT_MAINHAND && canDualWield &&
                ((invType != INVTYPE_2HWEAPON && !have2HWeaponEquipped) || (canTitanGrip && isValidTGWeapon))))
        {
            // Handle ring/trinket dual-slot logic
            Item* const equippedItems[2] = {
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot),
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot + 1)
            };

            if (equippedItems[0])
            {
                if (equippedItems[1])
                {
                    // Both slots are full - pick the worst item to replace, but only if new item is better
                    StatsWeightCalculator calc(bot);
                    calc.SetItemSetBonus(false);
                    calc.SetOverflowPenalty(false);

                    // Calculate new item score with random properties
                    int32 newItemRandomProp = item->GetItemRandomPropertyId();
                    float newItemScore = calc.CalculateItem(itemId, newItemRandomProp);

                    // Calculate equipped items scores with random properties
                    int32 firstRandomProp = equippedItems[0]->GetItemRandomPropertyId();
                    int32 secondRandomProp = equippedItems[1]->GetItemRandomPropertyId();
                    float firstItemScore = calc.CalculateItem(equippedItems[0]->GetTemplate()->ItemId, firstRandomProp);
                    float secondItemScore = calc.CalculateItem(equippedItems[1]->GetTemplate()->ItemId, secondRandomProp);

                    // Determine which slot (if any) should be replaced
                    bool betterThanFirst = newItemScore > firstItemScore;
                    bool betterThanSecond = newItemScore > secondItemScore;

                    // Early return if new item is not better than either equipped item
                    if (!betterThanFirst && !betterThanSecond)
                        return;

                    if (betterThanFirst && betterThanSecond)
                    {
                        // New item is better than both - replace the worse of the two equipped items
                        if (firstItemScore > secondItemScore)
                            dstSlot++; // Replace second slot (worse)
                        // else: keep dstSlot as-is (replace first slot)
                    }
                    else if (betterThanSecond)
                        dstSlot++; // Only better than second slot - replace it
                }
                else
                {
                    // Second slot empty, use it
                    dstSlot++;
                }
            }
        }

        // A two-hander in the main hand blocks the off-hand slot. Refuse rather
        // than fire an equip the core rejects every time (EQUIP_ERR_CANT_EQUIP_
        // WITH_TWOHANDED). The main-hand replacement paths above already returned,
        // so this only guards the fall-back off-hand equip (shield/holdable).
        if (dstSlot == EQUIPMENT_SLOT_OFFHAND && bot->IsTwoHandUsed())
            return;

        // Single-slot guard: only equip if this item beats whatever currently occupies the target
        // slot, re-scored HERE. The ring/trinket/main-hand paths above already picked the slot after
        // their own compare; every other (single) slot fell straight through and would equip
        // UNCONDITIONALLY. Within one equip-upgrade pass the slot may have just been filled by a
        // better item from the same candidate set, so without this two same-slot items (feet/chest/
        // head/...) ping-pong forever. Uses the same threshold as the gate (ItemUsageValue). Empty
        // slot => no occupant => equip.
        if (Item* occupant = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot))
        {
            StatsWeightCalculator cmp(bot);
            cmp.SetItemSetBonus(false);
            cmp.SetOverflowPenalty(false);
            float const newScore = cmp.CalculateItem(itemId, item->GetItemRandomPropertyId());
            float const curScore = cmp.CalculateItem(occupant->GetTemplate()->ItemId,
                occupant->GetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID));
            if (newScore <= curScore * sPlayerbotAIConfig.equipUpgradeThreshold)
                return;
        }

        // Capture the current occupant BEFORE the equip. HandleAutoEquipItemSlotOpcode
        // swaps: the worn item lands back in bags, where the next upgrade pass could
        // re-grab it (same-slot churn). We resolve it by GUID after the swap.
        ObjectGuid displacedGuid;
        if (Item* occupant = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot))
            displacedGuid = occupant->GetGUID();

        // Equip the item in the chosen slot
        {
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << dstSlot;
            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
        }

        // Best-effort top-up deposit of the displaced item; if it doesn't match an existing bank
        // stack, LEAVE IT IN THE BAG — never destroy here. Destroy is a last-resort event owned by
        // the bag-full graduated disposal (SmartDestroyItemAction: deposit -> disenchant -> sell ->
        // destroy-cheapest, gated at 90% full). Crucially the next upkeep's DepositEpicsToGuildBank
        // banks any tradeable, unwanted epic to ANY tab (FindAnyDepositTab, bypassing this top-up-only
        // path), so a displaced epic reaches the guild bank on its own — the old `else DestroyItem`
        // here nuked valuable displaced gear before that could happen. Anti-churn no longer needs the
        // immediate eviction: the single-slot scoring guard above (same StatsWeightCalculator) blocks
        // a lower-scored displaced piece from bouncing back. Autonomous upgrade path only; never a
        // weapon (a caster's displaced 1H must survive); never an item still equipped (failed equip).
        if (disposeDisplaced && displacedGuid)
        {
            if (Item* displaced = bot->GetItemByGuid(displacedGuid))
            {
                if (!displaced->IsEquipped() &&
                    displaced->GetTemplate()->Class != ITEM_CLASS_WEAPON)
                {
                    botAI->TryDepositLootToGuildBank(displaced);   // top-up if it fits; else keep in bag
                }
            }
        }
    }

    std::ostringstream out;
    out << "Equipping " << chat->FormatItem(itemProto);
    botAI->TellMaster(out);
}

ItemIds EquipAction::SelectInventoryItemsToEquip()
{
    CollectItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    ItemIds items;
    for (auto i = visitor.items.begin(); i != visitor.items.end(); ++i)
    {
        Item* item = *i;
        if (!item)
            continue;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!itemTemplate)
            continue;

        //TODO Expand to Glyphs and Gems, that can be placed in equipment
        //Pre-filter non-equipable items
        if (itemTemplate->InventoryType == INVTYPE_NON_EQUIP)
            continue;

        int32 randomProperty = item->GetItemRandomPropertyId();
        uint32 itemId = item->GetTemplate()->ItemId;
        std::string itemUsageParam;
        if (randomProperty != 0)
            itemUsageParam = std::to_string(itemId) + "," + std::to_string(randomProperty);
        else
            itemUsageParam = std::to_string(itemId);

        ItemUsage usage = AI_VALUE2(ItemUsage, "item upgrade", itemUsageParam);
        // Only feed genuine upgrades to the equipper. ITEM_USAGE_BAD_EQUIP means the gate scored
        // this item as NOT better than what's already equipped (an empty slot yields EQUIP, not
        // BAD_EQUIP), so auto-equipping it only displaces a better piece — and with the single-slot
        // equip below being unconditional, two same-slot items would ping-pong forever.
        // Skip a candidate the bot cannot uniquely-equip right now (e.g. a spare copy of an
        // already-worn ITEM_FLAG_UNIQUE_EQUIPPABLE ring/trinket). The gate scores it as an
        // upgrade over the OTHER slot, but the core rejects the 2nd unique every cycle, so
        // feeding it produces an endless "Equipping [X]" retry loop. Use the same core check
        // the equip packet later fails on so gate and outcome agree; a different better unique
        // (other entry / limit-category) still passes.
        if (bot->CanEquipUniqueItem(item) != EQUIP_ERR_OK)
            continue;

        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
            items.insert(itemId);
    }
    return items;
}

bool EquipUpgradesPacketAction::Execute(Event event)
{
    if (!sPlayerbotAIConfig.autoEquipUpgradeLoot && !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;
    std::string const source = event.GetSource();
    if (source == "trade status")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        uint32 status;
        p >> status;

        if (status != TRADE_STATUS_TRADE_ACCEPT)
            return false;
    }

    else if (source == "item push result")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        ObjectGuid playerGuid;
        uint32 received, created, sendChatMessage, itemSlot, itemId;
        uint8 bagSlot;

        p >> playerGuid;
        p >> received;
        p >> created;
        p >> sendChatMessage;
        p >> bagSlot;
        p >> itemSlot;
        p >> itemId;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
        if (item->InventoryType == INVTYPE_NON_EQUIP)
            return false;
    }

    ItemIds items = SelectInventoryItemsToEquip();
    EquipItems(items, sPlayerbotAIConfig.disposeDisplacedUpgradeGear);
    return true;
}

bool EquipUpgradeAction::Execute(Event /*event*/)
{
    ItemIds items = SelectInventoryItemsToEquip();
    EquipItems(items, sPlayerbotAIConfig.disposeDisplacedUpgradeGear);
    return true;
}
