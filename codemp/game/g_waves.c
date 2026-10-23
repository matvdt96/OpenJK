/*
===========================================================================
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

#include "g_local.h"

#define WAVES_FILE				"ext_data/waves.txt"
#define WAVES_FILE_SIZE			16384
#define MAX_WAVE_GROUPS			32
#define MAX_WAVE_GROUP_NPCS		16
#define MAX_WAVE_ALIVE			48
#define WAVES_ENEMY_TIME		500
#define WAVES_SPAWN_TRIES		32
#define WAVES_DEFEAT_TIME		6000
#define WAVES_REVIVE_MIN_DIST	64
#define WAVES_REVIVE_MAX_DIST	400
#define WAVES_SPECTATE_FOREVER	0x7fffffff
#define WAVES_HIT_POINTS		10
#define WAVES_HIT_INTERVAL		200
#define WAVES_KILL_POINTS		60
#define WAVES_BOSS_POINTS		500
#define WAVES_ROUND_POINTS		100
#define WAVES_REGEN_DELAY		5000

extern gentity_t *NPC_SpawnTypeAt( vec3_t origin, float yaw, char *npc_type, char *targetname, qboolean isVehicle, team_t teamowner );
extern void NPC_Begin( gentity_t *ent );
extern void G_SetEnemy( gentity_t *self, gentity_t *enemy );
extern void ClientRespawn( gentity_t *ent );
extern void CalculateRanks( void );

typedef struct waveNpc_s {
	char	type[MAX_QPATH];
	int		weight;
} waveNpc_t;

typedef struct waveGroup_s {
	int			fromRound;
	int			toRound;
	int			maxAlive;	// 0 = no limit
	int			numNpcs;
	waveNpc_t	npcs[MAX_WAVE_GROUP_NPCS];
} waveGroup_t;

typedef enum {
	WAVES_WAITING,
	WAVES_INTERMISSION,
	WAVES_ACTIVE,
	WAVES_DEFEAT,
	WAVES_ENDED
} wavesState_t;

typedef struct wavesSettings_s {
	int		firstDelay;
	int		intermission;
	int		maxAliveBase;
	int		maxAlivePerRound;
	int		maxAliveMax;
	int		countBase;
	int		countPerRound;
	int		spawnInterval;
	int		spawnIntervalPerRound;
	int		spawnIntervalMin;
	int		spawnMinDist;
	int		healthPerRound;
	int		healthMax;
	int		damagePerRound;
	int		damageMax;
	int		bossEvery;
	int		bossHealth;
	int		bossHealthGrowth;
} wavesSettings_t;

typedef struct waveEnemy_s {
	int			num;
	float		healthScale;
	qboolean	scaled;
	qboolean	boss;
	int			group;		// index in waveGroups, -1 = boss
} waveEnemy_t;

static wavesSettings_t	wavesSettings;
static waveGroup_t		waveGroups[MAX_WAVE_GROUPS];
static int				numWaveGroups;
static waveGroup_t		bossGroup;
static char				wavesConfigstring[MAX_STRING_CHARS];
static wavesShopItem_t	shopItems[MAX_WAVES_SHOP_ITEMS];
static int				numShopItems;
static int				wavesHitTime[MAX_CLIENTS][MAX_GENTITIES];

static struct {
	wavesState_t	state;
	int				round;
	int				stateTime;
	int				toSpawn;
	int				nextSpawnTime;
	int				nextEnemyTime;
	qboolean		bossPending;
	int				numBosses;
	waveEnemy_t		alive[MAX_WAVE_ALIVE];
	int				numAlive;
} waves;

static void G_WavesDefaultSettings( void ) {
	wavesSettings.firstDelay = 5000;
	wavesSettings.intermission = 10000;
	wavesSettings.maxAliveBase = 8;
	wavesSettings.maxAlivePerRound = 1;
	wavesSettings.maxAliveMax = 24;
	wavesSettings.countBase = 6;
	wavesSettings.countPerRound = 2;
	wavesSettings.spawnInterval = 500;
	wavesSettings.spawnIntervalPerRound = 25;
	wavesSettings.spawnIntervalMin = 250;
	wavesSettings.spawnMinDist = 512;
	wavesSettings.healthPerRound = 4;
	wavesSettings.healthMax = 200;
	wavesSettings.damagePerRound = 2;
	wavesSettings.damageMax = 150;
	wavesSettings.bossEvery = 4;
	wavesSettings.bossHealth = 5;
	wavesSettings.bossHealthGrowth = 25;
}

static qboolean G_WavesParseInt( const char **p, int *out ) {
	char *token = COM_ParseExt( p, qfalse );

	if ( !token[0] ) {
		return qfalse;
	}
	*out = atoi( token );
	return qtrue;
}

static void G_WavesParseSettings( const char **p ) {
	char	token[MAX_TOKEN_CHARS];
	int		value;

	while ( 1 ) {
		Q_strncpyz( token, COM_ParseExt( p, qtrue ), sizeof( token ) );
		if ( !token[0] || !Q_stricmp( token, "}" ) ) {
			return;
		}
		if ( !G_WavesParseInt( p, &value ) ) {
			Com_Printf( S_COLOR_YELLOW "%s: missing value for '%s'\n", WAVES_FILE, token );
			continue;
		}

		if ( !Q_stricmp( token, "firstDelay" ) )			wavesSettings.firstDelay = value * 1000;
		else if ( !Q_stricmp( token, "intermission" ) )		wavesSettings.intermission = value * 1000;
		else if ( !Q_stricmp( token, "maxAliveBase" ) )			wavesSettings.maxAliveBase = value;
		else if ( !Q_stricmp( token, "maxAlivePerRound" ) )		wavesSettings.maxAlivePerRound = value;
		else if ( !Q_stricmp( token, "maxAliveMax" ) )			wavesSettings.maxAliveMax = value;
		else if ( !Q_stricmp( token, "countBase" ) )			wavesSettings.countBase = value;
		else if ( !Q_stricmp( token, "countPerRound" ) )		wavesSettings.countPerRound = value;
		else if ( !Q_stricmp( token, "spawnInterval" ) )		wavesSettings.spawnInterval = value;
		else if ( !Q_stricmp( token, "spawnIntervalPerRound" ) )	wavesSettings.spawnIntervalPerRound = value;
		else if ( !Q_stricmp( token, "spawnIntervalMin" ) )		wavesSettings.spawnIntervalMin = value;
		else if ( !Q_stricmp( token, "spawnMinDist" ) )			wavesSettings.spawnMinDist = value;
		else if ( !Q_stricmp( token, "healthPerRound" ) )		wavesSettings.healthPerRound = value;
		else if ( !Q_stricmp( token, "healthMax" ) )			wavesSettings.healthMax = value;
		else if ( !Q_stricmp( token, "damagePerRound" ) )		wavesSettings.damagePerRound = value;
		else if ( !Q_stricmp( token, "damageMax" ) )			wavesSettings.damageMax = value;
		else if ( !Q_stricmp( token, "bossEvery" ) )			wavesSettings.bossEvery = value;
		else if ( !Q_stricmp( token, "bossHealth" ) )			wavesSettings.bossHealth = value;
		else if ( !Q_stricmp( token, "bossHealthGrowth" ) )		wavesSettings.bossHealthGrowth = value;
		else Com_Printf( S_COLOR_YELLOW "%s: unknown setting '%s'\n", WAVES_FILE, token );
	}
}

static void G_WavesParseGroup( const char **p, waveGroup_t *group ) {
	waveNpc_t	*npc;
	char		*token;

	memset( group, 0, sizeof( *group ) );
	group->fromRound = 1;

	while ( 1 ) {
		token = COM_ParseExt( p, qtrue );
		if ( !token[0] || !Q_stricmp( token, "}" ) ) {
			break;
		}

		if ( !Q_stricmp( token, "rounds" ) ) {
			G_WavesParseInt( p, &group->fromRound );
			G_WavesParseInt( p, &group->toRound );
		}
		else if ( !Q_stricmp( token, "maxAlive" ) ) {
			G_WavesParseInt( p, &group->maxAlive );
		}
		else if ( !Q_stricmp( token, "npc" ) ) {
			if ( group->numNpcs >= MAX_WAVE_GROUP_NPCS ) {
				Com_Printf( S_COLOR_YELLOW "%s: too many npcs in a group (max %d)\n", WAVES_FILE, MAX_WAVE_GROUP_NPCS );
				SkipRestOfLine( p );
				continue;
			}
			npc = &group->npcs[group->numNpcs];
			token = COM_ParseExt( p, qfalse );
			Q_strncpyz( npc->type, token, sizeof( npc->type ) );
			if ( !G_WavesParseInt( p, &npc->weight ) ) {
				npc->weight = 1;
			}
			if ( npc->type[0] && npc->weight > 0 ) {
				group->numNpcs++;
			}
		}
		else {
			Com_Printf( S_COLOR_YELLOW "%s: unknown group key '%s'\n", WAVES_FILE, token );
			SkipRestOfLine( p );
		}
	}
}

static void G_WavesLoad( void ) {
	static char		buf[WAVES_FILE_SIZE];
	fileHandle_t	f;
	const char		*p;
	char			*token;
	int				len;

	G_WavesDefaultSettings();
	numWaveGroups = 0;
	memset( &bossGroup, 0, sizeof( bossGroup ) );

	len = trap->FS_Open( WAVES_FILE, &f, FS_READ );
	if ( !f ) {
		Com_Printf( S_COLOR_YELLOW "%s not found, using a default wave\n", WAVES_FILE );
	}
	else if ( len >= WAVES_FILE_SIZE ) {
		Com_Printf( S_COLOR_YELLOW "%s is too big (max %d bytes), using a default wave\n", WAVES_FILE, WAVES_FILE_SIZE - 1 );
		trap->FS_Close( f );
	}
	else {
		trap->FS_Read( buf, len, f );
		buf[len] = 0;
		trap->FS_Close( f );

		p = buf;
		COM_BeginParseSession( WAVES_FILE );
		while ( 1 ) {
			char	block[MAX_TOKEN_CHARS];

			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] ) {
				break;
			}
			Q_strncpyz( block, token, sizeof( block ) );
			token = COM_ParseExt( &p, qtrue );
			if ( Q_stricmp( token, "{" ) ) {
				Com_Printf( S_COLOR_YELLOW "%s: expected '{' after '%s'\n", WAVES_FILE, block );
				break;
			}
			token = block;
			if ( !Q_stricmp( token, "settings" ) ) {
				G_WavesParseSettings( &p );
			}
			else if ( !Q_stricmp( token, "group" ) ) {
				if ( numWaveGroups >= MAX_WAVE_GROUPS ) {
					Com_Printf( S_COLOR_YELLOW "%s: too many groups (max %d)\n", WAVES_FILE, MAX_WAVE_GROUPS );
					SkipBracedSection( &p, 1 );
					continue;
				}
				G_WavesParseGroup( &p, &waveGroups[numWaveGroups] );
				if ( waveGroups[numWaveGroups].numNpcs ) {
					numWaveGroups++;
				}
			}
			else if ( !Q_stricmp( token, "boss" ) ) {
				G_WavesParseGroup( &p, &bossGroup );
			}
			else {
				Com_Printf( S_COLOR_YELLOW "%s: unknown block '%s'\n", WAVES_FILE, token );
				SkipBracedSection( &p, 1 );
			}
		}
	}

	if ( !numWaveGroups ) {
		waveGroups[0].fromRound = 1;
		waveGroups[0].toRound = 0;
		waveGroups[0].numNpcs = 1;
		Q_strncpyz( waveGroups[0].npcs[0].type, "stormtrooper", sizeof( waveGroups[0].npcs[0].type ) );
		waveGroups[0].npcs[0].weight = 1;
		numWaveGroups = 1;
	}

	Com_Printf( "Waves: %d groups and %d bosses loaded\n", numWaveGroups, bossGroup.numNpcs );
}

static qboolean G_WavesPlayerAlive( gentity_t *ent ) {
	return ( ent->inuse && ent->client
		&& ent->client->pers.connected == CON_CONNECTED
		&& ent->client->sess.sessionTeam == TEAM_BLUE
		&& !(ent->client->ps.pm_flags & PMF_FOLLOW)
		&& ent->client->tempSpectate < level.time
		&& ent->health > 0 );
}

static gentity_t *G_WavesClosestPlayer( vec3_t origin ) {
	gentity_t	*best = NULL;
	float		bestDist = 0.0f, dist;
	int			i;

	for ( i = 0; i < level.maxclients; i++ ) {
		gentity_t *ent = &g_entities[i];

		if ( !G_WavesPlayerAlive( ent ) ) {
			continue;
		}
		dist = DistanceSquared( origin, ent->r.currentOrigin );
		if ( !best || dist < bestDist ) {
			best = ent;
			bestDist = dist;
		}
	}
	return best;
}

static qboolean G_WavesGroupActive( waveGroup_t *groups, int index, qboolean checkRounds ) {
	waveGroup_t	*group = &groups[index];
	int			i, alive = 0;

	if ( !checkRounds ) {
		return qtrue;
	}
	if ( waves.round < group->fromRound || (group->toRound && waves.round > group->toRound) ) {
		return qfalse;
	}
	if ( group->maxAlive > 0 ) {
		for ( i = 0; i < waves.numAlive; i++ ) {
			if ( waves.alive[i].group == index ) {
				alive++;
			}
		}
		if ( alive >= group->maxAlive ) {
			return qfalse;
		}
	}
	return qtrue;
}

static const char *G_WavesPickNpc( waveGroup_t *groups, int numGroups, qboolean checkRounds, int *pickedGroup ) {
	int	total = 0, pick, i, j;

	for ( i = 0; i < numGroups; i++ ) {
		waveGroup_t *group = &groups[i];

		if ( !G_WavesGroupActive( groups, i, checkRounds ) ) {
			continue;
		}
		for ( j = 0; j < group->numNpcs; j++ ) {
			total += group->npcs[j].weight;
		}
	}
	if ( !total ) {
		return NULL;
	}

	pick = Q_irand( 0, total - 1 );
	for ( i = 0; i < numGroups; i++ ) {
		waveGroup_t *group = &groups[i];

		if ( !G_WavesGroupActive( groups, i, checkRounds ) ) {
			continue;
		}
		for ( j = 0; j < group->numNpcs; j++ ) {
			pick -= group->npcs[j].weight;
			if ( pick < 0 ) {
				*pickedGroup = i;
				return group->npcs[j].type;
			}
		}
	}
	return NULL;
}

static qboolean G_WavesSpotFree( vec3_t spot ) {
	vec3_t	mins, maxs;
	trace_t	tr;

	VectorSet( mins, -16, -16, DEFAULT_MINS_2 );
	VectorSet( maxs, 16, 16, DEFAULT_MAXS_2 );
	trap->Trace( &tr, spot, mins, maxs, spot, ENTITYNUM_NONE, MASK_NPCSOLID, qfalse, 0, 0 );
	return ( !tr.startsolid && !tr.allsolid );
}

static qboolean G_WavesSpotHidden( vec3_t spot ) {
	vec3_t	eye;
	trace_t	tr;
	int		i;

	for ( i = 0; i < level.maxclients; i++ ) {
		gentity_t *ent = &g_entities[i];

		if ( !G_WavesPlayerAlive( ent ) ) {
			continue;
		}
		if ( Distance( spot, ent->r.currentOrigin ) < wavesSettings.spawnMinDist ) {
			return qfalse;
		}
		VectorCopy( ent->r.currentOrigin, eye );
		eye[2] += ent->client->ps.viewheight;
		trap->Trace( &tr, eye, NULL, NULL, spot, ent->s.number, MASK_OPAQUE, qfalse, 0, 0 );
		if ( tr.fraction == 1.0f ) {
			return qfalse;
		}
	}
	return qtrue;
}

static qboolean G_WavesPickSpot( vec3_t spot ) {
	int	numNodes = trap->Nav_GetNumNodes();
	int	i;

	if ( numNodes <= 0 ) {
		return qfalse;
	}

	for ( i = 0; i < WAVES_SPAWN_TRIES; i++ ) {
		trap->Nav_GetNodePosition( Q_irand( 0, numNodes - 1 ), spot );
		if ( G_WavesSpotHidden( spot ) && G_WavesSpotFree( spot ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static float G_WavesRoundScale( int perRound, int max ) {
	float scale = 1.0f + perRound * (waves.round - 1) / 100.0f;

	if ( max > 0 && scale > max / 100.0f ) {
		scale = max / 100.0f;
	}
	return scale;
}

static int G_WavesMaxAlive( void ) {
	int maxAlive = wavesSettings.maxAliveBase + wavesSettings.maxAlivePerRound * (waves.round - 1);

	if ( maxAlive > wavesSettings.maxAliveMax ) {
		maxAlive = wavesSettings.maxAliveMax;
	}
	return Com_Clampi( 1, MAX_WAVE_ALIVE, maxAlive );
}

static int G_WavesSpawnInterval( void ) {
	int interval = wavesSettings.spawnInterval - wavesSettings.spawnIntervalPerRound * (waves.round - 1);

	return ( interval < wavesSettings.spawnIntervalMin ) ? wavesSettings.spawnIntervalMin : interval;
}

static void G_WavesSpawnEnemy( qboolean boss ) {
	gentity_t	*npc, *target;
	const char	*type;
	vec3_t		spot, dir, angles;
	float		yaw = 0.0f;
	waveEnemy_t	*enemy;
	int			group = -1;

	if ( boss ) {
		type = G_WavesPickNpc( &bossGroup, 1, qfalse, &group );
		group = -1;
	}
	else {
		type = G_WavesPickNpc( waveGroups, numWaveGroups, qtrue, &group );
	}
	if ( !type ) {
		if ( boss ) {
			waves.bossPending = qfalse;
		}
		return;
	}
	if ( !G_WavesPickSpot( spot ) ) {
		return;
	}

	target = G_WavesClosestPlayer( spot );
	if ( target ) {
		VectorSubtract( target->r.currentOrigin, spot, dir );
		vectoangles( dir, angles );
		yaw = angles[YAW];
	}

	npc = NPC_SpawnTypeAt( spot, yaw, (char *)type, NULL, qfalse, TEAM_RED );
	if ( boss ) {
		waves.bossPending = qfalse;
	}
	else {
		waves.toSpawn--;
	}
	if ( !npc || !npc->client ) {
		Com_Printf( S_COLOR_YELLOW "Waves: could not spawn '%s'\n", type );
		return;
	}

	npc->client->playerTeam = NPCTEAM_ENEMY;
	npc->client->enemyTeam = NPCTEAM_PLAYER;

	enemy = &waves.alive[waves.numAlive++];
	enemy->num = npc->s.number;
	enemy->scaled = qfalse;
	enemy->boss = boss;
	enemy->group = group;
	if ( boss ) {
		waves.numBosses++;
		enemy->healthScale = wavesSettings.bossHealth * (1.0f + wavesSettings.bossHealthGrowth * (waves.numBosses - 1) / 100.0f);
		trap->SendServerCommand( -1, va( "cp \"Round %d\nBoss: %s\"", waves.round, type ) );
	}
	else {
		enemy->healthScale = G_WavesRoundScale( wavesSettings.healthPerRound, wavesSettings.healthMax );
	}
}

static void G_WavesScaleHealth( gentity_t *npc, float scale ) {
	int health = (int)(npc->health * scale);

	if ( health < 1 ) {
		health = 1;
	}
	npc->health = npc->client->ps.stats[STAT_HEALTH] = health;
	npc->client->pers.maxHealth = npc->client->ps.stats[STAT_MAX_HEALTH] = health;
}

static void G_WavesPruneDead( void ) {
	int	i = 0;

	while ( i < waves.numAlive ) {
		waveEnemy_t	*enemy = &waves.alive[i];
		gentity_t	*npc = &g_entities[enemy->num];

		if ( !npc->inuse || !npc->client
			|| (npc->think != NPC_Begin && (npc->s.eType != ET_NPC || npc->health <= 0)) ) {
			*enemy = waves.alive[--waves.numAlive];
			continue;
		}
		if ( !enemy->scaled && npc->think != NPC_Begin ) {
			G_WavesScaleHealth( npc, enemy->healthScale );
			enemy->scaled = qtrue;
		}
		i++;
	}
}

static void G_WavesAssignEnemies( void ) {
	int	i;

	for ( i = 0; i < waves.numAlive; i++ ) {
		gentity_t *npc = &g_entities[waves.alive[i].num];
		gentity_t *target;

		if ( !npc->NPC || npc->think == NPC_Begin ) {
			continue;
		}
		if ( npc->enemy && G_WavesPlayerAlive( npc->enemy ) ) {
			continue;
		}
		target = G_WavesClosestPlayer( npc->r.currentOrigin );
		if ( target ) {
			G_SetEnemy( npc, target );
		}
	}
}

static int G_WavesNumPlayers( void ) {
	int	i, count = 0;

	for ( i = 0; i < level.maxclients; i++ ) {
		gclient_t *cl = &level.clients[i];

		if ( cl->pers.connected == CON_CONNECTED && cl->sess.sessionTeam == TEAM_BLUE ) {
			count++;
		}
	}
	return count;
}

static int G_WavesNumAlivePlayers( void ) {
	int	i, count = 0;

	for ( i = 0; i < level.maxclients; i++ ) {
		if ( G_WavesPlayerAlive( &g_entities[i] ) ) {
			count++;
		}
	}
	return count;
}

static void G_WavesReviveAll( void ) {
	int	i;

	for ( i = 0; i < level.maxclients; i++ ) {
		gentity_t *ent = &g_entities[i];

		if ( !ent->inuse || !ent->client || ent->client->pers.connected != CON_CONNECTED
			|| ent->client->sess.sessionTeam != TEAM_BLUE ) {
			continue;
		}

		if ( ent->client->tempSpectate >= level.time || ent->health <= 0 ) {
			ent->client->tempSpectate = 0;
			ClientRespawn( ent );
		}
		else {
			ent->health = ent->client->ps.stats[STAT_HEALTH] = ent->client->ps.stats[STAT_MAX_HEALTH];
			ent->client->ps.fd.forcePower = ent->client->ps.fd.forcePowerMax;
		}
	}
}

static void G_WavesStartIntermission( int delay ) {
	waves.state = WAVES_INTERMISSION;
	waves.stateTime = level.time + delay;
	G_WavesReviveAll();
}

static void G_WavesEndGame( void ) {
	waves.state = WAVES_ENDED;

	if ( trap->Cvar_VariableIntegerValue( "dedicated" ) ) {
		trap->SendConsoleCommand( EXEC_APPEND, "map_restart 0\n" );
	}
	else {
		trap->Cvar_Set( "ui_returnMenu", "createserver" );
		trap->SendConsoleCommand( EXEC_APPEND, "disconnect\n" );
	}
}

static void G_WavesStartRound( void ) {
	waves.round++;
	waves.toSpawn = wavesSettings.countBase + wavesSettings.countPerRound * (waves.round - 1);
	waves.bossPending = ( wavesSettings.bossEvery > 0 && bossGroup.numNpcs && !(waves.round % wavesSettings.bossEvery) );
	waves.nextSpawnTime = level.time;
	waves.state = WAVES_ACTIVE;
	trap->SendServerCommand( -1, va( "cp \"Round %d\"", waves.round ) );
}

static void G_WavesAddPoints( gentity_t *ent, int points ) {
	ent->client->ps.persistant[PERS_SCORE] += points;
	CalculateRanks();
}

static void G_WavesRoundBonus( void ) {
	int	i;

	for ( i = 0; i < level.maxclients; i++ ) {
		if ( G_WavesPlayerAlive( &g_entities[i] ) ) {
			G_WavesAddPoints( &g_entities[i], WAVES_ROUND_POINTS );
		}
	}
}

static waveEnemy_t *G_WavesFindEnemy( gentity_t *ent ) {
	int	i;

	for ( i = 0; i < waves.numAlive; i++ ) {
		if ( waves.alive[i].num == ent->s.number ) {
			return &waves.alive[i];
		}
	}
	return NULL;
}

void G_WavesEnemyDamaged( gentity_t *targ, gentity_t *attacker ) {
	int	*hitTime;

	if ( !attacker || !attacker->client || attacker->s.number >= MAX_CLIENTS || !G_WavesFindEnemy( targ ) ) {
		return;
	}
	hitTime = &wavesHitTime[attacker->s.number][targ->s.number];
	if ( level.time - *hitTime < WAVES_HIT_INTERVAL && *hitTime <= level.time ) {
		return;
	}
	*hitTime = level.time;
	G_WavesAddPoints( attacker, WAVES_HIT_POINTS );
}

void G_WavesEnemyKilled( gentity_t *self, gentity_t *attacker ) {
	waveEnemy_t	*enemy;

	if ( !attacker || !attacker->client || attacker->s.number >= MAX_CLIENTS ) {
		return;
	}
	enemy = G_WavesFindEnemy( self );
	if ( enemy ) {
		G_WavesAddPoints( attacker, enemy->boss ? WAVES_BOSS_POINTS : WAVES_KILL_POINTS );
	}
}

void G_WavesGivePoints( int points ) {
	int	i;

	for ( i = 0; i < level.maxclients; i++ ) {
		gentity_t *ent = &g_entities[i];

		if ( ent->inuse && ent->client && ent->client->pers.connected == CON_CONNECTED
			&& ent->client->sess.sessionTeam == TEAM_BLUE ) {
			G_WavesAddPoints( ent, points );
		}
	}
}

static int G_WavesItemLevel( gentity_t *ent, int index ) {
	const wavesShopItem_t	*item = &shopItems[index];
	int						itemLevel = ent->client->pers.waves.levels[index];

	if ( item->type == WAVES_ITEM_POWER && ent->client->pers.waves.role == WAVES_ROLE_FORCE && itemLevel < item->baseLevel ) {
		itemLevel = item->baseLevel;
	}
	return itemLevel;
}

static int G_WavesTotalLevels( gentity_t *ent, int type ) {
	int	i, total = 0;

	for ( i = 0; i < numShopItems; i++ ) {
		if ( shopItems[i].type == type ) {
			total += ent->client->pers.waves.levels[i];
		}
	}
	return total;
}

static int G_WavesBonus( gentity_t *ent, int type ) {
	int	i, bonus = 0;

	for ( i = 0; i < numShopItems; i++ ) {
		if ( shopItems[i].type == type ) {
			bonus += ent->client->pers.waves.levels[i] * shopItems[i].amount;
		}
	}
	return bonus;
}

int G_WavesMaxAmmo( gentity_t *ent, int ammo ) {
	if ( !ent->client ) {
		return ammoData[ammo].max;
	}
	return ammoData[ammo].max * ( 100 + G_WavesBonus( ent, WAVES_ITEM_MAXAMMO ) ) / 100;
}

int G_WavesForceRegenTime( gentity_t *ent, int regenTime ) {
	if ( level.gametype != GT_WAVES || ent->s.number >= MAX_CLIENTS ) {
		return regenTime;
	}
	return regenTime * 100 / ( 100 + G_WavesBonus( ent, WAVES_ITEM_FORCEREGEN ) );
}

static int G_WavesMaxHealth( gentity_t *ent ) {
	return WAVES_BASE_HEALTH + WAVES_HEALTH_STEP * G_WavesTotalLevels( ent, WAVES_ITEM_MAXHEALTH );
}

static int G_WavesMaxShield( gentity_t *ent ) {
	return WAVES_BASE_SHIELD + WAVES_SHIELD_STEP * G_WavesTotalLevels( ent, WAVES_ITEM_MAXSHIELD );
}

static void G_WavesSendInventory( gentity_t *ent ) {
	char	buf[MAX_STRING_CHARS];
	int		i;

	Com_sprintf( buf, sizeof( buf ), "wavesinv %i", ent->client->pers.waves.role );
	for ( i = 0; i < numShopItems; i++ ) {
		Q_strcat( buf, sizeof( buf ), va( " %i", G_WavesItemLevel( ent, i ) ) );
	}
	trap->SendServerCommand( ent->s.number, buf );
}

static void G_WavesGiveWeapon( gentity_t *ent, int weapon ) {
	gclient_t	*client = ent->client;
	int			ammo = weaponData[weapon].ammoIndex;

	client->ps.stats[STAT_WEAPONS] |= (1 << weapon);
	if ( ammo > AMMO_NONE && ammo < AMMO_MAX ) {
		client->ps.ammo[ammo] = G_WavesMaxAmmo( ent, ammo );
	}
}

static void G_WavesSetPower( gentity_t *ent, int power, int itemLevel ) {
	playerState_t *ps = &ent->client->ps;

	ps->fd.forcePowerLevel[power] = ps->fd.forcePowerBaseLevel[power] = itemLevel;
	if ( itemLevel > 0 ) {
		ps->fd.forcePowersKnown |= (1 << power);
	}
	else {
		ps->fd.forcePowersKnown &= ~(1 << power);
	}
}

static void G_WavesClampSaberStyle( gentity_t *ent ) {
	playerState_t	*ps = &ent->client->ps;
	int				maxStyle = ps->fd.forcePowerLevel[FP_SABER_OFFENSE];

	if ( maxStyle < FORCE_LEVEL_1 ) {
		maxStyle = FORCE_LEVEL_1;
	}
	if ( ps->fd.saberAnimLevel > maxStyle && ps->fd.saberAnimLevel < SS_DUAL ) {
		ps->fd.saberAnimLevel = maxStyle;
	}
	if ( ps->fd.saberDrawAnimLevel > maxStyle && ps->fd.saberDrawAnimLevel < SS_DUAL ) {
		ps->fd.saberDrawAnimLevel = maxStyle;
	}
	if ( ps->fd.saberAnimLevelBase > maxStyle && ps->fd.saberAnimLevelBase < SS_DUAL ) {
		ps->fd.saberAnimLevelBase = maxStyle;
	}
}

void G_WavesApplyLoadout( gentity_t *ent ) {
	gclient_t	*client = ent->client;
	int			role = client->pers.waves.role ? client->pers.waves.role : WAVES_ROLE_FORCE;
	int			i, maxHealth, maxShield;

	if ( level.gametype != GT_WAVES || client->sess.sessionTeam != TEAM_BLUE ) {
		return;
	}

	client->ps.stats[STAT_WAVES_ROLE] = client->pers.waves.role;
	client->pers.waves.joined = qtrue;

	for ( i = 0; i < NUM_FORCE_POWERS; i++ ) {
		G_WavesSetPower( ent, i, 0 );
	}
	memset( client->ps.ammo, 0, sizeof( client->ps.ammo ) );

	if ( role == WAVES_ROLE_FORCE ) {
		client->ps.stats[STAT_WEAPONS] = (1 << WP_SABER) | (1 << WP_MELEE);
		client->ps.weapon = WP_SABER;
		for ( i = 0; i < numShopItems; i++ ) {
			if ( shopItems[i].type == WAVES_ITEM_POWER ) {
				G_WavesSetPower( ent, shopItems[i].param, G_WavesItemLevel( ent, i ) );
			}
		}
		G_WavesClampSaberStyle( ent );
	}
	else {
		client->ps.stats[STAT_WEAPONS] = (1 << WP_BRYAR_PISTOL) | (1 << WP_MELEE);
		client->ps.weapon = WP_BRYAR_PISTOL;
		client->ps.ammo[weaponData[WP_BRYAR_PISTOL].ammoIndex] = G_WavesMaxAmmo( ent, weaponData[WP_BRYAR_PISTOL].ammoIndex );
		for ( i = 0; i < numShopItems; i++ ) {
			if ( shopItems[i].type == WAVES_ITEM_WEAPON && client->pers.waves.levels[i] ) {
				G_WavesGiveWeapon( ent, shopItems[i].param );
			}
		}
		for ( i = 0; i < numShopItems; i++ ) {
			if ( shopItems[i].type == WAVES_ITEM_CONSUMABLE ) {
				int ammo = weaponData[shopItems[i].param].ammoIndex;

				if ( client->pers.waves.consumables[ammo] > 0 ) {
					client->ps.stats[STAT_WEAPONS] |= (1 << shopItems[i].param);
					client->ps.ammo[ammo] = client->pers.waves.consumables[ammo];
				}
			}
		}
	}

	maxHealth = G_WavesMaxHealth( ent );
	maxShield = G_WavesMaxShield( ent );
	client->pers.maxHealth = client->ps.stats[STAT_MAX_HEALTH] = maxHealth;
	ent->health = client->ps.stats[STAT_HEALTH] = maxHealth;
	if ( client->ps.stats[STAT_ARMOR] > maxShield ) {
		client->ps.stats[STAT_ARMOR] = maxShield;
	}

	G_WavesSendInventory( ent );
}

void G_WavesSaveConsumables( gentity_t *ent ) {
	int	i;

	if ( level.gametype != GT_WAVES || !ent->client ) {
		return;
	}
	for ( i = 0; i < numShopItems; i++ ) {
		if ( shopItems[i].type == WAVES_ITEM_CONSUMABLE ) {
			int ammo = weaponData[shopItems[i].param].ammoIndex;

			ent->client->pers.waves.consumables[ammo] = ent->client->ps.ammo[ammo];
		}
	}
}

static qboolean G_WavesCanChangeRole( gentity_t *ent ) {
	return ( !ent->client->pers.waves.role || !ent->client->pers.waves.joined );
}

void G_WavesSetRole( gentity_t *ent, const char *name ) {
	int	role;

	if ( level.gametype != GT_WAVES || !ent->client ) {
		return;
	}
	if ( !Q_stricmp( name, "force" ) ) {
		role = WAVES_ROLE_FORCE;
	}
	else if ( !Q_stricmp( name, "gunner" ) ) {
		role = WAVES_ROLE_GUNNER;
	}
	else {
		trap->SendServerCommand( ent->s.number, "print \"usage: waves_setrole force | gunner\n\"" );
		return;
	}
	if ( role == ent->client->pers.waves.role ) {
		return;
	}
	if ( !G_WavesCanChangeRole( ent ) ) {
		trap->SendServerCommand( ent->s.number, "print \"You can't change role after joining the game\n\"" );
		return;
	}

	G_WavesSaveConsumables( ent );
	ent->client->pers.waves.role = role;
	if ( G_WavesPlayerAlive( ent ) ) {
		G_WavesApplyLoadout( ent );
	}
	else {
		ent->client->ps.stats[STAT_WAVES_ROLE] = role;
		G_WavesSendInventory( ent );
	}
}

static void G_WavesApplyItem( gentity_t *ent, const wavesShopItem_t *item, int itemLevel ) {
	gclient_t	*client = ent->client;
	int			ammo, maxShield;

	switch ( item->type ) {
	case WAVES_ITEM_WEAPON:
		G_WavesGiveWeapon( ent, item->param );
		break;
	case WAVES_ITEM_AMMO:
		for ( ammo = AMMO_NONE + 1; ammo < AMMO_MAX; ammo++ ) {
			int w;

			for ( w = WP_NONE + 1; w < WP_NUM_WEAPONS; w++ ) {
				if ( (client->ps.stats[STAT_WEAPONS] & (1 << w)) && weaponData[w].ammoIndex == ammo
					&& w != WP_THERMAL && w != WP_TRIP_MINE && w != WP_DET_PACK ) {
					client->ps.ammo[ammo] = G_WavesMaxAmmo( ent, ammo );
					break;
				}
			}
		}
		break;
	case WAVES_ITEM_CONSUMABLE:
		ammo = weaponData[item->param].ammoIndex;
		client->ps.stats[STAT_WEAPONS] |= (1 << item->param);
		client->ps.ammo[ammo] += item->amount;
		if ( client->ps.ammo[ammo] > G_WavesMaxAmmo( ent, ammo ) ) {
			client->ps.ammo[ammo] = G_WavesMaxAmmo( ent, ammo );
		}
		client->pers.waves.consumables[ammo] = client->ps.ammo[ammo];
		break;
	case WAVES_ITEM_POWER:
		G_WavesSetPower( ent, item->param, itemLevel );
		break;
	case WAVES_ITEM_MAXHEALTH:
		client->pers.maxHealth = client->ps.stats[STAT_MAX_HEALTH] = G_WavesMaxHealth( ent );
		ent->health += WAVES_HEALTH_STEP;
		if ( ent->health > client->ps.stats[STAT_MAX_HEALTH] ) {
			ent->health = client->ps.stats[STAT_MAX_HEALTH];
		}
		client->ps.stats[STAT_HEALTH] = ent->health;
		break;
	case WAVES_ITEM_MAXSHIELD:
		client->ps.stats[STAT_ARMOR] += WAVES_SHIELD_STEP;
		maxShield = G_WavesMaxShield( ent );
		if ( client->ps.stats[STAT_ARMOR] > maxShield ) {
			client->ps.stats[STAT_ARMOR] = maxShield;
		}
		break;
	case WAVES_ITEM_SHIELD:
		client->ps.stats[STAT_ARMOR] = G_WavesMaxShield( ent );
		break;
	default:
		break;
	}
}

void G_WavesBuy( gentity_t *ent, const char *name ) {
	gclient_t				*client = ent->client;
	const wavesShopItem_t	*item = NULL;
	int						i, index = -1, itemLevel, price, role;

	if ( level.gametype != GT_WAVES || !client || client->sess.sessionTeam != TEAM_BLUE ) {
		return;
	}
	for ( i = 0; i < numShopItems; i++ ) {
		if ( !Q_stricmp( shopItems[i].name, name ) ) {
			item = &shopItems[i];
			index = i;
			break;
		}
	}
	if ( !item ) {
		trap->SendServerCommand( ent->s.number, va( "print \"Unknown item '%s'\n\"", name ) );
		return;
	}

	role = client->pers.waves.role;
	if ( item->role != WAVES_ROLE_NONE && item->role != role ) {
		trap->SendServerCommand( ent->s.number, va( "print \"%s is not for your role\n\"", item->title ) );
		return;
	}
	if ( item->type == WAVES_ITEM_SHIELD && client->ps.stats[STAT_ARMOR] >= G_WavesMaxShield( ent ) ) {
		trap->SendServerCommand( ent->s.number, "print \"Your shield is already full\n\"" );
		return;
	}

	itemLevel = G_WavesItemLevel( ent, index );
	price = BG_WavesItemPrice( item, itemLevel );
	if ( price < 0 ) {
		trap->SendServerCommand( ent->s.number, va( "print \"%s is already at its best\n\"", item->title ) );
		return;
	}
	if ( client->ps.persistant[PERS_SCORE] < price ) {
		trap->SendServerCommand( ent->s.number, va( "print \"Not enough points for %s (%i)\n\"", item->title, price ) );
		return;
	}

	G_WavesAddPoints( ent, -price );
	if ( item->numLevels ) {
		client->pers.waves.levels[index] = itemLevel + 1;
	}
	if ( item->type == WAVES_ITEM_CONSUMABLE && !G_WavesPlayerAlive( ent ) ) {
		int ammo = weaponData[item->param].ammoIndex;

		client->pers.waves.consumables[ammo] += item->amount;
		if ( client->pers.waves.consumables[ammo] > G_WavesMaxAmmo( ent, ammo ) ) {
			client->pers.waves.consumables[ammo] = G_WavesMaxAmmo( ent, ammo );
		}
	}
	else if ( G_WavesPlayerAlive( ent ) ) {
		G_WavesApplyItem( ent, item, itemLevel + 1 );
	}
	G_WavesSendInventory( ent );
	trap->SendServerCommand( ent->s.number, va( "print \"Bought %s\n\"", item->title ) );
}

void G_WavesInit( void ) {
	memset( &waves, 0, sizeof( waves ) );
	waves.state = WAVES_WAITING;
	wavesConfigstring[0] = '\0';
	memset( wavesHitTime, 0, sizeof( wavesHitTime ) );
	G_WavesLoad();
	numShopItems = BG_WavesLoadShop( shopItems, MAX_WAVES_SHOP_ITEMS );
	Com_Printf( "Waves: %d shop items loaded\n", numShopItems );
}

static void G_WavesUpdateConfigstring( void ) {
	const char	*s;

	s = va( "%i %i %i %i", waves.round, waves.state, waves.numAlive + waves.toSpawn + (waves.bossPending ? 1 : 0),
		waves.state == WAVES_INTERMISSION ? waves.stateTime : 0 );
	if ( strcmp( s, wavesConfigstring ) ) {
		Q_strncpyz( wavesConfigstring, s, sizeof( wavesConfigstring ) );
		trap->SetConfigstring( CS_WAVES, s );
	}
}

void G_WavesRunFrame( void ) {
	if ( level.intermissiontime ) {
		return;
	}

	G_WavesUpdateConfigstring();

	G_WavesPruneDead();

	switch ( waves.state ) {
	case WAVES_WAITING:
		if ( G_WavesNumPlayers() > 0 ) {
			G_WavesStartIntermission( wavesSettings.firstDelay );
		}
		break;

	case WAVES_INTERMISSION:
		if ( level.time >= waves.stateTime ) {
			G_WavesStartRound();
		}
		break;

	case WAVES_ACTIVE:
		if ( G_WavesNumPlayers() > 0 && !G_WavesNumAlivePlayers() ) {
			trap->SendServerCommand( -1, va( "cp \"Defeat\nYou reached round %d\"", waves.round ) );
			waves.state = WAVES_DEFEAT;
			waves.stateTime = level.time + WAVES_DEFEAT_TIME;
			break;
		}
		if ( waves.bossPending && level.time >= waves.nextSpawnTime ) {
			G_WavesSpawnEnemy( qtrue );
			waves.nextSpawnTime = level.time + G_WavesSpawnInterval();
		}
		else if ( waves.toSpawn > 0 && waves.numAlive < G_WavesMaxAlive() && level.time >= waves.nextSpawnTime ) {
			G_WavesSpawnEnemy( qfalse );
			waves.nextSpawnTime = level.time + G_WavesSpawnInterval();
		}
		if ( level.time >= waves.nextEnemyTime ) {
			G_WavesAssignEnemies();
			waves.nextEnemyTime = level.time + WAVES_ENEMY_TIME;
		}
		if ( waves.toSpawn <= 0 && !waves.numAlive && !waves.bossPending ) {
			G_WavesRoundBonus();
			trap->SendServerCommand( -1, va( "cp \"Round %d complete\"", waves.round ) );
			G_WavesStartIntermission( wavesSettings.intermission );
		}
		break;

	case WAVES_DEFEAT:
		if ( level.time >= waves.stateTime ) {
			G_WavesEndGame();
		}
		break;

	case WAVES_ENDED:
		break;
	}
}

qboolean G_WavesHoldSpawn( gentity_t *ent ) {
	return ( level.gametype == GT_WAVES
		&& (waves.state == WAVES_ACTIVE || waves.state == WAVES_DEFEAT || waves.state == WAVES_ENDED)
		&& ent->client && ent->client->sess.sessionTeam == TEAM_BLUE );
}

void G_WavesMakeSpectator( gentity_t *ent ) {
	gclient_t *client = ent->client;

	client->tempSpectate = WAVES_SPECTATE_FOREVER;
	ent->health = client->ps.stats[STAT_HEALTH] = 1;
	ent->waterlevel = ent->watertype = 0;
	client->ps.weapon = WP_NONE;
	client->ps.stats[STAT_WEAPONS] = 0;
	client->ps.stats[STAT_HOLDABLE_ITEMS] = 0;
	client->ps.stats[STAT_HOLDABLE_ITEM] = 0;
	ent->takedamage = qfalse;
	trap->LinkEntity( (sharedEntity_t *)ent );
}

qboolean G_WavesSpawnNearTeammate( gentity_t *ent, vec3_t origin, vec3_t angles ) {
	gentity_t	*mates[MAX_CLIENTS], *mate;
	int			numMates = 0, numNodes = trap->Nav_GetNumNodes();
	int			i, tries;
	vec3_t		spot;
	float		dist;
	trace_t		tr;

	for ( i = 0; i < level.maxclients; i++ ) {
		if ( &g_entities[i] != ent && G_WavesPlayerAlive( &g_entities[i] ) ) {
			mates[numMates++] = &g_entities[i];
		}
	}
	if ( !numMates || numNodes <= 0 ) {
		return qfalse;
	}

	mate = mates[Q_irand( 0, numMates - 1 )];
	for ( tries = 0; tries < numNodes; tries++ ) {
		trap->Nav_GetNodePosition( Q_irand( 0, numNodes - 1 ), spot );
		dist = Distance( spot, mate->r.currentOrigin );
		if ( dist < WAVES_REVIVE_MIN_DIST || dist > WAVES_REVIVE_MAX_DIST ) {
			continue;
		}
		trap->Trace( &tr, mate->r.currentOrigin, NULL, NULL, spot, mate->s.number, MASK_SOLID, qfalse, 0, 0 );
		if ( tr.fraction < 1.0f || !G_WavesSpotFree( spot ) ) {
			continue;
		}
		VectorCopy( spot, origin );
		VectorSet( angles, 0, mate->client->ps.viewangles[YAW], 0 );
		return qtrue;
	}
	return qfalse;
}

void G_WavesKillAll( void ) {
	int	i;

	for ( i = 0; i < waves.numAlive; i++ ) {
		gentity_t *npc = &g_entities[waves.alive[i].num];

		if ( npc->inuse && npc->health > 0 ) {
			G_Damage( npc, NULL, NULL, NULL, NULL, npc->health + 100, DAMAGE_NO_PROTECTION, MOD_UNKNOWN );
		}
	}
	waves.toSpawn = 0;
	waves.bossPending = qfalse;
}

static int G_WavesDamageType( int mod ) {
	switch ( mod ) {
	case MOD_SABER:
		return WAVES_ITEM_SABERDAMAGE;
	case MOD_FORCE_DARK:
		return WAVES_ITEM_FORCEDAMAGE;
	case MOD_STUN_BATON:
	case MOD_MELEE:
	case MOD_CONC:
	case MOD_CONC_ALT:
		return WAVES_ITEM_WEAPONDAMAGE;
	default:
		return ( mod >= MOD_BRYAR_PISTOL && mod <= MOD_DET_PACK_SPLASH ) ? WAVES_ITEM_WEAPONDAMAGE : -1;
	}
}

int G_WavesScaleDamage( gentity_t *targ, gentity_t *attacker, int damage, int mod ) {
	int	i;

	if ( targ->client && targ->s.number < MAX_CLIENTS ) {
		targ->client->pers.waves.lastHurtTime = level.time;
	}
	if ( !attacker ) {
		return damage;
	}
	if ( attacker->client && attacker->s.number < MAX_CLIENTS ) {
		if ( targ->s.eType != ET_NPC ) {
			return damage;
		}
		return (int)( damage * ( 100 + G_WavesBonus( attacker, G_WavesDamageType( mod ) ) ) / 100.0f + 0.5f );
	}
	if ( attacker->s.eType != ET_NPC ) {
		return damage;
	}
	for ( i = 0; i < waves.numAlive; i++ ) {
		if ( waves.alive[i].num == attacker->s.number ) {
			return (int)(damage * G_WavesRoundScale( wavesSettings.damagePerRound, wavesSettings.damageMax ));
		}
	}
	return damage;
}

void G_WavesTimerActions( gentity_t *ent ) {
	gclient_t	*client = ent->client;
	int			maxHealth = client->ps.stats[STAT_MAX_HEALTH];
	int			regen;

	if ( ent->health > maxHealth ) {
		ent->health--;
	}
	else if ( G_WavesPlayerAlive( ent ) && level.time - client->pers.waves.lastHurtTime >= WAVES_REGEN_DELAY ) {
		regen = G_WavesBonus( ent, WAVES_ITEM_HEALTHREGEN );
		if ( regen > 0 && ent->health < maxHealth ) {
			ent->health = Q_min( ent->health + regen, maxHealth );
		}
	}

	if ( client->ps.stats[STAT_ARMOR] > G_WavesMaxShield( ent ) ) {
		client->ps.stats[STAT_ARMOR]--;
	}
}

void G_WavesSetRound( int round ) {
	G_WavesKillAll();
	waves.round = round - 1;
	G_WavesStartIntermission( 0 );
}

void G_WavesPrintStatus( void ) {
	static const char *stateNames[] = { "waiting", "intermission", "active", "defeat", "ended" };

	Com_Printf( "Waves: round %d, %s, %d alive, %d to spawn, %d nav nodes\n",
		waves.round, stateNames[waves.state], waves.numAlive, waves.toSpawn, trap->Nav_GetNumNodes() );
}
