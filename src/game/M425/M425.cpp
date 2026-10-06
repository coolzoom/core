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

#include "M425.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "World.h"
#include "DBCStores.h"
#include "Database/DatabaseEnv.h"
#include "Config/Config.h"
#include "Log.h"
#include "Utilities/Random.h"
#include "Policies/SingletonImp.h"

INSTANTIATE_SINGLETON_1(M425::Manager);

namespace M425
{
    bool IsCrossStoryline(WorldObject const* a, WorldObject const* b)
    {
        Player const* playerA = a->GetAffectingPlayer();
        Player const* playerB = b->GetAffectingPlayer();
        return playerA && playerB && !IsSameStoryline(playerA->GetRace(), playerB->GetRace());
    }

    void Manager::LoadConfig()
    {
        m_characterCreating = uint8(sConfig.GetIntDefault("M425.CharacterCreating", CREATING_BOTH));
        if (m_characterCreating > CREATING_HARADON_ONLY)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "M425.CharacterCreating (%u) must be 0, 1 or 2, using 0.", m_characterCreating);
            m_characterCreating = CREATING_BOTH;
        }

        static char const* const names[MAX_INTERACTION] =
        {
            "Chat", "Channel", "Group", "Guild", "Trade", "Auction", "Mail", "WhoList", "AddFriend", "Battleground"
        };
        for (uint32 i = 0; i < MAX_INTERACTION; ++i)
            m_crossStoryline[i] = sConfig.GetBoolDefault((std::string("M425.CrossStoryline.") + names[i]).c_str(), false);
    }

    void Manager::LoadFromDB()
    {
        m_classRules.clear();
        m_classLevelStats.clear();
        m_xpForLevel.clear();
        m_appearance.clear();

        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `class`, `power_type`, `max_power`, `start_power` FROM `m425_class`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 class_ = fields[0].GetUInt32();
                ClassRules rules;
                rules.powerType = fields[1].GetUInt8();
                rules.maxPower = fields[2].GetUInt16();
                rules.startPower = fields[3].GetUInt16();
                if (!sChrClassesStore.LookupEntry(class_) || rules.powerType >= MAX_POWERS)
                {
                    sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Table `m425_class` has invalid class %u or power type %u, skipped.", class_, rules.powerType);
                    continue;
                }
                m_classRules[class_] = rules;
            }
            while (result->NextRow());
        }

        uint32 const maxLevel = sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL);
        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `class`, `level`, `basehp`, `basemana` FROM `m425_classlevelstats`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 class_ = fields[0].GetUInt32();
                uint32 level = fields[1].GetUInt32();
                if (!level || level > maxLevel)
                    continue;
                auto& stats = m_classLevelStats[class_];
                if (stats.size() < maxLevel)
                    stats.resize(maxLevel, { 0, 0 });
                stats[level - 1] = { fields[2].GetUInt16(), fields[3].GetUInt16() };
            }
            while (result->NextRow());
        }
        for (auto& itr : m_classLevelStats)
        {
            // fill gaps from the previous level so a missing row never yields 0 health
            for (uint32 level = 1; level < itr.second.size(); ++level)
                if (!itr.second[level].health)
                    itr.second[level] = itr.second[level - 1];
            if (!itr.second[0].health)
            {
                sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Table `m425_classlevelstats` has no level 1 row for class %u, Haradon characters of this class use the WoW values.", itr.first);
                itr.second.clear();
            }
        }

        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `lvl`, `xp_for_next_level` FROM `m425_xp_for_level`"))
        {
            m_xpForLevel.resize(maxLevel + 1, 0);
            do
            {
                Field* fields = result->Fetch();
                uint32 level = fields[0].GetUInt32();
                if (level < m_xpForLevel.size())
                    m_xpForLevel[level] = fields[1].GetUInt32();
            }
            while (result->NextRow());
        }

        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `race`, `gender`, `skin`, `face`, `hair_style`, `hair_color`, `facial_hair` FROM `m425_appearance`"))
        {
            do
            {
                Field* fields = result->Fetch();
                AppearanceLimits limits;
                limits.skin = fields[2].GetUInt8();
                limits.face = fields[3].GetUInt8();
                limits.hairStyle = fields[4].GetUInt8();
                limits.hairColor = fields[5].GetUInt8();
                limits.facialHair = fields[6].GetUInt8();
                m_appearance[fields[0].GetUInt32() | (fields[1].GetUInt32() << 8)] = limits;
            }
            while (result->NextRow());
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded Haradon rules: %u classes, %u class level tables, %u XP levels, %u appearance sets",
            uint32(m_classRules.size()), uint32(m_classLevelStats.size()), uint32(m_xpForLevel.size()), uint32(m_appearance.size()));
    }

    bool Manager::IsCreationAllowed(uint32 race) const
    {
        switch (m_characterCreating)
        {
            case CREATING_AZEROTH_ONLY:
                return !IsHaradonRace(race);
            case CREATING_HARADON_ONLY:
                return IsHaradonRace(race);
        }
        return true;
    }

    bool Manager::CanInteract(Player const* a, Player const* b, Interaction interaction) const
    {
        return CanInteract(a->GetRace(), b->GetRace(), interaction);
    }

    bool Manager::CanInteract(uint32 raceA, ObjectGuid const& b, Interaction interaction) const
    {
        PlayerCacheData const* data = sObjectMgr.GetPlayerDataByGUID(b.GetCounter());
        return !data || CanInteract(raceA, data->uiRace, interaction);
    }

    ClassRules const* Manager::GetClassRules(uint32 race, uint32 class_) const
    {
        if (!IsHaradonRace(race))
            return nullptr;
        auto itr = m_classRules.find(class_);
        return itr != m_classRules.end() ? &itr->second : nullptr;
    }

    uint8 Manager::GetPowerType(uint32 race, uint32 class_) const
    {
        if (ClassRules const* rules = GetClassRules(race, class_))
            return rules->powerType;
        ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(class_);
        return entry && entry->powerType < MAX_POWERS ? uint8(entry->powerType) : uint8(POWER_MANA);
    }

    uint32 Manager::GetStartPower(uint32 race, uint32 class_, uint8 powerType) const
    {
        if (ClassRules const* rules = GetClassRules(race, class_))
            if (rules->powerType == powerType)
                return rules->startPower;
        return powerType == POWER_ENERGY ? 100 : 0;
    }

    bool Manager::GetClassLevelInfo(uint32 race, uint32 class_, uint32 level, PlayerClassLevelInfo* info) const
    {
        if (!IsHaradonRace(race) || !level)
            return false;
        auto itr = m_classLevelStats.find(class_);
        if (itr == m_classLevelStats.end() || itr->second.empty())
            return false;
        BaseStats const& stats = itr->second[std::min<size_t>(level, itr->second.size()) - 1];
        info->basehealth = stats.health;
        info->basemana = stats.mana;
        return true;
    }

    uint32 Manager::GetXPForLevel(uint32 race, uint32 level) const
    {
        if (!IsHaradonRace(race) || m_xpForLevel.empty())
            return 0;
        return m_xpForLevel[std::min<size_t>(level, m_xpForLevel.size() - 1)];
    }

    bool Manager::ValidateAppearance(uint32 race, uint8 gender, uint8 hairStyle, uint8 hairColor, uint8 face, uint8 facialHair, uint8 skin) const
    {
        auto itr = m_appearance.find(race | (gender << 8));
        if (itr == m_appearance.end())
            return false;
        AppearanceLimits const& limits = itr->second;
        return skin <= limits.skin && face <= limits.face && hairStyle <= limits.hairStyle &&
               hairColor <= limits.hairColor && facialHair <= limits.facialHair;
    }

    void Manager::SelectRandomAppearance(uint32 race, uint8 gender, uint8& hairStyle, uint8& hairColor, uint8& face, uint8& facialHair, uint8& skin) const
    {
        hairStyle = hairColor = face = facialHair = skin = 0;
        auto itr = m_appearance.find(race | (gender << 8));
        if (itr == m_appearance.end())
            return;
        AppearanceLimits const& limits = itr->second;
        auto pick = [](uint8 max) { return max ? uint8(urand(1, max)) : uint8(0); };
        skin = pick(limits.skin);
        face = pick(limits.face);
        hairStyle = pick(limits.hairStyle);
        hairColor = pick(limits.hairColor);
        facialHair = pick(limits.facialHair);
    }
}
