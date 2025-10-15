/*
===========================================================================
OpenJK Account System Implementation
Cross-server persistent accounts via REST API
===========================================================================
*/

#include "g_accounts.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

// Store account data in client session
void Account_StoreInClient(gentity_t *ent, accountData_t *data) {
	if (!ent || !ent->client || !data) return;

	ent->client->sess.accountId = data->accountId;
	Q_strncpyz(ent->client->sess.accountUsername, data->username, sizeof(ent->client->sess.accountUsername));
	Q_strncpyz(ent->client->sess.accountToken, data->token, sizeof(ent->client->sess.accountToken));
	ent->client->sess.accountLevel = data->level;
	ent->client->sess.accountExperience = data->experience;
	ent->client->sess.accountCredits = data->credits;
	ent->client->sess.accountAlignment = data->alignment;
	Q_strncpyz(ent->client->sess.accountRankTitle, data->rankTitle, sizeof(ent->client->sess.accountRankTitle));
	ent->client->sess.accountLoggedIn = qtrue;
}

// Clear account data from client
void Account_Clear(gentity_t *ent) {
	if (!ent || !ent->client) return;

	ent->client->sess.accountId = 0;
	ent->client->sess.accountUsername[0] = '\0';
	ent->client->sess.accountToken[0] = '\0';
	ent->client->sess.accountLevel = 0;
	ent->client->sess.accountExperience = 0;
	ent->client->sess.accountCredits = 0;
	ent->client->sess.accountAlignment = 0.0f;
	ent->client->sess.accountRankTitle[0] = '\0';
	ent->client->sess.accountLoggedIn = qfalse;
}

// Check if player is logged in
qboolean Account_IsLoggedIn(gentity_t *ent) {
	if (!ent || !ent->client) return qfalse;
	return ent->client->sess.accountLoggedIn;
}

/*
==============
HTTP_Post
Simple HTTP POST implementation
==============
*/
int HTTP_Post(const char *host, int port, const char *path, const char *jsonBody, char *response, int responseSize) {
	SOCKET sock;
	struct sockaddr_in server;
	struct hostent *he;
	char request[4096];
	char buffer[8192];
	int received, total = 0;
	int bodyLen = strlen(jsonBody);

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		return -1;
	}
#endif

	// Create socket
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET) {
#ifdef _WIN32
		WSACleanup();
#endif
		return -1;
	}

	// Resolve hostname
	he = gethostbyname(host);
	if (he == NULL) {
		closesocket(sock);
#ifdef _WIN32
		WSACleanup();
#endif
		return -1;
	}

	// Setup server address
	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);

	// Connect
	if (connect(sock, (struct sockaddr *)&server, sizeof(server)) == SOCKET_ERROR) {
		closesocket(sock);
#ifdef _WIN32
		WSACleanup();
#endif
		return -1;
	}

	// Build HTTP request
	Com_sprintf(request, sizeof(request),
		"POST %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n"
		"\r\n"
		"%s",
		path, host, port, bodyLen, jsonBody
	);

	// Send request
	if (send(sock, request, strlen(request), 0) == SOCKET_ERROR) {
		closesocket(sock);
#ifdef _WIN32
		WSACleanup();
#endif
		return -1;
	}

	// Receive response
	memset(response, 0, responseSize);
	while ((received = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
		buffer[received] = '\0';
		if (total + received < responseSize) {
			strcat(response, buffer);
			total += received;
		}
	}

	closesocket(sock);
#ifdef _WIN32
	WSACleanup();
#endif

	// Extract JSON body from HTTP response
	char *bodyStart = strstr(response, "\r\n\r\n");
	if (bodyStart) {
		bodyStart += 4;
		memmove(response, bodyStart, strlen(bodyStart) + 1);
	}

	return total;
}

/*
==============
JSON_GetString
Simple JSON string value extraction
==============
*/
qboolean JSON_GetString(const char *json, const char *key, char *out, int outSize) {
	char searchKey[128];
	char *start, *end;
	int len;

	Com_sprintf(searchKey, sizeof(searchKey), "\"%s\":\"", key);
	start = strstr(json, searchKey);
	if (!start) {
		// Try without quotes around value
		Com_sprintf(searchKey, sizeof(searchKey), "\"%s\":", key);
		start = strstr(json, searchKey);
		if (!start) return qfalse;
		start += strlen(searchKey);
		// Skip whitespace
		while (*start == ' ' || *start == '\t') start++;
		if (*start != '"') return qfalse;
		start++;
	} else {
		start += strlen(searchKey);
	}

	end = strchr(start, '"');
	if (!end) return qfalse;

	len = end - start;
	if (len >= outSize) len = outSize - 1;

	memcpy(out, start, len);
	out[len] = '\0';

	return qtrue;
}

/*
==============
JSON_GetInt
Simple JSON integer value extraction
==============
*/
qboolean JSON_GetInt(const char *json, const char *key, int *out) {
	char searchKey[128];
	char *start;

	Com_sprintf(searchKey, sizeof(searchKey), "\"%s\":", key);
	start = strstr(json, searchKey);
	if (!start) return qfalse;

	start += strlen(searchKey);
	while (*start == ' ' || *start == '\t') start++;

	*out = atoi(start);
	return qtrue;
}

/*
==============
JSON_GetFloat
Simple JSON float value extraction
==============
*/
qboolean JSON_GetFloat(const char *json, const char *key, float *out) {
	char searchKey[128];
	char *start;

	Com_sprintf(searchKey, sizeof(searchKey), "\"%s\":", key);
	start = strstr(json, searchKey);
	if (!start) return qfalse;

	start += strlen(searchKey);
	while (*start == ' ' || *start == '\t') start++;

	*out = atof(start);
	return qtrue;
}

/*
==============
JSON_GetBool
Simple JSON boolean value extraction
==============
*/
qboolean JSON_GetBool(const char *json, const char *key, qboolean *out) {
	char searchKey[128];
	char *start;

	Com_sprintf(searchKey, sizeof(searchKey), "\"%s\":", key);
	start = strstr(json, searchKey);
	if (!start) return qfalse;

	start += strlen(searchKey);
	while (*start == ' ' || *start == '\t') start++;

	if (strncmp(start, "true", 4) == 0) {
		*out = qtrue;
	} else if (strncmp(start, "false", 5) == 0) {
		*out = qfalse;
	} else {
		return qfalse;
	}

	return qtrue;
}

/*
==============
Account_Register
Register a new account
==============
*/
accountError_t Account_Register(const char *username, const char *password, accountData_t *outData) {
	char jsonBody[512];
	char response[4096];
	qboolean success;
	int result;

	if (!username || !password || !outData) {
		return ACCOUNT_ERROR_INVALID_FORMAT;
	}

	// Build JSON request
	Com_sprintf(jsonBody, sizeof(jsonBody),
		"{\"username\":\"%s\",\"password\":\"%s\"}",
		username, password
	);

	// Send HTTP request
	result = HTTP_Post(ACCOUNT_API_HOST, ACCOUNT_API_PORT, "/auth/register", jsonBody, response, sizeof(response));
	if (result <= 0) {
		return ACCOUNT_ERROR_NETWORK;
	}

	// Parse response
	if (!JSON_GetBool(response, "success", &success) || !success) {
		// Check if username exists error
		if (strstr(response, "exists") || strstr(response, "409")) {
			return ACCOUNT_ERROR_EXISTS;
		}
		return ACCOUNT_ERROR_SERVER;
	}

	// Extract account ID
	if (!JSON_GetInt(response, "account_id", &outData->accountId)) {
		return ACCOUNT_ERROR_SERVER;
	}

	Q_strncpyz(outData->username, username, sizeof(outData->username));
	outData->level = 1;
	outData->experience = 0;
	outData->credits = 100;
	outData->alignment = 0.0f;
	Q_strncpyz(outData->rankTitle, "Initiate", sizeof(outData->rankTitle));
	outData->isValid = qtrue;

	return ACCOUNT_SUCCESS;
}

/*
==============
Account_Login
Login to an existing account
==============
*/
accountError_t Account_Login(const char *username, const char *password, accountData_t *outData) {
	char jsonBody[512];
	char response[4096];
	char statsSection[2048];
	qboolean success;
	int result;
	char *statsStart;

	if (!username || !password || !outData) {
		return ACCOUNT_ERROR_INVALID_FORMAT;
	}

	// Build JSON request
	Com_sprintf(jsonBody, sizeof(jsonBody),
		"{\"username\":\"%s\",\"password\":\"%s\"}",
		username, password
	);

	// Send HTTP request
	result = HTTP_Post(ACCOUNT_API_HOST, ACCOUNT_API_PORT, "/auth/login", jsonBody, response, sizeof(response));
	if (result <= 0) {
		return ACCOUNT_ERROR_NETWORK;
	}

	// Parse response
	if (!JSON_GetBool(response, "success", &success) || !success) {
		return ACCOUNT_ERROR_INVALID_CREDENTIALS;
	}

	// Extract account data
	if (!JSON_GetInt(response, "account_id", &outData->accountId)) {
		return ACCOUNT_ERROR_SERVER;
	}

	if (!JSON_GetString(response, "username", outData->username, sizeof(outData->username))) {
		Q_strncpyz(outData->username, username, sizeof(outData->username));
	}

	if (!JSON_GetString(response, "token", outData->token, sizeof(outData->token))) {
		return ACCOUNT_ERROR_SERVER;
	}

	if (!JSON_GetString(response, "expires_at", outData->expiresAt, sizeof(outData->expiresAt))) {
		outData->expiresAt[0] = '\0';
	}

	// Extract stats section
	statsStart = strstr(response, "\"stats\":");
	if (statsStart) {
		statsStart += 8; // Skip "stats":
		// Copy until closing brace
		char *end = strchr(statsStart, '}');
		if (end) {
			int len = end - statsStart + 1;
			if (len < sizeof(statsSection)) {
				memcpy(statsSection, statsStart, len);
				statsSection[len] = '\0';

				JSON_GetInt(statsSection, "level", &outData->level);
				JSON_GetInt(statsSection, "experience", &outData->experience);
				JSON_GetInt(statsSection, "credits", &outData->credits);
				JSON_GetFloat(statsSection, "alignment", &outData->alignment);
				JSON_GetString(statsSection, "rank_title", outData->rankTitle, sizeof(outData->rankTitle));
			}
		}
	}

	outData->isValid = qtrue;

	return ACCOUNT_SUCCESS;
}

/*
==============
Cmd_Register_f
In-game /register command
==============
*/
void Cmd_Register_f(gentity_t *ent) {
	char username[64];
	char password[64];
	accountData_t accountData;
	accountError_t result;

	if (!ent || !ent->client) return;

	// Check if already logged in
	if (Account_IsLoggedIn(ent)) {
		trap->SendServerCommand(ent - g_entities, "print \"^3You are already logged in. Use ^7/logout^3 first.\n\"");
		return;
	}

	// Parse arguments
	if (trap->Argc() < 3) {
		trap->SendServerCommand(ent - g_entities, "print \"^3Usage: ^7/register <username> <password>\n\"");
		trap->SendServerCommand(ent - g_entities, "print \"^3Username: 3-32 characters, alphanumeric + underscore\n\"");
		trap->SendServerCommand(ent - g_entities, "print \"^3Password: Minimum 6 characters\n\"");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, password, sizeof(password));

	// Validate input
	if (strlen(username) < 3 || strlen(username) > 32) {
		trap->SendServerCommand(ent - g_entities, "print \"^1Error: Username must be 3-32 characters.\n\"");
		return;
	}

	if (strlen(password) < 6) {
		trap->SendServerCommand(ent - g_entities, "print \"^1Error: Password must be at least 6 characters.\n\"");
		return;
	}

	trap->SendServerCommand(ent - g_entities, "print \"^3Registering account...\n\"");

	// Attempt registration
	memset(&accountData, 0, sizeof(accountData));
	result = Account_Register(username, password, &accountData);

	switch (result) {
		case ACCOUNT_SUCCESS:
			trap->SendServerCommand(ent - g_entities,
				va("print \"^2Account created successfully!\n^3Account ID: ^7%d\n^3Now use ^7/login %s <password>^3 to login.\n\"",
				accountData.accountId, username));
			break;
		case ACCOUNT_ERROR_EXISTS:
			trap->SendServerCommand(ent - g_entities, "print \"^1Error: Username already exists. Please choose another.\n\"");
			break;
		case ACCOUNT_ERROR_NETWORK:
			trap->SendServerCommand(ent - g_entities, "print \"^1Error: Could not connect to account server.\n\"");
			break;
		case ACCOUNT_ERROR_INVALID_FORMAT:
			trap->SendServerCommand(ent - g_entities, "print \"^1Error: Invalid username or password format.\n\"");
			break;
		default:
			trap->SendServerCommand(ent - g_entities, "print \"^1Error: Account registration failed. Please try again.\n\"");
			break;
	}
}

/*
==============
Cmd_Login_f
In-game /login command
==============
*/
void Cmd_Login_f(gentity_t *ent) {
	char username[64];
	char password[64];
	accountData_t accountData;
	accountError_t result;

	if (!ent || !ent->client) return;

	// Check if already logged in
	if (Account_IsLoggedIn(ent)) {
		trap->SendServerCommand(ent - g_entities, "print \"^3You are already logged in.\n\"");
		return;
	}

	// Parse arguments
	if (trap->Argc() < 3) {
		trap->SendServerCommand(ent - g_entities, "print \"^3Usage: ^7/login <username> <password>\n\"");
		return;
	}

	trap->Argv(1, username, sizeof(username));
	trap->Argv(2, password, sizeof(password));

	trap->SendServerCommand(ent - g_entities, "print \"^3Logging in...\n\"");

	// Attempt login
	memset(&accountData, 0, sizeof(accountData));
	result = Account_Login(username, password, &accountData);

	switch (result) {
		case ACCOUNT_SUCCESS:
			// Store account data in client session
			Account_StoreInClient(ent, &accountData);

			trap->SendServerCommand(ent - g_entities,
				va("print \"^2Login successful! Welcome back, ^7%s^2!\n\"", username));
			trap->SendServerCommand(ent - g_entities,
				va("print \"^3Level: ^7%d ^3| Experience: ^7%d ^3| Credits: ^7%d\n\"",
				accountData.level, accountData.experience, accountData.credits));
			trap->SendServerCommand(ent - g_entities,
				va("print \"^3Rank: ^7%s ^3| Alignment: ^7%.1f\n\"",
				accountData.rankTitle, accountData.alignment));
			break;
		case ACCOUNT_ERROR_INVALID_CREDENTIALS:
			trap->SendServerCommand(ent - g_entities, "print \"^1Error: Invalid username or password.\n\"");
			break;
		case ACCOUNT_ERROR_NETWORK:
			trap->SendServerCommand(ent - g_entities, "print \"^1Error: Could not connect to account server.\n\"");
			break;
		default:
			trap->SendServerCommand(ent - g_entities, "print \"^1Error: Login failed. Please try again.\n\"");
			break;
	}
}

/*
==============
Cmd_Logout_f
In-game /logout command
==============
*/
void Cmd_Logout_f(gentity_t *ent) {
	if (!ent || !ent->client) return;

	if (!Account_IsLoggedIn(ent)) {
		trap->SendServerCommand(ent - g_entities, "print \"^3You are not logged in.\n\"");
		return;
	}

	trap->SendServerCommand(ent - g_entities,
		va("print \"^3Goodbye, ^7%s^3! You have been logged out.\n\"",
		ent->client->sess.accountUsername));

	Account_Clear(ent);
}

/*
==============
Cmd_AccountStats_f
In-game /account command - show account stats
==============
*/
void Cmd_AccountStats_f(gentity_t *ent) {
	if (!ent || !ent->client) return;

	if (!Account_IsLoggedIn(ent)) {
		trap->SendServerCommand(ent - g_entities, "print \"^3You are not logged in. Use ^7/login^3 or ^7/register^3.\n\"");
		return;
	}

	trap->SendServerCommand(ent - g_entities, "print \"^3========== Account Information ==========\n\"");
	trap->SendServerCommand(ent - g_entities,
		va("print \"^3Username: ^7%s ^3(ID: ^7%d^3)\n\"",
		ent->client->sess.accountUsername, ent->client->sess.accountId));
	trap->SendServerCommand(ent - g_entities,
		va("print \"^3Level: ^7%d ^3| Experience: ^7%d\n\"",
		ent->client->sess.accountLevel, ent->client->sess.accountExperience));
	trap->SendServerCommand(ent - g_entities,
		va("print \"^3Credits: ^7%d ^3| Alignment: ^7%.1f\n\"",
		ent->client->sess.accountCredits, ent->client->sess.accountAlignment));
	trap->SendServerCommand(ent - g_entities,
		va("print \"^3Rank: ^7%s\n\"",
		ent->client->sess.accountRankTitle));
	trap->SendServerCommand(ent - g_entities, "print \"^3========================================\n\"");
}
