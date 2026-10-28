// Images that must stay 404 instead of getting a placeholder, matched by file
// name in any folder. Each one is art players actually look at (tutorial
// pages, quest banners, battle backgrounds) with no substitute yet, and a
// placeholder would stay on a phone for good once downloaded. A 404 keeps the
// client asking, so it picks up the real file as soon as that is uploaded to
// R2 (an exact hit always wins, so no edit here is needed then).
// Source: the "needs_real_art" entries in re/work/assets-sweep/upload/report.md.

export const NO_PLACEHOLDER = new Set([
  'actionsset1.png',
  'actionsset2.png',
  'dungeon_battle_103100.jpg',
  'dungeon_battle_701100.jpg',
  'dungeon_battle_701600.jpg',
  'dungeon_battle_800122.jpg',
  'dungeon_battle_800123.jpg',
  'dungeon_battle_9104200.png',
  'lang_thai_btn_label1.png',
  'lang_thai_btn_label2.png',
  'sp_quest_banner_800141_1.png',
  'sp_quest_banner_800141_2.png',
  'sp_quest_banner_800141_3.png',
  'Spring21_ticket_event_icon.png',
  'TUT_ITEMS_IMG_PG1.png',
  'TUT_SYN_IMG_PG1.png',
  'TUT_SYN_IMG_PG2.png',
  'TUT_UPGRADES_IMG_PG1.png',
  'TUT_UPGRADES_IMG_PG2.png',
  'TUT_UPGRADES_IMG_PG3.png',
]);
