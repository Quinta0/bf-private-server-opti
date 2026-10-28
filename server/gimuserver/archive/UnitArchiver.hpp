#pragma once

#include <gimuserver/archive/archive.hpp>
#include <gimuserver/packets/all.hpp>
#include <gimuserver/utils/Random.hpp>

#include <drogon/drogon.h>

#include <json/value.h>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

/*!
* Loads and serves server-owned curated unit archive records.
*
* The archive is intentionally smaller than the official MST data. It contains
* only fields the server understands well enough to use when creating
* player-owned units.
*/
class UnitArchiver final
{
public:
	using UnitId = uint32_t;
	using UnitType = uint32_t;

	/*!
	* Gets the process-wide unit archiver.
	*
	* Setup() must be called during server initialization before request handlers
	* depend on archive data.
	*/
	static UnitArchiver& instance();

	/*!
	* Loads unit archive records from archive_root/unit.json.
	* @param serverObj Server configuration object from the Drogon plugin config.
	*/
	void setup(const Json::Value& serverObj);

	/*!
	* Re-points archived Lv1 (and, where present, max) stats at the balance
	* overlay's unit rows, so units granted from the archive -- gacha, present,
	* exchange, tutorial, selector -- start on the published statline.
	*
	* In memory only; archive_root/unit.json is never written.  Must run after
	* setup().  stats[0] takes the row's min_*, stats[1] (18 records carry
	* one) its max_*.
	*
	* @param rows Effective unit rows for the overridden ids.
	*/
	void applyBalanceStats(const std::vector<UnitMst>& rows);

	/*!
	* Looks up a unit archive record.
	*
	* @param unit_id Master unit ID from the unit archive.
	* @return Matching immutable archive record, or std::nullopt when not found.
	*/
	std::optional<UnitRecord> lookup(UnitId unit_id) const;

	/*! Whether the archive has the unit, without lookup()'s error on a miss. */
	bool contains(UnitId unit_id) const { return cache_.contains(unit_id); }

	/*!
	* Converts archive data to a subset of UserUnitInfo.
	*
	* Ownership fields such as user_id and user_unit_id are not modified.
	* @param unitRecord Archive record to read from.
	* @param unit_type_id Unit type to select from the archive stats.
	* @param unit Packet/database object to populate.
	* @return True if the packet was populated, false if the unit type is unsupported.
	*/
	static bool populatePacket(
		const UnitRecord& unitRecord,
		UnitType unit_type_id,
		UserUnitInfo& unit);

	/*! 
	* Gets a random normal unit type id.
	*/
	static UnitType getRandomType()
	{
		return RandomUInt(kMinUnitType, kMaxUnitType);
	}

	/*!
	* Every archived unit, for callers that must walk the whole set.
	*
	* Added because sweeping the id space is not viable here: unit ids are
	* sparse and run to 87,602,603, so a scan either misses units (a 90,000 cap
	* dropped 550 of 2,053) or performs tens of millions of lookups at boot.
	* The friend roster needs every evolution-chain base, which is a genuine
	* enumeration and not a lookup.
	*
	* @return The id -> record map, empty before setup().
	*/
	const std::unordered_map<UnitId, UnitRecord>& all() const { return cache_; }

	UnitArchiver(const UnitArchiver&) = delete;
	UnitArchiver& operator=(const UnitArchiver&) = delete;

private:
	using UnitRecordCache = std::unordered_map<UnitId, UnitRecord>;

	static constexpr UnitType kMinUnitType = 1;
	static constexpr UnitType kMaxUnitType = 5;

	UnitArchiver() = default;

	UnitRecordCache cache_;
};
