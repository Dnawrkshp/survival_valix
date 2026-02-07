#include <libdl/player.h>
#include <libdl/utils.h>
#include <libdl/game.h>
#include <libdl/net.h>
#include <libdl/area.h>
#include <libdl/spawnpoint.h>
#include <libdl/random.h>
#include <libdl/stdio.h>
#include <libdl/string.h>
#include <libdl/hud.h>
#include "utils.h"
#include "maputils.h"
#include "shared.h"
#include "soulcollector.h"

#define OOB_AREA_INDEX_START (0)
#define OOB_AREA_INDEX_END (9)
#define OOB_AREA_INDEX_BOSS (9)
#define OOB_AREA_INDEX_DEATH (10)
#define BOSS_ARENA_WEP_PICKUP_COUNT (16)
#define BOSS_ARENA_HACKERORB_COUNT (7)
#define BOSS_ARENA_JUMPPAD_MOBY_UID (103)
#define BOSS_ARENA_HACK_ORB_MOB_BONUS (50)
#define BOSS_ARENA_MAX_MINIONS (5)
#define BOSS_ARENA_MINIONS_AFTER (0.5)

// all but boss area jump pad
#define RANDOMIZE_JUMP_PADS_COUNT (10)

int mapSpawnMob(int spawnParamsIdx, VECTOR position, float yaw, int spawnFromUID, int spawnFlags);
void mapReturnPlayersToMap(void);
void mobForceIntoMapBounds(Moby *moby);
void mapOnMobKilled(Moby *moby, int killedByPlayerId, int killedByWeaponId);

// big al locations
VECTOR BigAlLocations[] = {
		{379.61, 383.4, 325.46, 15},
		{519.92, 534.12, 303.0734, 49.32361},
		{752.2502, 643.3903, 323.4836, -52.62326},
		{683.2401, 500.1401, 318.0001, 125.6676},
		{649.52, 169.82, 339.692, -137.7411},
};
const int BigAlLocationsCount = sizeof(BigAlLocations) / sizeof(VECTOR);

// boss arena location
VECTOR BossArenaLocation = {
		828.1999,
		278.4,
		292.3013,
		0};

char IsInBossFight = 0;
VECTOR WeaponPickupLocationBackups[BOSS_ARENA_WEP_PICKUP_COUNT];
Moby *BossHackerOrbs[BOSS_ARENA_HACKERORB_COUNT] = {};
int BossHackerOrbsStates[BOSS_ARENA_HACKERORB_COUNT] = {};
int LocalPlayerRespawnCuboid[GAME_MAX_LOCALS] = {-1, -1};
int LocalPlayerInJumpPad[GAME_MAX_LOCALS] = {0, 0};

//--------------------------------------------------------------------------
void gambitsEasyTick(void)
{
	soulcollectorActivateAll();
}

//--------------------------------------------------------------------------
void gambitsJumpPadsAlwaysOnTick(void)
{
	soulcollectorActivateAll();
}

//--------------------------------------------------------------------------
void gambitsJumpPadsRandomizeOnRoundComplete(int roundNumber)
{
	int count = 0;
	Moby *m = mobyListGetStart();
	int cuboids[RANDOMIZE_JUMP_PADS_COUNT];
	Moby *jumpPads[RANDOMIZE_JUMP_PADS_COUNT] = {};

	// collect all jump pads
	while (count < RANDOMIZE_JUMP_PADS_COUNT && (m = mobyFindNextByOClass(m, MOBY_ID_JUMP_PAD)))
	{
		jumpPads[count] = m;
		cuboids[count] = *(int *)(m->PVar + 24);
		++m;
		++count;
	}

	// randomize cuboids
	int i;
	for (i = 0; i < 100; ++i)
	{
		int i0 = randRangeInt(0, count - 1);
		int i1 = randRangeInt(0, count - 1);
		if (i0 == i1)
			continue;

		int swap = cuboids[i1];
		cuboids[i1] = cuboids[i0];
		cuboids[i0] = swap;
	}

	// assign
	for (i = 0; i < count; ++i)
	{
		Moby *jumpPad = jumpPads[i];
		if (!jumpPad || !jumpPad->PVar)
			continue;

		*(int *)(jumpPad->PVar + 24) = cuboids[i];
	}

	pushSnack(-1, "Jump Pads Randomized!", 30);
}

//--------------------------------------------------------------------------
void gambitsJumpPadsRandomizeInit(void)
{
	gambitsJumpPadsRandomizeOnRoundComplete(0);
}

//--------------------------------------------------------------------------
void gambitsHoverbootsInit(void)
{
	int i;

	// set hoverboots perks
	for (i = 0; i < GAME_MAX_PLAYERS; ++i)
	{
		MapConfig.State->PlayerStates[i].State.ItemStackable[STACKABLE_ITEM_HOVERBOOTS] = 10;
	}
}

//--------------------------------------------------------------------------
void valixOnEndBossFight(void)
{
	if (!IsInBossFight)
		return;

	// remove hacker ray
	int i;
	for (i = 0; i < GAME_MAX_LOCALS; ++i)
	{
		Player *player = playerGetFromSlot(i);
		if (!player || !player->PlayerMoby || !playerIsConnected(player))
			break;

		if (player->GadgetBox)
		{
			player->GadgetBox->Gadgets[WEAPON_ID_HACKER_RAY].Level = -1;
		}
	}

	// enable prestige
	Moby *prestigeMachineMoby = MapConfig.State->PrestigeMachine;
	if (prestigeMachineMoby)
	{
		prestigeMachineMoby->DrawDist = 64;
		prestigeMachineMoby->CollActive = 0;
		prestigeMachineMoby->ModeBits &= ~MOBY_MODE_BIT_DISABLED;
	}

	// move weapon pickups back
	int count = 0;
	Moby *moby = mobyListGetStart();
	while ((moby = mobyFindNextByOClass(moby, MOBY_ID_WEAPON_PICKUP)))
	{
		if (!mobyIsDestroyed(moby))
		{

			// restore
			vector_copy(moby->Position, WeaponPickupLocationBackups[count]);
			((void (*)(Moby *))moby->PUpdate)(moby);
			mobyUpdateTransform(moby);
			++count;
		}

		if (count >= BOSS_ARENA_WEP_PICKUP_COUNT)
			break;
		++moby;
	}

	// enable jump pad
	Moby *jumpPadMoby = mobyFindByUID(BOSS_ARENA_JUMPPAD_MOBY_UID);
	if (jumpPadMoby && jumpPadMoby->OClass == MOBY_ID_JUMP_PAD)
	{
		jumpPadMoby->ModeBits &= ~1;
		jumpPadMoby->CollActive = 0;
		mobySetState(jumpPadMoby, 0, -1);
	}

	IsInBossFight = 0;
}

//--------------------------------------------------------------------------
void valixOnBeginBossFight(void)
{
	if (IsInBossFight)
		return;

	// wait for round to start
	if (MapConfig.State->RoundCompleteTime)
		return;
	if (MapConfig.State->RoundEndTime)
		return;

	// help
	uiShowPopup(0, "Press \x11 to equip the Hacker Ray");

	// reset mob count
	MapConfig.State->RoundMaxMobCount = 0;
	MapConfig.SpecialRoundParams[0].SpawnParamCount = 3;

	// disable prestige
	Moby *prestigeMachineMoby = MapConfig.State->PrestigeMachine;
	if (prestigeMachineMoby)
	{
		prestigeMachineMoby->DrawDist = 0;
		prestigeMachineMoby->CollActive = -1;
		prestigeMachineMoby->ModeBits |= MOBY_MODE_BIT_DISABLED;
	}

	// teleport players to boss arena
	int i;
	for (i = 0; i < GAME_MAX_LOCALS; ++i)
	{
		Player *player = playerGetFromSlot(i);
		if (!player || !player->PlayerMoby || !playerIsConnected(player))
			break;

		playerSetPosRot(player, BossArenaLocation, player->PlayerRotation);

		// give hacker ray
		if (player->GadgetBox)
		{
			playerGiveWeapon(player->GadgetBox, WEAPON_ID_HACKER_RAY, 0, 0);
		}
	}

	// move weapon pickups to boss arena
	int count = 0;
	Moby *moby = mobyListGetStart();
	while ((moby = mobyFindNextByOClass(moby, MOBY_ID_WEAPON_PICKUP)))
	{
		if (!mobyIsDestroyed(moby))
		{

			// backup
			vector_copy(WeaponPickupLocationBackups[count], moby->Position);

			// place around arena
			float yaw = (count / (float)BOSS_ARENA_WEP_PICKUP_COUNT) * MATH_TAU;
			VECTOR offset;
			vector_fromyaw(offset, yaw);
			vector_scale(offset, offset, 25);
			vector_add(moby->Position, BossArenaLocation, offset);
			((void (*)(Moby *))moby->PUpdate)(moby);
			mobyUpdateTransform(moby);
			++count;
		}

		if (count >= BOSS_ARENA_WEP_PICKUP_COUNT)
			break;
		++moby;
	}

	// reset hacker orbs
	count = 0;
	moby = mobyListGetStart();
	memset(BossHackerOrbsStates, 0, sizeof(BossHackerOrbsStates));
	while ((moby = mobyFindNextByOClass(moby, MOBY_ID_HACKER_ORB)))
	{
		if (!mobyIsDestroyed(moby))
		{
			BossHackerOrbs[count] = moby;
			mobySetState(moby, 5, -1);
			++count;
		}

		if (count >= BOSS_ARENA_HACKERORB_COUNT)
			break;
		++moby;
	}

	// disable jump pad
	Moby *jumpPadMoby = mobyFindByUID(BOSS_ARENA_JUMPPAD_MOBY_UID);
	if (jumpPadMoby && jumpPadMoby->OClass == MOBY_ID_JUMP_PAD)
	{
		// mobySetState(jumpPadMoby, 1, -1);
		jumpPadMoby->ModeBits |= 1;
		jumpPadMoby->CollActive = -1;
	}

	IsInBossFight = 1;
}

//--------------------------------------------------------------------------
void valixOnBossFight(void)
{
	if (!IsInBossFight)
		return;

	int captured = 0;
	int count = 0;

	// check for hacker orb capture
	int i;
	for (i = 0; i < BOSS_ARENA_HACKERORB_COUNT; ++i)
	{
		Moby *moby = BossHackerOrbs[i];
		if (!moby)
			continue;

		if (moby->State != 4 && BossHackerOrbsStates[i] > 0)
			--BossHackerOrbsStates[i];

		// spawn more mobs when any hacker orb starts being hacked
		if ((moby->State == 1 || moby->State == 4) && BossHackerOrbsStates[i] == 0)
		{
			BossHackerOrbsStates[i] = TPS * 60 * 5;
			MapConfig.State->RoundMaxMobCount += BOSS_ARENA_HACK_ORB_MOB_BONUS;
		}

		captured += moby->State == 4;
		++count;
	}

	// spawn boss
	if (captured == count && IsInBossFight == 1)
	{
		if (mapSpawnMob(MOB_SPAWN_PARAM_KING_LEVIATHAN, BossArenaLocation, 0, -1, 0))
			IsInBossFight = 2;
	}

	// while boss is alive, keep spawning minions
	if (IsInBossFight == 2)
	{

		int spawnMinions = 0;
		if (MapConfig.State && MapConfig.State->BossMoby)
		{
			struct MobPVar *pvars = (struct MobPVar *)MapConfig.State->BossMoby->PVar;
			float health = pvars->MobVars.Health / pvars->MobVars.Config.MaxHealth;
			spawnMinions = health < BOSS_ARENA_MINIONS_AFTER;
		}

		// printf("max:%d total:%d alive:%d spawning:%d ticker:%d\n", MapConfig.State->RoundMaxMobCount, MapConfig.State->MobStats.TotalSpawnedThisRound, MapConfig.State->MobStats.TotalAlive, MapConfig.State->MobStats.TotalSpawning, MapConfig.State->RoundSpawnTicker);
		int mobsLeft = (MapConfig.State->RoundMaxMobCount - MapConfig.State->MobStats.TotalSpawnedThisRound) + MapConfig.State->MobStats.TotalAlive + MapConfig.State->MobStats.TotalSpawning;
		if (spawnMinions && mobsLeft < BOSS_ARENA_MAX_MINIONS)
		{

			MapConfig.State->RoundMaxMobCount += 1;
			MapConfig.SpecialRoundParams[0].SpawnParamCount = 1; // only spawn leviathans
			if (MapConfig.State->RoundSpawnTicker < 0)
				MapConfig.State->RoundSpawnTicker = 1;

			// Area_t area;
			// if (areaGetArea(OOB_AREA_INDEX_BOSS, &area)) {
			//   VECTOR p = {randRange(-1, 1), randRange(-1, 1), 0.1, 0};
			//   SpawnPoint* mobSpawn = spawnPointGet(area.Cuboids[2]);
			//   vector_apply(p, p, mobSpawn->M0);
			//   mapSpawnMob(MOB_SPAWN_PARAM_LEVIATHAN_COMMON, p, 0, -1, 0);
			// }
		}
	}
}

//--------------------------------------------------------------------------
void valixCheckForBossFightStateChange(void)
{
	if (!MapConfig.State)
		return;

	if (MapConfig.State->RoundIsSpecial && !IsInBossFight)
	{
		valixOnBeginBossFight();
	}
	else if (!MapConfig.State->RoundIsSpecial && IsInBossFight)
	{
		valixOnEndBossFight();
	}
}

//--------------------------------------------------------------------------
void valixHopBigAl(VECTOR position, float yaw)
{
	Moby *bigAl = mobyFindNextByOClass(mobyListGetStart(), MOBY_ID_BIG_AL);
	if (bigAl)
	{
		memcpy(bigAl->Position, position, 12);
		bigAl->Rotation[2] = yaw;
		mobyUpdateTransform(bigAl);
	}

	Moby *desk = mobyFindNextByOClass(mobyListGetStart(), 0x1DD7);
	if (desk)
	{
		desk->Rotation[2] = clampAngle(yaw + (MATH_PI / 2));
		vector_fromyaw(desk->Position, yaw);
		vector_scale(desk->Position, desk->Position, 2);
		vector_add(desk->Position, desk->Position, position);
		mobyUpdateTransform(desk);
	}
}

//--------------------------------------------------------------------------
int valixOnHopBigAlRemote(void *connection, void *data)
{
	VECTOR p;
	float yaw;
	memcpy(p, data, 12);
	memcpy(&yaw, data + 12, 4);
	yaw = clampAngle((-yaw + 180) * MATH_DEG2RAD);
	valixHopBigAl(p, yaw);

	return sizeof(VECTOR);
}

//--------------------------------------------------------------------------
void valixCheckForBigAlHop(void)
{
	static int lastRound = 0;

	if (!MapConfig.State)
		return;
	if (MapConfig.State->RoundCompleteTime)
		return;
	if (MapConfig.State->RoundEndTime)
		return;

	int round = MapConfig.State->RoundNumber;
	if (round != lastRound && gameAmIHost())
	{

		// pick random
		int r = rand(BigAlLocationsCount);
		float yaw = clampAngle((-BigAlLocations[r][3] + 180) * MATH_DEG2RAD);
		VECTOR p;
		memcpy(p, BigAlLocations[r], 12);

		// broadcast hop
		void *connection = netGetDmeServerConnection();
		if (connection)
		{
			netBroadcastCustomAppMessage(NET_DELIVERY_CRITICAL, connection, CUSTOM_MSG_TELEPORT_BIG_AL, sizeof(BigAlLocations[r]), BigAlLocations[r]);
		}

		// hop local
		valixHopBigAl(p, yaw);

		lastRound = round;
	}
}

//--------------------------------------------------------------------------
float valixGetSpawnDistanceMultiplier(void)
{
	if (!IsInBossFight)
		return 0.2;

	// guarantee all spawn points in arena are used
	return 0;
}

//--------------------------------------------------------------------------
int valixCanSpawnMobs(void)
{
	if (!IsInBossFight)
		return 1;

	// boss spawned, begin the chaos
	if (IsInBossFight >= 2)
		return 1;

	// only spawn when allocated
	return MapConfig.State->MobStats.TotalSpawnedThisRound < MapConfig.State->RoundMaxMobCount;
}

//--------------------------------------------------------------------------
void valixReturnPlayersToMap(void)
{
	int i;
	VECTOR p, r, o;

	for (i = 0; i < GAME_MAX_LOCALS; ++i)
	{
		Player *player = playerGetFromSlot(i);
		if (!playerIsValid(player))
			continue;

		// check for new respawn cuboid
		int areaIdx = OOB_AREA_INDEX_START;
		Area_t area;
		while (areaIdx <= OOB_AREA_INDEX_END)
		{
			if (!areaGetArea(areaIdx, &area))
				break;
			if (area.CuboidCount <= 1)
				break;
			if (!area.Cuboids)
				break;

			int cuboidIdx = 1;
			while (cuboidIdx < area.CuboidCount)
			{

				SpawnPoint *boxCuboid = spawnPointGet(area.Cuboids[cuboidIdx]);
				if (spawnPointIsPointInside(boxCuboid, player->PlayerPosition, NULL))
				{
					LocalPlayerRespawnCuboid[i] = area.Cuboids[0]; // respawn cuboid is first in list
					areaIdx = 10000;															 // stop loop
					break;
				}

				cuboidIdx++;
			}

			++areaIdx;
		}

		// if we're under the map, teleport back up
		if (player->PlayerPosition[2] < (gameGetDeathHeight() + 1))
		{

			// use respawn cuboid
			if (LocalPlayerRespawnCuboid[i] >= 0)
			{
				SpawnPoint *sp = spawnPointGet(LocalPlayerRespawnCuboid[i]);
				playerSetPosRot(player, &sp->M0[12], &sp->M1[12]);

				// if player is using jump pad don't penalize if jump pad fails
				if (!LocalPlayerInJumpPad[i])
				{
					playerSetHealth(player, maxf(0, player->Health - player->MaxHealth * 0.5));
				}

				LocalPlayerInJumpPad[i] = 0;
				continue;
			}

			// use player start
			playerGetSpawnpoint(player, p, r, 0);
			vector_fromyaw(o, (player->PlayerId / (float)GAME_MAX_PLAYERS) * MATH_TAU - MATH_PI);
			vector_scale(o, o, 2.5);
			vector_add(p, p, o);
			playerSetPosRot(player, p, r);

			// if player is using jump pad don't penalize if jump pad fails
			if (!LocalPlayerInJumpPad[i])
			{
				playerSetHealth(player, maxf(0, player->Health - player->MaxHealth * 0.5));
			}

			LocalPlayerInJumpPad[i] = 0;
		}
	}
}

//--------------------------------------------------------------------------
void valixOnMobKilled(Moby *moby, int killedByPlayerId, int killedByWeaponId)
{
	if (mobyIsMob(moby))
	{
		struct MobPVar *pvars = (struct MobPVar *)moby->PVar;
		if (pvars->MobVars.Config.MobAttribute == MOB_ATTRIBUTE_BOSS && IsInBossFight == 2)
		{
			IsInBossFight = 3;
		}
	}

	mapOnMobKilled(moby, killedByPlayerId, killedByWeaponId);
}

//--------------------------------------------------------------------------
void valixMobForceIntoMapBounds(Moby *moby)
{
	if (!moby)
		return;

	int i;
	VECTOR min = {200, 0, 260, 0};
	VECTOR max = {900, 800, 500, 0};
	struct MobPVar *pvars = (struct MobPVar *)moby->PVar;

	for (i = 0; i < 3; ++i)
	{
		if (moby->Position[i] < min[i])
		{
			moby->Position[i] = min[i];
			pvars->MobVars.Respawn = 1;
			break;
		}
		else if (moby->Position[i] > max[i])
		{
			moby->Position[i] = max[i];
			pvars->MobVars.Respawn = 1;
			break;
		}
	}

	// check for OOB death
	if (!pvars->MobVars.Destroy && !pvars->MobVars.Destroyed && !pvars->MobVars.Respawn)
	{
		Area_t area;
		if (areaGetArea(OOB_AREA_INDEX_DEATH, &area) && area.Cuboids)
		{
			int i;
			for (i = 0; i < area.CuboidCount; ++i)
			{
				if (spawnPointIsPointInside(spawnPointGet(area.Cuboids[i]), moby->Position, NULL))
				{
					pvars->MobVars.Respawn = 1;
					break;
				}
			}
		}
	}
}

//--------------------------------------------------------------------------
int valixGetResurrectPoint(Player *player, VECTOR outPos, VECTOR outRot, int firstRes)
{
	if (firstRes)
		return 0;
	if (!player->IsLocal)
		return 0;

	// use respawn cuboid
	int i = player->PlayerId;
	if (LocalPlayerRespawnCuboid[i] >= 0)
	{
		SpawnPoint *sp = spawnPointGet(LocalPlayerRespawnCuboid[i]);
		vector_copy(outPos, &sp->M0[12]);
		vector_copy(outRot, &sp->M1[12]);
		return 1;
	}

	return 0;
}

//--------------------------------------------------------------------------
void valixCheckPlayerInJumpPad(void)
{
	int i;
	for (i = 0; i < GAME_MAX_LOCALS; ++i)
	{
		Player *player = playerGetFromSlot(i);
		if (!playerIsValid(player))
			continue;

		// if jump pad state, enable walk-through-walls to prevent collision from stopping player
		// after the player lands on safe ground mark them as no longer in jump pad state
		if (player->PlayerState == PLAYER_STATE_MOON_JUMP)
		{
			LocalPlayerInJumpPad[i] = 1;
			player->timers.collOff = 5;
			player->timers.ignoreHeroColl = 5;
		}
		else if (LocalPlayerInJumpPad[i] && (player->Ground.onGood || playerIsDead(player)))
		{
			LocalPlayerInJumpPad[i] = 0;
		}
	}
}

//--------------------------------------------------------------------------
void valixTick(void)
{
	// valixReturnPlayersToMap();
	valixCheckForBigAlHop();
	valixCheckForBossFightStateChange();
	valixOnBossFight();
	valixCheckPlayerInJumpPad();

#if SOULCOLLECTOR
	// force enable all jumppads during infinite post round
	if (MapConfig.State && MapConfig.State->RoundEndTime == -1)
	{
		soulcollectorActivateAll();
	}
#endif
}

//--------------------------------------------------------------------------
void valixInit(void)
{
	netInstallCustomMsgHandler(CUSTOM_MSG_TELEPORT_BIG_AL, &valixOnHopBigAlRemote);

	MapConfig.Functions.CanSpawnMobsFunc = &valixCanSpawnMobs;
	MapConfig.Functions.OnPlayerGetResFunc = &valixGetResurrectPoint;
	MapConfig.Functions.GetSpawnDistanceMultiplierFunc = &valixGetSpawnDistanceMultiplier;

	// hook default survival functions
	HOOK_J_OP(&mapReturnPlayersToMap, &valixReturnPlayersToMap, 0);
	HOOK_J_OP(&mobForceIntoMapBounds, &valixMobForceIntoMapBounds, 0);

	// disable boss jump pad
	Moby *jumpPadMoby = mobyFindByUID(BOSS_ARENA_JUMPPAD_MOBY_UID);
	if (jumpPadMoby && jumpPadMoby->OClass == MOBY_ID_JUMP_PAD)
	{
		jumpPadMoby->ModeBits |= 1;
		jumpPadMoby->CollActive = -1;
	}
}
