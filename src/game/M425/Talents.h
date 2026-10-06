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

#ifndef MANGOS_M425_TALENTS_H
#define MANGOS_M425_TALENTS_H

#include "Common.h"
#include "Player.h"
#include "Policies/Singleton.h"
#include <array>
#include <unordered_map>
#include <vector>

// 425 talents (spell_talent) taught by the Haradon class trainers. A rank costs one point, a character has
// level - FIRST_LEVEL + 1 points, a talent needs required_points spent in its tree and its required talents at
// their last rank (MapServer Player::ActiveTalent). The ranks a character holds are read from its spells.
namespace M425
{
    uint32 const SPELL_FIRST = 40000;

    inline bool IsHaradonSpell(uint32 spellId) { return spellId >= SPELL_FIRST; }

    struct Talent
    {
        uint32 id = 0;
        uint32 class_ = 0;
        uint32 tree = 0;
        uint32 requiredPoints = 0;
        std::array<uint32, 3> requiredTalents = {};
        std::vector<std::vector<uint32>> ranks;             // [rank - 1] -> spells learned at that rank
    };

    class TalentManager
    {
        public:
            static uint32 const FIRST_LEVEL = 5;

            void LoadFromDB();

            // talent and rank (1-based) the spell teaches, nullptr if it is no Haradon talent
            Talent const* FindBySpell(uint32 spellId, uint32* rank = nullptr) const;

            uint32 GetRank(Player const* player, Talent const& talent) const;
            uint32 GetSpentPoints(Player const* player, uint32 tree = 0) const;
            uint32 GetTotalPoints(Player const* player) const;

            TrainerSpellState GetTrainerSpellState(Player const* player, uint32 spellId) const;
            bool Learn(Player* player, uint32 spellId) const;
            // forgets every talent of the character's class, false if none was learned
            bool Unlearn(Player* player) const;

        private:
            std::unordered_map<uint32, Talent> m_talents;
            std::unordered_map<uint32, std::pair<uint32, uint32>> m_spellRanks;     // spell -> talent, rank
            std::unordered_map<uint32, std::vector<uint32>> m_classTalents;          // class -> talents
    };
}

#define sM425Talents MaNGOS::Singleton<M425::TalentManager>::Instance()

#endif
