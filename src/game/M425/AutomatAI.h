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

#ifndef MANGOS_M425_AUTOMATAI_H
#define MANGOS_M425_AUTOMATAI_H

#include "BasicAI.h"
#include "GameObjectAI.h"
#include "Automat.h"
#include <set>

namespace M425
{
    // creature_template.ai_name = 'M425AutomatAI' (every Haradon creature). Like the 425 Creature it runs the
    // dispatcher automat (script 1) every second, twice as often in combat; the dispatcher loads the creature's
    // idle/combat/dead scripts. vmangos keeps aggro, threat, chase and evade; the scripts add spells, talk,
    // spawns, quest credit and waypoint walks.
    class AutomatAI : public BasicAI
    {
        public:
            explicit AutomatAI(Creature* creature);

            static int Permissible(Creature const*) { return PERMIT_BASE_NO; }

            void UpdateAI(uint32 const diff) override;
            void UpdateAI_corpse(uint32 const diff) override;
            void JustDied(Unit* killer) override;
            void JustRespawned() override;
            void JustReachedHome() override;
            void MovementInform(uint32 type, uint32 id) override;

            Script& GetScript() { return m_script; }
            CreatureScripts const* GetScripts() const { return m_scripts; }

            // MoveByWP(wp, 1) / MoveByWP(wp, 0)
            void StartWaypoint(uint32 waypointId);
            void StopWaypoint();

            // SetQuestPlayer / AddQuestPlayer / UpdateQuestPlayerQuest
            std::vector<ObjectGuid> m_questPlayers;
            // the friendly unit picked by HasFriendInCondition, cast at instead of the victim
            ObjectGuid m_friendTarget;
            // DisableNPC keeps the npc flags to give back
            uint32 m_disabledNpcFlags = 0;
            bool m_npcDisabled = false;
            // SetObjData / ObjDataEqual
            std::unordered_map<int32, int32> m_objData;

        private:
            void Tick(uint32 diff);
            void UpdateWaypoint(uint32 diff);
            void MoveToWaypoint();
            void NextWaypoint();

            Script m_script;
            CreatureScripts const* m_scripts;
            uint32 m_heartBeat = 0;

            Waypoint const* m_waypoint = nullptr;
            bool m_waypointMoving = false;
            int32 m_waypointWait = -1;               // ms left at the reached waypoint, -1 = not waiting
            std::unique_ptr<Script> m_waypointScript;
            uint32 m_waypointHeartBeat = 0;
    };

    // 425 script trigger spawns (m425_gameobject_trigger): enter/leave scripts for players inside the radius.
    class TriggerAI : public GameObjectAI
    {
        public:
            TriggerAI(GameObject* go, TriggerScripts const* scripts) : GameObjectAI(go), m_scripts(scripts) {}
            void UpdateAI(uint32 const diff) override;
        private:
            TriggerScripts const* m_scripts;
            std::set<ObjectGuid> m_inside;
            uint32 m_timer = 0;
    };

    // the trigger AI for a game object spawned from m425_gameobject_trigger, otherwise nullptr
    GameObjectAI* CreateGameObjectAI(GameObject* go);
}

#endif
