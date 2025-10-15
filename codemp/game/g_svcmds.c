/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
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

// this file holds commands that can be executed by the server console, but not remote clients

#include "g_local.h"
#include "g_teach.h" // TEACH: server console hook

// Forward declaration for G_Say (defined in g_cmds.c)
void G_Say( gentity_t *ent, gentity_t *target, int mode, const char *chatText );


/*
==============================================================================

PACKET FILTERING


You can add or remove addresses from the filter list with:

addip <ip>
removeip <ip>

The ip address is specified in dot format, and any unspecified digits will match any value, so you can specify an entire class C network with "addip 192.246.40".

Removeip will only remove an address specified exactly the same way.  You cannot addip a subnet, then removeip a single host.

listip
Prints the current list of filters.

g_filterban <0 or 1>

If 1 (the default), then ip addresses matching the current list will be prohibited from entering the game.  This is the default setting.

If 0, then only addresses matching the list will be allowed.  This lets you easily set up a private game, or a game that only allows players from your local network.


==============================================================================
*/

typedef struct ipFilter_s {
	uint32_t mask, compare;
} ipFilter_t;

#define	MAX_IPFILTERS (1024)

static ipFilter_t	ipFilters[MAX_IPFILTERS];
static int			numIPFilters;

/*
=================
StringToFilter
=================
*/
static qboolean StringToFilter( char *s, ipFilter_t *f ) {
	char num[128];
	int i, j;
	byteAlias_t b, m;

	b.ui = m.ui = 0u;

	for ( i=0; i<4; i++ ) {
		if ( *s < '0' || *s > '9' ) {
			if ( *s == '*' ) {
				// 'match any'
				// b[i] and m[i] to 0
				s++;
				if ( !*s )
					break;
				s++;
				continue;
			}
			trap->Print( "Bad filter address: %s\n", s );
			return qfalse;
		}

		j = 0;
		while ( *s >= '0' && *s <= '9' )
			num[j++] = *s++;

		num[j] = 0;
		b.b[i] = (byte)atoi( num );
		m.b[i] = 0xFF;

		if ( !*s )
			break;

		s++;
	}

	f->mask = m.ui;
	f->compare = b.ui;

	return qtrue;
}

static void TPrintF(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    Q_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    trap->Print("%s", buf);
    G_LogPrintf("%s", buf);
}



/*
=================
UpdateIPBans
=================
*/
static void UpdateIPBans( void ) {
	byteAlias_t b, m;
	int i, j;
	char ip[NET_ADDRSTRMAXLEN], iplist_final[MAX_CVAR_VALUE_STRING];

	*iplist_final = 0;
	for ( i=0; i<numIPFilters; i++ ) {
		if ( ipFilters[i].compare == 0xFFFFFFFFu )
			continue;

		b.ui = ipFilters[i].compare;
		m.ui = ipFilters[i].mask;
		*ip = 0;
		for ( j=0; j<4; j++ ) {
			if ( m.b[j] != 0xFF )
				Q_strcat( ip, sizeof( ip ), "*" );
			else
				Q_strcat( ip, sizeof( ip ), va( "%i", b.b[j] ) );
			Q_strcat( ip, sizeof( ip ), (j<3) ? "." : " " );
		}
		if ( strlen( iplist_final )+strlen( ip ) < MAX_CVAR_VALUE_STRING )
			Q_strcat( iplist_final, sizeof( iplist_final ), ip );
		else {
			Com_Printf( "g_banIPs overflowed at MAX_CVAR_VALUE_STRING\n" );
			break;
		}
	}

	trap->Cvar_Set( "g_banIPs", iplist_final );
}

/*
=================
G_FilterPacket
=================
*/
qboolean G_FilterPacket( char *from ) {
	int i;
	uint32_t in;
	byteAlias_t m;
	char *p;

	i = 0;
	p = from;
	while ( *p && i < 4 ) {
		m.b[i] = 0;
		while ( *p >= '0' && *p <= '9' ) {
			m.b[i] = m.b[i]*10 + (*p - '0');
			p++;
		}
		if ( !*p || *p == ':' )
			break;
		i++, p++;
	}

	in = m.ui;

	for ( i=0; i<numIPFilters; i++ ) {
		if ( (in & ipFilters[i].mask) == ipFilters[i].compare )
			return g_filterBan.integer != 0;
	}

	return g_filterBan.integer == 0;
}

/*
=================
AddIP
=================
*/
static void AddIP( char *str ) {
	int i;

	for ( i=0; i<numIPFilters; i++ ) {
		if ( ipFilters[i].compare == 0xFFFFFFFFu )
			break; // free spot
	}
	if ( i == numIPFilters ) {
		if ( numIPFilters == MAX_IPFILTERS ) {
			trap->Print( "IP filter list is full\n" );
			return;
		}
		numIPFilters++;
	}

	if ( !StringToFilter( str, &ipFilters[i] ) )
		ipFilters[i].compare = 0xFFFFFFFFu;

	UpdateIPBans();
}

/*
=================
G_ProcessIPBans
=================
*/
void G_ProcessIPBans( void ) {
	char *s = NULL, *t = NULL, str[MAX_CVAR_VALUE_STRING] = {0};

	Q_strncpyz( str, g_banIPs.string, sizeof( str ) );

	for ( t=s=g_banIPs.string; *t; t=s ) {
		s = strchr( s, ' ' );
		if ( !s )
			break;

		while ( *s == ' ' )
			*s++ = 0;

		if ( *t )
			AddIP( t );
	}
}

/*
=================
Svcmd_AddIP_f
=================
*/
void Svcmd_AddIP_f (void)
{
	char		str[MAX_TOKEN_CHARS];

	if ( trap->Argc() < 2 ) {
		trap->Print("Usage: addip <ip-mask>\n");
		return;
	}

	trap->Argv( 1, str, sizeof( str ) );

	AddIP( str );
}

/*
=================
Svcmd_RemoveIP_f
=================
*/
void Svcmd_RemoveIP_f (void)
{
	ipFilter_t	f;
	int			i;
	char		str[MAX_TOKEN_CHARS];

	if ( trap->Argc() < 2 ) {
		trap->Print("Usage: removeip <ip-mask>\n");
		return;
	}

	trap->Argv( 1, str, sizeof( str ) );

	if (!StringToFilter (str, &f))
		return;

	for (i=0 ; i<numIPFilters ; i++) {
		if (ipFilters[i].mask == f.mask	&&
			ipFilters[i].compare == f.compare) {
			ipFilters[i].compare = 0xffffffffu;
			trap->Print ("Removed.\n");

			UpdateIPBans();
			return;
		}
	}

	trap->Print ( "Didn't find %s.\n", str );
}

void Svcmd_ListIP_f (void)
{
	int		i, count = 0;
	byteAlias_t b;

	for(i = 0; i < numIPFilters; i++) {
		if ( ipFilters[i].compare == 0xffffffffu )
			continue;

		b.ui = ipFilters[i].compare;
		trap->Print ("%i.%i.%i.%i\n", b.b[0], b.b[1], b.b[2], b.b[3]);
		count++;
	}
	trap->Print ("%i bans.\n", count);
}

/*
===================
Svcmd_EntityList_f
===================
*/
void	Svcmd_EntityList_f (void) {
	int			e;
	gentity_t		*check;

	check = g_entities;
	for (e = 0; e < level.num_entities ; e++, check++) {
		if ( !check->inuse ) {
			continue;
		}
		trap->Print("%3i:", e);
		switch ( check->s.eType ) {
		case ET_GENERAL:
			trap->Print("ET_GENERAL          ");
			break;
		case ET_PLAYER:
			trap->Print("ET_PLAYER           ");
			break;
		case ET_ITEM:
			trap->Print("ET_ITEM             ");
			break;
		case ET_MISSILE:
			trap->Print("ET_MISSILE          ");
			break;
		case ET_SPECIAL:
			trap->Print("ET_SPECIAL          ");
			break;
		case ET_HOLOCRON:
			trap->Print("ET_HOLOCRON         ");
			break;
		case ET_MOVER:
			trap->Print("ET_MOVER            ");
			break;
		case ET_BEAM:
			trap->Print("ET_BEAM             ");
			break;
		case ET_PORTAL:
			trap->Print("ET_PORTAL           ");
			break;
		case ET_SPEAKER:
			trap->Print("ET_SPEAKER          ");
			break;
		case ET_PUSH_TRIGGER:
			trap->Print("ET_PUSH_TRIGGER     ");
			break;
		case ET_TELEPORT_TRIGGER:
			trap->Print("ET_TELEPORT_TRIGGER ");
			break;
		case ET_INVISIBLE:
			trap->Print("ET_INVISIBLE        ");
			break;
		case ET_NPC:
			trap->Print("ET_NPC              ");
			break;
		case ET_BODY:
			trap->Print("ET_BODY             ");
			break;
		case ET_TERRAIN:
			trap->Print("ET_TERRAIN          ");
			break;
		case ET_FX:
			trap->Print("ET_FX               ");
			break;
		default:
			trap->Print("%-3i                ", check->s.eType);
			break;
		}

		if ( check->classname ) {
			trap->Print("%s", check->classname);
		}
		trap->Print("\n");
	}
}

static void Svcmd_EntityDiag_f( void ) {
	const entityDiagnostics_t *diag = &level.entityDiagnostics;

	trap->Print("^5Entity diagnostics (current / peak)^7\n");
	trap->Print("  total         : %4i / %4i\n", diag->current.total, diag->peak.total);
	trap->Print("  players       : %4i / %4i\n", diag->current.players, diag->peak.players);
	trap->Print("  npcs          : %4i / %4i\n", diag->current.npcs, diag->peak.npcs);
	trap->Print("  missiles      : %4i / %4i\n", diag->current.missiles, diag->peak.missiles);
	trap->Print("  movers        : %4i / %4i\n", diag->current.movers, diag->peak.movers);
	trap->Print("  items         : %4i / %4i\n", diag->current.items, diag->peak.items);
	trap->Print("  bodies        : %4i / %4i\n", diag->current.bodies, diag->peak.bodies);
	trap->Print("  fx            : %4i / %4i\n", diag->current.fx, diag->peak.fx);
	trap->Print("  temp entities : %4i / %4i\n", diag->current.tempEntities, diag->peak.tempEntities);
	trap->Print("  other         : %4i / %4i\n", diag->current.other, diag->peak.other);
	trap->Print("^5Snapshot entities (current / peak): ^3%i / %i^7\n",
		diag->snapshotCurrent, diag->snapshotPeak);
}

qboolean StringIsInteger( const char *s );
/*
===================
ClientForString
===================
*/
gclient_t	*ClientForString( const char *s ) {
	gclient_t	*cl;
	int			idnum;
	char		cleanInput[MAX_STRING_CHARS];

	// numeric values could be slot numbers
	if ( StringIsInteger( s ) ) {
		idnum = atoi( s );
		if ( idnum >= 0 && idnum < level.maxclients ) {
			cl = &level.clients[idnum];
			if ( cl->pers.connected == CON_CONNECTED ) {
				return cl;
			}
		}
	}

	Q_strncpyz( cleanInput, s, sizeof(cleanInput) );
	Q_StripColor( cleanInput );

	// check for a name match
	for ( idnum=0,cl=level.clients ; idnum < level.maxclients ; idnum++,cl++ ) {
		if ( cl->pers.connected != CON_CONNECTED ) {
			continue;
		}
		if ( !Q_stricmp( cl->pers.netname_nocolor, cleanInput ) ) {
			return cl;
		}
	}

	trap->Print( "User %s is not on the server\n", s );
	return NULL;
}

/*
===================
Svcmd_ForceTeam_f

forceteam <player> <team>
===================
*/
void	Svcmd_ForceTeam_f( void ) {
	gclient_t	*cl;
	char		str[MAX_TOKEN_CHARS];

	if ( trap->Argc() < 3 ) {
		trap->Print("Usage: forceteam <player> <team>\n");
		return;
	}

	// find the player
	trap->Argv( 1, str, sizeof( str ) );
	cl = ClientForString( str );
	if ( !cl ) {
		return;
	}

	// set the team
	trap->Argv( 2, str, sizeof( str ) );
	SetTeam( &g_entities[cl - level.clients], str );
}

char *ConcatArgs( int start );
void Svcmd_Say_f( void ) {
	char *p = NULL;
	// don't let text be too long for malicious reasons
	char text[MAX_SAY_TEXT] = {0};

	if ( trap->Argc () < 2 )
		return;

	p = ConcatArgs( 1 );

	if ( strlen( p ) >= MAX_SAY_TEXT ) {
		p[MAX_SAY_TEXT-1] = '\0';
		G_SecurityLogPrintf( "Cmd_Say_f from -1 (server) has been truncated: %s\n", p );
	}

	Q_strncpyz( text, p, sizeof(text) );
	Q_strstrip( text, "\n\r", "  " );

	//G_LogPrintf( "say: server: %s\n", text );
	trap->SendServerCommand( -1, va("print \"server: %s\n\"", text ) );
}

typedef struct svcmd_s {
	const char	*name;
	void		(*func)(void);
	qboolean	dedicated;
} svcmd_t;

int svcmdcmp( const void *a, const void *b ) {
	return Q_stricmp( (const char *)a, ((svcmd_t*)b)->name );
}

svcmd_t svcmds[] = {
	{ "addbot",						Svcmd_AddBot_f,						qfalse },
	{ "addip",						Svcmd_AddIP_f,						qfalse },
	{ "botlist",					Svcmd_BotList_f,					qfalse },
	{ "entitylist",					Svcmd_EntityList_f,					qfalse },
	{ "entitydiag",					Svcmd_EntityDiag_f,					qfalse },
	{ "forceteam",					Svcmd_ForceTeam_f,					qfalse },
	{ "game_memory",				Svcmd_GameMem_f,					qfalse },
	{ "listip",						Svcmd_ListIP_f,						qfalse },
	{ "removeip",					Svcmd_RemoveIP_f,					qfalse },
	{ "say",						Svcmd_Say_f,						qtrue },
	{ "toggleallowvote",			Svcmd_ToggleAllowVote_f,			qfalse },
	{ "toggleuserinfovalidation",	Svcmd_ToggleUserinfoValidation_f,	qfalse },
	{ "teach",                     Svcmd_Teach_f,                 		qfalse },

};
static const size_t numsvcmds = ARRAY_LEN( svcmds );

/*
=================
ConsoleCommand

=================
*/
/*
=================
Svcmd_Tickrate_f

Show or change server tick rate (sv_fps) on-the-fly
=================
*/
static void Svcmd_Tickrate_f(void) {
    char arg[MAX_TOKEN_CHARS];

    trap->Argv(1, arg, sizeof(arg));

    if (!arg[0]) {
        /* Display current tick rate info */
        extern vmCvar_t sv_fps;
        trap->Cvar_Update(&sv_fps);

        float frameTime = 1000.0f / sv_fps.integer;
        trap->Print("=== SERVER TICK RATE ===\n");
        trap->Print("Current: %d FPS (%.1fms per frame)\n", sv_fps.integer, frameTime);
        trap->Print("\nTo change: tickrate <20|30|40|50|60>\n");
        trap->Print("\nClients need to type in console:\n");
        trap->Print("  /rate %d; cl_maxpackets %d; snaps %d\n",
                   sv_fps.integer * 625, sv_fps.integer, sv_fps.integer);
        trap->Print("========================\n");
    } else {
        /* Change tick rate */
        int newFps = atoi(arg);

        if (newFps < 20 || newFps > 60) {
            trap->Print("ERROR: tick rate must be between 20 and 60\n");
            return;
        }

        trap->Cvar_Set("sv_fps", va("%d", newFps));
        trap->Print("Tick rate changed to %d FPS (%.1fms per frame)\n",
                   newFps, 1000.0f / newFps);
        trap->Print("Change takes effect immediately!\n");
        trap->Print("\nTell clients to type:\n");
        trap->Print("  /rate %d; cl_maxpackets %d; snaps %d\n",
                   newFps * 625, newFps, newFps);

        /* Broadcast to all clients */
        trap->SendServerCommand(-1, va(
            "print \"^3[Server] ^7Tick rate changed to ^2%d FPS^7\n"
            "^3[Server] ^7Type in console: ^2/rate %d; cl_maxpackets %d; snaps %d^7\n\"",
            newFps, newFps * 625, newFps, newFps
        ));
    }
}

/*
=================
Svcmd_ServerPerf_f

Display server performance statistics
=================
*/
static void Svcmd_ServerPerf_f(void) {
    extern vmCvar_t sv_fps;
    trap->Cvar_Update(&sv_fps);

    float targetFrameTime = 1000.0f / sv_fps.integer;
    int activeEnts = 0;
    int activeMissiles = 0;
    int activePlayers = 0;

    /* Count active entities */
    for (int i = 0; i < level.num_entities; i++) {
        if (!g_entities[i].inuse) continue;
        activeEnts++;
        if (g_entities[i].s.eType == ET_MISSILE) activeMissiles++;
    }

    /* Count connected clients */
    for (int i = 0; i < level.maxclients; i++) {
        if (level.clients[i].pers.connected == CON_CONNECTED) {
            activePlayers++;
        }
    }

    trap->Print("=== SERVER PERFORMANCE ===\n");
    trap->Print("Tick Rate:      %d FPS (%.1fms target frame time)\n",
               sv_fps.integer, targetFrameTime);
    trap->Print("Server Time:    %d ms\n", level.time);
    trap->Print("Entities:       %d active / %d total / %d max\n",
               activeEnts, level.num_entities, MAX_GENTITIES);
    trap->Print("  - Missiles:   %d\n", activeMissiles);
    trap->Print("Clients:        %d / %d\n", activePlayers, level.maxclients);
    trap->Print("==========================\n");
    trap->Print("Note: Frame timing stats require additional instrumentation\n");
}

/*
=================
Svcmd_TrainingDuel_f

Configure training duel mode (reduced/no damage for practice)
=================
*/
static void Svcmd_TrainingDuel_f(void) {
    char arg[MAX_TOKEN_CHARS];
    extern vmCvar_t g_duelTrainingMode;
    extern vmCvar_t g_duelTrainingDamage;

    trap->Argv(1, arg, sizeof(arg));

    if (!arg[0]) {
        /* Display current training mode settings */
        trap->Cvar_Update(&g_duelTrainingMode);
        trap->Cvar_Update(&g_duelTrainingDamage);

        trap->Print("=== TRAINING DUEL MODE ===\n");
        trap->Print("Status:    %s\n", g_duelTrainingMode.integer ? "^2ENABLED^7" : "^1DISABLED^7");

        if (g_duelTrainingMode.integer) {
            if (g_duelTrainingDamage.integer == 0) {
                trap->Print("Damage:    ^3NO DAMAGE^7 (hits register but deal 0 damage)\n");
            } else if (g_duelTrainingDamage.integer > 0) {
                trap->Print("Damage:    ^3FIXED %d HP^7 per hit\n", g_duelTrainingDamage.integer);
            } else {
                int percent = -g_duelTrainingDamage.integer;
                trap->Print("Damage:    ^3%d%% ^7of normal damage\n", percent);
            }
        }

        trap->Print("\nUsage:\n");
        trap->Print("  trainingduel off         - Disable training mode (normal damage)\n");
        trap->Print("  trainingduel nodamage    - Enable with 0 damage (pure practice)\n");
        trap->Print("  trainingduel 1           - Enable with 1 HP per hit (training sabers)\n");
        trap->Print("  trainingduel 5           - Enable with 5 HP per hit\n");
        trap->Print("  trainingduel 50%%         - Enable with 50%% damage\n");
        trap->Print("==========================\n");
    } else {
        /* Change training mode */
        if (!Q_stricmp(arg, "off") || !Q_stricmp(arg, "0")) {
            trap->Cvar_Set("g_duelTrainingMode", "0");
            trap->Print("Training duel mode ^1DISABLED^7\n");
            trap->SendServerCommand(-1, "print \"^3[Server] ^7Training duel mode disabled - normal damage\n\"");
        }
        else if (!Q_stricmp(arg, "nodamage") || !Q_stricmp(arg, "none")) {
            trap->Cvar_Set("g_duelTrainingMode", "1");
            trap->Cvar_Set("g_duelTrainingDamage", "0");
            trap->Print("Training duel mode ^2ENABLED^7 - ^3NO DAMAGE^7\n");
            trap->SendServerCommand(-1, "print \"^3[Server] ^7Training duel mode: ^2NO DAMAGE^7 (pure practice)\n\"");
        }
        else if (!Q_stricmp(arg, "training") || !Q_stricmp(arg, "saber")) {
            trap->Cvar_Set("g_duelTrainingMode", "1");
            trap->Cvar_Set("g_duelTrainingDamage", "1");
            trap->Print("Training duel mode ^2ENABLED^7 - ^31 HP^7 per hit\n");
            trap->SendServerCommand(-1, "print \"^3[Server] ^7Training duel mode: ^21 HP^7 per hit (training sabers)\n\"");
        }
        else {
            /* Parse number or percentage */
            int value = atoi(arg);

            /* Check if it's a percentage */
            if (arg[strlen(arg)-1] == '%') {
                /* Percentage mode: store as negative */
                trap->Cvar_Set("g_duelTrainingMode", "1");
                trap->Cvar_Set("g_duelTrainingDamage", va("-%d", value));
                trap->Print("Training duel mode ^2ENABLED^7 - ^3%d%% damage^7\n", value);
                trap->SendServerCommand(-1, va("print \"^3[Server] ^7Training duel mode: ^2%d%% damage^7\n\"", value));
            }
            else if (value >= 0) {
                /* Fixed damage mode */
                trap->Cvar_Set("g_duelTrainingMode", "1");
                trap->Cvar_Set("g_duelTrainingDamage", va("%d", value));
                if (value == 0) {
                    trap->Print("Training duel mode ^2ENABLED^7 - ^3NO DAMAGE^7\n");
                    trap->SendServerCommand(-1, "print \"^3[Server] ^7Training duel mode: ^2NO DAMAGE^7\n\"");
                } else {
                    trap->Print("Training duel mode ^2ENABLED^7 - ^3%d HP^7 per hit\n", value);
                    trap->SendServerCommand(-1, va("print \"^3[Server] ^7Training duel mode: ^2%d HP^7 per hit\n\"", value));
                }
            }
            else {
                trap->Print("^1ERROR:^7 Invalid value. Use:\n");
                trap->Print("  off, nodamage, training, <number>, or <percent>%%\n");
            }
        }
    }
}

qboolean ConsoleCommand(void) {
    char     cmd[MAX_TOKEN_CHARS] = {0};
    svcmd_t* command = NULL;

    trap->Argv(0, cmd, sizeof(cmd));
    G_LogPrintf("svc:ConsoleCommand cmd='%s'\n", cmd);

    /* --- explicit fast-path for our commands --- */
    if (!Q_stricmp(cmd, "teach")) {
        G_LogPrintf("svc:dispatch -> teach\n");
        Svcmd_Teach_f();
        return qtrue;
    }

    if (!Q_stricmp(cmd, "tickrate")) {
        Svcmd_Tickrate_f();
        return qtrue;
    }

    if (!Q_stricmp(cmd, "serverperf") || !Q_stricmp(cmd, "perf")) {
        Svcmd_ServerPerf_f();
        return qtrue;
    }

    if (!Q_stricmp(cmd, "trainingduel") || !Q_stricmp(cmd, "training")) {
        Svcmd_TrainingDuel_f();
        return qtrue;
    }

    if (!Q_stricmp(cmd, "luke_say")) {
        Svcmd_LukeSay_f();
        return qtrue;
    }

    /* existing table lookup continues to work for the rest */
    command = (svcmd_t*)Q_LinearSearch(cmd, svcmds, numsvcmds,
                                       sizeof(svcmds[0]), svcmdcmp);
    if (!command)
        return qfalse;

    if (command->dedicated && !dedicated.integer)
        return qfalse;

    command->func();
    return qtrue;
}

/*
==================
Svcmd_LukeSay_f

Server command to make Luke say something
Usage: luke_say <message>
==================
*/
void Svcmd_LukeSay_f(void) {
    int i;
    gentity_t *luke = NULL;
    char message[MAX_STRING_CHARS];

    // Find Luke by name
    for (i = 0; i < level.maxclients; i++) {
        if (level.clients[i].pers.connected != CON_CONNECTED)
            continue;
        if (Q_stricmp(level.clients[i].pers.netname, "Luke Skywalker") == 0) {
            luke = &g_entities[i];
            break;
        }
    }

    if (!luke) {
        trap->Print("luke_say: Luke Skywalker not found\n");
        return;
    }

    // Get the message (all arguments after "luke_say")
    int argc = trap->Argc();
    if (argc < 2) {
        trap->Print("luke_say: Usage: luke_say <message>\n");
        return;
    }

    // Concatenate all arguments into one message
    message[0] = '\0';
    for (i = 1; i < argc; i++) {
        char arg[MAX_STRING_CHARS];
        trap->Argv(i, arg, sizeof(arg));
        if (i > 1) {
            Q_strcat(message, sizeof(message), " ");
        }
        Q_strcat(message, sizeof(message), arg);
    }

    // Use G_Say to properly broadcast the message (same as normal player chat)
    // This ensures the message appears in-game chat, not just console
    G_Say(luke, NULL, SAY_ALL, message);
}
