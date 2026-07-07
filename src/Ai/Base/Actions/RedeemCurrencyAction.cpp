#include "RedeemCurrencyAction.h"
#include "CurrencyGearIndex.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
#include <atomic>

// Mirrors the inline helper in PlayerbotFactory.cpp:2958.
static Item* StoreNewItemInInventorySlot(Player* player, uint32 newItemId, uint32 count)
{
    ItemPosCountVec vDest;
    if (player->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, vDest, newItemId, count) != EQUIP_ERR_OK)
        return nullptr;
    return player->StoreNewItem(vDest, newItemId, true, Item::GenerateItemRandomPropertyId(newItemId));
}

// Verify affordability then consume every RequiredItem of an ExtendedCost row from the bot's bags.
// Returns false (consuming nothing) if unaffordable.
static bool PayCost(Player* bot, uint32 extendedCostId)
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

// Synthesize the gear item and equip it if it slots.
static bool GrantAndEquip(Player* bot, uint32 gearId)
{
    Item* item = StoreNewItemInInventorySlot(bot, gearId, 1);
    if (!item)
        return false;
    uint16 dest;
    if (bot->CanEquipItem(NULL_SLOT, dest, item, true, true) == EQUIP_ERR_OK)
        bot->EquipItem(dest, item, true);   // equip immediately if it slots
    return true;
}

// Runs INLINE on the bot's own update tick (its map-worker context), so the redemption's inventory reads and
// mutations are serialized with the bot's other inventory ops on that same thread. It must NOT be deferred to
// the world thread (that raced the bot's worker) — see the 2026-07-06 crash arc.
//
// Two stand-downs protect against walking a bot whose inventory is being rebuilt elsewhere (freed Item* ->
// C0000005): (1) IsBotInitializing() — the post-boot login ramp, when the whole fleet is mass-RandomizeFirst'd;
// (2) the per-bot readiness guard — a bot mid-load / mid-teardown / being randomized.
bool RedeemCurrencyAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.tokenRedeemEnable)
        return false;

    // Stand down while the fleet is still loading/randomizing after boot (login ramp): inventories are being
    // torn down + rebuilt en masse then, and walking one derefs a freed Item*. The reliably-reproduced crash.
    if (sRandomPlayerbotMgr.IsBotInitializing())
        return false;

    // Only walk inventory when this bot is in a fully valid, stable state (not loading/teleporting/removing).
    if (!bot->GetSession() || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld())
        return false;

    static uint32 const CURR[] = {49426, 47241, 45624, 40753, 40752, 29434}; // high->low
    bool acted = false;

    // Bound the inventory churn per Execute. The buy loop below mutates the bags (Destroy/Store/Equip) and
    // re-walks every bag slot via GetItemCount each iteration; an UNBOUNDED pass over a large currency pile
    // reopened a freed-Item* crash in Bag::GetItemCount (2026-07-07, redeem_unbounded_loop_crash.md). Cap the
    // acting buys per tick — a large pile still fully drains, just over several upkeep passes (the
    // maintenance / 90%-full pipeline empties bags to the reagent vault between passes).
    uint32 const maxBuys = sPlayerbotAIConfig.tokenRedeemMaxBuysPerTick;
    uint32 buys = 0;

    // [RedeemProbe] confirm Execute is actually reached fleet-wide (rules out "action never dispatched").
    static std::atomic<uint32> s_execCalls{0};
    uint32 const probeN = ++s_execCalls;
    if (probeN % 500 == 1)
        LOG_INFO("playerbots", "[RedeemProbe] Execute reached (call #{}) by {}", probeN, bot->GetName());

    for (uint32 c : CURR)
    {
        if (buys >= maxBuys)   // per-tick churn budget spent; remaining currencies wait for the next pass
            break;

        uint32 bal = bot->GetItemCount(c, false);

        // [RedeemProbe] watch the two frozen currencies (Badge of Justice, Emblem of Triumph).
        bool const probeWatch = (c == 29434 || c == 47241);

        uint32 T = sCurrencyGearIndex.ThresholdFor(c);
        if (T == 0 || bal < T)    // dormant below threshold
        {
            if (probeWatch && bal > 0)
                LOG_INFO("playerbots", "[RedeemProbe] {} c={} bal={} T={} -> DORMANT (T=0 or bal<T, no drain)",
                         bot->GetName(), c, bal, T);
            continue;
        }

        uint32 const probeEnterBal = bal;   // [RedeemProbe] to measure how much this pass drained
        char const* probeBranch = "none";   // [RedeemProbe] last branch that acted this pass

        if (probeWatch)
            LOG_INFO("playerbots",
                     "[RedeemProbe] {} c={} ENTER bal={} T={} sink={} convert={} -> loop",
                     bot->GetName(), c, bal, T, uint32(sCurrencyGearIndex.SinkFor(c).size()),
                     sCurrencyGearIndex.ConvertTargetFor(c));

        while (buys < maxBuys)
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
            if (haveGear)
            {
                // Verify a slot can receive the gear BEFORE paying — PayCost destroys the currency.
                ItemPosCountVec gdest;
                bool const canStore = bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, gdest, best.gearId, 1) == EQUIP_ERR_OK;
                if (canStore && PayCost(bot, best.extendedCostId))
                {
                    if (GrantAndEquip(bot, best.gearId))
                    {
                        acted = true;
                        ++buys;
                        probeBranch = "gear";
                        LOG_INFO("playerbots", "Bots redeem: {} bought+equipped {} for {}x {}", bot->GetName(), best.gearId, best.cost, c);
                        continue;   // only loop again on a real, stored upgrade
                    }
                    break;   // room was verified; a store failure here is not expected — stop, don't re-drain
                }
                // no room, or PayCost failed (missing a non-currency reqitem) -> fall through to the sink
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
                    acted = true;
                    ++buys;
                    probeBranch = "sink";
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
                    probeBranch = "convert";
                }
            }
            break;   // this currency done for the pass
        }

        // [RedeemProbe] per-pass outcome for the two frozen currencies: which branch drained, and how much.
        if (probeWatch)
        {
            uint32 const probeEndBal = bot->GetItemCount(c, false);
            LOG_INFO("playerbots", "[RedeemProbe] {} c={} DONE branch={} drained={} ({}->{})",
                     bot->GetName(), c, probeBranch, probeEnterBal - probeEndBal, probeEnterBal, probeEndBal);
        }
    }

    return acted;
}
