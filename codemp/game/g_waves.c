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

extern gentity_t *NPC_SpawnTypeAt( vec3_t origin, float yaw, char *npc_type, char *targetname, qboolean isVehicle, team_t teamowner );
extern void NPC_Begin( gentity_t *ent );
extern void G_SetEnemy( gentity_t *self, gentity_t *enemy );
extern void ClientRespawn( gentity_t *ent );

typedef struct waveNpc_s {
	char	type[MAX_QPATH];
	int		weight;
} waveNpc_t;

typedef struct waveGroup_s {
	int			fromRound;
	int			toRound;
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
	int		maxAlive;
	int		countBase;
	int		countPerRound;
	int		spawnInterval;
	int		spawnMinDist;
} wavesSettings_t;

static wavesSettings_t	wavesSettings;
static waveGroup_t		waveGroups[MAX_WAVE_GROUPS];
static int				numWaveGroups;

static struct {
	wavesState_t	state;
	int				round;
	int				stateTime;
	int				toSpawn;
	int				nextSpawnTime;
	int				nextEnemyTime;
	int				alive[MAX_WAVE_ALIVE];
	int				numAlive;
} waves;

static void G_WavesDefaultSettings( void ) {
	wavesSettings.firstDelay = 5000;
	wavesSettings.intermission = 10000;
	wavesSettings.maxAlive = 12;
	wavesSettings.countBase = 6;
	wavesSettings.countPerRound = 2;
	wavesSettings.spawnInterval = 500;
	wavesSettings.spawnMinDist = 512;
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
		else if ( !Q_stricmp( token, "maxAlive" ) )			wavesSettings.maxAlive = Com_Clampi( 1, MAX_WAVE_ALIVE, value );
		else if ( !Q_stricmp( token, "countBase" ) )		wavesSettings.countBase = value;
		else if ( !Q_stricmp( token, "countPerRound" ) )	wavesSettings.countPerRound = value;
		else if ( !Q_stricmp( token, "spawnInterval" ) )	wavesSettings.spawnInterval = value;
		else if ( !Q_stricmp( token, "spawnMinDist" ) )		wavesSettings.spawnMinDist = value;
		else Com_Printf( S_COLOR_YELLOW "%s: unknown setting '%s'\n", WAVES_FILE, token );
	}
}

static void G_WavesParseGroup( const char **p ) {
	waveGroup_t	*group;
	waveNpc_t	*npc;
	char		*token;

	if ( numWaveGroups >= MAX_WAVE_GROUPS ) {
		Com_Printf( S_COLOR_YELLOW "%s: too many groups (max %d)\n", WAVES_FILE, MAX_WAVE_GROUPS );
		SkipBracedSection( p, 1 );
		return;
	}

	group = &waveGroups[numWaveGroups];
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

	if ( group->numNpcs ) {
		numWaveGroups++;
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
				G_WavesParseGroup( &p );
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

	Com_Printf( "Waves: %d groups loaded\n", numWaveGroups );
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

static const char *G_WavesPickNpc( void ) {
	int	total = 0, pick, i, j;

	for ( i = 0; i < numWaveGroups; i++ ) {
		waveGroup_t *group = &waveGroups[i];

		if ( waves.round < group->fromRound || (group->toRound && waves.round > group->toRound) ) {
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
	for ( i = 0; i < numWaveGroups; i++ ) {
		waveGroup_t *group = &waveGroups[i];

		if ( waves.round < group->fromRound || (group->toRound && waves.round > group->toRound) ) {
			continue;
		}
		for ( j = 0; j < group->numNpcs; j++ ) {
			pick -= group->npcs[j].weight;
			if ( pick < 0 ) {
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

static void G_WavesSpawnEnemy( void ) {
	gentity_t	*npc, *target;
	const char	*type;
	vec3_t		spot, dir, angles;
	float		yaw = 0.0f;

	type = G_WavesPickNpc();
	if ( !type || !G_WavesPickSpot( spot ) ) {
		return;
	}

	target = G_WavesClosestPlayer( spot );
	if ( target ) {
		VectorSubtract( target->r.currentOrigin, spot, dir );
		vectoangles( dir, angles );
		yaw = angles[YAW];
	}

	npc = NPC_SpawnTypeAt( spot, yaw, (char *)type, NULL, qfalse, TEAM_RED );
	if ( !npc || !npc->client ) {
		Com_Printf( S_COLOR_YELLOW "Waves: could not spawn '%s'\n", type );
		waves.toSpawn--;
		return;
	}

	npc->client->playerTeam = NPCTEAM_ENEMY;
	npc->client->enemyTeam = NPCTEAM_PLAYER;
	waves.alive[waves.numAlive++] = npc->s.number;
	waves.toSpawn--;
}

static void G_WavesPruneDead( void ) {
	int	i = 0;

	while ( i < waves.numAlive ) {
		gentity_t *npc = &g_entities[waves.alive[i]];

		if ( !npc->inuse || !npc->client
			|| (npc->think != NPC_Begin && (npc->s.eType != ET_NPC || npc->health <= 0)) ) {
			waves.alive[i] = waves.alive[--waves.numAlive];
			continue;
		}
		i++;
	}
}

static void G_WavesAssignEnemies( void ) {
	int	i;

	for ( i = 0; i < waves.numAlive; i++ ) {
		gentity_t *npc = &g_entities[waves.alive[i]];
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
	waves.nextSpawnTime = level.time;
	waves.state = WAVES_ACTIVE;
	trap->SendServerCommand( -1, va( "cp \"Round %d\"", waves.round ) );
}

void G_WavesInit( void ) {
	memset( &waves, 0, sizeof( waves ) );
	waves.state = WAVES_WAITING;
	G_WavesLoad();
}

void G_WavesRunFrame( void ) {
	if ( level.intermissiontime ) {
		return;
	}

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
		if ( waves.toSpawn > 0 && waves.numAlive < wavesSettings.maxAlive && level.time >= waves.nextSpawnTime ) {
			G_WavesSpawnEnemy();
			waves.nextSpawnTime = level.time + wavesSettings.spawnInterval;
		}
		if ( level.time >= waves.nextEnemyTime ) {
			G_WavesAssignEnemies();
			waves.nextEnemyTime = level.time + WAVES_ENEMY_TIME;
		}
		if ( waves.toSpawn <= 0 && !waves.numAlive ) {
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
		gentity_t *npc = &g_entities[waves.alive[i]];

		if ( npc->inuse && npc->health > 0 ) {
			G_Damage( npc, NULL, NULL, NULL, NULL, npc->health + 100, DAMAGE_NO_PROTECTION, MOD_UNKNOWN );
		}
	}
	waves.toSpawn = 0;
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
