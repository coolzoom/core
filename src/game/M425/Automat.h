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

#ifndef MANGOS_M425_AUTOMAT_H
#define MANGOS_M425_AUTOMAT_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Policies/Singleton.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Map;
class WorldObject;
class Unit;
class Creature;
class GameObject;
class Player;

// The 425 script virtual machine (DiaState.cpp / Scriptable.cpp of the 425 server): every script is a state
// machine whose states run bytecode instructions and whose transitions test bytecode conditions. The bytecode
// calls named functions (AutomatFunctions.cpp) with the script's units in 64 slots: 0 is the object running the
// script, 1 the player it runs for, 11+ creatures it spawned. Scripts are compiled .graphml files stored in
// m425_automat; their ids, waypoints, spawn ids and strings are 425 ids, mapped to vmangos ids on use.
namespace M425
{
    uint32 const SPELL_OFFSET           = 40000;
    uint32 const ITEM_OFFSET            = 100000;
    uint32 const QUEST_OFFSET           = 90000;
    uint32 const CREATURE_OFFSET        = 900000;
    uint32 const CREATURE_GUID_OFFSET   = 9000000;
    uint32 const GAMEOBJECT_OFFSET      = 900000;
    uint32 const GAMEOBJECT_GUID_OFFSET = 9000000;
    // never-spawned creatures whose kill credit stands for a scripted quest objective (M425Patch Ids.QuestCredit)
    uint32 const QUEST_CREDIT_BASE      = 1000000;

    // 02_factions.sql templates for the 425 UNIT_FACTION_TYPE values
    uint32 const FACTION_HOSTILE  = 2002;
    uint32 const FACTION_FRIENDLY = 2003;
    uint32 const FACTION_NEUTRAL  = 2006;

    inline uint32 QuestCreditEntry(uint32 quest425, uint32 objective) { return QUEST_CREDIT_BASE + quest425 * 4 + objective; }

    uint32 const AUTOMAT_SLOTS    = 64;
    uint32 const AUTOMAT_TIMERS   = 64;
    uint32 const AUTOMAT_STACK    = 8;
    uint32 const AUTOMAT_TOKENS   = 64;
    uint32 const FIRST_SPAWN_SLOT = 11;

    // the creature dispatcher (enemy_ai) and the defaults it loads when creature_proto script1-3 are 0
    uint32 const SCRIPT_CREATURE = 1;
    uint32 const SCRIPT_COMBAT   = 2;
    uint32 const SCRIPT_DEAD     = 3;
    uint32 const SCRIPT_IDLE     = 4;

    struct Token
    {
        enum Type : uint8 { INVALID, NUMBER, STRING, PARAM };

        Token() = default;
        explicit Token(float n) : type(NUMBER), number(n) {}
        Token(char const* s, uint32 len, Type t) : type(t), str(s), strLen(len) {}

        Type type = INVALID;
        float number = 0.0f;
        char const* str = nullptr;
        uint32 strLen = 0;
    };

    class TokenStack
    {
        public:
            void Push(Token const& token) { if (m_size < AUTOMAT_TOKENS) m_elems[m_size++] = token; }
            Token Pop() { if (m_size > 0) --m_size; return m_elems[m_size]; }
            float PopNumber() { Token t = Pop(); return t.type == Token::NUMBER ? t.number : 0.0f; }
            uint32 Size() const { return m_size; }
            void Clear() { m_size = 0; }
        private:
            Token m_elems[AUTOMAT_TOKENS];
            uint32 m_size = 0;
    };

    // Arguments of one call, in source order.
    class CallFunc
    {
        public:
            void Add(Token const& token) { m_args.push_back(token); }
            int32 Size() const { return int32(m_args.size()); }
            int32 Integer(int32 idx, int32 def = 0) const { int32 v; return TryGetInteger(idx, v) ? v : def; }
            float Float(int32 idx, float def = 0.0f) const { float v; return TryGetFloat(idx, v) ? v : def; }
            bool Bool(int32 idx, bool def = false) const { int32 v; return TryGetInteger(idx, v) ? v != 0 : def; }
            bool TryGetInteger(int32 idx, int32& val) const
            {
                if (!IsNumber(idx))
                    return false;
                val = int32(m_args[idx].number);
                return true;
            }
            bool TryGetFloat(int32 idx, float& val) const
            {
                if (!IsNumber(idx))
                    return false;
                val = m_args[idx].number;
                return true;
            }
            bool TryGetBool(int32 idx, bool& val) const
            {
                int32 v;
                if (!TryGetInteger(idx, v))
                    return false;
                val = v != 0;
                return true;
            }
        private:
            bool IsNumber(int32 idx) const { return idx >= 0 && idx < Size() && m_args[idx].type == Token::NUMBER; }
            std::vector<Token> m_args;
    };

    class Automat;

    class VirtualMachine
    {
        public:
            virtual ~VirtualMachine() = default;
            virtual void Run(char const* funcName, int32 argc) = 0;
            virtual bool IsTopAutomat(Automat const* automat) const = 0;
    };

    class ByteReader;

    class Instruction
    {
        public:
            bool Load(ByteReader& reader);
            Token Execute(TokenStack& stack, VirtualMachine& vm) const;
        private:
            std::vector<char> m_code;
    };

    class AutomatState
    {
        public:
            bool Load(ByteReader& reader, uint32 stateCount);
            bool NameIs(char const* name) const;
            void ExecuteInstructions(TokenStack& stack, VirtualMachine& vm) const;
            int32 TestTransitions(TokenStack& stack, VirtualMachine& vm) const;   // target state index, -1 = none
        private:
            struct Transition { uint32 target; Instruction condition; };
            std::string m_name;
            std::vector<Instruction> m_instructions;
            std::vector<Transition> m_transitions;
    };

    class AutomatData
    {
        public:
            bool Load(char const* data, uint32 size);
            AutomatState const* GetEntryState() const;
            AutomatState const* GetState(uint32 idx) const { return idx < m_states.size() ? &m_states[idx] : nullptr; }
            int32 IndexOf(AutomatState const* state) const { return state ? int32(state - m_states.data()) : -1; }
        private:
            std::vector<AutomatState> m_states;
    };

    // One running script file; results of Execute: 0 paused, -1 another automat was pushed, 1 reached "exit".
    class Automat
    {
        public:
            void Load(uint32 scriptId);
            int32 Execute(TokenStack& stack, VirtualMachine& vm);
            uint32 GetScriptId() const { return m_scriptId; }
        private:
            int32 RecurseExecute(TokenStack& stack, VirtualMachine& vm);

            uint32 m_scriptId = 0;
            AutomatData const* m_data = nullptr;
            AutomatState const* m_state = nullptr;
            AutomatState const* m_nextState = nullptr;
            uint32 m_recurseLevel = 0;
    };

    class AutomatAI;
    struct DungeonData;

    // ScriptBase + ScriptRunningContext + the per-script timers: what one object runs.
    class Script : public VirtualMachine
    {
        public:
            explicit Script(Map* map) : m_map(map) {}

            void SetUnit(WorldObject* unit, uint32 idx = 0);
            void LoadAutomat(uint32 scriptId);
            void KeepLastAndLoadAutomat(uint32 scriptId);
            void Clear() { m_automatCount = 0; m_stack.Clear(); }
            // runs until every automat pauses (false) or all of them reached exit (true)
            bool Update();
            bool IsLoaded() const { return m_automatCount > 0; }
            void ResetAllTimers();

            void Run(char const* funcName, int32 argc) override;
            bool IsTopAutomat(Automat const* automat) const override;

            Map* GetMap() const { return m_map; }
            WorldObject* GetMain() const { return m_main; }
            Creature* GetMainCreature() const;
            WorldObject* GetUnit(int32 idx) const;
            Unit* GetUnitAsUnit(int32 idx) const;
            Creature* GetCreature(int32 idx) const;
            Player* GetPlayer(int32 idx) const;
            ObjectGuid GetUnitGuid(int32 idx) const { return idx >= 0 && uint32(idx) < AUTOMAT_SLOTS ? m_units[idx] : ObjectGuid(); }
            void PushNumber(float n) { m_stack.Push(Token(n)); }

            AutomatAI* GetAI() const { return m_ai; }
            void SetAI(AutomatAI* ai) { m_ai = ai; }
            // checkpoint scripts belong to a dungeon instead of a creature; their spawns join the checkpoint
            bool IsCheckPointScript() const { return m_checkPoint; }
            void SetCheckPointScript() { m_checkPoint = true; }
            DungeonData* GetDungeon() const;

            uint32 m_timer[AUTOMAT_TIMERS] = {};
            int32 m_counter[AUTOMAT_TIMERS] = {};

        private:
            Automat m_automats[AUTOMAT_STACK];
            uint32 m_automatCount = 0;
            TokenStack m_stack;
            Map* m_map;
            WorldObject* m_main = nullptr;
            ObjectGuid m_units[AUTOMAT_SLOTS];
            AutomatAI* m_ai = nullptr;
            bool m_checkPoint = false;
    };

    struct Waypoint
    {
        uint32 id;
        uint32 nextId;
        uint32 map;
        float x, y, z;
        uint32 delay;
        uint32 emote;
        uint32 script;
    };

    struct CreatureScripts { uint32 idle, combat, dead; };
    struct TriggerScripts { uint32 enter, leave; float radius; };
    struct QuestScripts { uint32 accept, complete, objective[4]; };

    // DungeonCheckPointData: per dungeon instance, plus the value slots of the newer 425 servers.
    struct DungeonData
    {
        int32 checkPoint = 0;
        std::unordered_map<int32, int32> values;
        std::vector<ObjectGuid> objects;            // spawned by the checkpoint script, for KeyCreaturesDied
        std::unique_ptr<Script> script;
        uint32 heartBeat = 0;
    };

    // chat_menu rendered by M425Patch Gossip.cs: one variant per combination of the menu text's conditions
    struct MenuCondition { bool negate; bool aura; uint32 id; bool inLog; };
    struct MenuOption { std::string label; std::vector<uint8> scripts; int32 link; };
    struct MenuVariant { std::vector<MenuCondition> conditions; uint32 npcText; std::vector<MenuOption> options; };
    struct ChatMenu
    {
        uint32 scripts[6] = {};
        uint32 subMenus[6] = {};
        std::vector<MenuVariant> variants;
    };

    uint32 const GOSSIP_SENDER_MENU = 0x4250000;    // + menu id
    uint32 const GOSSIP_ACTION_VENDOR = 1000;
    uint32 const GOSSIP_ACTION_TRAINER = 1001;
    uint32 const GOSSIP_ACTION_UNLEARN_TALENTS = 1002;

    class AutomatManager
    {
        public:
            void LoadFromDB();

            // creatures with a 425 chat menu (Gossip.cpp); false leaves the gossip to vmangos
            bool OnGossipHello(Player* player, Creature* creature);
            bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action);

            AutomatData const* GetAutomat(uint32 scriptId) const;
            std::string const* GetText(uint32 id) const;
            CreatureScripts const* GetCreatureScripts(uint32 entry) const;
            TriggerScripts const* GetTriggerScripts(uint32 goGuid) const;
            Waypoint const* GetWaypoint(uint32 id) const;
            uint32 GetCreatureWaypoint(uint32 guid) const;

            // one-shot scripts of quests (QuestMgrEvent.cpp)
            void OnQuestAccept(Player* player, WorldObject* giver, uint32 questId) const;
            void OnQuestComplete(Player* player, WorldObject* ender, uint32 questId) const;
            void OnQuestObjective(Player* player, uint32 questId, uint32 objective) const;
            // a one-shot script run for a player against an object (triggers, quests, gossip)
            void RunOnce(uint32 scriptId, WorldObject* main, Player* player, Map* map) const;

            // dungeon checkpoint scripts (map_info scriptId1), updated with the map
            void UpdateMap(Map* map, uint32 diff);
            void RemoveMap(Map const* map);
            DungeonData* GetDungeon(Map const* map);

            void ReportUnknownFunction(char const* name);

        private:
            void LoadGossip();
            void ShowMenu(Player* player, Creature* creature, uint32 menuId, bool root);

            std::unordered_map<uint32, AutomatData> m_automats;
            std::unordered_map<uint32, std::string> m_texts;
            std::unordered_map<uint32, CreatureScripts> m_creatureScripts;
            std::unordered_map<uint32, TriggerScripts> m_triggers;
            std::unordered_map<uint32, QuestScripts> m_questScripts;
            std::unordered_map<uint32, Waypoint> m_waypoints;
            std::unordered_map<uint32, uint32> m_creatureWaypoints;
            std::unordered_map<uint32, uint32> m_mapScripts;

            std::mutex m_dungeonLock;
            std::unordered_map<Map const*, std::unique_ptr<DungeonData>> m_dungeons;

            std::mutex m_unknownLock;
            std::unordered_map<std::string, uint32> m_unknown;

            std::unordered_map<uint32, ChatMenu> m_menus;
            std::unordered_map<uint32, uint32> m_creatureMenus;
            // per player: the menus opened from the root, for link_menu=0 (back)
            std::mutex m_menuLock;
            std::unordered_map<ObjectGuid, std::vector<uint32>> m_menuStacks;
    };

    // AutomatFunctions.cpp: false when the name is not a script function
    bool CallFunction(Script& script, char const* name, CallFunc const& params);
}

#define sM425Automat MaNGOS::Singleton<M425::AutomatManager>::Instance()

#endif
