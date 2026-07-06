/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "CurrencyGearIndex.h"
#include "DBCStores.h"      // sItemExtendedCostStore
#include "DBCStructure.h"   // ItemExtendedCostEntry, MAX_ITEM_EXTENDED_COST_REQUIREMENTS
#include "DatabaseEnv.h"    // WorldDatabase
#include "QueryResult.h"    // ResultSet (full def for Fetch/NextRow)
#include "Field.h"          // Field::Get
#include "ItemTemplate.h"   // ITEM_CLASS_*
#include "ObjectMgr.h"      // sObjectMgr->GetItemTemplate
#include "Log.h"

std::vector<CurrencyGearIndex::GearOption> const CurrencyGearIndex::_emptyGear;
std::vector<CurrencyGearIndex::SinkOption> const CurrencyGearIndex::_emptySink;

void CurrencyGearIndex::Build()
{
    if (_built)
        return;
    _built = true;

    // 1) Map extendedCostId -> [gear item ids sold at that cost], via npc_vendor.
    std::unordered_map<uint32, std::vector<uint32>> extToItems;
    if (QueryResult r = WorldDatabase.Query("SELECT item, ExtendedCost FROM npc_vendor WHERE ExtendedCost > 0"))
    {
        do
        {
            Field* f = r->Fetch();
            uint32 item = f[0].Get<uint32>();
            uint32 ext  = f[1].Get<uint32>();
            extToItems[ext].push_back(item);
        } while (r->NextRow());
    }

    // Classify one leaf item under one currency (currencyItemId) at the given cost.
    auto classify = [&](uint32 currency, uint32 leaf, uint32 cost, uint32 extId)
    {
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(leaf);
        if (!p)
            return;

        bool const equippable = p->InventoryType != 0;
        if (equippable)
        {
            _gear[currency].push_back({leaf, cost, extId, 0});
        }
        else if (p->Class == ITEM_CLASS_GEM || p->Class == ITEM_CLASS_TRADE_GOODS || p->Class == ITEM_CLASS_RECIPE)
        {
            // Sink: gems / crafting mats / patterns purchasable with this currency.
            // Repeatable-only filter applied in Task 2 refinement.
            _sink[currency].push_back({leaf, cost});
        }
        else if (p->Class == ITEM_CLASS_MONEY)
        {
            // Class 10 = currency token exchange (e.g. emblem -> emblem).
            _convert[currency] = leaf;
        }
        // else: unique mounts, commendation badges, tier tokens — handled by two-hop (Task 2) or excluded.
    };

    // 2) Walk ItemExtendedCost DBC: each entry E requires reqitem[s] at reqitemcount[s];
    //    lookup the vendor items sold at that extended cost.
    for (uint32 id = 0; id < sItemExtendedCostStore.GetNumRows(); ++id)
    {
        ItemExtendedCostEntry const* e = sItemExtendedCostStore.LookupEntry(id);
        if (!e)
            continue;

        auto it = extToItems.find(e->ID);
        if (it == extToItems.end())
            continue;

        for (uint8 s = 0; s < MAX_ITEM_EXTENDED_COST_REQUIREMENTS; ++s)
        {
            uint32 req = e->reqitem[s];
            uint32 cnt = e->reqitemcount[s];
            if (!req || !cnt)
                continue;

            for (uint32 leaf : it->second)
                classify(req, leaf, cnt, e->ID);
        }
    }

    // Two-hop token expansion + sink refinement + threshold population are added in Task 2.
    LOG_INFO("playerbots", "[CurrencyGearIndex] one-hop built: {} currencies with gear, {} with sink",
             uint32(_gear.size()), uint32(_sink.size()));
}

std::vector<CurrencyGearIndex::GearOption> const& CurrencyGearIndex::GearFor(uint32 c) const
{
    auto it = _gear.find(c);
    return it == _gear.end() ? _emptyGear : it->second;
}

std::vector<CurrencyGearIndex::SinkOption> const& CurrencyGearIndex::SinkFor(uint32 c) const
{
    auto it = _sink.find(c);
    return it == _sink.end() ? _emptySink : it->second;
}

uint32 CurrencyGearIndex::ConvertTargetFor(uint32 c) const
{
    auto it = _convert.find(c);
    return it == _convert.end() ? 0 : it->second;
}

uint32 CurrencyGearIndex::ThresholdFor(uint32 c) const
{
    auto it = _threshold.find(c);
    return it == _threshold.end() ? 0 : it->second;
}

void CurrencyGearIndex::DebugDump() const
{
    // Known WotLK emblem/token currency item ids for golden-value verification.
    static uint32 const EMB[] = {29434, 40752, 40753, 45624, 47241, 49426};
    for (uint32 c : EMB)
    {
        LOG_INFO("playerbots", "[CGIdx dump] currency {} gear={} sink={} convert={} T={}",
                 c, uint32(GearFor(c).size()), uint32(SinkFor(c).size()),
                 ConvertTargetFor(c), ThresholdFor(c));
    }
    // Tier token spot-check (e.g. Vanquisher's Mark of Sanctification = 45644).
    LOG_INFO("playerbots", "[CGIdx dump] token 45644 gear={}", uint32(GearFor(45644).size()));
}
