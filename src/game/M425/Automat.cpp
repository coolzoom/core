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

#include "Automat.h"
#include "M425.h"
#include "Map.h"
#include "Player.h"
#include "Creature.h"
#include "GameObject.h"
#include "Timer.h"
#include "Log.h"
#include "Database/DatabaseEnv.h"
#include "Policies/SingletonImp.h"
#include <cstring>
#include <strings.h>

INSTANTIATE_SINGLETON_1(M425::AutomatManager);

namespace M425
{
    namespace
    {
        enum Opcode : int32
        {
            OP_ADD = 0, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NOT, OP_AND, OP_OR, OP_BITAND, OP_BITOR, OP_BITXOR,
            OP_UADD, OP_USUB,
            CMP_EQ = 20, CMP_NEQ, CMP_LT, CMP_GT, CMP_LTE, CMP_GTE,
            FUNC = 30, FAST_FUNC = 31,
            CONSTANT = 40,
            STRING = 50,
            PARAM = 60,
        };

        uint32 const MAX_RECURSE = 64;
        uint32 const DUNGEON_HEARTBEAT = 1000;

        template<typename T> T Read(char const* p)
        {
            T v;
            memcpy(&v, p, sizeof(T));
            return v;
        }

        // the bytes of a string token up to its terminating NUL
        std::string View(Token const& t)
        {
            uint32 len = 0;
            while (len < t.strLen && t.str[len])
                ++len;
            return std::string(t.str, len);
        }

        void HandleOp(int32 op, TokenStack& stack)
        {
            float op1 = stack.PopNumber();
            float result = 0.0f;
            if (op == OP_UADD)
                result = op1;
            else if (op == OP_USUB)
                result = -op1;
            else if (op == OP_NOT)
                result = !int32(op1);
            else
            {
                float op2 = stack.PopNumber();
                switch (op)
                {
                    case OP_ADD: result = op1 + op2; break;
                    case OP_SUB: result = op1 - op2; break;
                    case OP_MUL: result = op1 * op2; break;
                    case OP_DIV: result = op2 != 0.0f ? op1 / op2 : 0.0f; break;
                    case OP_MOD: result = int32(op2) != 0 ? float(int32(op1) % int32(op2)) : 0.0f; break;
                    case OP_AND: result = float(int32(op1) && int32(op2)); break;
                    case OP_OR: result = float(int32(op1) || int32(op2)); break;
                    case OP_BITAND: result = float(int32(op1) & int32(op2)); break;
                    case OP_BITOR: result = float(int32(op1) | int32(op2)); break;
                    case OP_BITXOR: result = float(int32(op1) ^ int32(op2)); break;
                }
            }
            stack.Push(Token(result));
        }

        template<typename T> bool Compare(int32 cmp, T const& a, T const& b)
        {
            switch (cmp)
            {
                case CMP_EQ: return a == b;
                case CMP_NEQ: return a != b;
                case CMP_LT: return a < b;
                case CMP_GT: return a > b;
                case CMP_LTE: return a <= b;
                case CMP_GTE: return a >= b;
            }
            return false;
        }

        void HandleCmp(int32 cmp, TokenStack& stack)
        {
            Token tok1 = stack.Pop();
            Token tok2 = stack.Pop();
            bool result = false;
            if (tok1.type == Token::NUMBER && tok2.type == Token::NUMBER)
                result = Compare(cmp, tok1.number, tok2.number);
            else if (tok1.type == Token::STRING && tok2.type == Token::STRING)
                result = Compare(cmp, strcasecmp(View(tok1).c_str(), View(tok2).c_str()), 0);
            stack.Push(Token(result ? 1.0f : 0.0f));
        }
    }

    // Little-endian reader over a compiled script; any overrun fails the whole file.
    class ByteReader
    {
        public:
            ByteReader(char const* data, uint32 size) : m_data(data), m_size(size) {}
            bool ReadS32(int32& v)
            {
                if (m_pos + 4 > m_size)
                    return false;
                v = Read<int32>(m_data + m_pos);
                m_pos += 4;
                return true;
            }
            bool ReadBlob(int32 len, char const*& out)
            {
                if (len < 0 || m_pos + uint32(len) > m_size)
                    return false;
                out = m_data + m_pos;
                m_pos += uint32(len);
                return true;
            }
        private:
            char const* m_data;
            uint32 m_size;
            uint32 m_pos = 0;
    };

    //==================================================================================
    bool Instruction::Load(ByteReader& reader)
    {
        int32 len, textLen;
        char const* code;
        char const* text;
        if (!reader.ReadS32(len) || !reader.ReadBlob(len, code))
            return false;
        m_code.assign(code, code + len);
        // the source text of the instruction follows, only useful for debugging
        return reader.ReadS32(textLen) && reader.ReadBlob(textLen, text);
    }

    Token Instruction::Execute(TokenStack& stack, VirtualMachine& vm) const
    {
        char const* ptr = m_code.data();
        char const* end = ptr + m_code.size();
        while (ptr + 4 <= end)
        {
            int32 instr = Read<int32>(ptr);
            ptr += 4;
            if (instr >= OP_ADD && instr <= OP_USUB)
                HandleOp(instr, stack);
            else if (instr >= CMP_EQ && instr <= CMP_GTE)
                HandleCmp(instr, stack);
            else if (instr == FUNC || instr == FAST_FUNC)
            {
                if (ptr + 12 > end)
                    break;
                int32 argc = Read<int32>(ptr + 4);
                int32 nameLen = Read<int32>(ptr + 8);
                ptr += 12;
                if (nameLen < 0 || ptr + nameLen > end)
                    break;
                std::string name(ptr, strnlen(ptr, nameLen));
                ptr += nameLen;
                vm.Run(name.c_str(), argc);
            }
            else if (instr == CONSTANT)
            {
                if (ptr + 4 > end)
                    break;
                stack.Push(Token(Read<float>(ptr)));
                ptr += 4;
            }
            else if (instr == STRING || instr == PARAM)
            {
                if (ptr + 4 > end)
                    break;
                int32 len = Read<int32>(ptr);
                ptr += 4;
                if (len < 0 || ptr + len > end)
                    break;
                stack.Push(Token(ptr, uint32(len), instr == STRING ? Token::STRING : Token::PARAM));
                ptr += len;
            }
            else
                break;
        }

        Token result(0.0f);
        if (stack.Size() > 0)
            result = stack.Pop();
        // an instruction leaves nothing on the stack
        stack.Clear();
        return result;
    }

    //==================================================================================
    bool AutomatState::Load(ByteReader& reader, uint32 stateCount)
    {
        int32 nameLen, type, count;
        char const* name;
        if (!reader.ReadS32(nameLen) || !reader.ReadBlob(nameLen, name) || !reader.ReadS32(type))
            return false;
        m_name.assign(name, strnlen(name, nameLen));

        if (!reader.ReadS32(count) || count < 0)
            return false;
        m_instructions.resize(count);
        for (Instruction& i : m_instructions)
            if (!i.Load(reader))
                return false;

        if (!reader.ReadS32(count) || count < 0)
            return false;
        m_transitions.resize(count);
        for (Transition& t : m_transitions)
        {
            int32 target;
            if (!reader.ReadS32(target) || target < 0 || uint32(target) >= stateCount || !t.condition.Load(reader))
                return false;
            t.target = uint32(target);
        }
        return true;
    }

    bool AutomatState::NameIs(char const* name) const
    {
        return strcasecmp(m_name.c_str(), name) == 0;
    }

    void AutomatState::ExecuteInstructions(TokenStack& stack, VirtualMachine& vm) const
    {
        for (Instruction const& i : m_instructions)
            i.Execute(stack, vm);
    }

    int32 AutomatState::TestTransitions(TokenStack& stack, VirtualMachine& vm) const
    {
        for (Transition const& t : m_transitions)
            if (t.condition.Execute(stack, vm).number != 0.0f)
                return int32(t.target);
        return -1;
    }

    //==================================================================================
    bool AutomatData::Load(char const* data, uint32 size)
    {
        ByteReader reader(data, size);
        int32 version, endian, nameLen, count;
        char const* name;
        if (!reader.ReadS32(version) || !reader.ReadS32(endian) || !reader.ReadS32(nameLen) ||
            !reader.ReadBlob(nameLen, name) || !reader.ReadS32(count) || count < 0)
            return false;
        m_states.resize(count);
        for (AutomatState& s : m_states)
            if (!s.Load(reader, uint32(count)))
                return false;
        return true;
    }

    AutomatState const* AutomatData::GetEntryState() const
    {
        for (AutomatState const& s : m_states)
            if (s.NameIs("entry"))
                return &s;
        return nullptr;
    }

    //==================================================================================
    void Automat::Load(uint32 scriptId)
    {
        m_scriptId = scriptId;
        m_data = sM425Automat.GetAutomat(scriptId);
        m_state = nullptr;
        m_nextState = m_data ? m_data->GetEntryState() : nullptr;
    }

    int32 Automat::Execute(TokenStack& stack, VirtualMachine& vm)
    {
        // a missing script finishes at once instead of pausing its stack forever
        if (!m_data)
            return 1;
        m_recurseLevel = 0;
        return RecurseExecute(stack, vm);
    }

    int32 Automat::RecurseExecute(TokenStack& stack, VirtualMachine& vm)
    {
        if (m_nextState)
        {
            m_nextState->ExecuteInstructions(stack, vm);
            m_state = m_nextState;
            m_nextState = nullptr;
        }

        if (m_state && m_state->NameIs("exit"))
            return 1;

        if (!vm.IsTopAutomat(this))
            return -1;

        m_nextState = nullptr;
        if (m_state)
        {
            int32 next = m_state->TestTransitions(stack, vm);
            if (next >= 0)
                m_nextState = m_data->GetState(uint32(next));
        }
        if (!m_nextState)
            return 0;

        if (++m_recurseLevel > MAX_RECURSE)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "M425: script %u recurses too deep", m_scriptId);
            m_recurseLevel = 0;
            return 0;
        }
        return RecurseExecute(stack, vm);
    }

    //==================================================================================
    void Script::SetUnit(WorldObject* unit, uint32 idx)
    {
        if (!unit || idx >= AUTOMAT_SLOTS)
            return;
        m_units[idx] = unit->GetObjectGuid();
        if (idx == 0)
            m_main = unit;
        if (!m_map)
            m_map = unit->GetMap();
    }

    void Script::LoadAutomat(uint32 scriptId)
    {
        if (m_automatCount >= AUTOMAT_STACK)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "M425: automat stack full loading script %u", scriptId);
            return;
        }
        m_automats[m_automatCount++].Load(scriptId);
    }

    void Script::KeepLastAndLoadAutomat(uint32 scriptId)
    {
        if (m_automatCount > 1)
            m_automatCount = 1;
        LoadAutomat(scriptId);
    }

    bool Script::Update()
    {
        // a script pushing automata every pass would spin forever
        for (uint32 passes = 0; m_automatCount > 0 && passes < 64; ++passes)
        {
            int32 result = m_automats[m_automatCount - 1].Execute(m_stack, *this);
            if (result == 0)
                return false;
            if (result == 1 && m_automatCount > 0)
                --m_automatCount;
        }
        return m_automatCount == 0;
    }

    void Script::ResetAllTimers()
    {
        uint32 now = WorldTimer::getMSTime();
        for (uint32& t : m_timer)
            t = now;
    }

    void Script::Run(char const* funcName, int32 argc)
    {
        CallFunc params;
        while (argc-- > 0)
        {
            Token val = m_stack.Pop();
            // keyword arguments are pushed as value + PARAM name; the 425 functions ignore them
            if (val.type == Token::PARAM)
                m_stack.Pop();
            else
                params.Add(val);
        }
        if (!CallFunction(*this, funcName, params))
            sM425Automat.ReportUnknownFunction(funcName);
    }

    bool Script::IsTopAutomat(Automat const* automat) const
    {
        return m_automatCount > 0 && automat == &m_automats[m_automatCount - 1];
    }

    Creature* Script::GetMainCreature() const
    {
        return m_main ? m_main->ToCreature() : nullptr;
    }

    WorldObject* Script::GetUnit(int32 idx) const
    {
        if (idx == 0)
            return m_main;
        if (idx < 0 || uint32(idx) >= AUTOMAT_SLOTS || !m_units[idx] || !m_map)
            return nullptr;
        return m_map->GetWorldObject(m_units[idx]);
    }

    Unit* Script::GetUnitAsUnit(int32 idx) const
    {
        WorldObject* obj = GetUnit(idx);
        return obj ? obj->ToUnit() : nullptr;
    }

    Creature* Script::GetCreature(int32 idx) const
    {
        WorldObject* obj = GetUnit(idx);
        return obj ? obj->ToCreature() : nullptr;
    }

    Player* Script::GetPlayer(int32 idx) const
    {
        WorldObject* obj = GetUnit(idx);
        return obj ? obj->ToPlayer() : nullptr;
    }

    DungeonData* Script::GetDungeon() const
    {
        return m_map ? sM425Automat.GetDungeon(m_map) : nullptr;
    }

    //==================================================================================
    namespace
    {
        bool DecodeHex(char const* hex, std::vector<char>& out)
        {
            size_t len = strlen(hex);
            if (len % 2)
                return false;
            out.resize(len / 2);
            for (size_t i = 0; i < out.size(); ++i)
            {
                auto nibble = [](char c) -> int
                {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    return -1;
                };
                int hi = nibble(hex[i * 2]);
                int lo = nibble(hex[i * 2 + 1]);
                if (hi < 0 || lo < 0)
                    return false;
                out[i] = char(hi << 4 | lo);
            }
            return true;
        }

        template<typename F> uint32 Load(char const* sql, F&& row)
        {
            uint32 count = 0;
            if (std::unique_ptr<QueryResult> result = WorldDatabase.Query(sql))
            {
                do
                {
                    row(result->Fetch());
                    ++count;
                } while (result->NextRow());
            }
            return count;
        }
    }

    void AutomatManager::LoadFromDB()
    {
        m_automats.clear();
        uint32 bad = 0;
        // the bytecode has NULs, so it comes as hex
        Load("SELECT `id`, HEX(`data`) FROM `m425_automat`", [&](Field* f)
        {
            std::vector<char> bytes;
            AutomatData data;
            if (!DecodeHex(f[1].GetString(), bytes) || !data.Load(bytes.data(), uint32(bytes.size())))
            {
                ++bad;
                return;
            }
            m_automats.emplace(f[0].GetUInt32(), std::move(data));
        });

        Load("SELECT `id`, `text` FROM `m425_script_text`", [&](Field* f) { m_texts[f[0].GetUInt32()] = f[1].GetCppString(); });
        Load("SELECT `entry`, `idle`, `combat`, `dead` FROM `m425_creature_script`", [&](Field* f)
        {
            m_creatureScripts[f[0].GetUInt32()] = { f[1].GetUInt32(), f[2].GetUInt32(), f[3].GetUInt32() };
        });
        Load("SELECT `guid`, `enter_script`, `leave_script`, `radius` FROM `m425_gameobject_trigger`", [&](Field* f)
        {
            m_triggers[f[0].GetUInt32()] = { f[1].GetUInt32(), f[2].GetUInt32(), f[3].GetFloat() };
        });
        Load("SELECT `quest`, `accept`, `complete`, `objective1`, `objective2`, `objective3`, `objective4` FROM `m425_quest_script`", [&](Field* f)
        {
            m_questScripts[f[0].GetUInt32()] = { f[1].GetUInt32(), f[2].GetUInt32(), { f[3].GetUInt32(), f[4].GetUInt32(), f[5].GetUInt32(), f[6].GetUInt32() } };
        });
        Load("SELECT `id`, `next_id`, `map`, `x`, `y`, `z`, `delay`, `emote`, `script` FROM `m425_waypoint`", [&](Field* f)
        {
            m_waypoints[f[0].GetUInt32()] = { f[0].GetUInt32(), f[1].GetUInt32(), f[2].GetUInt32(), f[3].GetFloat(), f[4].GetFloat(), f[5].GetFloat(), f[6].GetUInt32(), f[7].GetUInt32(), f[8].GetUInt32() };
        });
        Load("SELECT `guid`, `waypoint` FROM `m425_creature_waypoint`", [&](Field* f) { m_creatureWaypoints[f[0].GetUInt32()] = f[1].GetUInt32(); });
        Load("SELECT `map`, `script1` FROM `m425_map_script` WHERE `script1` <> 0", [&](Field* f) { m_mapScripts[f[0].GetUInt32()] = f[1].GetUInt32(); });

        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> M425: %u automata (%u unreadable), %u creature, %u trigger, %u quest bindings, %u waypoints",
            uint32(m_automats.size()), bad, uint32(m_creatureScripts.size()), uint32(m_triggers.size()), uint32(m_questScripts.size()), uint32(m_waypoints.size()));
    }

    AutomatData const* AutomatManager::GetAutomat(uint32 scriptId) const
    {
        auto it = m_automats.find(scriptId);
        return it != m_automats.end() ? &it->second : nullptr;
    }

    std::string const* AutomatManager::GetText(uint32 id) const
    {
        auto it = m_texts.find(id);
        return it != m_texts.end() ? &it->second : nullptr;
    }

    CreatureScripts const* AutomatManager::GetCreatureScripts(uint32 entry) const
    {
        auto it = m_creatureScripts.find(entry);
        return it != m_creatureScripts.end() ? &it->second : nullptr;
    }

    TriggerScripts const* AutomatManager::GetTriggerScripts(uint32 goGuid) const
    {
        auto it = m_triggers.find(goGuid);
        return it != m_triggers.end() ? &it->second : nullptr;
    }

    Waypoint const* AutomatManager::GetWaypoint(uint32 id) const
    {
        auto it = m_waypoints.find(id);
        return it != m_waypoints.end() ? &it->second : nullptr;
    }

    uint32 AutomatManager::GetCreatureWaypoint(uint32 guid) const
    {
        auto it = m_creatureWaypoints.find(guid);
        return it != m_creatureWaypoints.end() ? it->second : 0;
    }

    void AutomatManager::RunOnce(uint32 scriptId, WorldObject* main, Player* player, Map* map) const
    {
        if (!scriptId || !map)
            return;
        Script script(map);
        script.LoadAutomat(scriptId);
        if (main)
            script.SetUnit(main, 0);
        if (player)
            script.SetUnit(player, 1);
        script.Update();
    }

    void AutomatManager::OnQuestAccept(Player* player, WorldObject* giver, uint32 questId) const
    {
        auto it = m_questScripts.find(questId);
        if (it != m_questScripts.end())
            RunOnce(it->second.accept, giver, player, player->GetMap());
    }

    void AutomatManager::OnQuestComplete(Player* player, WorldObject* ender, uint32 questId) const
    {
        auto it = m_questScripts.find(questId);
        if (it != m_questScripts.end())
            RunOnce(it->second.complete, ender, player, player->GetMap());
    }

    void AutomatManager::OnQuestObjective(Player* player, uint32 questId, uint32 objective) const
    {
        auto it = m_questScripts.find(questId);
        if (it != m_questScripts.end() && objective < 4)
            RunOnce(it->second.objective[objective], nullptr, player, player->GetMap());
    }

    DungeonData* AutomatManager::GetDungeon(Map const* map)
    {
        if (!map->IsDungeon() || !IsHaradonMap(map->GetId()))
            return nullptr;
        std::lock_guard<std::mutex> lock(m_dungeonLock);
        std::unique_ptr<DungeonData>& data = m_dungeons[map];
        if (!data)
            data = std::make_unique<DungeonData>();
        return data.get();
    }

    void AutomatManager::UpdateMap(Map* map, uint32 diff)
    {
        DungeonData* data = GetDungeon(map);
        if (!data)
            return;
        if (!data->script)
        {
            auto it = m_mapScripts.find(map->GetId());
            if (it == m_mapScripts.end())
                return;
            data->script = std::make_unique<Script>(map);
            data->script->SetCheckPointScript();
            data->script->LoadAutomat(it->second);
        }
        data->heartBeat += diff;
        if (data->heartBeat < DUNGEON_HEARTBEAT)
            return;
        data->heartBeat = 0;
        data->script->Update();
    }

    void AutomatManager::RemoveMap(Map const* map)
    {
        std::lock_guard<std::mutex> lock(m_dungeonLock);
        m_dungeons.erase(map);
    }

    void AutomatManager::ReportUnknownFunction(char const* name)
    {
        std::lock_guard<std::mutex> lock(m_unknownLock);
        if (m_unknown[name]++ == 0)
            sLog.Out(LOG_BASIC, LOG_LVL_DETAIL, "M425: script function %s() is not implemented", name);
    }
}
