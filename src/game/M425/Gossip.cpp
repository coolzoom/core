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

// 425 NPC chat menus (MapServer ChatHandler.cpp HandleGossipHello / HandleGossipChoose / HandleGossipBack) on the
// vmangos gossip window.

#include "Automat.h"
#include "Player.h"
#include "Creature.h"
#include "GossipDef.h"
#include "WorldSession.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"

namespace M425
{
    namespace
    {
        std::vector<MenuCondition> ParseConditions(std::string const& text)
        {
            // q<quest>[i] / a<spell>, '!' negates (M425Patch Gossip.cs)
            std::vector<MenuCondition> result;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find(',', pos);
                std::string token = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
                pos = end == std::string::npos ? text.size() : end + 1;
                MenuCondition cond {};
                size_t i = 0;
                if (i < token.size() && token[i] == '!')
                {
                    cond.negate = true;
                    ++i;
                }
                if (i >= token.size() || (token[i] != 'q' && token[i] != 'a'))
                    continue;
                cond.aura = token[i++] == 'a';
                cond.id = uint32(strtoul(token.c_str() + i, nullptr, 10));
                cond.inLog = !token.empty() && token.back() == 'i';
                result.push_back(cond);
            }
            return result;
        }

        bool Holds(MenuCondition const& cond, Player* player)
        {
            bool value;
            if (cond.aura)
                value = player->HasAura(cond.id);
            else
            {
                // TextParserImpl has_quest: in the log and not finished yet, or in the log at all with inlog=1
                QuestStatus status = player->GetQuestStatus(cond.id);
                value = status == QUEST_STATUS_INCOMPLETE || (cond.inLog && status == QUEST_STATUS_COMPLETE);
            }
            return value != cond.negate;
        }
    }

    void AutomatManager::LoadGossip()
    {
        m_menus.clear();
        m_creatureMenus.clear();

        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `id`, `script1`, `script2`, `script3`, `script4`, `script5`, `script6`, "
            "`menu1`, `menu2`, `menu3`, `menu4`, `menu5`, `menu6` FROM `m425_chat_menu`"))
        {
            do
            {
                Field* f = result->Fetch();
                ChatMenu& menu = m_menus[f[0].GetUInt32()];
                for (int i = 0; i < 6; ++i)
                {
                    menu.scripts[i] = f[1 + i].GetUInt32();
                    menu.subMenus[i] = f[7 + i].GetUInt32();
                }
            } while (result->NextRow());
        }

        std::map<std::pair<uint32, uint32>, size_t> variantIndex;
        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `menu`, `variant`, `conditions`, `npc_text` FROM `m425_menu_variant` ORDER BY `menu`, `variant`"))
        {
            do
            {
                Field* f = result->Fetch();
                auto menu = m_menus.find(f[0].GetUInt32());
                if (menu == m_menus.end())
                    continue;
                variantIndex[{ f[0].GetUInt32(), f[1].GetUInt32() }] = menu->second.variants.size();
                menu->second.variants.push_back({ ParseConditions(f[2].GetCppString()), f[3].GetUInt32(), {} });
            } while (result->NextRow());
        }

        uint32 options = 0;
        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `menu`, `variant`, `label`, `scripts`, `link` FROM `m425_menu_option` ORDER BY `menu`, `variant`, `option`"))
        {
            do
            {
                Field* f = result->Fetch();
                auto index = variantIndex.find({ f[0].GetUInt32(), f[1].GetUInt32() });
                if (index == variantIndex.end())
                    continue;
                MenuOption option { f[2].GetCppString(), {}, f[4].GetInt32() };
                std::string scripts = f[3].GetCppString();
                for (char const* p = scripts.c_str(); *p; )
                {
                    char* end;
                    unsigned long slot = strtoul(p, &end, 10);
                    if (end == p)
                        break;
                    if (slot >= 1 && slot <= 6)
                        option.scripts.push_back(uint8(slot));
                    p = *end ? end + 1 : end;
                }
                m_menus[f[0].GetUInt32()].variants[index->second].options.push_back(std::move(option));
                ++options;
            } while (result->NextRow());
        }

        if (std::unique_ptr<QueryResult> result = WorldDatabase.Query("SELECT `entry`, `menu` FROM `m425_creature_menu`"))
        {
            do
            {
                Field* f = result->Fetch();
                if (m_menus.count(f[1].GetUInt32()))
                    m_creatureMenus[f[0].GetUInt32()] = f[1].GetUInt32();
            } while (result->NextRow());
        }

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> M425: %u chat menus (%u options), %u creatures with a menu",
            uint32(m_menus.size()), options, uint32(m_creatureMenus.size()));
    }

    void AutomatManager::ShowMenu(Player* player, Creature* creature, uint32 menuId, bool root)
    {
        auto menu = m_menus.find(menuId);
        if (menu == m_menus.end())
        {
            player->PlayerTalkClass->CloseGossip();
            return;
        }

        MenuVariant const* variant = nullptr;
        for (MenuVariant const& v : menu->second.variants)
        {
            if (std::all_of(v.conditions.begin(), v.conditions.end(), [player](MenuCondition const& c) { return Holds(c, player); }))
            {
                variant = &v;
                break;
            }
        }

        PlayerMenu* talk = player->PlayerTalkClass;
        talk->ClearMenus();
        if (root)
        {
            player->PrepareQuestMenu(creature->GetObjectGuid());
            if (creature->IsVendor())
                talk->GetGossipMenu().AddMenuItem(GOSSIP_ICON_VENDOR, "我想看看你的货物。", GOSSIP_SENDER_MENU + menuId, GOSSIP_ACTION_VENDOR, "");
            if (creature->IsTrainerOf(player, false))
                talk->GetGossipMenu().AddMenuItem(GOSSIP_ICON_TRAINER, "我想学习天赋。", GOSSIP_SENDER_MENU + menuId, GOSSIP_ACTION_TRAINER, "");
            if (creature->CanTrainAndResetTalentsOf(player))
                talk->GetGossipMenu().AddMenuItem(GOSSIP_ICON_TRAINER, "我想重置我的天赋。", GOSSIP_SENDER_MENU + menuId, GOSSIP_ACTION_UNLEARN_TALENTS, "");
        }
        if (variant)
            for (uint32 i = 0; i < variant->options.size(); ++i)
                talk->GetGossipMenu().AddMenuItem(GOSSIP_ICON_CHAT, variant->options[i].label, GOSSIP_SENDER_MENU + menuId, i, "");
        talk->SendGossipMenu(variant ? variant->npcText : 0, creature->GetObjectGuid());
    }

    bool AutomatManager::OnGossipHello(Player* player, Creature* creature)
    {
        auto menu = m_creatureMenus.find(creature->GetEntry());
        if (menu == m_creatureMenus.end())
            return false;
        {
            std::lock_guard<std::mutex> lock(m_menuLock);
            m_menuStacks[player->GetObjectGuid()] = { menu->second };
        }
        ShowMenu(player, creature, menu->second, true);
        return true;
    }

    bool AutomatManager::OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action)
    {
        if (sender < GOSSIP_SENDER_MENU || !m_creatureMenus.count(creature->GetEntry()))
            return false;
        uint32 const menuId = sender - GOSSIP_SENDER_MENU;
        auto menu = m_menus.find(menuId);
        if (menu == m_menus.end())
            return false;

        if (action == GOSSIP_ACTION_VENDOR)
        {
            player->PlayerTalkClass->CloseGossip();
            player->GetSession()->SendListInventory(creature->GetObjectGuid());
            return true;
        }
        if (action == GOSSIP_ACTION_TRAINER)
        {
            player->PlayerTalkClass->CloseGossip();
            player->GetSession()->SendTrainerList(creature->GetObjectGuid());
            return true;
        }
        if (action == GOSSIP_ACTION_UNLEARN_TALENTS)
        {
            player->PlayerTalkClass->CloseGossip();
            player->SendTalentWipeConfirm(creature->GetObjectGuid());
            return true;
        }

        // the option list of the variant that is showing; the conditions may have changed since, as in 425
        MenuVariant const* variant = nullptr;
        for (MenuVariant const& v : menu->second.variants)
        {
            if (std::all_of(v.conditions.begin(), v.conditions.end(), [player](MenuCondition const& c) { return Holds(c, player); }))
            {
                variant = &v;
                break;
            }
        }
        if (!variant || action >= variant->options.size())
        {
            player->PlayerTalkClass->CloseGossip();
            return true;
        }
        MenuOption const option = variant->options[action];

        // DlgNPCChat::HyperlinkCallback: the link's scripts first, then its menu jump
        for (uint8 slot : option.scripts)
            RunOnce(menu->second.scripts[slot - 1], creature, player, creature->GetMap());

        uint32 next = 0;
        bool root = false;
        if (option.link > 0 && option.link <= 6 && menu->second.subMenus[option.link - 1])
        {
            next = menu->second.subMenus[option.link - 1];
            std::lock_guard<std::mutex> lock(m_menuLock);
            m_menuStacks[player->GetObjectGuid()].push_back(next);
        }
        else if (option.link == 0)
        {
            std::lock_guard<std::mutex> lock(m_menuLock);
            std::vector<uint32>& stack = m_menuStacks[player->GetObjectGuid()];
            if (stack.size() > 1)
                stack.pop_back();
            if (!stack.empty())
            {
                next = stack.back();
                root = stack.size() == 1;
            }
        }

        if (next && player->IsInWorld() && player->IsWithinDistInMap(creature, INTERACTION_DISTANCE))
            ShowMenu(player, creature, next, root);
        else
        {
            player->PlayerTalkClass->CloseGossip();
            std::lock_guard<std::mutex> lock(m_menuLock);
            m_menuStacks.erase(player->GetObjectGuid());
        }
        return true;
    }
}
