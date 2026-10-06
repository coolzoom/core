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

#include "Talents.h"
#include "M425.h"
#include "SpellMgr.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"
#include "Policies/SingletonImp.h"

INSTANTIATE_SINGLETON_1(M425::TalentManager);

namespace M425
{
    void TalentManager::LoadFromDB()
    {
        m_talents.clear();
        m_spellRanks.clear();
        m_classTalents.clear();

        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `talent`, `class`, `tree`, `required_points`, `required_talent1`, `required_talent2`, `required_talent3` FROM `m425_talent`"))
        {
            do
            {
                Field* f = result->Fetch();
                Talent talent;
                talent.id = f[0].GetUInt32();
                talent.class_ = f[1].GetUInt32();
                talent.tree = f[2].GetUInt32();
                talent.requiredPoints = f[3].GetUInt32();
                for (uint32 i = 0; i < talent.requiredTalents.size(); ++i)
                    talent.requiredTalents[i] = f[4 + i].GetUInt32();
                m_talents[talent.id] = talent;
            } while (result->NextRow());
        }

        uint32 spells = 0;
        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `talent`, `rank`, `spell` FROM `m425_talent_spell` ORDER BY `talent`, `rank`, `slot`"))
        {
            do
            {
                Field* f = result->Fetch();
                uint32 const talentId = f[0].GetUInt32();
                uint32 const rank = f[1].GetUInt32();
                uint32 const spellId = f[2].GetUInt32();
                auto talent = m_talents.find(talentId);
                if (talent == m_talents.end() || !rank || !sSpellMgr.GetSpellEntry(spellId))
                {
                    sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Table `m425_talent_spell` has unknown talent %u or spell %u, skipped.", talentId, spellId);
                    continue;
                }
                if (talent->second.ranks.size() < rank)
                    talent->second.ranks.resize(rank);
                talent->second.ranks[rank - 1].push_back(spellId);
                m_spellRanks.emplace(spellId, std::make_pair(talentId, rank));
                ++spells;
            } while (result->NextRow());
        }

        for (auto itr = m_talents.begin(); itr != m_talents.end();)
        {
            // a rank without spells can't be detected on the character, the talent would never reach its last rank
            bool const valid = !itr->second.ranks.empty() &&
                std::none_of(itr->second.ranks.begin(), itr->second.ranks.end(), [](std::vector<uint32> const& r) { return r.empty(); });
            if (!valid)
            {
                sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Haradon talent %u has a rank without spells, skipped.", itr->first);
                for (auto const& rank : itr->second.ranks)
                    for (uint32 spellId : rank)
                        m_spellRanks.erase(spellId);
                itr = m_talents.erase(itr);
                continue;
            }
            m_classTalents[itr->second.class_].push_back(itr->first);
            ++itr;
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> M425: %u talents (%u spells)", uint32(m_talents.size()), spells);
    }

    Talent const* TalentManager::FindBySpell(uint32 spellId, uint32* rank) const
    {
        auto itr = m_spellRanks.find(spellId);
        if (itr == m_spellRanks.end())
            return nullptr;
        if (rank)
            *rank = itr->second.second;
        return &m_talents.at(itr->second.first);
    }

    uint32 TalentManager::GetRank(Player const* player, Talent const& talent) const
    {
        for (uint32 rank = uint32(talent.ranks.size()); rank > 0; --rank)
            if (player->HasSpell(talent.ranks[rank - 1].front()))
                return rank;
        return 0;
    }

    uint32 TalentManager::GetSpentPoints(Player const* player, uint32 tree) const
    {
        auto talents = m_classTalents.find(player->GetClass());
        if (talents == m_classTalents.end())
            return 0;
        uint32 points = 0;
        for (uint32 id : talents->second)
        {
            Talent const& talent = m_talents.at(id);
            if (!tree || talent.tree == tree)
                points += GetRank(player, talent);
        }
        return points;
    }

    uint32 TalentManager::GetTotalPoints(Player const* player) const
    {
        return player->GetLevel() >= FIRST_LEVEL ? player->GetLevel() - FIRST_LEVEL + 1 : 0;
    }

    TrainerSpellState TalentManager::GetTrainerSpellState(Player const* player, uint32 spellId) const
    {
        uint32 rank = 0;
        Talent const* talent = FindBySpell(spellId, &rank);
        if (!talent || !IsHaradonRace(player->GetRace()) || talent->class_ != player->GetClass())
            return TRAINER_SPELL_RED;

        uint32 const current = GetRank(player, *talent);
        if (current >= rank)
            return TRAINER_SPELL_GRAY;

        // the tree requirement and the required talents only count when the talent is new, as in ActiveTalent
        if (!current)
        {
            if (GetSpentPoints(player, talent->tree) < talent->requiredPoints)
                return TRAINER_SPELL_RED;
            for (uint32 required : talent->requiredTalents)
            {
                if (!required)
                    continue;
                auto itr = m_talents.find(required);
                if (itr == m_talents.end() || GetRank(player, itr->second) < itr->second.ranks.size())
                    return TRAINER_SPELL_RED;
            }
        }

        uint32 const spent = GetSpentPoints(player);
        uint32 const total = GetTotalPoints(player);
        if (spent > total || total - spent < rank - current)
            return TRAINER_SPELL_RED;
        return TRAINER_SPELL_GREEN;
    }

    bool TalentManager::Learn(Player* player, uint32 spellId) const
    {
        if (GetTrainerSpellState(player, spellId) != TRAINER_SPELL_GREEN)
            return false;
        uint32 rank = 0;
        Talent const* talent = FindBySpell(spellId, &rank);
        if (uint32 const current = GetRank(player, *talent))
            for (uint32 old : talent->ranks[current - 1])
                player->RemoveSpell(old, false, false);
        for (uint32 learn : talent->ranks[rank - 1])
            player->LearnSpell(learn, false);
        return true;
    }

    bool TalentManager::Unlearn(Player* player) const
    {
        auto talents = m_classTalents.find(player->GetClass());
        if (talents == m_classTalents.end())
            return false;
        bool any = false;
        for (uint32 id : talents->second)
        {
            Talent const& talent = m_talents.at(id);
            uint32 const rank = GetRank(player, talent);
            if (!rank)
                continue;
            for (uint32 spellId : talent.ranks[rank - 1])
            {
                player->RemoveAurasDueToSpell(spellId);
                player->RemoveSpell(spellId, false, false);
            }
            any = true;
        }
        return any;
    }
}
