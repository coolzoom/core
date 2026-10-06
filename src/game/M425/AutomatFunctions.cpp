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

// The functions the 425 scripts call, after the executors of the 425 Scriptable.cpp (and the later
// additions of the 4.2.5 server). Argument 0 is the first argument in the script source. A function that
// answers a question pushes 1 or 0; one that only acts pushes nothing, as in the 425 server.

#include "Automat.h"
#include "AutomatAI.h"
#include "M425.h"
#include "Creature.h"
#include "GameObject.h"
#include "Player.h"
#include "Group.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Chat.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Timer.h"
#include "Utilities/Random.h"
#include <ctime>
#include <functional>
#include <list>

namespace M425
{
    namespace
    {
        using Fn = bool (*)(Script&, CallFunc const&);

        uint32 const MAX_TEAM_SHARE   = 100;          // TeamSystem::MAX_TEAM_SHARE, quest credit range for groups
        float const SIGHT_RANGE       = 20.0f;        // HasEnemyInSight without a range
        float const MAP_SEARCH_RANGE  = 250.0f;       // HasCreatureInMap by entry looks around, not map-wide
        uint32 const SUMMON_CORPSE_MS = 60000;
        uint32 const GOLD_CURRENCY    = 0;             // eGold

        //------------------------------------------------------------------------------
        // id mapping and lookups

        SpellEntry const* SpellOf(int32 id425)
        {
            return id425 > 0 ? sSpellMgr.GetSpellEntry(SPELL_OFFSET + uint32(id425)) : nullptr;
        }

        uint32 FactionOf(int32 type425)
        {
            switch (type425)
            {
                case 0: return FACTION_HOSTILE;
                case 1: return FACTION_FRIENDLY;
                default: return FACTION_NEUTRAL;
            }
        }

        Creature* CreatureBySpawn(Map* map, int32 spawnId)
        {
            if (!map || spawnId <= 0)
                return nullptr;
            uint32 low = CREATURE_GUID_OFFSET + uint32(spawnId);
            CreatureData const* data = sObjectMgr.GetCreatureData(low);
            return data ? map->GetCreature(data->GetObjectGuid(low)) : nullptr;
        }

        GameObject* GameObjectBySpawn(Map* map, int32 spawnId)
        {
            if (!map || spawnId <= 0)
                return nullptr;
            uint32 low = GAMEOBJECT_GUID_OFFSET + uint32(spawnId);
            GameObjectData const* data = sObjectMgr.GetGOData(low);
            return data ? map->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, data->id, low)) : nullptr;
        }

        AutomatAI* AutomatOf(Creature* creature)
        {
            return creature ? dynamic_cast<AutomatAI*>(creature->AI()) : nullptr;
        }

        // the player a quest function acts on: the slot's player, the slot creature's player victim, else slot 1
        Player* QuestPlayer(Script& s, int32 slot)
        {
            if (Player* player = s.GetPlayer(slot))
                return player;
            if (Creature* creature = s.GetCreature(slot))
                if (Unit* victim = creature->GetVictim())
                    if (Player* player = victim->ToPlayer())
                        return player;
            return slot != 1 ? s.GetPlayer(1) : nullptr;
        }

        std::string Text(uint32 id, CallFunc const& params, int32 numberArg, Player const* player)
        {
            std::string const* text = sM425Automat.GetText(id);
            if (!text)
                return std::string();
            std::string out;
            for (size_t i = 0; i < text->size(); ++i)
            {
                char c = (*text)[i];
                char n = i + 1 < text->size() ? (*text)[i + 1] : 0;
                if (c == '%' && (n == 'd' || n == 'u'))
                {
                    out += std::to_string(params.Integer(numberArg));
                    ++i;
                }
                else if (c == '%' && n == 's')
                {
                    out += player ? player->GetName() : std::to_string(params.Integer(numberArg));
                    ++i;
                }
                else
                    out += c;
            }
            return out;
        }

        bool IsTrainingDummy(Creature const* c)
        {
            return c->GetEntry() == CREATURE_OFFSET + 1226;
        }

        struct RangeCheck
        {
            WorldObject const* obj;
            float range;
            std::function<bool(Unit*)> pred;
            WorldObject const& GetFocusObject() const { return *obj; }
            bool operator()(Unit* u) { return u->IsAlive() && obj->IsWithinDistInMap(u, range) && pred(u); }
        };

        std::list<Unit*> UnitsInRange(WorldObject* obj, float range, std::function<bool(Unit*)> pred)
        {
            std::list<Unit*> units;
            RangeCheck check{ obj, range, std::move(pred) };
            MaNGOS::UnitListSearcher<RangeCheck> searcher(units, check);
            Cell::VisitAllObjects(obj, searcher, range);
            return units;
        }

        // C++ HasEnemyInSight(proto, false, range): a living hostile unit of that 425 entry (0 = any) in range
        Unit* FindEnemy(Creature* me, int32 proto, float range)
        {
            if (proto > 0)
            {
                Creature* c = me->FindNearestCreature(CREATURE_OFFSET + uint32(proto), range);
                return c && me->IsHostileTo(c) ? c : nullptr;
            }
            std::list<Unit*> units = UnitsInRange(me, range, [me](Unit* u) { return me->IsHostileTo(u) && u->IsTargetableBy(me); });
            return units.empty() ? nullptr : units.front();
        }

        bool PctMatches(float pct, int32 type, int32 check)
        {
            int32 per = std::max(0, std::min(100, int32(pct)));
            return (type == 0 && per < check) || (type == 1 && per > check);
        }

        bool HasPowerFor(Unit const* caster, SpellEntry const* spell)
        {
            return spell && caster->GetPower(Powers(spell->powerType)) >= spell->GetManaCost();
        }

        void Engage(Creature* me, Unit* target)
        {
            if (me && target && me->IsHostileTo(target) && me->AI())
                me->AI()->AttackStart(target);
        }

        // NormalCreateCreatureHook / DungeonCreateCreatureHook, then join the summoner's fight
        void AfterCreate(Script& s, Creature* summoned)
        {
            if (s.IsCheckPointScript())
            {
                if (DungeonData* dungeon = s.GetDungeon())
                    dungeon->objects.push_back(summoned->GetObjectGuid());
            }
            else
            {
                for (uint32 i = FIRST_SPAWN_SLOT; i < AUTOMAT_SLOTS; ++i)
                    if (!s.GetUnit(int32(i)))
                    {
                        s.SetUnit(summoned, i);
                        break;
                    }
            }
            Creature* me = s.GetMainCreature();
            if (me && me->IsInCombat() && me->GetVictim() && me->GetFactionTemplateId() == summoned->GetFactionTemplateId())
                Engage(summoned, me->GetVictim());
        }

        Creature* Summon(Script& s, int32 entry425, float x, float y, float z, float o)
        {
            uint32 entry = CREATURE_OFFSET + uint32(entry425);
            if (entry425 <= 0 || !sObjectMgr.GetCreatureTemplate(entry))
                return nullptr;
            Creature* summoned = nullptr;
            if (WorldObject* main = s.GetMain())
                summoned = main->SummonCreature(entry, x, y, z, o, TEMPSUMMON_CORPSE_TIMED_DESPAWN, SUMMON_CORPSE_MS);
            else if (Map* map = s.GetMap())
                summoned = map->SummonCreature(entry, x, y, z, o, TEMPSUMMON_CORPSE_TIMED_DESPAWN, SUMMON_CORPSE_MS);
            if (summoned)
                AfterCreate(s, summoned);
            return summoned;
        }

        Creature* SummonAtWaypoint(Script& s, int32 entry425, int32 waypointId)
        {
            Waypoint const* wp = sM425Automat.GetWaypoint(uint32(waypointId));
            if (!wp)
                return nullptr;
            Creature* summoned = Summon(s, entry425, wp->x, wp->y, wp->z, 0.0f);
            if (AutomatAI* ai = AutomatOf(summoned))
                ai->StartWaypoint(wp->id);
            return summoned;
        }

        Creature* SummonAtSpawn(Script& s, int32 entry425, int32 spawnId)
        {
            CreatureData const* data = sObjectMgr.GetCreatureData(CREATURE_GUID_OFFSET + uint32(std::max(0, spawnId)));
            if (!data)
                return nullptr;
            return Summon(s, entry425, data->position.x, data->position.y, data->position.z, data->position.o);
        }

        void CreditQuest(Player* player, int32 quest425, int32 objective)
        {
            if (!player || quest425 <= 0 || objective < 0 || objective >= 4)
                return;
            if (player->GetQuestStatus(QUEST_OFFSET + uint32(quest425)) == QUEST_STATUS_INCOMPLETE)
                player->KilledMonsterCredit(QuestCreditEntry(uint32(quest425), uint32(objective)));
        }

        void SendScriptString(Script& s, Player* player, std::string const& text, bool warning)
        {
            auto send = [&](Player* p)
            {
                if (warning)
                    p->GetSession()->SendNotification("%s", text.c_str());
                else
                    ChatHandler(p).SendSysMessage(text.c_str());
            };
            if (player)
                send(player);
            else if (Map* map = s.GetMap())
                if (map->IsDungeon())
                    for (auto const& ref : map->GetPlayers())
                        if (Player* p = ref.getSource())
                            send(p);
        }

        //------------------------------------------------------------------------------
        // LoadOtherScriptExecutor

        uint32 ScriptOr(uint32 script, uint32 fallback) { return script ? script : fallback; }

        bool RunAutomat(Script& s, CallFunc const& p) { s.LoadAutomat(uint32(p.Integer(0))); return true; }

        bool RunIdleScript(Script& s, CallFunc const&)
        {
            if (AutomatAI* ai = s.GetAI())
                s.KeepLastAndLoadAutomat(ScriptOr(ai->GetScripts() ? ai->GetScripts()->idle : 0, SCRIPT_IDLE));
            return true;
        }

        bool RunCombatScript(Script& s, CallFunc const&)
        {
            if (AutomatAI* ai = s.GetAI())
                s.KeepLastAndLoadAutomat(ScriptOr(ai->GetScripts() ? ai->GetScripts()->combat : 0, SCRIPT_COMBAT));
            return true;
        }

        bool RunDeadScript(Script& s, CallFunc const&)
        {
            if (AutomatAI* ai = s.GetAI())
                s.KeepLastAndLoadAutomat(ScriptOr(ai->GetScripts() ? ai->GetScripts()->dead : 0, SCRIPT_DEAD));
            return true;
        }

        //------------------------------------------------------------------------------
        // SystemScriptExecutor

        bool ValidTimer(int32 idx) { return idx >= 0 && uint32(idx) < AUTOMAT_TIMERS; }

        bool StartTimer(Script& s, CallFunc const& p)
        {
            int32 idx = p.Integer(0);
            if (ValidTimer(idx))
                s.m_timer[idx] = WorldTimer::getMSTime();
            return true;
        }

        bool StartCounter(Script& s, CallFunc const& p)
        {
            int32 idx = p.Integer(0);
            if (ValidTimer(idx))
                s.m_counter[idx] = 0;
            return true;
        }

        bool AddCounter(Script& s, CallFunc const& p)
        {
            int32 idx;
            if (p.TryGetInteger(0, idx) && ValidTimer(idx))
                ++s.m_counter[idx];
            return true;
        }

        bool HasTimerCounted(Script& s, CallFunc const& p)
        {
            int32 idx, target;
            uint32 now = WorldTimer::getMSTime();
            if (p.TryGetInteger(0, idx))
            {
                if (p.TryGetInteger(1, target))
                {
                    if (!ValidTimer(idx))
                        return true;
                    if (WorldTimer::getMSTimeDiff(s.m_timer[idx], now) >= uint32(target))
                    {
                        if (!p.Bool(2))
                            s.m_timer[idx] = now;
                        s.PushNumber(1);
                        return true;
                    }
                }
                else if (WorldTimer::getMSTimeDiff(s.m_timer[0], now) >= uint32(idx))
                {
                    // HasTimerCounted(ms): timer 0, restarted when it fires
                    s.m_timer[0] = now;
                    s.PushNumber(1);
                    return true;
                }
            }
            s.PushNumber(0);
            return true;
        }

        bool HasCounterCounted(Script& s, CallFunc const& p)
        {
            int32 idx, target;
            if (p.TryGetInteger(0, idx) && p.TryGetInteger(1, target))
            {
                if (!ValidTimer(idx))
                    return true;
                s.PushNumber(s.m_counter[idx] == target ? 1 : 0);
                return true;
            }
            s.PushNumber(0);
            return true;
        }

        bool CounterInRange(Script& s, CallFunc const& p)
        {
            int32 idx = p.Integer(0, -1);
            s.PushNumber(ValidTimer(idx) && s.m_counter[idx] >= p.Integer(1, -1) && s.m_counter[idx] < p.Integer(2, -1) ? 1 : 0);
            return true;
        }

        bool RandomCounter(Script& s, CallFunc const& p)
        {
            int32 idx = p.Integer(0, -1);
            int32 max = p.Integer(1, -1);
            if (ValidTimer(idx) && max > 1)
                s.m_counter[idx] = int32(urand(0, uint32(max - 1)));
            return true;
        }

        bool SetSOState(Script& s, CallFunc const& p)
        {
            WorldObject* obj = s.GetUnit(p.Integer(0));
            if (GameObject* go = obj ? obj->ToGameObject() : nullptr)
                go->SetGoState(p.Integer(1) ? GO_STATE_ACTIVE : GO_STATE_READY);
            return true;
        }

        bool AddMember(Script& s, CallFunc const& p)
        {
            int32 spawnId, slot;
            if (p.TryGetInteger(0, spawnId) && p.TryGetInteger(1, slot) && slot >= 0)
                if (Creature* c = CreatureBySpawn(s.GetMap(), spawnId))
                    s.SetUnit(c, uint32(slot));
            return true;
        }

        bool AddSOMember(Script& s, CallFunc const& p)
        {
            int32 spawnId, slot;
            if (p.TryGetInteger(0, spawnId) && p.TryGetInteger(1, slot) && slot >= 0)
                if (GameObject* go = GameObjectBySpawn(s.GetMap(), spawnId))
                    s.SetUnit(go, uint32(slot));
            return true;
        }

        bool SendMessage(Script& s, CallFunc const& p)
        {
            int32 strId;
            if (!p.TryGetInteger(1, strId))
                return true;
            Player* player = s.GetPlayer(1);
            std::string text = Text(uint32(strId), p, 2, player);
            if (!text.empty())
                SendScriptString(s, player, text, false);
            return true;
        }

        bool SendWarning(Script& s, CallFunc const& p)
        {
            Player* player = s.GetPlayer(p.Integer(2, 1));
            std::string text = Text(uint32(p.Integer(1, 16)), p, 3, player);
            if (player && !text.empty())
                SendScriptString(s, player, text, true);
            return true;
        }

        //------------------------------------------------------------------------------
        // QuestScriptExecutor

        bool HasQuest(Script& s, CallFunc const& p)
        {
            int32 quest = p.Integer(0, -1);
            Player* player = quest > 0 ? QuestPlayer(s, p.Integer(1)) : nullptr;
            s.PushNumber(player && player->GetQuestStatus(QUEST_OFFSET + uint32(quest)) == QUEST_STATUS_INCOMPLETE ? 1 : 0);
            return true;
        }

        bool HasQuestFinished(Script& s, CallFunc const& p)
        {
            int32 quest;
            Player* player = p.TryGetInteger(0, quest) && quest > 0 ? QuestPlayer(s, p.Integer(1, 1)) : nullptr;
            s.PushNumber(player && player->GetQuestRewardStatus(QUEST_OFFSET + uint32(quest)) ? 1 : 0);
            return true;
        }

        bool GivePlayerItem(Script& s, CallFunc const& p)
        {
            int32 item;
            if (p.TryGetInteger(0, item) && item > 0)
                if (Player* player = QuestPlayer(s, p.Integer(2)))
                    player->StoreNewItemInBestSlots(ITEM_OFFSET + uint32(item), uint32(std::max(1, p.Integer(1, 1))));
            return true;
        }

        bool TakePlayerItem(Script& s, CallFunc const& p)
        {
            int32 item;
            if (p.TryGetInteger(0, item) && item > 0)
            {
                uint32 count = uint32(std::max(1, p.Integer(1, 1)));
                Player* player = QuestPlayer(s, p.Integer(2));
                if (player && player->HasItemCount(ITEM_OFFSET + uint32(item), count))
                {
                    player->DestroyItemCount(ITEM_OFFSET + uint32(item), count, true);
                    s.PushNumber(1);
                    return true;
                }
            }
            s.PushNumber(0);
            return true;
        }

        bool HasItem(Script& s, CallFunc const& p)
        {
            int32 item;
            Player* player = p.TryGetInteger(0, item) && item > 0 ? QuestPlayer(s, p.Integer(3)) : nullptr;
            if (player)
            {
                bool more = p.Bool(1, true);
                int32 num = p.Integer(2, 1);
                int32 count = int32(player->GetItemCount(ITEM_OFFSET + uint32(item)));
                s.PushNumber((more && count >= num) || (!more && count < num) ? 1 : 0);
                return true;
            }
            s.PushNumber(0);
            return true;
        }

        bool SetQuestStatus(Script& s, CallFunc const& p)
        {
            int32 quest;
            if (p.TryGetInteger(0, quest) && quest > 0 && p.Integer(1) == 0)
                if (Player* player = s.GetPlayer(p.Integer(2, 1)))
                    if (player->GetQuestStatus(QUEST_OFFSET + uint32(quest)) == QUEST_STATUS_INCOMPLETE)
                        player->FailQuest(QUEST_OFFSET + uint32(quest));
            return true;
        }

        bool UpdateQuest(Script& s, CallFunc const& p)
        {
            int32 quest;
            if (p.TryGetInteger(0, quest))
                CreditQuest(s.GetPlayer(p.Integer(2, 1)), quest, p.Integer(1));
            return true;
        }

        bool UpdateTeamQuest(Script& s, CallFunc const& p)
        {
            int32 quest;
            Player* player = p.TryGetInteger(0, quest) ? s.GetPlayer(p.Integer(2, 1)) : nullptr;
            if (!player)
                return true;
            Group* group = player->GetGroup();
            if (!group)
            {
                CreditQuest(player, quest, p.Integer(1));
                return true;
            }
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->getSource())
                    if (member->IsInMap(player) && member->IsWithinDistInMap(player, MAX_TEAM_SHARE))
                        CreditQuest(member, quest, p.Integer(1));
            return true;
        }

        bool UpdateQuestPlayerQuest(Script& s, CallFunc const& p)
        {
            int32 quest;
            AutomatAI* ai = s.GetAI();
            Creature* me = s.GetMainCreature();
            if (!p.TryGetInteger(0, quest) || !ai || !me)
                return true;
            for (ObjectGuid const& guid : ai->m_questPlayers)
                if (Player* player = me->GetMap()->GetPlayer(guid))
                    if (me->IsWithinDistInMap(player, MAX_TEAM_SHARE))
                        CreditQuest(player, quest, p.Integer(1));
            ai->m_questPlayers.clear();
            return true;
        }

        bool SetQuestPlayer(Script& s, CallFunc const& p)
        {
            int32 slot;
            AutomatAI* ai = s.GetAI();
            Player* player = p.TryGetInteger(0, slot) ? s.GetPlayer(slot) : nullptr;
            if (!ai || !player)
                return true;
            ai->m_questPlayers.clear();
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->getSource())
                        ai->m_questPlayers.push_back(member->GetObjectGuid());
            }
            else
                ai->m_questPlayers.push_back(player->GetObjectGuid());
            return true;
        }

        bool AddQuestPlayer(Script& s, CallFunc const& p)
        {
            int32 idx;
            AutomatAI* ai = s.GetAI();
            int32 slot = p.Integer(1, 1);
            if (!ai || !p.TryGetInteger(0, idx) || idx < 0 || uint32(idx) >= ai->m_questPlayers.size() || slot < 0)
                return true;
            if (Player* player = s.GetMap() ? s.GetMap()->GetPlayer(ai->m_questPlayers[idx]) : nullptr)
                s.SetUnit(player, uint32(slot));
            return true;
        }

        bool IsLevelInRange(Script& s, CallFunc const& p)
        {
            if (Player* player = s.GetPlayer(1))
            {
                uint32 level = player->GetLevel();
                s.PushNumber(uint32(p.Integer(0)) <= level && level <= uint32(p.Integer(1)) ? 1 : 0);
            }
            return true;
        }

        // legend (hard) dungeon mode and exhaustion do not exist in vmangos
        bool PushZeroForPlayer(Script& s, CallFunc const&)
        {
            if (s.GetPlayer(1))
                s.PushNumber(0);
            return true;
        }

        //------------------------------------------------------------------------------
        // CheckPointScriptExecutor (dungeons only, unknown elsewhere as in the 425 server)

        bool CheckCheckPoint(Script& s, CallFunc const& p)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            int32 target;
            s.PushNumber(p.TryGetInteger(0, target) && d->checkPoint == target ? 1 : 0);
            return true;
        }

        bool IncreaseCheckPoint(Script& s, CallFunc const&)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            if (d->checkPoint < 64)
                ++d->checkPoint;
            return true;
        }

        bool DecreaseCheckPoint(Script& s, CallFunc const&)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            if (d->checkPoint > 0)
                --d->checkPoint;
            return true;
        }

        bool SetCheckPoint(Script& s, CallFunc const& p)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            int32 step;
            if (p.TryGetInteger(0, step) && step <= 64)
                d->checkPoint = step;
            return true;
        }

        bool KeyCreaturesDied(Script& s, CallFunc const& p)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            int32 type = p.Integer(0);
            bool cleared = true;
            for (ObjectGuid& guid : d->objects)
            {
                Creature* c = guid ? s.GetMap()->GetCreature(guid) : nullptr;
                if (!c || !c->IsAlive())
                {
                    guid.Clear();
                    continue;
                }
                // 1 = only hostile creatures count, 2 = only friendly ones
                if ((type == 1 && c->GetFactionTemplateId() != FACTION_HOSTILE) || (type == 2 && c->GetFactionTemplateId() != FACTION_FRIENDLY))
                    continue;
                cleared = false;
                break;
            }
            s.PushNumber(cleared ? 1 : 0);
            return true;
        }

        bool IsLegend(Script& s, CallFunc const&)
        {
            if (!s.GetDungeon())
                return false;
            s.PushNumber(0);
            return true;
        }

        bool DungeonValue(Script& s, CallFunc const& p, int32 cmp)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            int32 value = d->values[p.Integer(0)];
            int32 other = p.Integer(1);
            s.PushNumber((cmp == 0 && value == other) || (cmp > 0 && value > other) || (cmp < 0 && value < other) ? 1 : 0);
            return true;
        }

        bool SetDungeonValue(Script& s, CallFunc const& p)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            d->values[p.Integer(0)] = p.Integer(1);
            return true;
        }

        bool IncreaseDungeonValue(Script& s, CallFunc const& p)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            d->values[p.Integer(0)] += p.Integer(1, 1);
            return true;
        }

        bool DecreaseDungeonValue(Script& s, CallFunc const& p)
        {
            DungeonData* d = s.GetDungeon();
            if (!d)
                return false;
            d->values[p.Integer(0)] -= p.Integer(1, 1);
            return true;
        }

        bool DungeonFinish(Script& s, CallFunc const&)
        {
            return s.GetDungeon() != nullptr;
        }

        //------------------------------------------------------------------------------
        // CreateCreatureScriptExecutor

        // CreateCreature(entry, wp) | (entry, 0, wp) | (entry, 1, dx, dy, dz) | (entry, 2, spawnId)
        bool CreateCreature(Script& s, CallFunc const& p)
        {
            int32 entry;
            if (!p.TryGetInteger(0, entry))
                return true;
            if (p.Size() == 2)
            {
                SummonAtWaypoint(s, entry, p.Integer(1));
                return true;
            }
            switch (p.Integer(1))
            {
                case 0:
                    SummonAtWaypoint(s, entry, p.Integer(2));
                    break;
                case 1:
                    if (p.Size() == 5 && s.GetMain())
                    {
                        // the offset is in 425 axes (x, height, z), the server's are (-z, -x, height)
                        WorldObject* main = s.GetMain();
                        Summon(s, entry, main->GetPositionX() - p.Float(4), main->GetPositionY() - p.Float(2),
                            main->GetPositionZ() + p.Float(3), main->GetOrientation());
                    }
                    break;
                case 2:
                    if (p.Size() == 3)
                        SummonAtSpawn(s, entry, p.Integer(2));
                    break;
            }
            return true;
        }

        bool AddCreatureById(Script& s, CallFunc const& p)
        {
            s.PushNumber(SummonAtSpawn(s, p.Integer(0), p.Integer(1)) ? 1 : 0);
            return true;
        }

        //------------------------------------------------------------------------------
        // CreatureScriptExecutor

        bool StopMeleeAttack(Script& s, CallFunc const&)
        {
            if (Creature* me = s.GetMainCreature())
                if (me->AI())
                    me->AI()->SetMeleeAttack(false);
            return true;
        }

        bool MeleeAttack(Script& s, CallFunc const&)
        {
            if (Creature* me = s.GetMainCreature())
                if (me->AI())
                    me->AI()->SetMeleeAttack(true);
            return true;
        }

        bool EnableMeleeAttack(Script& s, CallFunc const& p)
        {
            if (Creature* c = s.GetCreature(p.Integer(1)))
                if (c->AI())
                    c->AI()->SetMeleeAttack(p.Bool(0));
            return true;
        }

        bool HasEnemyInSight(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && !IsTrainingDummy(me) && me->IsInCombat() ? 1 : 0);
            return true;
        }

        bool BeingAttacked(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && !IsTrainingDummy(me) && me->IsInCombat() && !me->GetThreatManager().isThreatListEmpty() ? 1 : 0);
            return true;
        }

        bool MoveToTarget(Script& s, CallFunc const& p)
        {
            Creature* me = s.GetMainCreature();
            bool move;
            if (me && me->AI() && p.TryGetBool(0, move))
                me->AI()->SetCombatMovement(move);
            return true;
        }

        bool Escape(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            if (me && me->GetVictim())
                me->GetMotionMaster()->MoveFleeing(me->GetVictim(), 5000);
            return true;
        }

        bool EscapeFinished(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            if (!me)
            {
                s.PushNumber(0);
                return true;
            }
            s.PushNumber(me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FLEEING_MOTION_TYPE ? 1 : 0);
            return true;
        }

        bool CallHelp(Script& s, CallFunc const& p)
        {
            float range;
            if (Creature* me = s.GetMainCreature())
                if (p.TryGetFloat(0, range))
                    me->CallForHelp(range);
            return true;
        }

        bool ChangeFaction(Script& s, CallFunc const& p)
        {
            int32 faction;
            if (p.TryGetInteger(0, faction))
                if (Creature* c = s.GetCreature(p.Integer(1)))
                    c->SetFactionTemplateId(FactionOf(faction));
            return true;
        }

        bool IsFaction(Script& s, CallFunc const& p)
        {
            int32 faction;
            Creature* c = p.TryGetInteger(0, faction) ? s.GetCreature(p.Integer(1)) : nullptr;
            s.PushNumber(c && c->GetFactionTemplateId() == FactionOf(faction) ? 1 : 0);
            return true;
        }

        bool MeleeAttackInRange(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && me->GetVictim() && me->CanReachWithMeleeAutoAttack(me->GetVictim()) ? 1 : 0);
            return true;
        }

        bool MeleeAttackOutRange(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && me->GetVictim() && !me->CanReachWithMeleeAutoAttack(me->GetVictim()) ? 1 : 0);
            return true;
        }

        // the 425 ranged auto attack has no vmangos counterpart; those creatures fight in melee
        bool PushZero(Script& s, CallFunc const&) { s.PushNumber(0); return true; }
        bool PushOne(Script& s, CallFunc const&) { s.PushNumber(1); return true; }
        bool Nothing(Script&, CallFunc const&) { return true; }

        bool IsDead(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && !me->IsAlive() ? 1 : 0);
            return true;
        }

        bool IsTargetDisappeared(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && (!me->GetVictim() || !me->GetVictim()->IsAlive()) ? 1 : 0);
            return true;
        }

        bool IsTargetDead(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && (!me->GetVictim() || !me->GetVictim()->IsAlive()) && !me->IsInCombat() ? 1 : 0);
            return true;
        }

        bool OutOfTrackingRange(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && (!me->GetVictim() || me->IsInEvadeMode()) ? 1 : 0);
            return true;
        }

        bool Kill(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            if (me && me->IsAlive())
                me->DoKillUnit();
            return true;
        }

        bool Idle(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            if (me && me->IsAlive() && me->IsInCombat() && me->AI())
                me->AI()->EnterEvadeMode();
            return true;
        }

        void Say(Creature* speaker, std::string const& text, int32 type, Unit const* target)
        {
            if (text.empty())
                return;
            if (type == 1)
                speaker->MonsterYell(text.c_str(), LANG_UNIVERSAL, target);
            else
                speaker->MonsterSay(text.c_str(), LANG_UNIVERSAL, target);
        }

        bool ShowDialog(Script& s, CallFunc const& p)
        {
            int32 type, strId;
            if (Creature* me = s.GetMainCreature())
                if (p.TryGetInteger(0, type) && p.TryGetInteger(1, strId))
                    Say(me, Text(uint32(strId), p, 2, s.GetPlayer(1)), type, nullptr);
            return true;
        }

        // Chat(start, target, string, type, emote): target 64+ = nobody
        bool Chat(Script& s, CallFunc const& p)
        {
            int32 start, target, idx;
            if (!p.TryGetInteger(0, start) || !p.TryGetInteger(1, target) || !p.TryGetInteger(2, idx))
                return true;
            Creature* speaker = s.GetCreature(start);
            if (!speaker || !speaker->IsAlive())
                return true;
            Unit* listener = nullptr;
            if (target >= 0 && uint32(target) < AUTOMAT_SLOTS)
            {
                listener = s.GetUnitAsUnit(target);
                if (!listener || !listener->IsAlive())
                    return true;
                speaker->SetFacingToObject(listener);
            }
            Say(speaker, Text(uint32(idx), p, 5, listener ? listener->ToPlayer() : s.GetPlayer(1)), p.Integer(3), listener);
            return true;
        }

        bool PercentCondition(Script& s, CallFunc const& p, Unit* unit, bool health)
        {
            int32 type, check;
            if (unit && p.TryGetInteger(0, type) && p.TryGetInteger(1, check))
            {
                float pct = health ? unit->GetHealthPercent() : unit->GetPowerPercent(unit->GetPowerType());
                s.PushNumber(PctMatches(pct, type, check) ? 1 : 0);
                return true;
            }
            s.PushNumber(0);
            return true;
        }

        Unit* Victim(Script& s)
        {
            Creature* me = s.GetMainCreature();
            return me ? me->GetVictim() : nullptr;
        }

        bool InHPCondition(Script& s, CallFunc const& p) { return PercentCondition(s, p, s.GetUnitAsUnit(0), true); }
        bool InMPCondition(Script& s, CallFunc const& p) { return PercentCondition(s, p, s.GetUnitAsUnit(0), false); }
        bool InTargetHPCondition(Script& s, CallFunc const& p) { return PercentCondition(s, p, Victim(s), true); }
        bool InTargetMPCondition(Script& s, CallFunc const& p) { return PercentCondition(s, p, Victim(s), false); }

        bool IsInRange(Script& s, CallFunc const& p)
        {
            float range;
            Unit* victim = Victim(s);
            s.PushNumber(victim && p.TryGetFloat(0, range) && s.GetMain()->IsWithinDistInMap(victim, range) ? 1 : 0);
            return true;
        }

        bool FollowTarget(Script& s, CallFunc const& p)
        {
            Creature* me = s.GetMainCreature();
            bool follow;
            if (!me || !p.TryGetBool(0, follow))
                return true;
            Unit* target = me->GetVictim() ? me->GetVictim() : s.GetUnitAsUnit(1);
            if (follow && target)
                me->GetMotionMaster()->MoveFollow(target, 2.0f, M_PI_F);
            else if (!follow && me->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveIdle();
            return true;
        }

        // CastSpell(spell, caster slot, target slot = the caster's target)
        bool CastSpell(Script& s, CallFunc const& p)
        {
            int32 spellId;
            SpellEntry const* spell = p.TryGetInteger(0, spellId) ? SpellOf(spellId) : nullptr;
            int32 start = p.Integer(1);
            int32 targetSlot = p.Integer(2, -1);
            Unit* caster = s.GetUnitAsUnit(start);
            if (!spell || !caster)
                return true;
            Unit* target = nullptr;
            if (targetSlot >= 0)
            {
                target = s.GetUnitAsUnit(targetSlot);
                if (!target)
                    return true;
            }
            else
            {
                AutomatAI* ai = start == 0 ? s.GetAI() : nullptr;
                if (ai && ai->m_friendTarget)
                    target = caster->GetMap()->GetUnit(ai->m_friendTarget);
                if (!target)
                    target = caster->GetVictim() ? caster->GetVictim() : caster;
            }
            caster->CastSpell(target, spell, false);
            if (AutomatAI* ai = s.GetAI())
                ai->m_friendTarget.Clear();
            return true;
        }

        bool CastSpellToPositionOfTarget(Script& s, CallFunc const& p)
        {
            int32 spellId;
            SpellEntry const* spell = p.TryGetInteger(0, spellId) ? SpellOf(spellId) : nullptr;
            Unit* me = s.GetUnitAsUnit(0);
            if (spell && me && me->GetVictim())
                me->CastSpell(me->GetVictim()->GetPositionX(), me->GetVictim()->GetPositionY(), me->GetVictim()->GetPositionZ(), spell, false);
            return true;
        }

        bool LearnSpell(Script& s, CallFunc const& p)
        {
            int32 spellId;
            SpellEntry const* spell = p.TryGetInteger(0, spellId) ? SpellOf(spellId) : nullptr;
            Unit* start = s.GetUnitAsUnit(p.Integer(1));
            int32 targetSlot = p.Integer(2);
            Unit* target = targetSlot >= 0 ? s.GetUnitAsUnit(targetSlot) : (start ? start->GetVictim() : nullptr);
            Player* player = target ? target->ToPlayer() : nullptr;
            if (spell && player && !player->HasSpell(spell->Id))
            {
                player->LearnSpell(spell->Id, false);
                s.PushNumber(1);
                return true;
            }
            s.PushNumber(0);
            return true;
        }

        bool HasSpellAura(Script& s, CallFunc const& p)
        {
            int32 id;
            Unit* unit = p.TryGetInteger(0, id) && id > 0 ? s.GetUnitAsUnit(p.Integer(1)) : nullptr;
            s.PushNumber(unit && unit->HasAura(SPELL_OFFSET + uint32(id)) ? 1 : 0);
            return true;
        }

        bool SelectRandomTarget(Script& s, CallFunc const& p)
        {
            Creature* me = s.GetMainCreature();
            bool noCurrent;
            if (!me || !p.TryGetBool(0, noCurrent))
                return true;
            float range = p.Float(2, 30.0f);
            std::vector<Unit*> candidates;
            for (HostileReference* ref : me->GetThreatManager().getThreatList())
                if (Unit* u = ref->getTarget())
                    if (u->IsAlive() && me->IsWithinDistInMap(u, range) && !(noCurrent && u == me->GetVictim()))
                        candidates.push_back(u);
            if (candidates.empty())
                return true;
            Unit* target = candidates[urand(0, uint32(candidates.size() - 1))];
            // take the top of the threat list so the switch holds
            float top = me->GetVictim() ? me->GetThreatManager().getThreat(me->GetVictim()) : 0.0f;
            me->GetThreatManager().addThreatDirectly(target, top + 1.0f);
            me->AI()->AttackStart(target);
            return true;
        }

        bool ClearHatred(Script& s, CallFunc const& p)
        {
            if (Creature* c = s.GetCreature(p.Integer(0)))
                for (HostileReference* ref : c->GetThreatManager().getThreatList())
                    ref->setThreat(0.0f);
            return true;
        }

        bool TargetInSpellRange(Script& s, CallFunc const& p)
        {
            int32 spellId;
            Creature* me = s.GetMainCreature();
            SpellEntry const* spell = p.TryGetInteger(0, spellId) ? SpellOf(spellId) : nullptr;
            Unit* victim = me ? me->GetVictim() : nullptr;
            s.PushNumber(spell && victim && spell->IsTargetInRange(me, victim) && me->HasInArc(victim) ? 1 : 0);
            return true;
        }

        bool HasPower(Script& s, CallFunc const& p)
        {
            int32 spellId;
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && p.TryGetInteger(0, spellId) && HasPowerFor(me, SpellOf(spellId)) ? 1 : 0);
            return true;
        }

        bool CastingSpell(Script& s, CallFunc const& p)
        {
            Creature* me = s.GetMainCreature();
            int32 spellId = p.Integer(0);
            bool casting = false;
            if (me && me->IsNonMeleeSpellCasted(false))
            {
                if (spellId == 0)
                    casting = true;
                else
                    for (uint32 type : { CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL })
                        if (Spell* spell = me->GetCurrentSpell(CurrentSpellTypes(type)))
                            casting |= spell->m_spellInfo->Id == SPELL_OFFSET + uint32(spellId);
            }
            s.PushNumber(casting ? 1 : 0);
            return true;
        }

        bool CanCastSpell(Script& s, CallFunc const& p)
        {
            int32 spellId;
            Creature* me = s.GetMainCreature();
            SpellEntry const* spell = p.TryGetInteger(0, spellId) ? SpellOf(spellId) : nullptr;
            if (!me || !spell || me->IsNonMeleeSpellCasted(false) || !HasPowerFor(me, spell) ||
                me->HasUnitState(UNIT_STATE_CAN_NOT_REACT_OR_LOST_CONTROL))
            {
                s.PushNumber(0);
                return true;
            }
            Unit* victim = me->GetVictim();
            if (victim && Spells::GetSpellMaxRange(sSpellRangeStore.LookupEntry(spell->rangeIndex)) > 0.0f &&
                (!spell->IsTargetInRange(me, victim) || !me->HasInArc(victim)))
            {
                s.PushNumber(0);
                return true;
            }
            s.PushNumber(1);
            return true;
        }

        bool HasFriendInCondition(Script& s, CallFunc const& p)
        {
            Creature* me = s.GetMainCreature();
            float range;
            if (!me || IsTrainingDummy(me) || !p.TryGetFloat(0, range))
            {
                s.PushNumber(0);
                return true;
            }
            int32 condition = p.Integer(1);
            int32 type = p.Integer(2);
            int32 value = p.Integer(3);
            bool setTarget = p.Bool(4);
            std::list<Unit*> friends = UnitsInRange(me, range, [&](Unit* u)
            {
                if (!me->IsFriendlyTo(u))
                    return false;
                float pct = condition == 0 ? u->GetHealthPercent() : u->GetPowerPercent(u->GetPowerType());
                return PctMatches(pct, type, value);
            });
            if (friends.empty())
            {
                s.PushNumber(0);
                return true;
            }
            if (setTarget)
                if (AutomatAI* ai = s.GetAI())
                    ai->m_friendTarget = friends.front()->GetObjectGuid();
            s.PushNumber(1);
            return true;
        }

        bool HasCreatureInMap(Script& s, CallFunc const& p)
        {
            WorldObject* main = s.GetMain();
            bool bySpawn;
            int32 id;
            if (!main || !p.TryGetBool(0, bySpawn) || !p.TryGetInteger(1, id))
            {
                s.PushNumber(0);
                return true;
            }
            float dist = p.Float(2, 9999999.0f);
            Creature* c = bySpawn ? CreatureBySpawn(s.GetMap(), id)
                : (id > 0 ? main->FindNearestCreature(CREATURE_OFFSET + uint32(id), std::min(dist, MAP_SEARCH_RANGE)) : nullptr);
            s.PushNumber(c && c->IsAlive() && main->GetDistance(c) <= dist ? 1 : 0);
            return true;
        }

        // HasCreatureInRange(proto, range, slot?) / HasCreature(proto, slot?): with a slot, fight that creature
        bool HasCreatureCommon(Script& s, CallFunc const& p, float range, int32 slotArg)
        {
            Creature* me = s.GetMainCreature();
            int32 proto, slot;
            if (!me || !p.TryGetInteger(0, proto))
            {
                s.PushNumber(0);
                return true;
            }
            if (p.TryGetInteger(slotArg, slot))
            {
                Creature* target = s.GetCreature(slot);
                if (target && target->IsAlive())
                {
                    Engage(me, target);
                    s.PushNumber(1);
                    return true;
                }
            }
            else if (!IsTrainingDummy(me))
            {
                if (Unit* enemy = FindEnemy(me, proto, range))
                {
                    if (!me->IsInCombat())
                        Engage(me, enemy);
                    s.PushNumber(1);
                    return true;
                }
            }
            s.PushNumber(0);
            return true;
        }

        bool HasCreatureInRange(Script& s, CallFunc const& p) { return HasCreatureCommon(s, p, p.Float(1, SIGHT_RANGE), 2); }
        bool HasCreature(Script& s, CallFunc const& p) { return HasCreatureCommon(s, p, SIGHT_RANGE, 1); }

        bool CanHunt(Script& s, CallFunc const& p)
        {
            Creature* me = s.GetMainCreature();
            int32 proto;
            Unit* enemy = me && !IsTrainingDummy(me) && !me->IsInCombat() && p.TryGetInteger(0, proto) ? FindEnemy(me, proto, SIGHT_RANGE) : nullptr;
            if (enemy)
                Engage(me, enemy);
            s.PushNumber(enemy ? 1 : 0);
            return true;
        }

        bool CanFindEnemyInRange(Script& s, CallFunc const& p)
        {
            Creature* me = s.GetMainCreature();
            s.PushNumber(me && FindEnemy(me, 0, p.Float(0)) ? 1 : 0);
            return true;
        }

        bool IsType(Script& s, CallFunc const& p)
        {
            int32 type;
            WorldObject* obj = p.TryGetInteger(0, type) ? s.GetUnit(p.Integer(1)) : nullptr;
            bool match = false;
            if (obj && obj->ToCreature())
                match = obj->GetEntry() == CREATURE_OFFSET + uint32(type);
            else if (obj && obj->ToGameObject())
                match = obj->GetEntry() == GAMEOBJECT_OFFSET + uint32(type);
            s.PushNumber(match ? 1 : 0);
            return true;
        }

        bool IsTargetType(Script& s, CallFunc const& p)
        {
            int32 type;
            Unit* victim = p.TryGetInteger(0, type) ? Victim(s) : nullptr;
            s.PushNumber(victim && victim->ToCreature() && victim->GetEntry() == CREATURE_OFFSET + uint32(type) ? 1 : 0);
            return true;
        }

        bool Disappear(Script& s, CallFunc const& p)
        {
            WorldObject* obj = s.GetUnit(p.Integer(0));
            if (Creature* c = obj ? obj->ToCreature() : nullptr)
                c->DespawnOrUnsummon();
            else if (GameObject* go = obj ? obj->ToGameObject() : nullptr)
                go->Despawn();
            return true;
        }

        bool ChangeDisplay(Script& s, CallFunc const& p)
        {
            int32 entry;
            Creature* me = s.GetMainCreature();
            if (me && p.TryGetInteger(0, entry) && entry > 0 && sObjectMgr.GetCreatureTemplate(CREATURE_OFFSET + uint32(entry)))
                me->UpdateEntry(CREATURE_OFFSET + uint32(entry));
            return true;
        }

        bool DisableNPC(Script& s, CallFunc const& p)
        {
            bool disable;
            Creature* me = s.GetMainCreature();
            AutomatAI* ai = s.GetAI();
            if (!me || !ai || !p.TryGetBool(0, disable) || disable == ai->m_npcDisabled)
                return true;
            ai->m_npcDisabled = disable;
            if (disable)
            {
                ai->m_disabledNpcFlags = me->GetUInt32Value(UNIT_NPC_FLAGS);
                me->SetUInt32Value(UNIT_NPC_FLAGS, 0);
            }
            else
                me->SetUInt32Value(UNIT_NPC_FLAGS, ai->m_disabledNpcFlags);
            return true;
        }

        bool CostMoney(Script& s, CallFunc const& p)
        {
            int32 cost, slot;
            Player* player = p.TryGetInteger(0, cost) && p.TryGetInteger(1, slot) ? s.GetPlayer(slot) : nullptr;
            if (!player)
            {
                s.PushNumber(0);
                return true;
            }
            if (cost > 0 && player->GetMoney() < uint32(cost))
            {
                player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, nullptr, 0, 0);
                s.PushNumber(0);
                return true;
            }
            player->ModifyMoney(-cost);
            s.PushNumber(1);
            return true;
        }

        bool CanCastModCurrency(Script& s, CallFunc const& p)
        {
            Player* player = s.GetMain() ? s.GetMain()->ToPlayer() : nullptr;
            s.PushNumber(player && uint32(p.Integer(0)) == GOLD_CURRENCY && int64(player->GetMoney()) + p.Integer(1) >= 0 ? 1 : 0);
            return true;
        }

        bool CreateStaticObj(Script& s, CallFunc const& p)
        {
            if (GameObject* go = GameObjectBySpawn(s.GetMap(), p.Integer(0)))
                if (!go->isSpawned())
                    go->Respawn();
            return true;
        }

        //------------------------------------------------------------------------------
        // later 4.2.5 additions

        bool IsInCombat(Script& s, CallFunc const& p)
        {
            Unit* unit = s.GetUnitAsUnit(p.Integer(0));
            s.PushNumber(unit && unit->IsInCombat() ? 1 : 0);
            return true;
        }

        bool PauseMove(Script& s, CallFunc const&)
        {
            if (Creature* me = s.GetMainCreature())
                me->StopMoving();
            return true;
        }

        bool ChangeHpPct(Script& s, CallFunc const& p)
        {
            Unit* unit = s.GetUnitAsUnit(p.Integer(1));
            if (unit && unit->IsAlive())
                unit->SetHealth(std::max(1u, uint32(uint64(unit->GetMaxHealth()) * uint32(std::max(0, p.Integer(0))) / 100)));
            return true;
        }

        bool SetVisible(Script& s, CallFunc const& p)
        {
            if (Creature* c = s.GetCreature(p.Integer(0)))
                c->SetVisibility(p.Bool(1) ? VISIBILITY_ON : VISIBILITY_OFF);
            return true;
        }

        bool StartFearRun(Script& s, CallFunc const& p)
        {
            if (Creature* me = s.GetMainCreature())
                me->GetMotionMaster()->MoveRandom(true, p.Float(1, 15.0f));
            return true;
        }

        bool StopFearRun(Script& s, CallFunc const&)
        {
            Creature* me = s.GetMainCreature();
            if (me && me->GetMotionMaster()->GetCurrentMovementGeneratorType() == RANDOM_MOTION_TYPE)
                me->GetMotionMaster()->MoveIdle();
            return true;
        }

        // RunScriptInRangeObj(script, range, type): bit 1 players, bit 2 creatures, each as the main unit
        bool RunScriptInRangeObj(Script& s, CallFunc const& p)
        {
            uint32 scriptId = uint32(p.Integer(0));
            WorldObject* main = s.GetMain();
            if (!scriptId || !main)
                return true;
            int32 type = p.Integer(2, 1);
            std::list<Unit*> units = UnitsInRange(main, p.Float(1, 30.0f), [&](Unit* u)
            {
                return u != main && (((type & 1) && u->IsPlayer()) || ((type & 2) && u->IsCreature()));
            });
            for (Unit* u : units)
                sM425Automat.RunOnce(scriptId, u, u->ToPlayer() ? u->ToPlayer() : s.GetPlayer(1), s.GetMap());
            return true;
        }

        bool SetObjData(Script& s, CallFunc const& p)
        {
            if (AutomatAI* ai = AutomatOf(s.GetCreature(p.Integer(0))))
                ai->m_objData[p.Integer(1)] = p.Integer(2);
            return true;
        }

        bool ObjDataEqual(Script& s, CallFunc const& p)
        {
            AutomatAI* ai = AutomatOf(s.GetCreature(p.Integer(0)));
            s.PushNumber(ai && ai->m_objData[p.Integer(1)] == p.Integer(2) ? 1 : 0);
            return true;
        }

        bool IsDuringHourBetween(Script& s, CallFunc const& p)
        {
            int32 begin, end;
            if (!p.TryGetInteger(0, begin) || !p.TryGetInteger(1, end))
            {
                s.PushNumber(0);
                return true;
            }
            if (begin < 0 || begin > 24 || end < 0 || end > 24)
                return true;
            time_t now = time(nullptr);
            tm local = *localtime(&now);
            s.PushNumber(local.tm_hour >= begin && local.tm_hour < end ? 1 : 0);
            return true;
        }

        bool IsDayOfWeek(Script& s, CallFunc const& p)
        {
            time_t now = time(nullptr);
            int32 today = localtime(&now)->tm_wday;
            for (int32 i = 0; i < p.Size(); ++i)
            {
                int32 day;
                if (!p.TryGetInteger(i, day))
                    continue;
                if (day < 0 || day >= 7)
                    return true;
                if (day == today)
                {
                    s.PushNumber(1);
                    return true;
                }
            }
            s.PushNumber(0);
            return true;
        }

        bool IsTargetInTeam(Script& s, CallFunc const&)
        {
            Player* player = s.GetPlayer(1);
            s.PushNumber(player && player->GetGroup() ? 1 : 0);
            return true;
        }

        bool TargetIsOppositeGender(Script& s, CallFunc const&)
        {
            Player* player = s.GetMain() ? s.GetMain()->ToPlayer() : nullptr;
            Unit* target = player ? player->GetSelectedUnit() : nullptr;
            s.PushNumber(target && target->IsPlayer() && target->GetGender() != player->GetGender() ? 1 : 0);
            return true;
        }

        std::unordered_map<std::string, Fn> const& Functions()
        {
            static std::unordered_map<std::string, Fn> const functions =
            {
                { "RunAutomat", RunAutomat }, { "RunIdleScript", RunIdleScript }, { "RunCombatScript", RunCombatScript },
                { "RunDeadScript", RunDeadScript },

                { "StartTimer", StartTimer }, { "StartCounter", StartCounter }, { "AddCounter", AddCounter },
                { "HasTimerCounted", HasTimerCounted }, { "HasCounterCounted", HasCounterCounted },
                { "CounterInRange", CounterInRange }, { "Random", RandomCounter }, { "SetSOState", SetSOState },
                { "AddMember", AddMember }, { "AddSOMember", AddSOMember }, { "SendMessage", SendMessage },
                { "SendWarning", SendWarning }, { "TrackPlayerEvent", Nothing },

                { "HasQuest", HasQuest }, { "HasQuestFinished", HasQuestFinished }, { "GivePlayerItem", GivePlayerItem },
                { "GivePlayerItemEx", GivePlayerItem }, { "TakePlayerItem", TakePlayerItem }, { "HasItem", HasItem },
                { "SetQuestStatus", SetQuestStatus }, { "UpdateQuest", UpdateQuest }, { "UpdateTeamQuest", UpdateTeamQuest },
                { "UpdateQuestPlayerQuest", UpdateQuestPlayerQuest }, { "SetQuestPlayer", SetQuestPlayer },
                { "AddQuestPlayer", AddQuestPlayer }, { "IsLevelInRange", IsLevelInRange },
                { "IsLegendDungeonMode", PushZeroForPlayer }, { "IsExhausted", PushZeroForPlayer },

                { "CheckCheckPoint", CheckCheckPoint }, { "IncreaseCheckPoint", IncreaseCheckPoint },
                { "DecreaseCheckPoint", DecreaseCheckPoint }, { "SetCheckPoint", SetCheckPoint },
                { "KeyCreaturesDied", KeyCreaturesDied }, { "IsLegend", IsLegend },
                { "DungeonValueEqual", [](Script& s, CallFunc const& p) { return DungeonValue(s, p, 0); } },
                { "DungeonValueGreater", [](Script& s, CallFunc const& p) { return DungeonValue(s, p, 1); } },
                { "DungeonValueLess", [](Script& s, CallFunc const& p) { return DungeonValue(s, p, -1); } },
                { "SetDungeonValue", SetDungeonValue }, { "IncreaseDungeonValue", IncreaseDungeonValue },
                { "DecreaseDungeonValue", DecreaseDungeonValue }, { "DungeonFinish", DungeonFinish },

                { "CreateCreature", CreateCreature }, { "AddCreatureById", AddCreatureById },

                { "StopMeleeAttack", StopMeleeAttack }, { "MeleeAttack", MeleeAttack }, { "EnableMeleeAttack", EnableMeleeAttack },
                { "SetAnim", Nothing }, { "MakeEmote", Nothing }, { "Emotion", Nothing }, { "PlaySound", Nothing },
                { "HasEnemyInSight", HasEnemyInSight }, { "BeingAttacked", BeingAttacked }, { "MoveToTarget", MoveToTarget },
                { "Escape", Escape }, { "EscapeFinished", EscapeFinished }, { "CallHelp", CallHelp },
                { "ChangeFaction", ChangeFaction }, { "IsFaction", IsFaction },
                { "MeleeAttackInRange", MeleeAttackInRange }, { "MeleeAttackOutRange", MeleeAttackOutRange },
                { "RangeAttackInRange", PushZero }, { "RangeAttackOutRange", PushZero }, { "RangeAttack", Nothing },
                { "IsDead", IsDead }, { "IsTargetDead", IsTargetDead }, { "IsTargetDisappeared", IsTargetDisappeared },
                { "Kill", Kill }, { "Idle", Idle }, { "OutOfTrackingRange", OutOfTrackingRange }, { "ShowDialog", ShowDialog },
                { "InHPCondition", InHPCondition }, { "InMPCondition", InMPCondition },
                { "InTargetHPCondition", InTargetHPCondition }, { "InTargetMPCondition", InTargetMPCondition },
                { "IsInRange", IsInRange }, { "FollowTarget", FollowTarget }, { "CastSpell", CastSpell },
                { "CastSpellToPositionOfTarget", CastSpellToPositionOfTarget }, { "CastRandomSpell", Nothing },
                { "LearnSpell", LearnSpell }, { "HasSpellAura", HasSpellAura }, { "HasAuraSub", PushOne },
                { "SelectRandomTarget", SelectRandomTarget }, { "SelectSpecTarget", Nothing }, { "ClearHatred", ClearHatred },
                { "CancelThreated", Nothing }, { "TargetInSpellRange", TargetInSpellRange }, { "MoveByWP",
                    [](Script& s, CallFunc const& p)
                    {
                        int32 wp;
                        if (AutomatAI* ai = AutomatOf(s.GetCreature(p.Integer(2))))
                            if (p.TryGetInteger(0, wp))
                            {
                                if (p.Integer(1, 1) != 0 && sM425Automat.GetWaypoint(uint32(wp)))
                                    ai->StartWaypoint(uint32(wp));
                                else
                                    ai->StopWaypoint();
                            }
                        return true;
                    } },
                { "HasPower", HasPower }, { "CastingSpell", CastingSpell }, { "CanCastSpell", CanCastSpell },
                { "CanHunt", CanHunt }, { "HasCreature", HasCreature }, { "HasCreatureInRange", HasCreatureInRange },
                { "HasCreatureInMap", HasCreatureInMap }, { "HasFriendInCondition", HasFriendInCondition },
                { "CanFindEnemyInRange", CanFindEnemyInRange }, { "UpdateTarget", Nothing },
                { "IsType", IsType }, { "IsTargetType", IsTargetType }, { "Disappear", Disappear }, { "Chat", Chat },
                { "ChangeDisplay", ChangeDisplay }, { "DisableNPC", DisableNPC }, { "DisableMe", Nothing },
                { "CostMoney", CostMoney }, { "SetAsWatchTarget", Nothing }, { "IncreasePlayersExhausting", Nothing },
                { "CanCastModCurrency", CanCastModCurrency }, { "CreateStaticObj", CreateStaticObj },

                { "IsInCombat", IsInCombat }, { "PauseMove", PauseMove }, { "ChangeHpPct", ChangeHpPct },
                { "SetVisible", SetVisible }, { "StartFearRun", StartFearRun }, { "StopFearRun", StopFearRun },
                { "RunScriptInRangeObj", RunScriptInRangeObj }, { "SetObjData", SetObjData }, { "ObjDataEqual", ObjDataEqual },
                { "IsDuringHourBetween", IsDuringHourBetween }, { "IsDayOfWeek", IsDayOfWeek },
                { "IsTargetInTeam", IsTargetInTeam }, { "TargetIsOppositeGender", TargetIsOppositeGender },
                { "TrackEvent", Nothing }, { "AddTargetAttractionPoint", Nothing }, { "GetHalloweenReward", Nothing },
                // 425 currencies that vmangos lacks
                { "IsStoneConsumePointsEnough", PushZero }, { "TakeStoneConsumePoints", PushZero },
            };
            return functions;
        }
    }

    bool CallFunction(Script& script, char const* name, CallFunc const& params)
    {
        auto it = Functions().find(name);
        return it != Functions().end() && it->second(script, params);
    }
}
