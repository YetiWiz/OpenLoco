#include "Station.h"
#include "CompanyManager.h"
#include "IndustryManager.h"
#include "Localisation/StringIds.h"
#include "Map/BuildingElement.h"
#include "Map/IndustryElement.h"
#include "Map/StationElement.h"
#include "Map/TileManager.h"
#include "MessageManager.h"
#include "Objects/AirportObject.h"
#include "Objects/BuildingObject.h"
#include "Objects/CargoObject.h"
#include "Objects/IndustryObject.h"
#include "Objects/ObjectManager.h"
#include "Objects/RoadStationObject.h"
#include "OpenLoco.h"
#include "TownManager.h"
#include "Ui/WindowManager.h"
#include "ViewportManager.h"
#include <OpenLoco/Interop/Interop.hpp>
#include <OpenLoco/Math/Bound.hpp>
#include <algorithm>
#include <cassert>

using namespace OpenLoco::Interop;
using namespace OpenLoco::World;
using namespace OpenLoco::Ui;
using namespace OpenLoco::Literals;

namespace OpenLoco
{
    constexpr uint8_t kMinCargoRating = 0;
    constexpr uint8_t kMaxCargoRating = 200;
    constexpr uint8_t catchmentSize = 4;

    struct CargoSearchState
    {
    private:
        inline static loco_global<uint8_t[kMapSize], 0x00F00484> _map;
        inline static loco_global<uint32_t, 0x0112C68C> _filter;
        inline static loco_global<uint32_t[kMaxCargoStats], 0x0112C690> _score;
        inline static loco_global<uint32_t, 0x0112C710> _producedCargoTypes;
        inline static loco_global<IndustryId[kMaxCargoStats], 0x0112C7D2> _industry;
        inline static loco_global<uint8_t, 0x0112C7F2> _byte_112C7F2;

    public:
        bool mapHas2(const tile_coord_t x, const tile_coord_t y) const
        {
            return (_map[y * kMapColumns + x] & (1 << enumValue(CatchmentFlags::flag_1))) != 0;
        }

        void mapRemove2(const tile_coord_t x, const tile_coord_t y)
        {
            _map[y * kMapColumns + x] &= ~(1 << enumValue(CatchmentFlags::flag_1));
        }

        void setTile(const tile_coord_t x, const tile_coord_t y, const CatchmentFlags flag)
        {
            _map[y * kMapColumns + x] |= (1 << enumValue(flag));
        }

        void resetTile(const tile_coord_t x, const tile_coord_t y, const CatchmentFlags flag)
        {
            _map[y * kMapColumns + x] &= ~(1 << enumValue(flag));
        }

        void setTileRegion(tile_coord_t x, tile_coord_t y, int16_t xTileCount, int16_t yTileCount, const CatchmentFlags flag)
        {
            auto xStart = x;
            auto xTileStartCount = xTileCount;
            while (yTileCount > 0)
            {
                while (xTileCount > 0)
                {
                    setTile(x, y, flag);
                    x++;
                    xTileCount--;
                }

                x = xStart;
                xTileCount = xTileStartCount;
                y++;
                yTileCount--;
            }
        }

        void resetTileRegion(tile_coord_t x, tile_coord_t y, int16_t xTileCount, int16_t yTileCount, const CatchmentFlags flag)
        {
            auto xStart = x;
            auto xTileStartCount = xTileCount;
            while (yTileCount > 0)
            {
                while (xTileCount > 0)
                {
                    resetTile(x, y, flag);
                    x++;
                    xTileCount--;
                }

                x = xStart;
                xTileCount = xTileStartCount;
                y++;
                yTileCount--;
            }
        }

        uint32_t filter() const
        {
            return _filter;
        }

        void filter(const uint32_t value)
        {
            _filter = value;
        }

        void resetScores()
        {
            std::fill_n(_score.get(), kMaxCargoStats, 0);
        }

        uint32_t score(const uint8_t cargo)
        {
            return _score[cargo];
        }

        void addScore(const uint8_t cargo, const int32_t value)
        {
            _score[cargo] += value;
        }

        uint32_t producedCargoTypes() const
        {
            return _producedCargoTypes;
        }

        void resetProducedCargoTypes()
        {
            _producedCargoTypes = 0;
        }

        void addProducedCargoType(const uint8_t cargoId)
        {
            _producedCargoTypes = _producedCargoTypes | (1 << cargoId);
        }

        void byte_112C7F2(const uint8_t value)
        {
            _byte_112C7F2 = value;
        }

        void resetIndustryMap()
        {
            std::fill_n(_industry.get(), kMaxCargoStats, IndustryId::null);
        }

        IndustryId getIndustry(const uint8_t cargo) const
        {
            return _industry[cargo];
        }

        void setIndustry(const uint8_t cargo, const IndustryId id)
        {
            _industry[cargo] = id;
        }
    };

    static StationElement* getStationElement(const Pos3& pos);

    // 0x0048B23E
    void Station::update()
    {
        updateCargoAcceptance();
    }

    // 0x00492640
    void Station::updateCargoAcceptance()
    {
        CargoSearchState cargoSearchState;
        uint32_t currentAcceptedCargo = calcAcceptedCargo(cargoSearchState);
        uint32_t originallyAcceptedCargo = 0;
        for (uint32_t cargoId = 0; cargoId < kMaxCargoStats; cargoId++)
        {
            auto& stationCargoStats = cargoStats[cargoId];
            stationCargoStats.industryId = cargoSearchState.getIndustry(cargoId);
            if (stationCargoStats.isAccepted())
            {
                originallyAcceptedCargo |= (1 << cargoId);
            }

            bool isNowAccepted = (currentAcceptedCargo & (1 << cargoId)) != 0;
            stationCargoStats.isAccepted(isNowAccepted);
        }

        if (originallyAcceptedCargo != currentAcceptedCargo)
        {
            if (owner == CompanyManager::getControllingId())
            {
                alertCargoAcceptanceChange(originallyAcceptedCargo, currentAcceptedCargo);
            }
            invalidateWindow();
        }
    }

    // 0x00492683
    void Station::alertCargoAcceptanceChange(uint32_t oldCargoAcc, uint32_t newCargoAcc)
    {
        for (uint32_t cargoId = 0; cargoId < kMaxCargoStats; cargoId++)
        {
            bool acceptedBefore = (oldCargoAcc & (1 << cargoId)) != 0;
            bool acceptedNow = (newCargoAcc & (1 << cargoId)) != 0;
            if (acceptedBefore && !acceptedNow)
            {
                MessageManager::post(
                    MessageType::cargoNoLongerAccepted,
                    owner,
                    enumValue(id()),
                    cargoId);
            }
            else if (!acceptedBefore && acceptedNow)
            {
                MessageManager::post(
                    MessageType::cargoNowAccepted,
                    owner,
                    enumValue(id()),
                    cargoId);
            }
        }
    }

    // 0x00492026
    static uint32_t doCalcAcceptedCargo(const Station* station, CargoSearchState& cargoSearchState)
    {
        cargoSearchState.resetScores();
        cargoSearchState.resetProducedCargoTypes();

        if (station != nullptr)
        {
            for (uint16_t i = 0; i < station->stationTileSize; i++)
            {
                auto pos = station->stationTiles[i];
                auto stationElement = getStationElement(pos);

                if (stationElement == nullptr)
                {
                    continue;
                }

                cargoSearchState.byte_112C7F2(0);

                if (stationElement->stationType() == StationType::roadStation)
                {
                    auto obj = ObjectManager::get<RoadStationObject>(stationElement->objectId());

                    if (obj->hasFlags(RoadStationFlags::passenger))
                    {
                        cargoSearchState.filter(cargoSearchState.filter() | (1 << obj->cargoType));
                    }
                    else if (obj->hasFlags(RoadStationFlags::freight))
                    {
                        cargoSearchState.filter(cargoSearchState.filter() | ~(1 << obj->cargoType));
                    }
                }
                else
                {
                    cargoSearchState.filter(~0);
                }
            }
        }

        if (cargoSearchState.filter() == 0)
        {
            cargoSearchState.filter(~0);
        }

        for (tile_coord_t ty = 0; ty < kMapColumns; ty++)
        {
            for (tile_coord_t tx = 0; tx < kMapRows; tx++)
            {
                if (cargoSearchState.mapHas2(tx, ty))
                {
                    auto pos = Pos2(tx * kTileSize, ty * kTileSize);
                    auto tile = TileManager::get(pos);

                    for (auto& el : tile)
                    {
                        if (el.isGhost())
                        {
                            continue;
                        }
                        switch (el.type())
                        {
                            case ElementType::industry:
                            {
                                auto& industryEl = el.get<IndustryElement>();
                                auto* industry = industryEl.industry();

                                if (industry == nullptr || industry->under_construction != 0xFF)
                                {
                                    break;
                                }
                                const auto* obj = industry->getObject();

                                if (obj == nullptr)
                                {
                                    break;
                                }

                                for (auto cargoId : obj->requiredCargoType)
                                {
                                    if (cargoId != 0xFF && (cargoSearchState.filter() & (1 << cargoId)))
                                    {
                                        cargoSearchState.addScore(cargoId, 8);
                                        cargoSearchState.setIndustry(cargoId, industry->id());
                                    }
                                }

                                for (auto cargoId : obj->producedCargoType)
                                {
                                    if (cargoId != 0xFF && (cargoSearchState.filter() & (1 << cargoId)))
                                    {
                                        cargoSearchState.addProducedCargoType(cargoId);
                                    }
                                }

                                break;
                            }
                            case ElementType::building:
                            {
                                auto& buildingEl = el.get<BuildingElement>();

                                if (buildingEl.has_40() || !buildingEl.isConstructed())
                                {
                                    break;
                                }

                                const auto* obj = buildingEl.getObject();

                                if (obj == nullptr)
                                {
                                    break;
                                }
                                for (int i = 0; i < 2; i++)
                                {
                                    const auto cargoId = obj->producedCargoType[i];
                                    if (cargoId != 0xFF && (cargoSearchState.filter() & (1 << cargoId)))
                                    {
                                        cargoSearchState.addScore(cargoId, obj->var_A6[i]);

                                        if (obj->producedQuantity[i] != 0)
                                        {
                                            cargoSearchState.addProducedCargoType(cargoId);
                                        }
                                    }
                                }

                                for (int i = 0; i < 2; i++)
                                {
                                    if (obj->var_A4[i] != 0xFF && (cargoSearchState.filter() & (1 << obj->var_A4[i])))
                                    {
                                        cargoSearchState.addScore(obj->var_A4[i], obj->var_A8[i]);
                                    }
                                }

                                // Multi tile buildings should only be counted once so remove the other tiles from the search
                                if (obj->hasFlags(BuildingObjectFlags::largeTile))
                                {
                                    auto index = buildingEl.multiTileIndex();
                                    tile_coord_t xPos = (pos.x - World::offsets[index].x) / kTileSize;
                                    tile_coord_t yPos = (pos.y - World::offsets[index].y) / kTileSize;

                                    cargoSearchState.mapRemove2(xPos + 0, yPos + 0);
                                    cargoSearchState.mapRemove2(xPos + 0, yPos + 1);
                                    cargoSearchState.mapRemove2(xPos + 1, yPos + 0);
                                    cargoSearchState.mapRemove2(xPos + 1, yPos + 1);
                                }

                                break;
                            }
                            default:
                                continue;
                        }
                    }
                }
            }
        }

        uint32_t acceptedCargos = 0;

        for (uint8_t cargoId = 0; cargoId < kMaxCargoStats; cargoId++)
        {
            if (cargoSearchState.score(cargoId) >= 8)
            {
                acceptedCargos |= (1 << cargoId);
            }
        }

        return acceptedCargos;
    }

    // 0x00491FE0
    // THIS FUNCTION ONLY TO BE CALLED ON NORMAL STATIONS
    uint32_t Station::calcAcceptedCargo(CargoSearchState& cargoSearchState) const
    {
        cargoSearchState.byte_112C7F2(1);
        cargoSearchState.filter(0);

        cargoSearchState.resetIndustryMap();

        setCatchmentDisplay(this, CatchmentFlags::flag_1);

        return doCalcAcceptedCargo(this, cargoSearchState);
    }

    // 0x00491FE0
    // WARNING: this may be called with station (ebp) = -1
    // THIS FUNCTION ONLY TO BE CALLED ON GHOST TRACK STATIONS
    PotentialCargo calcAcceptedCargoTrackStationGhost(const Station* ghostStation, const Pos2& location, const uint32_t filter)
    {
        CargoSearchState cargoSearchState;
        cargoSearchState.byte_112C7F2(1);
        cargoSearchState.filter(0);

        cargoSearchState.filter(filter);

        cargoSearchState.resetIndustryMap();

        setCatchmentDisplay(ghostStation, CatchmentFlags::flag_1);

        sub_491BF5(location, CatchmentFlags::flag_1);

        PotentialCargo res{};
        res.accepted = doCalcAcceptedCargo(ghostStation, cargoSearchState);
        res.produced = cargoSearchState.producedCargoTypes();
        return res;
    }

    // 0x00491F43
    // WARNING: this may be called with station (ebp) = -1
    // THIS FUNCTION ONLY TO BE CALLED ON GHOST AIRPORT STATIONS
    PotentialCargo calcAcceptedCargoAirportGhost(const Station* ghostStation, const uint8_t type, const Pos2& location, const uint8_t rotation, const uint32_t filter)
    {
        CargoSearchState cargoSearchState;
        cargoSearchState.byte_112C7F2(1);
        cargoSearchState.filter(0);

        cargoSearchState.filter(filter);

        cargoSearchState.resetIndustryMap();

        setCatchmentDisplay(ghostStation, CatchmentFlags::flag_1);

        sub_491C6F(type, location, rotation, CatchmentFlags::flag_1);

        PotentialCargo res{};
        res.accepted = doCalcAcceptedCargo(ghostStation, cargoSearchState);
        res.produced = cargoSearchState.producedCargoTypes();
        return res;
    }

    // 0x00491F93
    // WARNING: this may be called with station (ebp) = -1
    // THIS FUNCTION ONLY TO BE CALLED ON GHOST DOCK STATIONS
    PotentialCargo calcAcceptedCargoDockGhost(const Station* ghostStation, const Pos2& location, const uint32_t filter)
    {
        CargoSearchState cargoSearchState;
        cargoSearchState.byte_112C7F2(1);
        cargoSearchState.filter(0);

        cargoSearchState.filter(filter);

        cargoSearchState.resetIndustryMap();

        setCatchmentDisplay(ghostStation, CatchmentFlags::flag_1);

        sub_491D20(location, CatchmentFlags::flag_1);

        PotentialCargo res{};
        res.accepted = doCalcAcceptedCargo(ghostStation, cargoSearchState);
        res.produced = cargoSearchState.producedCargoTypes();
        return res;
    }

    static void setStationCatchmentRegion(CargoSearchState& cargoSearchState, TilePos2 minPos, TilePos2 maxPos, const CatchmentFlags flags);

    // 0x00491D70
    // catchment flag should not be shifted (1, 2, 3, 4) and NOT (1 << 0, 1 << 1)
    void setCatchmentDisplay(const Station* station, const CatchmentFlags catchmentFlag)
    {
        CargoSearchState cargoSearchState;
        cargoSearchState.resetTileRegion(0, 0, kMapColumns, kMapRows, catchmentFlag);

        if (station == nullptr)
            return;

        if (station->stationTileSize == 0)
            return;

        for (uint16_t i = 0; i < station->stationTileSize; i++)
        {
            auto pos = station->stationTiles[i];
            pos.z &= ~((1 << 1) | (1 << 0));

            auto stationElement = getStationElement(pos);

            if (stationElement == nullptr)
                continue;

            switch (stationElement->stationType())
            {
                case StationType::airport:
                {
                    auto airportObject = ObjectManager::get<AirportObject>(stationElement->objectId());

                    auto [minPos, maxPos] = airportObject->getAirportExtents(pos, stationElement->rotation());

                    minPos.x -= catchmentSize;
                    minPos.y -= catchmentSize;
                    maxPos.x += catchmentSize;
                    maxPos.y += catchmentSize;

                    setStationCatchmentRegion(cargoSearchState, minPos, maxPos, catchmentFlag);
                }
                break;
                case StationType::docks:
                {
                    TilePos2 minPos(pos);
                    auto maxPos = minPos;

                    minPos.x -= catchmentSize;
                    minPos.y -= catchmentSize;
                    // Docks are always size 2x2
                    maxPos.x += catchmentSize + 1;
                    maxPos.y += catchmentSize + 1;

                    setStationCatchmentRegion(cargoSearchState, minPos, maxPos, catchmentFlag);
                }
                break;
                default:
                {
                    TilePos2 minPos(pos);
                    auto maxPos = minPos;

                    minPos.x -= catchmentSize;
                    minPos.y -= catchmentSize;
                    maxPos.x += catchmentSize;
                    maxPos.y += catchmentSize;

                    setStationCatchmentRegion(cargoSearchState, minPos, maxPos, catchmentFlag);
                }
            }
        }
    }

    // 0x0049B4E0
    void Station::deliverCargoToTown(uint8_t cargoType, uint16_t cargoQuantity)
    {
        auto twn = TownManager::get(town);
        twn->monthlyCargoDelivered[cargoType] = Math::Bound::add(twn->monthlyCargoDelivered[cargoType], cargoQuantity);
    }

    // 0x0042F489
    void Station::deliverCargoToStation(const uint8_t cargoType, const uint8_t cargoQuantity)
    {
        auto& stationCargoStat = cargoStats[cargoType];
        stationCargoStat.quantity = Math::Bound::add(stationCargoStat.quantity, cargoQuantity);
        stationCargoStat.enrouteAge = 0;
        stationCargoStat.origin = id();
        updateCargoDistribution();
    }

    // 0x0048F7D1
    void Station::sub_48F7D1()
    {
        registers regs;
        regs.ebx = enumValue(id());
        call(0x0048F7D1, regs);
    }

    // 0x00492A98
    char* Station::getStatusString(char* buffer)
    {
        char* ptr = buffer;
        *ptr = '\0';

        for (uint32_t cargoId = 0; cargoId < kMaxCargoStats; cargoId++)
        {
            auto& stationCargoStat = cargoStats[cargoId];

            if (stationCargoStat.quantity == 0)
                continue;

            if (*buffer != '\0')
                ptr = StringManager::formatString(ptr, StringIds::waiting_cargo_separator);

            loco_global<uint32_t, 0x112C826> _commonFormatArgs;
            *_commonFormatArgs = stationCargoStat.quantity;

            auto cargo = ObjectManager::get<CargoObject>(cargoId);
            auto unitName = stationCargoStat.quantity == 1 ? cargo->unitNameSingular : cargo->unitNamePlural;
            ptr = StringManager::formatString(ptr, unitName, &*_commonFormatArgs);
        }

        string_id suffix = *buffer == '\0' ? StringIds::nothing_waiting : StringIds::waiting;
        return StringManager::formatString(ptr, suffix);
    }

    // 0x00492793
    bool Station::updateCargo()
    {
        bool atLeastOneGoodRating = false;
        bool quantityUpdated = false;

        var_3B0 = std::min(var_3B0 + 1, 255);
        var_3B1 = std::min(var_3B1 + 1, 255);

        auto& rng = gPrng();
        for (uint32_t i = 0; i < kMaxCargoStats; i++)
        {
            auto& stationCargo = cargoStats[i];
            if (!stationCargo.empty())
            {
                if (stationCargo.quantity != 0 && stationCargo.origin != id())
                {
                    stationCargo.enrouteAge = std::min(stationCargo.enrouteAge + 1, 255);
                }
                else
                {
                    // Change from vanilla to deal with the cargo transfer bug:
                    // Reset enroute age once the station cargo gets cleared
                    // or else the age keeps increasing
                    stationCargo.enrouteAge = 0;
                }
                stationCargo.age = std::min(stationCargo.age + 1, 255);

                auto targetRating = calculateCargoRating(stationCargo);
                // Limit to +/- 2 minimum change
                auto ratingDelta = std::clamp(targetRating - stationCargo.rating, -2, 2);
                stationCargo.rating += ratingDelta;

                if (stationCargo.rating <= 50)
                {
                    // Rating < 25%, decrease cargo
                    if (stationCargo.quantity >= 400)
                    {
                        stationCargo.quantity -= rng.randNext(1, 32);
                        quantityUpdated = true;
                    }
                    else if (stationCargo.quantity >= 200)
                    {
                        stationCargo.quantity -= rng.randNext(1, 8);
                        quantityUpdated = true;
                    }
                }
                if (stationCargo.rating >= 100)
                {
                    atLeastOneGoodRating = true;
                }
                if (stationCargo.rating <= 100 && stationCargo.quantity != 0)
                {
                    if (stationCargo.rating <= rng.randNext(0, 127))
                    {
                        stationCargo.quantity = std::max(0, stationCargo.quantity - rng.randNext(1, 4));
                        quantityUpdated = true;
                    }
                }
            }
        }

        updateCargoDistribution();

        auto w = WindowManager::find(WindowType::station, enumValue(id()));
        if (w != nullptr && (w->currentTab == 2 || w->currentTab == 1 || quantityUpdated))
        {
            w->invalidate();
        }

        return atLeastOneGoodRating;
    }

    // 0x004927F6
    int32_t Station::calculateCargoRating(const StationCargoStats& cargo) const
    {
        int32_t rating = 0;

        // Bonus if cargo is fresh
        if (cargo.age <= 45)
        {
            rating += 40;
            if (cargo.age <= 30)
            {
                rating += 45;
                if (cargo.age <= 15)
                {
                    rating += 45;
                    if (cargo.age <= 7)
                    {
                        rating += 35;
                    }
                }
            }
        }

        // Penalty if lots of cargo waiting
        rating -= 130;
        if (cargo.quantity <= 1000)
        {
            rating += 30;
            if (cargo.quantity <= 500)
            {
                rating += 30;
                if (cargo.quantity <= 300)
                {
                    rating += 30;
                    if (cargo.quantity <= 200)
                    {
                        rating += 20;
                        if (cargo.quantity <= 100)
                        {
                            rating += 20;
                        }
                    }
                }
            }
        }

        if ((flags & (StationFlags::flag_7 | StationFlags::flag_8)) == StationFlags::none && !CompanyManager::isPlayerCompany(owner))
        {
            rating = 120;
        }

        Speed16 vehicleSpeed = std::min(cargo.vehicleSpeed, 250_mph);
        if (vehicleSpeed > 35_mph)
        {
            rating += ((vehicleSpeed - 35_mph).getRaw()) / 4;
        }

        if (cargo.vehicleAge < 4)
        {
            rating += 10;
            if (cargo.vehicleAge < 2)
            {
                rating += 10;
                if (cargo.vehicleAge < 1)
                {
                    rating += 13;
                }
            }
        }

        return std::clamp<int32_t>(rating, kMinCargoRating, kMaxCargoRating);
    }

    // 0x004929DB
    void Station::updateCargoDistribution()
    {
        invalidateWindow();
        WindowManager::invalidate(Ui::WindowType::stationList);
        bool hasChanged = false;
        for (uint8_t i = 0; i < kMaxCargoStats; ++i)
        {
            auto& stationCargo = cargoStats[i];
            auto newDensity = 0;
            if (stationCargo.quantity != 0)
            {
                if (stationTileSize != 0)
                {
                    newDensity = stationCargo.quantity / stationTileSize;
                    auto* cargoObj = ObjectManager::get<CargoObject>(i);
                    newDensity += (1 << cargoObj->var_14) - 1;
                    newDensity >>= cargoObj->var_14;

                    newDensity = std::min(newDensity, 15);
                }
            }
            if (stationCargo.densityPerTile != newDensity)
            {
                stationCargo.densityPerTile = newDensity;
                hasChanged = true;
            }
        }

        if (hasChanged)
        {
            for (auto i = 0; i < stationTileSize; ++i)
            {
                const auto& tile = stationTiles[i];
                Ui::ViewportManager::invalidate({ tile.x, tile.y }, tile.z & 0xFFFC, tile.z + 32, ZoomLevel::full);
            }
        }
    }

    // 0x0048DCA5
    void Station::updateLabel()
    {
        registers regs;
        regs.esi = X86Pointer(this);
        call(0x0048DCA5, regs);
    }

    // 0x004CBA2D
    void Station::invalidate()
    {
        Ui::ViewportManager::invalidate(this);
    }

    void Station::invalidateWindow()
    {
        WindowManager::invalidate(WindowType::station, enumValue(id()));
    }

    // 0x0048F6D4
    static StationElement* getStationElement(const Pos3& pos)
    {
        auto tile = TileManager::get(pos.x, pos.y);
        auto baseZ = pos.z / 4;

        for (auto& element : tile)
        {
            auto* stationElement = element.as<StationElement>();

            if (stationElement == nullptr)
            {
                continue;
            }

            if (stationElement->baseZ() != baseZ)
            {
                continue;
            }

            if (!stationElement->isFlag5())
            {
                return stationElement;
            }
            else
            {
                return nullptr;
            }
        }
        return nullptr;
    }

    // 0x00491EDC
    static void setStationCatchmentRegion(CargoSearchState& cargoSearchState, TilePos2 minPos, TilePos2 maxPos, const CatchmentFlags flag)
    {
        minPos.x = std::max(minPos.x, static_cast<coord_t>(0));
        minPos.y = std::max(minPos.y, static_cast<coord_t>(0));
        maxPos.x = std::min(maxPos.x, static_cast<coord_t>(kMapColumns - 1));
        maxPos.y = std::min(maxPos.y, static_cast<coord_t>(kMapRows - 1));

        maxPos.x -= minPos.x;
        maxPos.y -= minPos.y;
        maxPos.x++;
        maxPos.y++;

        cargoSearchState.setTileRegion(minPos.x, minPos.y, maxPos.x, maxPos.y, flag);
    }

    // 0x00491BF5
    void sub_491BF5(const Pos2& pos, const CatchmentFlags flag)
    {
        TilePos2 minPos(pos);
        auto maxPos = minPos;
        maxPos.x += catchmentSize;
        maxPos.y += catchmentSize;
        minPos.x -= catchmentSize;
        minPos.y -= catchmentSize;

        CargoSearchState cargoSearchState;

        setStationCatchmentRegion(cargoSearchState, minPos, maxPos, flag);
    }

    // 0x00491C6F
    void sub_491C6F(const uint8_t type, const Pos2& pos, const uint8_t rotation, const CatchmentFlags flag)
    {
        auto airportObject = ObjectManager::get<AirportObject>(type);

        auto [minPos, maxPos] = airportObject->getAirportExtents(pos, rotation);

        minPos.x -= catchmentSize;
        minPos.y -= catchmentSize;
        maxPos.x += catchmentSize;
        maxPos.y += catchmentSize;

        CargoSearchState cargoSearchState;

        setStationCatchmentRegion(cargoSearchState, minPos, maxPos, flag);
    }

    // 0x00491D20
    void sub_491D20(const Pos2& pos, const CatchmentFlags flag)
    {
        TilePos2 minPos(pos);
        auto maxPos = minPos + TilePos2{ 1, 1 };
        maxPos.x += catchmentSize;
        maxPos.y += catchmentSize;
        minPos.x -= catchmentSize;
        minPos.y -= catchmentSize;

        CargoSearchState cargoSearchState;

        setStationCatchmentRegion(cargoSearchState, minPos, maxPos, flag);
    }

    string_id getTransportIconsFromStationFlags(const StationFlags flags)
    {
        constexpr string_id label_icons[] = {
            StringIds::label_icons_none,
            StringIds::label_icons_rail,
            StringIds::label_icons_road,
            StringIds::label_icons_rail_road,
            StringIds::label_icons_air,
            StringIds::label_icons_rail_air,
            StringIds::label_icons_road_air,
            StringIds::label_icons_rail_road_air,
            StringIds::label_icons_water,
            StringIds::label_icons_rail_water,
            StringIds::label_icons_road_water,
            StringIds::label_icons_rail_road_water,
            StringIds::label_icons_air_water,
            StringIds::label_icons_rail_air_water,
            StringIds::label_icons_road_air_water,
            StringIds::label_icons_rail_road_air_water,
        };

        const auto label = enumValue(flags & StationFlags::allModes);
        return label_icons[label];
    }
}