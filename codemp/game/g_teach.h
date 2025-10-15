#ifndef G_TEACH_H
#define G_TEACH_H

#include "g_local.h"
#include "bg_public.h"
#include <qcommon/q_shared.h>

/* Combat event types for duel replay */
typedef enum {
    COMBAT_EVENT_NONE = 0,
    COMBAT_EVENT_HIT,         /* Saber hit landed */
    COMBAT_EVENT_BLOCK,       /* Attack blocked */
    COMBAT_EVENT_PARRY,       /* Saber parry */
    COMBAT_EVENT_CLASH,       /* Saber clash */
    COMBAT_EVENT_KNOCKBACK,   /* Player knocked back */
    COMBAT_EVENT_FORCE_PUSH,  /* Force push used */
    COMBAT_EVENT_FORCE_PULL,  /* Force pull used */
    COMBAT_EVENT_FORCE_GRIP,  /* Force grip used */
    COMBAT_EVENT_FORCE_LIGHTNING, /* Force lightning used */
    COMBAT_EVENT_DEATH        /* Player killed */
} combatEventType_t;

/* Combat event recording for duel replay */
typedef struct {
    int timestamp;       /* Milliseconds since recording start */
    combatEventType_t eventType;  /* Type of event */
    int player1;         /* Initiator (attacker/force user) - 0=A, 1=B */
    int player2;         /* Target (victim) - 0=A, 1=B, -1=none */
    int damage;          /* Damage dealt (if hit) */
    vec3_t knockback;    /* Knockback vector (if applicable) */
    int hitLocation;     /* Hit location (G2 bone/body part) */
    int blockType;       /* Block type (normal/parry) */
} teach_combat_event_t;

/* One recorded usercmd sample (compact) */
typedef struct {
    int ms;            /* milliseconds since record start */
    int buttons;       /* ucmd->buttons */
    int ay, ap, ar;    /* angles: yaw,pitch,roll (shorts promoted to int) */
    signed char f;     /* forwardmove  */
    signed char r;     /* rightmove    */
    signed char u;     /* upmove       */
    int gc;            /* ucmd->generic_cmd (e.g. saber cycle) */
    int style;         /* saberAnimLevel at this frame (-1 if unknown) */
    int wy, wp, wr;    /* world viewangles (shorts promoted to int) */
    qboolean haveWorldAngles;

    /* State-augmented data for drift correction */
    vec3_t origin;     /* Authoritative position */
    vec3_t velocity;   /* Authoritative velocity */
    int groundEntityNum; /* Ground contact (-1 = airborne) */
    int pm_flags;      /* Jump/knockback/water states */
    int pm_time;       /* Time remaining in special states */
    int saberMove;     /* Current saber animation phase */
    int torsoAnim;     /* Current torso animation index */
    int legsAnim;      /* Current legs animation index */
    int torsoTimer;    /* Animation timer for torso */
    int legsTimer;     /* Animation timer for legs */
    int weaponTime;    /* Weapon firing/swing timer */
    int dualSabers;    /* Has dual sabers equipped */
    int saberHolstered;/* Saber holster state (0=all on, 1=first off, 2=second off, 3=both off) */
    qboolean haveState; /* Does this frame have state data */

    /* Combat state for duel replay */
    int health;        /* Current HP */
    int maxHealth;     /* Max HP */
    int forcePower;    /* Current force power */
    int forcePowerMax; /* Max force power */
    int saberBlocked;  /* Block/parry state (BLOCKED_NONE, BLOCKED_PARRY_DONE, etc.) */
    int saberBlocking; /* Currently blocking */
} teach_frame_t;

/* Frame chunk for large recordings - linked list approach */
#define FRAMES_PER_CHUNK 512  /* ~115KB per chunk for dual frames, ~50KB for single frames */

typedef struct teach_chunk_s {
    teach_frame_t frames[FRAMES_PER_CHUNK];
    int count;  /* Number of frames actually stored in this chunk (0-512) */
    struct teach_chunk_s *next;  /* Next chunk in linked list */
} teach_chunk_t;

/* Slot-hijack playback state */
typedef struct {
    qboolean active;
    char     name[64];

    teach_chunk_t *chunks;  /* Linked list of frame chunks */
    int      count;         /* Total frame count across all chunks */

    int      clientNum;     /* target client slot */
    float    rate;          /* 1.0 = realtime */
    qboolean loop;

    int   startTime;        /* server msec at play start */
    int   last_ms;          /* last applied frame ms */
    int   last_idx;         /* last applied index */
    int   lastStyle;        /* last applied saber style */
    int   lastCmdServerTime;/* last ucmd->serverTime used */
    char  saved_pmoveFixed[8];
    char  saved_pmoveMsec[8];
    qboolean cvarsGuarded;
    vec3_t lastViewAngles;  /* latest recorded viewangles (deg) */
    qboolean haveViewAngles;/* set true when angles available this frame */
    int     lastCmdAngles[3];/* last applied cmd->angles (short units) */
    qboolean haveCmdAngles; /* true when lastCmdAngles is valid */
    vec3_t baseRecordedAngles; /* first frame recorded angles (deg) */
    vec3_t baseWorldAngles;    /* world/client angles at playback start */
    qboolean haveAngleBase;    /* base captured flag */
    qboolean inForcedSetView;  /* true while teach drives SetClientViewAngle */

    /* Training bot mode */
    int   targetPlayerNum;     /* Player to chase (-1 = disabled) */
    vec3_t trainingOffset;     /* Offset from target player */
} teach_play_t;

/* Active recording state */
typedef struct {
    qboolean    active;
    int         clientNum;
    int         startTime;
    char        name[64];
    fileHandle_t fh;        /* output file */
    int         pendingGenericCmd; /* latched generic command for current frame */
    int         pendingSaberStyle; /* latched saber style for current frame */
} teach_rec_t;

/* Dual-actor recording state */
typedef struct {
    qboolean    active;
    int         clientNumA;
    int         clientNumB;
    int         startTime;
    char        name[64];
    fileHandle_t fh;        /* output file */
    int         pendingGenericCmdA;
    int         pendingSaberStyleA;
    int         pendingGenericCmdB;
    int         pendingSaberStyleB;

    /* Combat event recording */
    teach_combat_event_t *events;  /* Dynamic array of combat events */
    int         eventCount;         /* Number of events recorded */
    int         eventCapacity;      /* Allocated capacity */
} teach_duel_rec_t;

/* Dual frame - both actors at same timestamp */
typedef struct {
    int t;                  /* timestamp ms */
    teach_frame_t A;        /* actor A frame */
    teach_frame_t B;        /* actor B frame */
    qboolean hasInitialState;  /* true for first frame with origin data */
    vec3_t originA;         /* initial position for actor A */
    vec3_t originB;         /* initial position for actor B */
} teach_duel_frame_t;

typedef struct teach_duel_chunk_s {
    teach_duel_frame_t frames[FRAMES_PER_CHUNK];
    int count;  /* Number of frames actually stored in this chunk (0-512) */
    struct teach_duel_chunk_s *next;  /* Next chunk in linked list */
} teach_duel_chunk_t;

/* Dual playback state */
typedef struct {
    qboolean active;
    char     name[64];

    teach_duel_chunk_t *chunks;  /* Linked list of frame chunks */
    int      totalFrames;         /* Total frames across all chunks */

    int      clientNumA;
    int      clientNumB;
    float    rate;
    qboolean loop;

    int      startTime;
    int      last_ms;
    int      last_idx;
    int      lastCmdServerTimeA;
    int      lastCmdServerTimeB;
    int      lastStyleA;         /* Last applied saber style for player A */
    int      lastStyleB;         /* Last applied saber style for player B */

    /* Cache for performance - points to last accessed chunk */
    teach_duel_chunk_t *cachedChunk;
    int      cachedChunkStartIdx;
} teach_duel_play_t;

/* Globals (status only) */
extern teach_play_t g_teachPlay;
extern teach_rec_t  g_teachRec;
extern teach_duel_rec_t g_teachDuelRec;
extern teach_duel_play_t g_teachDuelPlay;

/* Core API (call sites in game) */
qboolean Teach_IsPlaying(void);
qboolean Teach_IsPlayingFor(const gentity_t *ent);
qboolean Teach_IsControllingClient(int clientNum);  /* Check if client is under teach playback control */
qboolean Teach_IsForcingViewFor(const gentity_t *ent);
void Teach_RecordUsercmd(gentity_t *ent, const usercmd_t *ucmd);
void Teach_FilterOrPlayUcmd(gentity_t *ent, usercmd_t *ucmd);
qboolean Teach_HaveCurrentAnglesFor(const gentity_t *ent, vec3_t outAngles);
qboolean Teach_GetTargetViewAngles(const gentity_t *ent, vec3_t outAngles);
void Teach_ApplyForcedView(gentity_t *ent);
void Teach_RunFrame(void);
void Teach_DuelPostPmove(gentity_t *ent);
void Teach_PlayPostPmove(gentity_t *ent);

/* Combat event recording */
void Teach_RecordCombatEvent(combatEventType_t type, gentity_t *player1, gentity_t *player2,
                              int damage, vec3_t knockback, int hitLocation);

/* Console command dispatcher (called from g_svcmds) */
void Svcmd_Teach_f(void);  /* OpenJK expects a void(void) handler */

/* Puppet (clientless ghost) */
qboolean Teach_PuppetSpawn(const char *name, int hereCid, float rate, qboolean loop);
void     Teach_PuppetKill(void);
qboolean Teach_PuppetIsActive(void);

#endif /* G_TEACH_H */





