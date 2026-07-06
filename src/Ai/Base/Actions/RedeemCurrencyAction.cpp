#include "RedeemCurrencyAction.h"
#include "CurrencyGearIndex.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"

// Mirrors the inline helper in PlayerbotFactory.cpp:2958.
static Item* StoreNewItemInInventorySlot(Player* player, uint32 newItemId, uint32 count)
{
    ItemPosCountVec vDest;
    if (player->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, vDest, newItemId, count) != EQUIP_ERR_OK)
        return nullptr;
    return player->StoreNewItem(vDest, newItemId, true, Item::GenerateItemRandomPropertyId(newItemId));
}

bool RedeemCurrencyAction::PayCost(uint32 extendedCostId)
{
    ItemExtendedCostEntry const* e = sItemExtendedCostStore.LookupEntry(extendedCostId);
    if (!e)
        return false;
    for (uint8 s = 0; s < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++s)   // affordability
        if (e->reqitem[s] && e->reqitemcount[s] &&
            bot->GetItemCount(e->reqitem[s], false) < e->reqitemcount[s])
            return false;
    for (uint8 s = 0; s < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++s)   // consume
        if (e->reqitem[s] && e->reqitemcount[s])
            bot->DestroyItemCount(e->reqitem[s], e->reqitemcount[s], true);
    return true;
}

bool RedeemCurrencyAction::GrantAndEquip(uint32 gearId)
{
    Item* item = StoreNewItemInInventorySlot(bot, gearId, 1);
    if (!item)
        return false;
    uint16 dest;
    if (bot->CanEquipItem(NULL_SLOT, dest, item, true, true) == EQUIP_ERR_OK)
        bot->EquipItem(dest, item, true);   // equip immediately if it slots
    return true;
}

bool RedeemCurrencyAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.tokenRedeemEnable)
        return false;

    static uint32 const CURR[] = {49426, 47241, 45624, 40753, 40752, 29434}; // high->low
    uint32 const cap = sPlayerbotAIConfig.tokenRedeemMaxBuysPerUpkeep;
    bool acted = false;

    for (uint32 c : CURR)
    {
        uint32 bal = bot->GetItemCount(c, false);

        // cadence guard: skip if balance has not risen since the last pass
        auto it = botAI->tokenRedeemLastBalance.find(c);
        if (it != botAI->tokenRedeemLastBalance.end() && bal <= it->second)
            continue;

        uint32 T = sCurrencyGearIndex.ThresholdFor(c);
        if (T == 0 || bal < T)    // dormant below threshold
            continue;

        while (_buys < cap)
        {
            bal = bot->GetItemCount(c, false);

            // 1) best affordable genuine upgrade
            CurrencyGearIndex::GearOption best{}; float bestScore = 0.0f; bool haveGear = false;
            for (auto const& o : sCurrencyGearIndex.GearFor(c))
            {
                if (o.cost > bal) continue;
                ItemTemplate const* p = sObjectMgr->GetItemTemplate(o.gearId);
                if (!p || bot->BotCanUseItem(p) != EQUIP_ERR_OK) continue;
                ItemUsage u = botAI->GetAiObjectContext()->GetValue<ItemUsage>("item upgrade", std::to_string(o.gearId))->Get();
                if (u != ITEM_USAGE_EQUIP && u != ITEM_USAGE_REPLACE) continue;
                StatsWeightCalculator calc(bot); calc.SetItemSetBonus(false); calc.SetOverflowPenalty(false);
                float sc = calc.CalculateItem(o.gearId, 0);
                if (!haveGear || sc > bestScore) { best = o; bestScore = sc; haveGear = true; }
            }
            if (haveGear && PayCost(best.extendedCostId))
            {
                if (GrantAndEquip(best.gearId))
                {
                    ++_buys; acted = true;
                    LOG_INFO("playerbots", "Bots redeem: {} bought+equipped {} for {}x {}", bot->GetName(), best.gearId, best.cost, c);
                    continue;   // only loop again on a real, stored upgrade
                }
                break;   // paid but the gear couldn't be stored (bags full) — stop, don't re-drain currency
            }

            // 2) no upgrade -> random affordable sink (gems/mats -> reagent vault)
            std::vector<CurrencyGearIndex::SinkOption> aff;
            for (auto const& s : sCurrencyGearIndex.SinkFor(c))
                if (s.cost <= bal) aff.push_back(s);
            if (!aff.empty())
            {
                CurrencyGearIndex::SinkOption s = aff[urand(0, aff.size() - 1)];
                ItemPosCountVec sdest;
                if (bot->GetItemCount(c, false) >= s.cost &&
                    bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, sdest, s.itemId, 1) == EQUIP_ERR_OK)
                {
                    bot->DestroyItemCount(c, s.cost, true);            // room verified above; no currency lost
                    StoreNewItemInInventorySlot(bot, s.itemId, 1);
                    ++_buys; acted = true;
                    continue;
                }
            }

            // 3) no gear, no sink -> convert then let the target currency drain next pass
            uint32 tgt = sCurrencyGearIndex.ConvertTargetFor(c);
            if (tgt && bal > 0)
            {
                ItemPosCountVec cdest;
                if (bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, cdest, tgt, bal) == EQUIP_ERR_OK)
                {
                    bot->DestroyItemCount(c, bal, true);         // room verified; no currency lost
                    StoreNewItemInInventorySlot(bot, tgt, bal);  // 1:1 conversion
                    acted = true;
                }
            }
            break;   // this currency done for the pass
        }
    }

    // record post-pass balances for cadence guard on the next upkeep call
    for (uint32 c : CURR)
        botAI->tokenRedeemLastBalance[c] = bot->GetItemCount(c, false);

    _buys = 0;   // reset per upkeep invocation
    return acted;
}
