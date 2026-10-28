#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/*!
* Starter pack overlay: <balance_root>/server/starter.json.
*
* What a player finds in the present box when they finish the tutorial (the
* "Gifts have been awarded to you" dialog), on top of what CreateUser hands
* every new account (5000 zel, 1000 karma, one potion, the element starter and
* Burny and Sparky, the two units the tutorial's fusion step needs).  A reset
* account plays the tutorial again, so it gets the pack again.
*
*   {"format": 1, "gems": 5, "zel": 10000, "karma": 0,
*    "items": [{"id": 20000, "count": 3}], "units": [{"id": 10011}]}
*
* Every key is required and no other key is taken.  gems 0..9,999, zel and
* karma 0..99,999,999, at most 10 item lines (count 1..999, an item_mst id) and
* 10 units (each an archive unit: one present, one copy).  A pack must give
* something: the dialog promises gifts.  A file that fails any check is not
* used at all: the defaults below stay in force and forgeStatus.php says why.
* The file is looked at again on every tutorial completion and every
* forgeStatus.php request (re-read only when it changed), so a publish needs
* no restart.
*/
namespace forge
{

struct StarterItem
{
	int32_t id = 0;
	int32_t count = 0;
	bool operator==(const StarterItem&) const = default;
};

/*! The defaults are what the server gave before an overlay could change it. */
struct StarterPack
{
	int32_t gems = 5;
	int32_t zel = 10000;
	int32_t karma = 0;
	std::vector<StarterItem> items{ { 20000, 3 } };
	std::vector<int32_t> units;
	bool operator==(const StarterPack&) const = default;
};

inline constexpr int64_t kStarterMaxGems = 9'999;
inline constexpr int64_t kStarterMaxCurrency = 99'999'999;
inline constexpr size_t kStarterMaxItems = 10;
inline constexpr int64_t kStarterMaxItemCount = 999;
inline constexpr size_t kStarterMaxUnits = 10;

struct StarterParse
{
	std::optional<StarterPack> pack;
	std::string error;        //!< one plain sentence when pack is empty
};

/*! Parses and checks starter.json; `unitKnown` / `itemKnown` say whether an id is in the game. */
StarterParse parseStarter(const std::string& text, const std::function<bool(int32_t)>& unitKnown,
	const std::function<bool(int32_t)>& itemKnown);

/*! The pack in force now (re-reads the file when it changed); keeps forgeStatus.php up to date. */
StarterPack starterPack();

/*! At the end of boot, so forgeStatus.php reports the file before anyone finishes the tutorial. */
void loadStarterAtBoot();

} // namespace forge
