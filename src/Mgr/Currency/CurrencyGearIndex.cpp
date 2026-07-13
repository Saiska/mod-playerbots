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
#include <algorithm>

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

    // Two-hop: if a currency's gear list actually points at a TIER TOKEN (misc/junk/epic that itself
    // has gear in _gear), inline the token's leaves with viaTokenId set, and drop the raw-token entry.
    for (auto& [currency, opts] : _gear)
    {
        std::vector<GearOption> expanded;
        for (GearOption const& o : opts)
        {
            ItemTemplate const* p = sObjectMgr->GetItemTemplate(o.gearId);
            bool const isToken = p && p->Class == ITEM_CLASS_MISC && p->SubClass == ITEM_SUBCLASS_JUNK &&
                                 p->Quality == ITEM_QUALITY_EPIC && _gear.count(o.gearId);
            if (!isToken)
            {
                expanded.push_back(o);
                continue;
            }
            for (GearOption const& leaf : _gear[o.gearId])   // token -> gear (already one-hop built)
                expanded.push_back({leaf.gearId, o.cost, o.extendedCostId, o.gearId});  // cost = emblem cost of the token
        }
        opts.swap(expanded);
    }

    // Sink: keep only repeatably-purchasable non-gear (npc_vendor.maxcount == 0), so a drain reaches 0.
    std::unordered_map<uint32, bool> unlimited;   // itemId -> any vendor sells it with maxcount 0
    if (QueryResult r = WorldDatabase.Query("SELECT item, MIN(maxcount) FROM npc_vendor GROUP BY item"))
    {
        do { Field* f = r->Fetch(); unlimited[f[0].Get<uint32>()] = (f[1].Get<uint32>() == 0); }
        while (r->NextRow());
    }
    for (auto& [currency, sinks] : _sink)
    {
        std::vector<SinkOption> keep;
        for (SinkOption const& s : sinks)
            if (unlimited[s.itemId])
                keep.push_back(s);
        sinks.swap(keep);
    }

    // Threshold T[currency] = CHEAPEST gear cost (incl. token path): redeem may act as soon as
    // anything is buyable. Max was correct when each era-currency had a narrow cost band; the
    // unified ladder (~30..1400) would leave bots dormant until they hoard the top item's price.
    for (auto const& [currency, opts] : _gear)
    {
        uint32 t = 0;
        for (GearOption const& o : opts)
            t = t ? std::min(t, o.cost) : o.cost;
        _threshold[currency] = t;
    }

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
