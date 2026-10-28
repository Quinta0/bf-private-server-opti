#pragma once

#include <gimuserver/archive/archive.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

/*!
* Capture rules: <balance_root>/server/capture.json, which units
* can be captured and how often (ticket #155).  MissionStart applies
* them to the battle it serves, after the slot rolls and the bosses' base-form
* captures, so they reach every battle: archive, hand-built and snapshot
* missions alike.  The tutorial's missions 1 and 2 are left alone.
*
*   {"units":    {"<unit id>": {"enabled": false}
*                             | {"rate": 25}
*                             | {"enabled": true, "rate": 10}},
*    "missions": {"<mission id>": {"<unit id>": {... the same ...}}}}
*
* The unit id is the unit a capture gives: a monster's unit_drop_id, else the
* unit it is drawn as.  A mission's entry for that unit wins over the unit's.
* One exception: an "enabled": true rule on the unit a monster is drawn as wins
* over the unit it drops, so turning on a boss's own form is not hidden by its
* base-form drop (applyBossDrops) or an archive capture of another form.
*   "enabled": false   no capture of that unit;
*   "rate": 0-100      the capture chance wherever the unit can be captured;
*   "enabled": true    also makes it capturable where it was not, at "rate"
*                      or else kDefaultCaptureRate (the archive's 40% floor).
* A unit the file does not name keeps the archive's own chance, and without a
* file nothing changes at all.  The file is re-read when it changes (checked on
* every call by size and time); a file that does not read, or any bad value,
* keeps the last good rules and is logged.
*/
namespace forge
{

constexpr uint32_t kDefaultCaptureRate = 40;   // MissionArchiver::kMinCaptureChance

struct CaptureRule
{
	std::optional<bool> enabled;
	std::optional<uint32_t> rate;
};

struct CaptureRules
{
	std::unordered_map<uint32_t, CaptureRule> units;
	std::unordered_map<uint32_t, std::unordered_map<uint32_t, CaptureRule>> missions;

	/*! The rule for `unit` in `mission`, or nullptr when the file names neither. */
	const CaptureRule* find(uint32_t mission, uint32_t unit) const;
};

/*! The rules in force (empty without a file). */
std::shared_ptr<const CaptureRules> captureRules();

/*!
* Applies the rules to one served battle; `capturable(unit)` says whether
* MissionEnd could hand that unit out (UnitArchiver), checked before a unit is
* made capturable where it was not.  On a `handBuilt` (hand-authored) record a capture
* keeps the unit the battle names: a rule on the drawn unit does not replace it.
*
* @return How many monsters changed.
*/
size_t applyCaptureRules(MissionRecord& record, const CaptureRules& rules,
	bool (*capturable)(uint32_t unit), bool handBuilt = false);

} // namespace forge
