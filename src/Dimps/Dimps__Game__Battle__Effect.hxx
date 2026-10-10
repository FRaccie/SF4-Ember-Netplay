#pragma once

#include <windows.h>

#include "Dimps__Game.hxx"

using Dimps::Game::GameMementoKey;

namespace Dimps {
	namespace Game {
		namespace Battle {
			namespace Effect {
				void Locate(HMODULE peRoot);

				struct ActorManager
				{
					typedef struct __publicMethods {
						size_t(ActorManager::* GetMementoSize)();
					} __publicMethods;

					static void Locate(HMODULE peRoot);
					static __publicMethods publicMethods;
				};

				struct Unit
				{
					typedef struct __privateMethods {
						// TODO
					} __privateMethods;

					typedef struct __publicMethods {
						void (Unit::* RecordToInternalMementoKey)(GameMementoKey::MementoID* id);
						void (Unit::* RestoreFromInternalMementoKey)(GameMementoKey::MementoID* id);
					} __publicMethods;

					typedef struct __staticMethods {
						// TODO
					} __staticMethods;

					// What a fighter has in flight (a fireball, one at a time in
					// this game): the first of its effect objects, null when it
					// has none. An effect object
					// keeps its attack boxes where a fighter does
					// (Chara::Actor::GetAttackBoxes), the ones that hit in list 3.
					// The unit holds a manager a side at +8; a manager its first
					// object at +0x84 and their count at +0x8C. Read off the
					// in-game hitboxes test's fireball.
					static void* GetFirstEffect(Unit* u, unsigned side);
					enum { BOXES_HIT = 3 };

					static void Locate(HMODULE peRoot);
					static __privateMethods privateMethods;
					static __publicMethods publicMethods;
					static __staticMethods staticMethods;

					// Instance values here
				};
			}
		}
	}
}