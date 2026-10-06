# Haradon storyline (m425)

This branch lets one vmangos world server host two storylines:

- **Azeroth**: the unchanged 1.12 game (races 1-8).
- **Haradon**: the 425 game (混沌与秩序 4.2.5), with its races, maps, creatures, quests and scripts
  ported onto vmangos (races 10-14).

Characters of different storylines are always hostile to each other. NPCs keep their own factions.

## Installing

The server data is generated from the 425 client/server package by the `M425Patch` tool in the
NetCoreClient repository:

```bash
VMANGOS=/path/to/vmangos \
VMANGOS_DATA=/path/to/vmangos/data \
CLIENT_BUILD=5875 \
WORLD_DB=mangos \
NetCoreClient/tools/build-m425.sh --apply
```

The script:

1. copies the server DBCs it changes to `dbc.orig/` next to `dbc/` on the first run, and extends
   them (always starting from those originals) with the 425 races, spells, factions, maps, areas
   and display infos;
2. writes the world SQL to `sql/custom/m425/` (`00`-`89` install, `99_uninstall.sql` removes
   everything again);
3. exports the 425 terrain (heights, holes, water) to `maps/6xxAABB.map` and builds `mmaps/` for those maps
   with `MoveMapGenerator` (`--skip-mmaps` keeps the existing ones);
4. with `--apply`, imports the install files into `$WORLD_DB`.

`build-m425.sh --restore --apply` puts the original DBCs back, removes the 6xx maps and mmaps and runs
`99_uninstall.sql`.

The 425 maps have no vmaps: line of sight is never blocked there and buildings are not walkable surfaces for
the server, so creatures standing on 425 building models can be placed at ground height.

## Id ranges

| Kind | Range |
|---|---|
| Maps | 600+ |
| Spells | 40000+ |
| Quests | 90000+ |
| Items | 100000+ |
| Creatures / game objects | 900000+ |
| Creature / game object guids | 9000000 + 425 spawn id |
| Quest credit creatures | 1000000 + quest × 4 + objective |
| Faction templates | 2002 hostile, 2003 friendly, 2006 neutral |
| `npc_text` / `broadcast_text` | 900000+ |

## Server changes

- `SharedDefines.h`: the Haradon races and race masks.
- `M425/M425.*`: storyline lookups (`IsHaradonRace`, `IsHaradonMap`, `IsCrossStoryline`), loaded
  race/class creation rules and the configuration.
- Character creation (`CharacterHandler.cpp`): the Haradon race/class combinations,
  `M425.CharacterCreating`, and the one-side-per-account rule applied per storyline. Start
  positions come from `playercreateinfo` in the generated SQL.
- Hostility (`Object.cpp`, `Creature.cpp`, `GridNotifiers.*`): players of different storylines are
  hostile; creatures are not affected.
- Separation between storylines, each controlled by an `M425.CrossStoryline.*` option: chat
  (garbled like cross-faction chat), channels, groups, guilds, trade, auction houses, mail, who
  list, friends and battlegrounds.
- `M425/Automat.*`, `M425/AutomatAI.*`, `M425/AutomatFunctions.cpp`: the 425 script virtual
  machine. It runs the original graph scripts from `m425_automat`:
  - creatures with `ai_name = 'M425AutomatAI'` run their idle/combat/dead scripts and waypoints;
  - type-102 game object spawns from `m425_gameobject_trigger` run enter/leave scripts;
  - quest accept/complete/objective scripts from `m425_quest_script`;
  - dungeon checkpoint scripts from `m425_map_script`.

  vmangos still does aggro, threat, chasing and evading; the scripts do spells, talk, summons,
  quest credit, waypoints and dungeon progress.
- `M425/Gossip.cpp`: the 425 NPC chat menus (`m425_creature_menu`, `m425_menu_variant`,
  `m425_menu_option`). The generator renders every combination of a menu's quest/aura conditions
  into its own `npc_text`; the server shows the variant whose conditions hold, runs an option's
  chat menu scripts and follows its submenu/back/close link. Vendors and trainers get their
  service options on the root menu.
- `M425/Talents.*`: 425 talents are taught by the Haradon class trainers (`m425_talent`,
  `m425_talent_spell`, `npc_trainer`). Trainers list the talent spells themselves; a rank costs one
  point, a character has level − 4 points, and a new talent needs its tree's required points and
  its required talents at their last rank. The ranks are read from the character's spells, so
  nothing extra is saved. A new rank replaces the previous rank's spells, and the class trainer's
  "reset talents" option forgets them all (the usual respec cost applies). Hooks are in
  `ObjectMgr::LoadTrainers`, `WorldSession::SendTrainerList` / `HandleTrainerBuySpellOpcode`,
  `Player::GetTrainerSpellState` and `Player::ResetTalents`.

## Configuration

See the `HARADON STORYLINE (M425)` section of `mangosd.conf`. By default new characters may pick
either storyline and every cross-storyline feature is disabled.

## Known limitations

- 425 creatures keep their weapons in the display model, there is no `creature_equip_template` data.
- Ranged attack script functions return 0; emote and animation functions do nothing.
- Legend mode and exhaustion functions return 0.
- `HasCreatureInMap` by entry only searches 250 yards around the script owner.
- Script chat type 1 is a yell, everything else a say.
- Text emotes with `own_team_only` are only filtered by team.
- Battleground queueing only checks the group leader's storyline.
- `AllowTwoSide.Interaction.Chat` also bypasses the cross-storyline garbling.
