#pragma once

#include "Rules.hpp"

#include <gimuserver/gme/common/Common.hpp>

#include <drogon/drogon.h>

#include <memory>
#include <optional>
#include <string>

namespace forge
{

/*!
* The squad rules of rules.json against the squad a MissionStart fights with
* (the Forge server design, section 4.3).
*
* A normal quest reads user_decks squad `deckNum` (Z0Y4RoD7).  A trial reads
* user_trial_decks for its dungeon type (trialDeckRange): the client names its
* normal squad even for a trial, so every non-empty trial squad of the group
* must follow every rule.  Strategy Zone reserves (member_type 2) count for
* elements, one element, banned and required units, not for max units or
* cost.  The helper is `helperId` (h7eY3sAK): none, an own unit (MYUNIT), or a
* friend's / stranger's card resolved as the picker showed it.
*
* @return The first broken rule (in file order, checks in the design's order), or nullopt.
*/
drogon::Task<std::optional<Refusal>> checkSquad(std::shared_ptr<const RulesSnapshot> snapshot,
	gme::UserIdentity identity, int32_t missionId, int32_t deckNum, std::string helperId);

} // namespace forge
