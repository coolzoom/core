/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MANGOS_M425_H
#define MANGOS_M425_H

#include "Common.h"
#include "SharedDefines.h"
#include "ObjectGuid.h"
#include "Policies/Singleton.h"
#include <unordered_map>
#include <vector>

class Player;
class WorldObject;
struct PlayerClassLevelInfo;

// Second storyline (Haradon, ported from the 425 game) next to Azeroth.
// Races 10-14 are Haradon; Light (10, 11, 14) plays as ALLIANCE and Dark (12, 13) as HORDE inside it,
// players of different storylines are always hostile, and most social features stay within one storyline.
// World data lives in sql/custom/m425, the server DBC rows come from NetCoreClient/tools/build-m425.sh.
namespace M425
{
    enum Storyline : uint8
    {
        STORYLINE_AZEROTH = 0,
        STORYLINE_HARADON = 1,
    };

    enum CharacterCreating : uint8
    {
        CREATING_BOTH         = 0,
        CREATING_AZEROTH_ONLY = 1,
        CREATING_HARADON_ONLY = 2,
    };

    // Features that are blocked between storylines unless M425.CrossStoryline.<name> = 1.
    enum Interaction : uint8
    {
        INTERACTION_CHAT,
        INTERACTION_CHANNEL,
        INTERACTION_GROUP,
        INTERACTION_GUILD,
        INTERACTION_TRADE,
        INTERACTION_AUCTION,
        INTERACTION_MAIL,
        INTERACTION_WHO_LIST,
        INTERACTION_ADD_FRIEND,
        INTERACTION_BATTLEGROUND,
        MAX_INTERACTION
    };

    uint32 const CLASSMASK_HARADON =
        (1 << (CLASS_WARRIOR - 1)) | (1 << (CLASS_HUNTER - 1)) | (1 << (CLASS_PRIEST - 1)) | (1 << (CLASS_MAGE - 1));

    uint32 const MAP_FIRST = 600;
    uint32 const MAP_LAST  = 699;
    uint32 const CREATURE_FIRST = 900000;
    uint32 const CREATURE_LAST  = 999999;
    // AuctionHouse.dbc row added by build-m425.sh, one market for both Haradon sides
    uint32 const AUCTION_HOUSE = 8;

    inline bool IsHaradonRace(uint32 race) { return race >= RACE_M425_HUMAN && race <= RACE_M425_MENDEL; }
    inline Storyline StorylineForRace(uint32 race) { return IsHaradonRace(race) ? STORYLINE_HARADON : STORYLINE_AZEROTH; }
    inline bool IsHaradonMap(uint32 mapId) { return mapId >= MAP_FIRST && mapId <= MAP_LAST; }
    inline bool IsHaradonCreature(uint32 entry) { return entry >= CREATURE_FIRST && entry <= CREATURE_LAST; }
    inline bool IsSameStoryline(uint32 raceA, uint32 raceB) { return StorylineForRace(raceA) == StorylineForRace(raceB); }
    // NPC services (trainers, auctioneers) are bound to the storyline the NPC comes from
    inline bool IsNpcOfStoryline(uint32 creatureEntry, uint32 playerRace)
    {
        return IsHaradonRace(playerRace) == IsHaradonCreature(creatureEntry);
    }

    // Haradon races only play the four 425 classes; Azeroth races keep their playercreateinfo combinations.
    inline bool IsAllowedRaceClass(uint32 race, uint32 class_)
    {
        return !IsHaradonRace(race) || (class_ < MAX_CLASSES && ((1 << (class_ - 1)) & CLASSMASK_HARADON));
    }

    // Player owners of both objects (pets, totems and charmed units count as their owner) are in different storylines.
    bool IsCrossStoryline(WorldObject const* a, WorldObject const* b);

    struct ClassRules
    {
        uint8 powerType = 0;
        uint16 maxPower = 0;   // 0 = the usual Unit::GetCreatePowers value
        uint16 startPower = 0;
    };

    struct AppearanceLimits
    {
        uint8 skin = 0;
        uint8 face = 0;
        uint8 hairStyle = 0;
        uint8 hairColor = 0;
        uint8 facialHair = 0;
    };

    class Manager
    {
        public:
            void LoadConfig();
            void LoadFromDB();

            bool IsCreationAllowed(uint32 race) const;
            bool IsInteractionAllowed(Interaction interaction) const { return m_crossStoryline[interaction]; }
            // Same storyline, or the feature is allowed across storylines by config.
            bool CanInteract(uint32 raceA, uint32 raceB, Interaction interaction) const
            {
                return IsSameStoryline(raceA, raceB) || IsInteractionAllowed(interaction);
            }
            bool CanInteract(Player const* a, Player const* b, Interaction interaction) const;
            // other character may be offline, its race comes from the player cache
            bool CanInteract(uint32 raceA, ObjectGuid const& b, Interaction interaction) const;
            bool CanUseNpc(uint32 playerRace, uint32 creatureEntry, Interaction interaction) const
            {
                return IsNpcOfStoryline(creatureEntry, playerRace) || IsInteractionAllowed(interaction);
            }

            ClassRules const* GetClassRules(uint32 race, uint32 class_) const;
            // Powers value for the race/class pair, Haradon rules first, then ChrClasses.dbc.
            uint8 GetPowerType(uint32 race, uint32 class_) const;
            uint32 GetStartPower(uint32 race, uint32 class_, uint8 powerType) const;
            bool GetClassLevelInfo(uint32 race, uint32 class_, uint32 level, PlayerClassLevelInfo* info) const;
            // 0 when the race uses the WoW curve
            uint32 GetXPForLevel(uint32 race, uint32 level) const;

            bool ValidateAppearance(uint32 race, uint8 gender, uint8 hairStyle, uint8 hairColor, uint8 face, uint8 facialHair, uint8 skin) const;
            void SelectRandomAppearance(uint32 race, uint8 gender, uint8& hairStyle, uint8& hairColor, uint8& face, uint8& facialHair, uint8& skin) const;

        private:
            uint8 m_characterCreating = CREATING_BOTH;
            bool m_crossStoryline[MAX_INTERACTION] = {};

            std::unordered_map<uint32, ClassRules> m_classRules;
            struct BaseStats { uint16 health; uint16 mana; };
            std::unordered_map<uint32, std::vector<BaseStats>> m_classLevelStats;            // class -> [level - 1]
            std::vector<uint32> m_xpForLevel;                                                // [level]
            std::unordered_map<uint32, AppearanceLimits> m_appearance;                       // race | gender << 8
    };
}

#define sM425 MaNGOS::Singleton<M425::Manager>::Instance()

#endif
