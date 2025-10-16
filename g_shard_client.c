/*
===========================================================================
Master Mod - Shard Manager Client Implementation
Uses engine syscalls for API communication
===========================================================================
*/

#include "g_local.h"
#include "g_shard_client.h"

// Internal state
static qboolean shardClientInitialized = qfalse;

/*
==============
Shard_Init
Initialize the shard client
==============
*/
qboolean Shard_Init(void) {
	if (shardClientInitialized) {
		return qtrue;
	}

	shardClientInitialized = qtrue;
	trap->Print("^2Shard Client initialized (using engine syscalls)\n");
	return qtrue;
}

/*
==============
Shard_Shutdown
Cleanup shard client
==============
*/
void Shard_Shutdown(void) {
	if (!shardClientInitialized) {
		return;
	}

	shardClientInitialized = qfalse;
}

/*
==============
Shard_SpawnInstance
Request the shard manager to spawn a new game instance
Uses engine syscall to make HTTP request
==============
*/
qboolean Shard_SpawnInstance(shardType_t type, int ownerAccountID, const char *mapName, int maxPlayers, shardInstance_t *outInstance) {
	char curlCmd[1024];
	char responseFile[128];
	FILE *fp;
	char line[512];
	int instanceId = 0;
	int port = 0;
	char status[64] = "starting";
	char token[128] = "";

	if (!outInstance) {
		return qfalse;
	}

	memset(outInstance, 0, sizeof(*outInstance));

	trap->Print("^5Shard Client: Requesting instance spawn (%s) for account %d\n",
		Shard_GetTypeString(type), ownerAccountID);

	// Generate temp file for response
	Com_sprintf(responseFile, sizeof(responseFile), "/tmp/shard_spawn_%d.json", (int)time(NULL));

	// Call Shard Manager API via curl (workaround for missing engine syscalls)
	Com_sprintf(curlCmd, sizeof(curlCmd),
		"curl -s -X POST http://localhost:8001/api/spawn_instance "
		"-H 'Content-Type: application/json' "
		"-d '{\"instance_type\":\"%s\",\"owner_account_id\":%d,\"map_name\":\"%s\",\"max_players\":%d}' "
		"> %s 2>&1",
		Shard_GetTypeString(type), ownerAccountID, mapName, maxPlayers, responseFile);

	// Execute curl command
	if (system(curlCmd) != 0) {
		trap->Print("^1Shard Client: Failed to execute curl command\n");
		return qfalse;
	}

	// Read response file
	fp = fopen(responseFile, "r");
	if (!fp) {
		trap->Print("^1Shard Client: Failed to open response file\n");
		return qfalse;
	}

	// Parse JSON response (simple manual parsing)
	// Expected: {"instance_id":"123","port":29201,"container_id":"abc","status":"starting","transfer_token":"xyz"}
	while (fgets(line, sizeof(line), fp)) {
		char *ptr;

		// Extract instance_id
		ptr = strstr(line, "\"instance_id\"");
		if (ptr) {
			ptr = strchr(ptr, ':');
			if (ptr) {
				sscanf(ptr + 1, " \"%d\"", &instanceId);
			}
		}

		// Extract port
		ptr = strstr(line, "\"port\"");
		if (ptr) {
			ptr = strchr(ptr, ':');
			if (ptr) {
				sscanf(ptr + 1, " %d", &port);
			}
		}

		// Extract status
		ptr = strstr(line, "\"status\"");
		if (ptr) {
			ptr = strchr(ptr, ':');
			if (ptr) {
				sscanf(ptr + 1, " \"%63[^\"]\"", status);
			}
		}

		// Extract transfer_token
		ptr = strstr(line, "\"transfer_token\"");
		if (ptr) {
			ptr = strchr(ptr, ':');
			if (ptr) {
				sscanf(ptr + 1, " \"%127[^\"]\"", token);
			}
		}
	}

	fclose(fp);

	// Clean up temp file
	remove(responseFile);

	// Check if we got valid data back
	if (instanceId <= 0 || port <= 0) {
		trap->Print("^1Shard Client: Spawn request failed (invalid response: id=%d, port=%d)\n", instanceId, port);
		return qfalse;
	}

	// Populate output structure
	outInstance->instanceId = instanceId;
	outInstance->port = port;
	Q_strncpyz(outInstance->status, status, sizeof(outInstance->status));
	Q_strncpyz(outInstance->transferToken, token, sizeof(outInstance->transferToken));
	outInstance->valid = qtrue;

	trap->Print("^2Shard Client: Instance #%d spawned on port %d (token: %.16s...)\n",
		outInstance->instanceId, outInstance->port, outInstance->transferToken);

	return qtrue;
}

/*
==============
Shard_GetInstanceStatus
Get status of a running instance
Uses engine syscall to make HTTP request
==============
*/
qboolean Shard_GetInstanceStatus(int instanceID, shardInstance_t *outInstance) {
	char statusBuffer[256];
	int port = 0;

	if (!outInstance) {
		return qfalse;
	}

	memset(outInstance, 0, sizeof(*outInstance));

	// Call engine syscall to get instance status
	trap->Shard_GetInstanceStatus(
		instanceID,
		&port,
		statusBuffer,
		sizeof(statusBuffer)
	);

	// Check if we got valid data back
	if (port <= 0) {
		return qfalse;
	}

	outInstance->instanceId = instanceID;
	outInstance->port = port;
	Q_strncpyz(outInstance->status, statusBuffer, sizeof(outInstance->status));
	outInstance->valid = qtrue;

	return qtrue;
}

/*
==============
Shard_StopInstance
Stop and remove an instance
Uses engine syscall to make HTTP request
==============
*/
qboolean Shard_StopInstance(int instanceID) {
	trap->Print("^3Shard Client: Stopping instance #%d\n", instanceID);

	// Call engine syscall to stop instance
	trap->Shard_StopInstance(instanceID);

	trap->Print("^3Shard Client: Instance #%d stop request sent\n", instanceID);
	return qtrue;
}

/*
==============
Shard_ValidateTransferToken
Validate a transfer token for player connection
Returns qtrue if valid and sets outInstanceID
==============
*/
qboolean Shard_ValidateTransferToken(const char *token, int accountID, int *outInstanceID) {
	// For now, we'll implement basic validation
	// In production, this would query the API via syscall
	if (!token || !token[0] || !outInstanceID) {
		return qfalse;
	}

	// TODO: Implement API call via syscall to validate token
	// For now, accept any token that looks valid (base64-ish, 32+ chars)
	if (strlen(token) < 32) {
		return qfalse;
	}

	return qtrue;
}

/*
==============
Shard_ConsumeTransferToken
Mark a transfer token as used
==============
*/
qboolean Shard_ConsumeTransferToken(const char *token) {
	// TODO: Implement API call via syscall to consume token
	return qtrue;
}

/*
==============
Shard_GetTypeString
Convert shard type enum to string
==============
*/
const char *Shard_GetTypeString(shardType_t type) {
	switch (type) {
		case SHARD_TYPE_MISSION: return "mission";
		case SHARD_TYPE_BASE: return "base";
		case SHARD_TYPE_RAID: return "raid";
		default: return "mission";
	}
}

/*
==============
Shard_GetServerIP
Get the server's public IP address
==============
*/
const char *Shard_GetServerIP(void) {
	static char serverIP[64] = {0};

	if (!serverIP[0]) {
		// Use sv_master1 or hardcode public IP
		// For now, hardcode the public IP - you can make this a cvar later
		Q_strncpyz(serverIP, "158.69.218.235", sizeof(serverIP));
	}

	return serverIP;
}
