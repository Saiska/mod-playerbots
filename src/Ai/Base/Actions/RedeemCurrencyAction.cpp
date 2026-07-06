#include "RedeemCurrencyAction.h"
#include "CurrencyGearIndex.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PlayerbotOperation.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "Playerbots.h"
#include "StatsWeightCalculator.h"

// Mirrors the inline helper in PlayerbotFactory.cpp:2958. World-thread only (mutates inventory).
static Item* StoreNewItemInInventorySlot(Player* player, uint32 newItemId, uint32 count)
{
    ItemPosCountVec vDest;
    if (player->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, vDest, newItemId, count) != EQUIP_ERR_OK)
        return nullptr;
    return player->StoreNewItem(vDest, newItemId, true, Item::GenerateItemRandomPropertyId(newItemId));
}

// Verify affordability then consume every RequiredItem of an ExtendedCost row from the bot's bags.
// Returns false (consuming nothing) if unaffordable. World-thread only.
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

// Synthesize the gear item and equip it if it slots. World-thread only.
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

// One upkeep redemption pass, executed on the WORLD THREAD.
//
// Player inventory work (GetItemCount / DestroyItemCount / StoreNewItem / EquipItem) is NOT safe on a
// MapUpdater worker thread: the bot's bags are mutated concurrently by the world thread (GearFloor fills,
// bot randomization during the login ramp, etc.), so a worker-side bag walk can deref a freed Item* ->
// C0000005 (live crash 2026-07-06, symbolized to RedeemCurrencyAction::Execute -> Player::GetItemCount).
// PlayerbotWorldThreadProcessor runs this operation on the world thread, where all core inventory mutation
// happens, so the reads/writes are serialized and safe. Mirrors the existing group/guild/RaidSim operations.
class RedeemCurrencyOperation : public PlayerbotOperation
{
public:
    explicit RedeemCurrencyOperation(ObjectGuid botGuid) : m_botGuid(botGuid) {}

    ObjectGuid GetBotGuid() const override { return m_botGuid; }
    std::string GetName() const override { return "RedeemCurrency"; }
    uint32 GetPriority() const override { return 10; }   // normal background op

    bool Execute() override
    {
        Player* bot = ObjectAccessor::FindPlayer(m_botGuid);
        if (!bot)
            return false;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            return false;

        // Only walk inventory when the bot is fully valid and stable. During the login ramp bots are being
        // loaded / randomized / rotated out (RandomizeFirst, logout); a mid-setup or mid-teardown inventory
        // holds a freed Item* that GetItemCount would deref -> C0000005 (live crash 2026-07-06, reproduced).
        // Standard bot-safety guard used across the codebase (e.g. PlayerbotAI.cpp:540).
        if (!bot->GetSession() || !bot->IsInWorld() || bot->IsBeingTeleported() || bot->IsDuringRemoveFromWorld())
            return false;

        static uint32 const CURR[] = {49426, 47241, 45624, 40753, 40752, 29434}; // high->low
        uint32 const cap = sPlayerbotAIConfig.tokenRedeemMaxBuysPerUpkeep;
        uint32 buys = 0;
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

            while (buys < cap)
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
                    // Verify a slot can receive the gear BEFORE paying — PayCost destroys the currency, so
                    // (like the sink/convert paths below) never pay unless the item can actually be stored.
                    ItemPosCountVec gdest;
                    bool const canStore = bot->CanStoreNewItem(INVENTORY_SLOT_BAG_0, NULL_SLOT, gdest, best.gearId, 1) == EQUIP_ERR_OK;
                    if (canStore && PayCost(bot, best.extendedCostId))
                    {
                        if (GrantAndEquip(bot, best.gearId))
                        {
                            ++buys; acted = true;
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
                        ++buys; acted = true;
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

        // record post-pass balances for the cadence guard on the next upkeep call
        for (uint32 c : CURR)
            botAI->tokenRedeemLastBalance[c] = bot->GetItemCount(c, false);

        return acted;
    }

private:
    ObjectGuid m_botGuid;
};

bool RedeemCurrencyAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.tokenRedeemEnable)
        return false;

    // This action runs on a MapUpdater worker thread; defer the actual redemption (which reads and mutates
    // the bot's inventory) to the world thread, where core inventory mutation is safe. See RedeemCurrencyOperation.
    PlayerbotWorldThreadProcessor::instance().QueueOperation(std::make_unique<RedeemCurrencyOperation>(bot->GetGUID()));
    return true;
}
