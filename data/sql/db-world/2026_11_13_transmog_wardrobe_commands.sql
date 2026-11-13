-- Help for the `.transmog wardrobe` debug commands (wardrobe UI protocol without a client).

DELETE FROM `command` WHERE `name` IN ('transmog wardrobe', 'transmog wardrobe list', 'transmog wardrobe cur');
INSERT INTO `command` (`name`, `security`, `help`) VALUES
('transmog wardrobe', 2, 'Syntax: .transmog wardrobe $subcommand\nType .transmog wardrobe to see the list of possible subcommands or .help transmog wardrobe $subcommand to see info on subcommands.'),
('transmog wardrobe list', 2, 'Syntax: .transmog wardrobe list <slot>\nPrints the wardrobe UI messages a LIST request for <slot> would send for the selected player (or yourself). <slot> uses the client numbering: 1 = head ... 19 = tabard.'),
('transmog wardrobe cur', 2, 'Syntax: .transmog wardrobe cur\nPrints the wardrobe UI CUR message (current look of every transmoggable slot) for the selected player (or yourself).');
