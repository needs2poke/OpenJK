/*
===========================================================================
Master Mod Server Sharding Terminal System
Terminal entity interaction and PIN validation
===========================================================================
*/

#include "g_terminal.h"
#include "g_accounts.h"
#include "g_shard_client.h"

// Default PIN code for Phase 1 testing
#define DEFAULT_PIN "1337"

// Portal spawn delay (in milliseconds)
#define PORTAL_SPAWN_DELAY 3000

/*
================
Cmd_TerminalPIN_f
/terminal_pin <code> - Enter PIN to unlock terminal access
================
*/
void Cmd_TerminalPIN_f(gentity_t *ent) {
	char pin[32];
	int clientNum = ent - g_entities;

	if (!ent->client) {
		return;
	}

	if (trap->Argc() < 2) {
		trap->SendServerCommand(clientNum, "cp \"^3Usage: /terminal_pin <code>\"");
		return;
	}

	trap->Argv(1, pin, sizeof(pin));

	// Validate PIN
	if (Q_stricmp(pin, DEFAULT_PIN) == 0) {
		ent->client->sess.terminalUnlocked = qtrue;
		trap->SendServerCommand(clientNum,
			"cp \"^2PIN Accepted!\\n^7You unlocked portal access!\"");
		trap->Print("^2Player %s unlocked terminal access with correct PIN\n",
			ent->client->pers.netname);
	} else {
		trap->SendServerCommand(clientNum, "cp \"^1Invalid PIN code!\"");
		trap->Print("^3Player %s entered invalid PIN: %s\n",
			ent->client->pers.netname, pin);
	}
}

/*
================
Portal_Touch
Touch callback for portal entities - transfers player to shard instance
================
*/
static void Portal_Touch(gentity_t *self, gentity_t *other, trace_t *trace) {
	int clientNum;
	char cmd[256];
	char userinfo[MAX_INFO_STRING];
	const char *clientIP;
	const char *serverIP;
	int accountID;

	// Only players can use portals
	if (!other || !other->client) {
		return;
	}

	// Throttle touches - only allow once per second
	if (level.time - self->genericValue1 < 1000) {
		return;
	}
	self->genericValue1 = level.time;

	clientNum = other - g_entities;
	accountID = other->client->sess.accountId;

	// Get client IP from userinfo
	trap->GetUserinfo(clientNum, userinfo, sizeof(userinfo));
	clientIP = Info_ValueForKey(userinfo, "ip");
	if (!clientIP || !clientIP[0]) {
		clientIP = "unknown";
	}

	// Get server IP
	serverIP = Shard_GetServerIP();

	// LOG FOR ORCHESTRATOR (CRITICAL - exact format)
	// Format: [PORTAL] client=IP:PORT accountID=X instanceID=Y port=Z
	// Note: PORT is 0 because we can't get client port from game module
	// Orchestrator will use fallback methods to discover port
	trap->Print("^5[PORTAL] client=%s:0 accountID=%d instanceID=%d port=%d\n",
		clientIP, accountID, self->count, self->health);

	// Send feedback to player
	trap->SendServerCommand(clientNum,
		"cp \"^3Transferring to shard instance...\\n^7Please wait (5 sec)\"");

	// Also print old manual connect info to console as fallback
	Com_sprintf(cmd, sizeof(cmd),
		"print \"^2[PORTAL] Transferring... (or manual: ^5/connect %s:%d^2)\\n\"",
		serverIP, self->health);
	trap->SendServerCommand(clientNum, cmd);
}

/*
================
Terminal_SpawnPortal
Spawn a portal entity that connects to a shard instance
================
*/
static void Terminal_SpawnPortal(gentity_t *terminal, const shardInstance_t *instance) {
	gentity_t *portal;
	vec3_t spawnPos;

	trap->Print("^5[DEBUG] Terminal_SpawnPortal START\n");

	// Spawn portal 64 units in front of terminal
	VectorCopy(terminal->r.currentOrigin, spawnPos);
	spawnPos[2] += 32; // Raise it a bit

	trap->Print("^5[DEBUG] Calling G_Spawn\n");
	portal = G_Spawn();
	if (!portal) {
		trap->Print("^1ERROR: Failed to spawn portal entity\n");
		return;
	}

	trap->Print("^5[DEBUG] Setting portal properties\n");
	portal->classname = "shard_portal";
	portal->s.eType = ET_GENERAL;  // Use ET_GENERAL so model is visible
	VectorCopy(spawnPos, portal->s.origin);
	VectorCopy(spawnPos, portal->s.pos.trBase);
	VectorCopy(spawnPos, portal->r.currentOrigin);

	trap->Print("^5[DEBUG] About to call G_NewString with token: %.16s...\n", instance->transferToken);
	// Store instance info in portal
	portal->count = instance->instanceId;
	portal->health = instance->port;
	portal->message = G_NewString(instance->transferToken);  // Allocate memory for token
	trap->Print("^5[DEBUG] G_NewString completed\n");

	// Setup portal appearance (blue glowing effect)
	portal->s.modelindex = G_ModelIndex("models/map_objects/mp/sphere.md3");
	portal->s.constantLight = 0x0000FFFF; // Blue glow

	// Make it solid and trigger
	VectorSet(portal->r.mins, -24, -24, -24);
	VectorSet(portal->r.maxs, 24, 24, 48);
	portal->r.contents = CONTENTS_TRIGGER;
	portal->clipmask = MASK_PLAYERSOLID;

	// Set touch callback
	portal->touch = Portal_Touch;

	trap->LinkEntity((sharedEntity_t *)portal);

	trap->Print("^2Portal spawned at (%f, %f, %f) for instance #%d (port %d)\n",
		spawnPos[0], spawnPos[1], spawnPos[2], instance->instanceId, instance->port);
}

/*
================
Terminal_ThinkSpawnInstance
Think function to spawn instance after delay
================
*/
static void Terminal_ThinkSpawnInstance(gentity_t *terminal) {
	shardInstance_t instance;
	int accountID;

	if (!terminal->activator || !terminal->activator->client) {
		trap->Print("^1Terminal spawn failed: No activator\n");
		return;
	}

	// Get player's account ID
	accountID = terminal->activator->client->sess.accountId;
	if (accountID <= 0) {
		trap->SendServerCommand(terminal->activator - g_entities,
			"cp \"^1Error: No account linked!\\n^3Login required for portal access\"");
		return;
	}

	// Request instance spawn from shard manager
	trap->SendServerCommand(terminal->activator - g_entities,
		"cp \"^3Spawning mission instance...\\n^7Please wait...\"");

	if (Shard_SpawnInstance(SHARD_TYPE_MISSION, accountID, "mp/ffa3", 8, &instance)) {
		// Success! Spawn portal
		char msg[256];
		Com_sprintf(msg, sizeof(msg), "cp \"^2Mission Server Ready!\\n^7Port: %d\\n^3Portal opening...\"", instance.port);
		trap->SendServerCommand(terminal->activator - g_entities, msg);

		Terminal_SpawnPortal(terminal, &instance);

		trap->Print("^2Instance spawned for player %s (account %d): port %d\n",
			terminal->activator->client->pers.netname, accountID, instance.port);
	} else {
		trap->SendServerCommand(terminal->activator - g_entities,
			"cp \"^1Instance spawn failed!\\n^3Please try again later\"");
		trap->Print("^1Failed to spawn instance for account %d\n", accountID);
	}

	terminal->activator = NULL;
	terminal->nextthink = 0;
}

/*
================
terminal_use
USE callback for terminal entities
================
*/
void terminal_use(gentity_t *self, gentity_t *other, gentity_t *activator) {
	if (!activator || !activator->client) {
		return;
	}

	int clientNum = activator - g_entities;

	trap->Print("^6Terminal %d used by player %s (unlocked: %d)\n",
		self->s.number, activator->client->pers.netname,
		activator->client->sess.terminalUnlocked);

	if (activator->client->sess.terminalUnlocked) {
		// Spawn instance after delay
		trap->SendServerCommand(clientNum,
			"cp \"^2Terminal Unlocked!\\n^7Initiating portal sequence...\"");
		trap->Print("^2Player %s requesting mission instance\n",
			activator->client->pers.netname);

		// Set up delayed spawn
		self->activator = activator;
		self->nextthink = level.time + PORTAL_SPAWN_DELAY;
		self->think = Terminal_ThinkSpawnInstance;
	} else {
		trap->SendServerCommand(clientNum,
			"cp \"^3Terminal Locked\\n^7Enter PIN code: /terminal_pin <code>\"");
		trap->Print("^3Player %s tried locked terminal\n",
			activator->client->pers.netname);
	}
}
