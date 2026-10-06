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

#include "AutomatAI.h"
#include "Creature.h"
#include "GameObject.h"
#include "Player.h"
#include "Map.h"
#include "MotionMaster.h"

namespace M425
{
    namespace
    {
        uint32 const HEARTBEAT_NORMAL = 1000;
        uint32 const HEARTBEAT_COMBAT = 500;
        uint32 const WAYPOINT_POINT   = 0x425;
        uint32 const TRIGGER_INTERVAL = 500;
        // WayPointScript watches the units of slots 0-10
        int32 const WAYPOINT_WATCHED_SLOTS = 11;
    }

    AutomatAI::AutomatAI(Creature* creature) : BasicAI(creature), m_script(nullptr),
        m_scripts(sM425Automat.GetCreatureScripts(creature->GetEntry()))
    {
        m_script.SetAI(this);
    }

    void AutomatAI::Tick(uint32 diff)
    {
        if (!m_script.GetMain())
        {
            m_script.SetUnit(m_creature);
            m_script.LoadAutomat(SCRIPT_CREATURE);
            if (uint32 waypoint = sM425Automat.GetCreatureWaypoint(m_creature->GetDBTableGUIDLow()))
                StartWaypoint(waypoint);
        }

        m_heartBeat += diff;
        if (m_heartBeat < (m_creature->IsInCombat() ? HEARTBEAT_COMBAT : HEARTBEAT_NORMAL))
            return;
        m_heartBeat = 0;
        m_script.Update();
    }

    void AutomatAI::UpdateAI(uint32 const diff)
    {
        Tick(diff);

        if (!m_creature->IsInCombat())
            UpdateWaypoint(diff);

        if (!m_creature->SelectHostileTarget() || !m_creature->GetVictim())
            return;

        DoMeleeAttackIfReady();
    }

    void AutomatAI::UpdateAI_corpse(uint32 const diff)
    {
        // dead scripts keep running on the corpse (timed spawns, talk)
        Tick(diff);
    }

    void AutomatAI::JustDied(Unit* /*killer*/)
    {
        StopWaypoint();
        m_heartBeat = 0;
        if (m_script.GetMain())
            m_script.Update();
    }

    void AutomatAI::JustRespawned()
    {
        BasicAI::JustRespawned();
        SetMeleeAttack(true);
        SetCombatMovement(true);
        m_questPlayers.clear();
        m_friendTarget.Clear();
        m_objData.clear();
        if (!m_script.GetMain())
            return;
        m_script.Clear();
        m_script.LoadAutomat(SCRIPT_CREATURE);
        if (uint32 waypoint = sM425Automat.GetCreatureWaypoint(m_creature->GetDBTableGUIDLow()))
            StartWaypoint(waypoint);
    }

    void AutomatAI::JustReachedHome()
    {
        if (m_waypoint && !m_waypointScript && m_waypointWait < 0)
            MoveToWaypoint();
    }

    void AutomatAI::MovementInform(uint32 type, uint32 id)
    {
        if (type != POINT_MOTION_TYPE || id != WAYPOINT_POINT || !m_waypoint)
            return;
        m_waypointMoving = false;
        if (m_waypoint->script)
        {
            m_waypointScript = std::make_unique<Script>(m_creature->GetMap());
            m_waypointScript->SetAI(this);
            m_waypointScript->SetUnit(m_creature);
            m_waypointScript->LoadAutomat(m_waypoint->script);
            m_waypointHeartBeat = 0;
        }
        else
            m_waypointWait = int32(m_waypoint->delay);
    }

    void AutomatAI::StartWaypoint(uint32 waypointId)
    {
        m_waypoint = sM425Automat.GetWaypoint(waypointId);
        m_waypointScript.reset();
        m_waypointWait = -1;
        if (m_waypoint && !m_creature->IsInCombat() && m_creature->IsAlive())
            MoveToWaypoint();
    }

    void AutomatAI::StopWaypoint()
    {
        m_waypoint = nullptr;
        m_waypointScript.reset();
        m_waypointWait = -1;
        if (m_waypointMoving)
        {
            m_waypointMoving = false;
            if (m_creature->IsAlive() && !m_creature->IsInCombat())
                m_creature->GetMotionMaster()->MoveIdle();
        }
    }

    void AutomatAI::MoveToWaypoint()
    {
        m_waypointMoving = true;
        m_creature->GetMotionMaster()->MovePoint(WAYPOINT_POINT, m_waypoint->x, m_waypoint->y, m_waypoint->z, MOVE_PATHFINDING);
    }

    void AutomatAI::NextWaypoint()
    {
        m_waypointWait = -1;
        m_waypoint = m_waypoint ? sM425Automat.GetWaypoint(m_waypoint->nextId) : nullptr;
        if (m_waypoint)
            MoveToWaypoint();
    }

    void AutomatAI::UpdateWaypoint(uint32 diff)
    {
        if (!m_waypoint)
            return;

        if (m_waypointScript)
        {
            m_waypointHeartBeat += diff;
            if (m_waypointHeartBeat < HEARTBEAT_NORMAL)
                return;
            m_waypointHeartBeat = 0;

            // WayPointScript::Update: pause while a watched unit fights, finish when one is gone
            bool finished = false;
            for (int32 i = 0; i < WAYPOINT_WATCHED_SLOTS && !finished; ++i)
            {
                if (!m_waypointScript->GetUnitGuid(i) && i != 0)
                    continue;
                WorldObject* obj = m_waypointScript->GetUnit(i);
                Unit* unit = obj ? obj->ToUnit() : nullptr;
                if (!obj)
                    finished = true;
                else if (unit && unit->IsInCombat())
                {
                    m_waypointScript->ResetAllTimers();
                    return;
                }
                else if (unit && !unit->IsAlive())
                    finished = true;
            }
            if (!finished)
                finished = m_waypointScript->Update();
            if (finished)
            {
                m_waypointScript.reset();
                NextWaypoint();
            }
            return;
        }

        if (m_waypointWait >= 0)
        {
            m_waypointWait -= int32(diff);
            if (m_waypointWait < 0)
                NextWaypoint();
        }
    }

    //==================================================================================
    void TriggerAI::UpdateAI(uint32 const diff)
    {
        m_timer += diff;
        if (m_timer < TRIGGER_INTERVAL)
            return;
        m_timer = 0;
        if (!me->IsInWorld() || !me->isSpawned())
            return;

        Map* map = me->GetMap();
        std::vector<Player*> entered;
        for (auto const& ref : map->GetPlayers())
        {
            Player* player = ref.getSource();
            if (player && player->IsInWorld() && me->IsWithinDistInMap(player, m_scripts->radius) && m_inside.insert(player->GetObjectGuid()).second)
                entered.push_back(player);
        }

        std::vector<ObjectGuid> inside(m_inside.begin(), m_inside.end());
        for (ObjectGuid const& guid : inside)
        {
            Player* player = map->GetPlayer(guid);
            if (!player)
                m_inside.erase(guid);
            else if (!me->IsWithinDistInMap(player, m_scripts->radius))
            {
                m_inside.erase(guid);
                sM425Automat.RunOnce(m_scripts->leave, me, player, map);
            }
        }

        for (Player* player : entered)
            sM425Automat.RunOnce(m_scripts->enter, me, player, map);
    }

    GameObjectAI* CreateGameObjectAI(GameObject* go)
    {
        TriggerScripts const* scripts = sM425Automat.GetTriggerScripts(go->GetDBTableGUIDLow());
        return scripts ? new TriggerAI(go, scripts) : nullptr;
    }
}
