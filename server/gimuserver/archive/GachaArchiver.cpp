#include "GachaArchiver.hpp"

#include "UnitArchiver.hpp"

#include <gimuserver/drogon/ServerCacheMst.hpp>
#include <gimuserver/forge/ArchiveOverlay.hpp>
#include <gimuserver/utils/JsonFile.hpp>
#include <gimuserver/utils/Random.hpp>

#include <drogon/drogon.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

GachaArchiver& GachaArchiver::instance()
{
	static GachaArchiver instance;
	return instance;
}

namespace
{

/*!
* Why the Forge's gacha.json record cannot be sold, or nullopt.  A record that
* passes can be pulled: every unit it rolls sorts by rarity (UnitArchiver) and
* has a summon effect, which is exactly where a bad pool would 500 a pull.
*/
std::optional<std::string> forgeGachaProblem(const GachaRecord& record,
	const std::set<int32_t>& gates, const std::set<int32_t>& effectRarities)
{
	const auto gate = std::to_string(record.id);
	if (record.id == 0)
		return std::string("A banner id must be above 0.");
	if (!gates.contains(static_cast<int32_t>(record.id)))
		return "Banner " + gate + " has no gacha_mst row (its name and price); add the row first.";
	if (record.id >= 20161 && record.id <= 20213)
		return "Banner " + gate + " is in 20161-20213, the ids Race Pick and Unit Pick use.";
	if (record.pool.empty())
		return "Banner " + gate + " has no units in its pool.";
	if (record.pool.size() > 5000)
		return "Banner " + gate + " has more than 5,000 units in its pool.";
	int64_t total = 0;
	for (const auto& pull : record.pool)
	{
		const auto unit = std::to_string(pull.unit_id);
		if (pull.weight < 1 || pull.weight > 1'000'000)
			return "Unit " + unit + "'s weight must be from 1 to 1,000,000.";
		total += pull.weight;
		const auto archived = UnitArchiver::instance().lookup(pull.unit_id);
		if (!archived)
			return "Unit " + unit + " is not in the unit archive, so a pull that rolls it would fail.";
		if (!effectRarities.contains(static_cast<int32_t>(archived->rarity)))
			return "Unit " + unit + "'s rarity has no summon effect (gacha_effect_mst), so a pull that rolls it would fail.";
	}
	if (total > std::numeric_limits<int32_t>::max())
		return "Banner " + gate + "'s weights add up to more than 2,147,483,647.";
	return std::nullopt;
}

} // namespace

void GachaArchiver::setup(const Json::Value& serverObj)
{
	LOG_INFO << "Setting up gacha archiver cache. "
		"If you see this after initialization, it is a bug.";

	const auto archiveRoot = serverObj["archive_root"].asString();
	const auto mstRoot = serverObj["mst_root"].asString();
	if (archiveRoot.empty() || mstRoot.empty())
	{
		LOG_ERROR << "Unable to set up gacha archiver cache: "
			<< "archive_root_empty=" << archiveRoot.empty()
			<< ", mst_root_empty=" << mstRoot.empty();
		return;
	}

	std::vector<GachaRecord> records;
	try
	{
		records = LoadJson<std::vector<GachaRecord>>(archiveRoot, "gacha.json");
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR << "Unable to set up gacha archiver cache: " << ex.what();
		return;
	}
	// The cache's copy, which ServerCache::Setup already merged with the
	// Forge's gacha_mst overlay: one merge, so a gate's price and name are
	// the same in the Initialize reply, GachaInfo and what a pull charges.
	// Without an overlay it is the same file read the same way as before.
	std::vector<GachaMst> gachas = theServer()->cache().initializeResp().gachas;

	{
		std::set<int32_t> gates;
		for (const auto& gacha : gachas)
			gates.insert(gacha.id);
		std::set<int32_t> effectRarities;
		for (const auto& effect : theServer()->cache().initializeResp().gacha_effects)
			if (effect.weight > 0)
				effectRarities.insert(static_cast<int32_t>(effect.rarity));
		records = forge::mergeRecords<GachaRecord>("gacha.json", std::move(records),
			[](const GachaRecord& r) { return std::to_string(r.id); },
			[](const std::string& text, GachaRecord& out) { return forge::readStrict(out, text); },
			[&gates, &effectRarities](const GachaRecord& r) { return forgeGachaProblem(r, gates, effectRarities); },
			/*allowDelete=*/false);
	}

	cache_.clear();
	cache_.reserve(records.size());
	order_.clear();
	order_.reserve(records.size());
	for (auto& record : records)
	{
		if (!cache_.contains(record.id))
			order_.push_back(record.id);
		cache_.insert_or_assign(record.id, std::move(record));
	}

	mstCache_.clear();
	mstCache_.reserve(gachas.size());
	for (auto& gacha : gachas)
	{
		mstCache_.insert_or_assign(gacha.id, std::move(gacha));
	}

	LOG_INFO << "Loaded " << cache_.size()
		<< " gacha archive records from " << archiveRoot << "/gacha.json";
}

std::optional<GachaRecord> GachaArchiver::lookup(GachaId gacha_id) const
{
	const auto it = cache_.find(gacha_id);
	if (it == cache_.end())
	{
		LOG_ERROR << "Unable to find gacha archive record " << gacha_id;
		return std::nullopt;
	}

	return it->second;
}

bool GachaArchiver::populatePacket(const GachaRecord& record, GachaInfoMst& mst) const
{
	const auto it = mstCache_.find(record.id);
	if (it == mstCache_.end())
	{
		LOG_ERROR << "Unable to populate gacha info: missing GachaMst row "
			<< record.id;
		return false;
	}

	const auto& gachaMst = it->second;
	mst.id = gachaMst.id;
	mst.gems = gachaMst.need_gems;
	mst.button = record.button.value_or(gachaMst.btn_img);
	mst.background = record.background.value_or(gachaMst.bg_img);
	mst.door = record.door.value_or(gachaMst.door_img);
	mst.caption_msg = record.caption_msg.value_or(gachaMst.caption_msg);
	mst.detail_msg = record.detail_msg.value_or(gachaMst.detail_msg);
	mst.name = gachaMst.name;
	mst.type = gachaMst.type;
	mst.priority = record.priority;
	mst.friend_points = gachaMst.need_friend_point;
	mst.group_id = gachaMst.gacha_group_id;

	return true;
}

std::vector<uint32_t> GachaArchiver::summonFrom(const GachaRecord& record, uint32_t count) const
{
	uint32_t total = 0;
	std::vector<const GachaPull*> pulls;
	pulls.reserve(record.pool.size());
	for (const auto& pull : record.pool)
	{
		if (pull.weight > 0)
		{
			pulls.push_back(&pull);
			total += pull.weight;
		}
	}

	if (pulls.empty())
	{
		LOG_ERROR << "Unable to summon from gacha archive record "
			<< record.id << ": empty unit pool";
		return {};
	}

	std::vector<uint32_t> units;
	units.reserve(count);
	for (uint32_t i = 0; i < count; ++i)
	{
		auto roll = RandomUInt(1, total);
		for (const auto& pull : pulls)
		{
			if (roll <= pull->weight)
			{
				units.push_back(pull->unit_id);
				break;
			}
			roll -= pull->weight;
		}
	}

	// Client only shows animation effects for the last unit, so make sure
	// it's the one with the highest rarity.
	std::sort(units.begin(), units.end(), [](uint32_t lhs, uint32_t rhs) {
		const auto left = UnitArchiver::instance().lookup(lhs);
		const auto right = UnitArchiver::instance().lookup(rhs);
		if (!left || !right)
		{
			throw std::runtime_error("Unable to sort summoned units by rarity");
		}

		return left->rarity < right->rarity;
	});

	return units;
}

std::vector<GachaInfoMst> GachaArchiver::populateAllPackets() const
{
	std::vector<const GachaRecord*> records;
	records.reserve(order_.size());
	for (auto it = order_.rbegin(); it != order_.rend(); ++it)
	{
		records.push_back(&cache_.at(*it));
	}

	std::vector<GachaInfoMst> msts;
	msts.reserve(records.size());
	for (const auto& record : records)
	{
		GachaInfoMst mst;
		if (populatePacket(*record, mst))
		{
			msts.push_back(std::move(mst));
		}
	}

	return msts;
}
