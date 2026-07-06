/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 *
 * CurrencyGearIndex — boot-time reverse index: token/emblem currency -> usable gear
 * (two hops: emblem -> tier token -> gear), plus sink (gems/mats) and conversion targets.
 * Built once from sItemExtendedCostStore + npc_vendor; read O(1) at runtime, no per-tick scan.
 * See docs/specs/2026-07-06-token-aware-looting-design.md.
 */
#ifndef _PLAYERBOT_CURRENCYGEARINDEX_H
#define _PLAYERBOT_CURRENCYGEARINDEX_H

#include "Common.h"
#include <unordered_map>
#include <vector>

class CurrencyGearIndex
{
public:
    struct GearOption
    {
        uint32 gearId = 0;
        uint32 cost = 0;
        uint32 extendedCostId = 0;
        uint32 viaTokenId = 0;
    };

    struct SinkOption
    {
        uint32 itemId = 0;
        uint32 cost = 0;
    };

    static CurrencyGearIndex& instance()
    {
        static CurrencyGearIndex i;
        return i;
    }

    void Build();
    std::vector<GearOption> const& GearFor(uint32 currencyItemId) const;
    std::vector<SinkOption> const& SinkFor(uint32 currencyItemId) const;
    uint32 ConvertTargetFor(uint32 currencyItemId) const;
    uint32 ThresholdFor(uint32 currencyItemId) const;
    bool IsCurrency(uint32 itemId) const
    {
        return _gear.find(itemId) != _gear.end() || _sink.find(itemId) != _sink.end();
    }
    void DebugDump() const;

private:
    bool _built = false;
    std::unordered_map<uint32, std::vector<GearOption>> _gear;
    std::unordered_map<uint32, std::vector<SinkOption>> _sink;
    std::unordered_map<uint32, uint32> _convert;
    std::unordered_map<uint32, uint32> _threshold;
    static std::vector<GearOption> const _emptyGear;
    static std::vector<SinkOption> const _emptySink;
};

#define sCurrencyGearIndex CurrencyGearIndex::instance()

#endif  // _PLAYERBOT_CURRENCYGEARINDEX_H
