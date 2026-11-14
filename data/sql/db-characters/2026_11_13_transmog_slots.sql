-- Slot-bound transmog looks (Transmogrification.WardrobeUI.SlotBound): the look a character picked for each
-- equipment slot. The visual itself is still carried per item by `custom_transmogrification`.
CREATE TABLE IF NOT EXISTS `custom_transmogrification_slots` (
  `Owner` int(10) unsigned NOT NULL COMMENT 'Player guidLow',
  `Slot` tinyint(3) unsigned NOT NULL COMMENT 'EQUIPMENT_SLOT_*',
  `FakeEntry` int(10) unsigned NOT NULL COMMENT 'Item entry, 1 = hidden',
  PRIMARY KEY (`Owner`, `Slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
