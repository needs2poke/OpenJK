/*
===========================================================================
OpenJK Account System Integration
Cross-server persistent accounts via REST API
===========================================================================
*/

#ifndef G_ACCOUNTS_H
#define G_ACCOUNTS_H

#include "g_local.h"

// Account API configuration
#define ACCOUNT_API_HOST "158.69.218.235"
#define ACCOUNT_API_PORT 8000

// Account status codes
typedef enum {
	ACCOUNT_SUCCESS = 0,
	ACCOUNT_ERROR_NETWORK,
	ACCOUNT_ERROR_EXISTS,
	ACCOUNT_ERROR_INVALID_CREDENTIALS,
	ACCOUNT_ERROR_INVALID_FORMAT,
	ACCOUNT_ERROR_SERVER,
	ACCOUNT_ERROR_TIMEOUT
} accountError_t;

// Account data structure
typedef struct {
	int			accountId;
	char		username[32];
	char		token[512];
	char		expiresAt[64];
	int			level;
	int			experience;
	int			credits;
	float		alignment;
	char		rankTitle[32];
	qboolean	isValid;
} accountData_t;

// Function prototypes
void Cmd_Register_f(gentity_t *ent);
void Cmd_Login_f(gentity_t *ent);
void Cmd_Logout_f(gentity_t *ent);
void Cmd_AccountStats_f(gentity_t *ent);

accountError_t Account_Register(const char *username, const char *password, accountData_t *outData);
accountError_t Account_Login(const char *username, const char *password, accountData_t *outData);
void Account_Clear(gentity_t *ent);
qboolean Account_IsLoggedIn(gentity_t *ent);

// Internal HTTP helpers
int HTTP_Post(const char *host, int port, const char *path, const char *jsonBody, char *response, int responseSize);
int HTTP_Get(const char *host, int port, const char *path, const char *token, char *response, int responseSize);

// JSON parsing helpers
qboolean JSON_GetString(const char *json, const char *key, char *out, int outSize);
qboolean JSON_GetInt(const char *json, const char *key, int *out);
qboolean JSON_GetFloat(const char *json, const char *key, float *out);
qboolean JSON_GetBool(const char *json, const char *key, qboolean *out);

#endif // G_ACCOUNTS_H
