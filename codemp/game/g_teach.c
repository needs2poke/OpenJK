// codemp/game/g_teach.c

#include "g_local.h"
#include "bg_public.h"
#include "g_teach.h"

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* Forward declarations */
static const teach_frame_t* Teach_GetFrame(int idx);

/* ============================================================
   Small utils
   ============================================================ */

static void TPrintF(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    Q_vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    trap->Print("%s", msg);
}

static void T_CvarGet(const char *name, char *out, int outSize) {
    trap->Cvar_VariableStringBuffer(name, out, outSize);
}

/* Read one text line from FS into dst (NUL-terminated).
   Returns length written (>=0). 0 means EOF/no chars. */
static int T_ReadLine(fileHandle_t f, char *dst, int max) {
    int n = 0;
    while (n + 1 < max) {
        char c;
        int r = trap->FS_Read(&c, 1, f);
        if (r <= 0) {
            break; /* EOF */
        }
        if (c == '\r') {
            continue; /* normalize */
        }
        if (c == '\n') {
            break;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
    return n;
}

static int T_ShortDelta(int target, int cmd) {
    int delta = target - cmd;
    if (delta > 32767) {
        delta -= 65536;
    } else if (delta < -32768) {
        delta += 65536;
    }
    return delta;
}

/* ============================================================
   Globals
   ============================================================ */

teach_play_t g_teachPlay;
teach_rec_t  g_teachRec;
teach_duel_rec_t g_teachDuelRec;
teach_duel_play_t g_teachDuelPlay;
static qboolean g_duelWroteInitial = qfalse;

/* Forward declarations */
static void Teach_ApplyDriftCorrection(gentity_t *ent, const teach_frame_t *fr);

qboolean Teach_IsPlaying(void) {
    return g_teachPlay.active;
}

qboolean Teach_IsPlayingFor(const gentity_t *ent) {
    if (!ent || !ent->client) {
        return qfalse;
    }
    /* Check single playback */
    if (g_teachPlay.active && ent->s.number == g_teachPlay.clientNum) {
        return qtrue;
    }
    /* Check dual playback */
    if (g_teachDuelPlay.active) {
        if (ent->s.number == g_teachDuelPlay.clientNumA || ent->s.number == g_teachDuelPlay.clientNumB) {
            return qtrue;
        }
    }
    return qfalse;
}

qboolean Teach_IsControllingClient(int clientNum) {
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
        return qfalse;
    }
    /* Check single playback */
    if (g_teachPlay.active && clientNum == g_teachPlay.clientNum) {
        return qtrue;
    }
    /* Check dual playback */
    if (g_teachDuelPlay.active) {
        if (clientNum == g_teachDuelPlay.clientNumA || clientNum == g_teachDuelPlay.clientNumB) {
            return qtrue;
        }
    }
    return qfalse;
}

/* ============================================================
   Info helpers
   ============================================================ */

static void T_PrintWhere(void) {
    char fs_game[64], fs_homepath[MAX_QPATH], fs_basepath[MAX_QPATH];
    T_CvarGet("fs_game", fs_game, sizeof(fs_game));
    T_CvarGet("fs_homepath", fs_homepath, sizeof(fs_homepath));
    T_CvarGet("fs_basepath", fs_basepath, sizeof(fs_basepath));
    TPrintF("teach: fs_game='%s'\n", fs_game);
    TPrintF("teach: fs_homepath='%s'\n", fs_homepath);
    TPrintF("teach: fs_basepath='%s'\n", fs_basepath);
}

static void T_PrintStatus(void) {
    if (g_teachRec.active) {
        TPrintF("teach: recording cid %d -> %s\n", g_teachRec.clientNum, g_teachRec.name);
    } else {
        TPrintF("teach: not recording\n");
    }
    if (g_teachDuelRec.active) {
        TPrintF("teach: recording DUEL cid %d + %d -> %s\n",
            g_teachDuelRec.clientNumA, g_teachDuelRec.clientNumB, g_teachDuelRec.name);
    } else {
        TPrintF("teach: not recording duel\n");
    }
    if (g_teachPlay.active) {
        TPrintF("teach: playing '%s' on cid %d (rate=%.2f loop=%d idx=%d/%d)\n",
            g_teachPlay.name, g_teachPlay.clientNum, g_teachPlay.rate, (int)g_teachPlay.loop,
            g_teachPlay.last_idx, g_teachPlay.count);
    } else {
        TPrintF("teach: not playing\n");
    }
    if (g_teachDuelPlay.active) {
        TPrintF("teach: playing DUEL '%s' on cid %d + %d (rate=%.2f loop=%d idx=%d/%d)\n",
            g_teachDuelPlay.name, g_teachDuelPlay.clientNumA, g_teachDuelPlay.clientNumB,
            g_teachDuelPlay.rate, (int)g_teachDuelPlay.loop,
            g_teachDuelPlay.last_idx, g_teachDuelPlay.totalFrames);
    } else {
        TPrintF("teach: not playing duel\n");
    }
    if (Teach_PuppetIsActive()) {
        TPrintF("teach: puppet active\n");
    }
}

/* ============================================================
   Recording
   ============================================================ */

static void Teach_RecordStart(int clientNum, const char *name) {
    if (g_teachRec.active) {
        TPrintF("teach: already recording\n");
        return;
    }
    if (clientNum < 0 || clientNum >= MAX_CLIENTS || !g_entities[clientNum].client) {
        TPrintF("teach: invalid client %d\n", clientNum);
        return;
    }

    Com_sprintf(g_teachRec.name, sizeof(g_teachRec.name), "teach__%s.teach.jsonl", name);

    g_teachRec.fh = 0;
    trap->FS_Open(g_teachRec.name, &g_teachRec.fh, FS_WRITE);
    if (!g_teachRec.fh) {
        TPrintF("teach: open failed: %s\n", g_teachRec.name);
        return;
    }

    static const char *hdr = "# teach recording start\n";
    trap->FS_Write((void*)hdr, (int)strlen(hdr), g_teachRec.fh);

    g_teachRec.active    = qtrue;
    g_teachRec.clientNum = clientNum;
    g_teachRec.startTime = level.time;
    g_teachRec.pendingGenericCmd = 0;
    g_teachRec.pendingSaberStyle = -1;

    TPrintF("teach: recording cid %d -> %s\n", clientNum, g_teachRec.name);
}

static void Teach_RecordStop(void) {
    if (!g_teachRec.active) {
        return;
    }
    static const char *end = "# teach end\n";
    trap->FS_Write((void*)end, (int)strlen(end), g_teachRec.fh);
    trap->FS_Close(g_teachRec.fh);
    g_teachRec.fh = 0;
    g_teachRec.active = qfalse;
    g_teachRec.pendingGenericCmd = 0;
    g_teachRec.pendingSaberStyle = -1;
    TPrintF("teach: record stopped (%s)\n", g_teachRec.name);
}

void Teach_RecordUsercmd(gentity_t *ent, const usercmd_t *ucmd) {
    if (!g_teachRec.active) return;
    if (!ent || !ent->client) return;
    if (ent->s.number != g_teachRec.clientNum) return;
    if (!g_teachRec.fh) return;

    const int relTime = level.time - g_teachRec.startTime;
    teach_frame_t fr;

    fr.ms      = relTime;
    fr.buttons = ucmd->buttons;
    fr.ay      = ucmd->angles[YAW];
    fr.ap      = ucmd->angles[PITCH];
    fr.ar      = ucmd->angles[ROLL];
    fr.f       = ucmd->forwardmove;
    fr.r       = ucmd->rightmove;
    fr.u       = ucmd->upmove;
    fr.gc      = ucmd->generic_cmd;
    fr.style   = ent->client ? ent->client->ps.fd.saberAnimLevel : -1;
    fr.haveWorldAngles = qtrue;
    fr.wy      = (short)(ucmd->angles[YAW] + ent->client->ps.delta_angles[YAW]);
    fr.wp      = (short)(ucmd->angles[PITCH] + ent->client->ps.delta_angles[PITCH]);
    fr.wr      = (short)(ucmd->angles[ROLL] + ent->client->ps.delta_angles[ROLL]);

    /* Capture state data for drift correction */
    fr.haveState = qtrue;
    VectorCopy(ent->client->ps.origin, fr.origin);
    VectorCopy(ent->client->ps.velocity, fr.velocity);
    fr.groundEntityNum = ent->client->ps.groundEntityNum;
    fr.pm_flags = ent->client->ps.pm_flags;
    fr.pm_time = ent->client->ps.pm_time;
    fr.saberMove = ent->client->ps.saberMove;
    fr.torsoAnim = ent->client->ps.torsoAnim;
    fr.legsAnim = ent->client->ps.legsAnim;
    fr.torsoTimer = ent->client->ps.torsoTimer;
    fr.legsTimer = ent->client->ps.legsTimer;
    fr.weaponTime = ent->client->ps.weaponTime;
    fr.dualSabers = (ent->client->saber[1].model[0] != 0);
    fr.saberHolstered = ent->client->ps.saberHolstered;

    /* Capture combat state for duel replay */
    fr.health = ent->health;
    fr.maxHealth = ent->client->ps.stats[STAT_MAX_HEALTH];
    fr.forcePower = ent->client->ps.fd.forcePower;
    fr.forcePowerMax = ent->client->ps.fd.forcePowerMax;
    fr.saberBlocked = ent->client->ps.saberBlocked;
    fr.saberBlocking = ent->client->ps.saberBlocking;

    if (g_teachRec.pendingGenericCmd) {
        fr.gc = g_teachRec.pendingGenericCmd;
        g_teachRec.pendingGenericCmd = 0;
    }

    if (g_teachRec.pendingSaberStyle >= 0) {
        fr.style = g_teachRec.pendingSaberStyle;
        g_teachRec.pendingSaberStyle = -1;
    }

    char line[512];
    int len = Com_sprintf(line, sizeof(line),
        "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d,"
        "\"ox\":%.2f,\"oy\":%.2f,\"oz\":%.2f,\"vx\":%.2f,\"vy\":%.2f,\"vz\":%.2f,\"ground\":%d,\"pmf\":%d,\"pmt\":%d,\"sm\":%d,"
        "\"ta\":%d,\"la\":%d,\"tt\":%d,\"lt\":%d,\"wt\":%d,\"ds\":%d,\"sh\":%d,"
        "\"hp\":%d,\"maxhp\":%d,\"fp\":%d,\"maxfp\":%d,\"sblk\":%d,\"sblking\":%d}\n",
        fr.ms, fr.buttons, fr.ay, fr.ap, fr.ar,
        (int)fr.f, (int)fr.r, (int)fr.u,
        fr.gc, fr.style, fr.wy, fr.wp, fr.wr,
        fr.origin[0], fr.origin[1], fr.origin[2],
        fr.velocity[0], fr.velocity[1], fr.velocity[2],
        fr.groundEntityNum, fr.pm_flags, fr.pm_time, fr.saberMove,
        fr.torsoAnim, fr.legsAnim, fr.torsoTimer, fr.legsTimer, fr.weaponTime,
        fr.dualSabers, fr.saberHolstered,
        fr.health, fr.maxHealth, fr.forcePower, fr.forcePowerMax, fr.saberBlocked, fr.saberBlocking);
    trap->FS_Write(line, len, g_teachRec.fh);
}

/* ============================================================
   Combat Event Recording
   ============================================================ */

void Teach_RecordCombatEvent(combatEventType_t type, gentity_t *player1, gentity_t *player2,
                              int damage, vec3_t knockback, int hitLocation) {
    if (!g_teachDuelRec.active) {
        return;  /* Only record combat events during duel recording */
    }

    /* Determine which player is which (0=A, 1=B, -1=not in recording) */
    int p1Index = -1, p2Index = -1;

    if (player1) {
        if (player1->s.number == g_teachDuelRec.clientNumA) p1Index = 0;
        else if (player1->s.number == g_teachDuelRec.clientNumB) p1Index = 1;
    }

    if (player2) {
        if (player2->s.number == g_teachDuelRec.clientNumA) p2Index = 0;
        else if (player2->s.number == g_teachDuelRec.clientNumB) p2Index = 1;
    }

    /* Only record if at least one player is in the duel */
    if (p1Index == -1 && p2Index == -1) {
        return;
    }

    /* Expand event array if needed */
    if (g_teachDuelRec.eventCount >= g_teachDuelRec.eventCapacity) {
        int newCap = g_teachDuelRec.eventCapacity == 0 ? 256 : g_teachDuelRec.eventCapacity * 2;
        teach_combat_event_t *newEvents = (teach_combat_event_t *)G_Alloc(sizeof(teach_combat_event_t) * newCap);

        if (g_teachDuelRec.events) {
            memcpy(newEvents, g_teachDuelRec.events, sizeof(teach_combat_event_t) * g_teachDuelRec.eventCount);
            /* Note: Can't free old array as G_Alloc doesn't have a matching free in level memory */
        }

        g_teachDuelRec.events = newEvents;
        g_teachDuelRec.eventCapacity = newCap;
    }

    /* Record the event */
    teach_combat_event_t *evt = &g_teachDuelRec.events[g_teachDuelRec.eventCount++];
    evt->timestamp = level.time - g_teachDuelRec.startTime;
    evt->eventType = type;
    evt->player1 = p1Index;
    evt->player2 = p2Index;
    evt->damage = damage;
    evt->hitLocation = hitLocation;
    evt->blockType = 0;  /* Will be set for block events */

    if (knockback) {
        VectorCopy(knockback, evt->knockback);
    } else {
        VectorClear(evt->knockback);
    }
}

/* ============================================================
   Dual-actor recording
   ============================================================ */

static void Teach_DuelRecordStart(int clientA, int clientB, const char *name) {
    if (g_teachDuelRec.active) {
        TPrintF("teach: duel recording already active\n");
        return;
    }
    if (clientA < 0 || clientA >= MAX_CLIENTS || !g_entities[clientA].client) {
        TPrintF("teach: invalid client A %d\n", clientA);
        return;
    }
    if (clientB < 0 || clientB >= MAX_CLIENTS || !g_entities[clientB].client) {
        TPrintF("teach: invalid client B %d\n", clientB);
        return;
    }
    if (clientA == clientB) {
        TPrintF("teach: clients A and B must be different\n");
        return;
    }

    Com_sprintf(g_teachDuelRec.name, sizeof(g_teachDuelRec.name), "teach__%s.duel.jsonl", name);

    g_teachDuelRec.fh = 0;
    trap->FS_Open(g_teachDuelRec.name, &g_teachDuelRec.fh, FS_WRITE);
    if (!g_teachDuelRec.fh) {
        TPrintF("teach: open failed: %s\n", g_teachDuelRec.name);
        return;
    }

    static const char *hdr = "# teach duel recording start\n";
    trap->FS_Write((void*)hdr, (int)strlen(hdr), g_teachDuelRec.fh);

    g_teachDuelRec.active = qtrue;
    g_teachDuelRec.clientNumA = clientA;
    g_teachDuelRec.clientNumB = clientB;
    g_teachDuelRec.startTime = level.time;
    g_teachDuelRec.pendingGenericCmdA = 0;
    g_teachDuelRec.pendingSaberStyleA = -1;
    g_teachDuelRec.pendingGenericCmdB = 0;
    g_teachDuelRec.pendingSaberStyleB = -1;
    g_duelWroteInitial = qfalse;

    /* Initialize combat event array */
    g_teachDuelRec.events = NULL;
    g_teachDuelRec.eventCount = 0;
    g_teachDuelRec.eventCapacity = 0;

    TPrintF("teach: recording duel cid %d + %d -> %s\n", clientA, clientB, g_teachDuelRec.name);
}

static void Teach_DuelRecordStop(void) {
    if (!g_teachDuelRec.active) {
        return;
    }

    /* Write combat events to file */
    if (g_teachDuelRec.eventCount > 0) {
        char eventLine[512];
        for (int i = 0; i < g_teachDuelRec.eventCount; i++) {
            teach_combat_event_t *evt = &g_teachDuelRec.events[i];
            const char *eventName = "unknown";

            switch (evt->eventType) {
                case COMBAT_EVENT_HIT: eventName = "hit"; break;
                case COMBAT_EVENT_BLOCK: eventName = "block"; break;
                case COMBAT_EVENT_PARRY: eventName = "parry"; break;
                case COMBAT_EVENT_CLASH: eventName = "clash"; break;
                case COMBAT_EVENT_KNOCKBACK: eventName = "knockback"; break;
                case COMBAT_EVENT_FORCE_PUSH: eventName = "push"; break;
                case COMBAT_EVENT_FORCE_PULL: eventName = "pull"; break;
                case COMBAT_EVENT_FORCE_GRIP: eventName = "grip"; break;
                case COMBAT_EVENT_FORCE_LIGHTNING: eventName = "lightning"; break;
                case COMBAT_EVENT_DEATH: eventName = "death"; break;
                default: break;
            }

            Com_sprintf(eventLine, sizeof(eventLine),
                "{\"t\":%d,\"event\":\"%s\",\"p1\":%d,\"p2\":%d,\"dmg\":%d,\"kbx\":%.2f,\"kby\":%.2f,\"kbz\":%.2f,\"loc\":%d}\n",
                evt->timestamp, eventName, evt->player1, evt->player2, evt->damage,
                evt->knockback[0], evt->knockback[1], evt->knockback[2], evt->hitLocation);

            trap->FS_Write(eventLine, (int)strlen(eventLine), g_teachDuelRec.fh);
        }
        TPrintF("teach: wrote %d combat events\n", g_teachDuelRec.eventCount);
    }

    static const char *end = "# teach duel end\n";
    trap->FS_Write((void*)end, (int)strlen(end), g_teachDuelRec.fh);
    trap->FS_Close(g_teachDuelRec.fh);
    g_teachDuelRec.fh = 0;
    g_teachDuelRec.active = qfalse;

    /* Clear event array */
    g_teachDuelRec.events = NULL;
    g_teachDuelRec.eventCount = 0;
    g_teachDuelRec.eventCapacity = 0;

    TPrintF("teach: duel recording stopped (%s)\n", g_teachDuelRec.name);
}

void Teach_DuelRecordUsercmd(gentity_t *entA, const usercmd_t *ucmdA, gentity_t *entB, const usercmd_t *ucmdB) {
    if (!g_teachDuelRec.active) return;
    if (!entA || !entA->client || !entB || !entB->client) return;
    if (!g_teachDuelRec.fh) return;

    const int relTime = level.time - g_teachDuelRec.startTime;

    /* Write initial state header on first frame */
    if (!g_duelWroteInitial) {
        char header[512];
        Com_sprintf(header, sizeof(header),
            "{\"initial\":{\"originA\":[%.2f,%.2f,%.2f],\"originB\":[%.2f,%.2f,%.2f]}}\n",
            entA->client->ps.origin[0], entA->client->ps.origin[1], entA->client->ps.origin[2],
            entB->client->ps.origin[0], entB->client->ps.origin[1], entB->client->ps.origin[2]);
        trap->FS_Write(header, (int)strlen(header), g_teachDuelRec.fh);
        g_duelWroteInitial = qtrue;
    }

    /* Build actor A frame */
    teach_frame_t frA;
    frA.ms = relTime;
    frA.buttons = ucmdA->buttons;
    frA.ay = ucmdA->angles[YAW];
    frA.ap = ucmdA->angles[PITCH];
    frA.ar = ucmdA->angles[ROLL];
    frA.f = ucmdA->forwardmove;
    frA.r = ucmdA->rightmove;
    frA.u = ucmdA->upmove;
    frA.gc = ucmdA->generic_cmd;
    frA.style = entA->client->ps.fd.saberAnimLevel;
    frA.haveWorldAngles = qtrue;
    frA.wy = (short)(ucmdA->angles[YAW] + entA->client->ps.delta_angles[YAW]);
    frA.wp = (short)(ucmdA->angles[PITCH] + entA->client->ps.delta_angles[PITCH]);
    frA.wr = (short)(ucmdA->angles[ROLL] + entA->client->ps.delta_angles[ROLL]);

    if (g_teachDuelRec.pendingGenericCmdA) {
        frA.gc = g_teachDuelRec.pendingGenericCmdA;
        g_teachDuelRec.pendingGenericCmdA = 0;
    }
    if (g_teachDuelRec.pendingSaberStyleA >= 0) {
        frA.style = g_teachDuelRec.pendingSaberStyleA;
        g_teachDuelRec.pendingSaberStyleA = -1;
    }

    /* Build actor B frame */
    teach_frame_t frB;
    frB.ms = relTime;
    frB.buttons = ucmdB->buttons;
    frB.ay = ucmdB->angles[YAW];
    frB.ap = ucmdB->angles[PITCH];
    frB.ar = ucmdB->angles[ROLL];
    frB.f = ucmdB->forwardmove;
    frB.r = ucmdB->rightmove;
    frB.u = ucmdB->upmove;
    frB.gc = ucmdB->generic_cmd;
    frB.style = entB->client->ps.fd.saberAnimLevel;
    frB.haveWorldAngles = qtrue;
    frB.wy = (short)(ucmdB->angles[YAW] + entB->client->ps.delta_angles[YAW]);
    frB.wp = (short)(ucmdB->angles[PITCH] + entB->client->ps.delta_angles[PITCH]);
    frB.wr = (short)(ucmdB->angles[ROLL] + entB->client->ps.delta_angles[ROLL]);

    if (g_teachDuelRec.pendingGenericCmdB) {
        frB.gc = g_teachDuelRec.pendingGenericCmdB;
        g_teachDuelRec.pendingGenericCmdB = 0;
    }
    if (g_teachDuelRec.pendingSaberStyleB >= 0) {
        frB.style = g_teachDuelRec.pendingSaberStyleB;
        g_teachDuelRec.pendingSaberStyleB = -1;
    }

    /* Write single line with both actors */
    char line[512];
    int len = Com_sprintf(line, sizeof(line),
        "{\"t\":%d,\"A\":{\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d},\"B\":{\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d}}\n",
        relTime,
        frA.buttons, frA.ay, frA.ap, frA.ar, (int)frA.f, (int)frA.r, (int)frA.u, frA.gc, frA.style, frA.wy, frA.wp, frA.wr,
        frB.buttons, frB.ay, frB.ap, frB.ar, (int)frB.f, (int)frB.r, (int)frB.u, frB.gc, frB.style, frB.wy, frB.wp, frB.wr);
    trap->FS_Write(line, len, g_teachDuelRec.fh);
}

/* ============================================================
   Dual playback
   ============================================================ */

static teach_duel_chunk_t* T_LoadDuel(const char *name, int *outCount) {
    char fname[MAX_QPATH];
    Com_sprintf(fname, sizeof(fname), "teach__%s.duel.jsonl", name);

    fileHandle_t fh = 0;
    const int len = trap->FS_Open(fname, &fh, FS_READ);
    if (!fh || len <= 0) {
        TPrintF("teach: duel load failed: %s\n", fname);
        if (fh) trap->FS_Close(fh);
        return NULL;
    }

    /* Use chunked allocation - supports unlimited recording length */
    teach_duel_chunk_t *firstChunk = NULL;
    teach_duel_chunk_t *currentChunk = NULL;
    int totalCount = 0;
    int chunkCount = 0;
    vec3_t initialOriginA = {0, 0, 0};
    vec3_t initialOriginB = {0, 0, 0};
    qboolean foundInitial = qfalse;

    char line[1024];
    int lineNum = 0;
    while (T_ReadLine(fh, line, sizeof(line)) > 0) {
        lineNum++;
        if (line[0] == '#') continue;  // Skip comments

        /* Check for initial state header */
        if (!foundInitial && strstr(line, "\"initial\"")) {
            float ax, ay, az, bx, by, bz;
            int n = sscanf(line, " { \"initial\" : { \"originA\" : [ %f , %f , %f ] , \"originB\" : [ %f , %f , %f ] } } ",
                           &ax, &ay, &az, &bx, &by, &bz);
            if (n == 6) {
                initialOriginA[0] = ax; initialOriginA[1] = ay; initialOriginA[2] = az;
                initialOriginB[0] = bx; initialOriginB[1] = by; initialOriginB[2] = bz;
                foundInitial = qtrue;
                TPrintF("teach: loaded initial positions A=(%.1f,%.1f,%.1f) B=(%.1f,%.1f,%.1f)\n",
                        ax, ay, az, bx, by, bz);
                continue;
            }
        }

        teach_duel_frame_t df;
        memset(&df, 0, sizeof(df));
        df.A.style = -1;
        df.B.style = -1;
        df.A.haveWorldAngles = qfalse;
        df.B.haveWorldAngles = qfalse;

        int fwdA=0, rgtA=0, upA=0, fwdB=0, rgtB=0, upB=0;
        int wyA=0, wpA=0, wrA=0, wyB=0, wpB=0, wrB=0;
        int buttonsA=0, ayA=0, apA=0, arA=0, gcA=0, styleA=0;
        int buttonsB=0, ayB=0, apB=0, arB=0, gcB=0, styleB=0;

        /* Parse dual-frame JSON line - use looser format with spaces */
        int n = sscanf(line,
            " { \"t\" : %d , \"A\" : { \"buttons\" : %d , \"ay\" : %d , \"ap\" : %d , \"ar\" : %d , \"f\" : %d , \"r\" : %d , \"u\" : %d , \"gc\" : %d , \"style\" : %d , \"wy\" : %d , \"wp\" : %d , \"wr\" : %d } , \"B\" : { \"buttons\" : %d , \"ay\" : %d , \"ap\" : %d , \"ar\" : %d , \"f\" : %d , \"r\" : %d , \"u\" : %d , \"gc\" : %d , \"style\" : %d , \"wy\" : %d , \"wp\" : %d , \"wr\" : %d } } ",
            &df.t,
            &buttonsA, &ayA, &apA, &arA, &fwdA, &rgtA, &upA, &gcA, &styleA, &wyA, &wpA, &wrA,
            &buttonsB, &ayB, &apB, &arB, &fwdB, &rgtB, &upB, &gcB, &styleB, &wyB, &wpB, &wrB);

        /* Convert to struct */
        df.A.buttons = buttonsA;
        df.A.ay = ayA;
        df.A.ap = apA;
        df.A.ar = arA;
        df.A.gc = gcA;
        df.A.style = styleA;
        df.A.wy = (short)wyA;
        df.A.wp = (short)wpA;
        df.A.wr = (short)wrA;

        df.B.buttons = buttonsB;
        df.B.ay = ayB;
        df.B.ap = apB;
        df.B.ar = arB;
        df.B.gc = gcB;
        df.B.style = styleB;
        df.B.wy = (short)wyB;
        df.B.wp = (short)wpB;
        df.B.wr = (short)wrB;

        if (lineNum <= 3 && n != 25) {
            TPrintF("teach: parse fail line %d, matched %d/25 fields: %s\n", lineNum, n, line);
        }

        if (n == 25) {
            df.A.ms = df.t;
            df.B.ms = df.t;
            df.A.f = (signed char)fwdA;
            df.A.r = (signed char)rgtA;
            df.A.u = (signed char)upA;
            df.B.f = (signed char)fwdB;
            df.B.r = (signed char)rgtB;
            df.B.u = (signed char)upB;
            df.A.haveWorldAngles = qtrue;
            df.B.haveWorldAngles = qtrue;

            /* Attach initial positions to first frame */
            if (totalCount == 0 && foundInitial) {
                df.hasInitialState = qtrue;
                VectorCopy(initialOriginA, df.originA);
                VectorCopy(initialOriginB, df.originB);
            } else {
                df.hasInitialState = qfalse;
            }

            /* Allocate new chunk if needed */
            if (!currentChunk || currentChunk->count >= FRAMES_PER_CHUNK) {
                teach_duel_chunk_t *newChunk = (teach_duel_chunk_t*)G_Alloc(sizeof(teach_duel_chunk_t));
                if (!newChunk) {
                    TPrintF("teach: failed to allocate chunk after %d frames\n", totalCount);
                    break;
                }
                memset(newChunk, 0, sizeof(teach_duel_chunk_t));
                newChunk->count = 0;
                newChunk->next = NULL;

                if (!firstChunk) {
                    firstChunk = newChunk;
                    currentChunk = newChunk;
                } else {
                    currentChunk->next = newChunk;
                    currentChunk = newChunk;
                }
                chunkCount++;
            }

            /* Store frame in current chunk */
            currentChunk->frames[currentChunk->count++] = df;
            totalCount++;
        }
    }
    trap->FS_Close(fh);

    if (totalCount == 0) {
        TPrintF("teach: duel load produced 0 frames\n");
        return NULL;
    }

    *outCount = totalCount;
    TPrintF("teach: loaded %d dual frames (%d chunks, ~%d KB) from %s\n",
            totalCount, chunkCount, (chunkCount * sizeof(teach_duel_chunk_t)) / 1024, fname);
    return firstChunk;
}

static void Teach_DuelPlayStop(void) {
    if (!g_teachDuelPlay.active) {
        return;
    }

    /* Clean up duel state and collision flags */
    #define EF_TEACH_BOT EF_NOT_USED_1

    /* Restore both clients */
    for (int i = 0; i < 2; i++) {
        int cnum = (i == 0) ? g_teachDuelPlay.clientNumA : g_teachDuelPlay.clientNumB;
        if (cnum >= 0 && cnum < level.maxclients) {
            gentity_t *pe = &g_entities[cnum];
            if (pe && pe->client) {
                pe->client->buttons = 0;
                pe->client->oldbuttons = 0;
                pe->client->pers.cmd.forwardmove = 0;
                pe->client->pers.cmd.rightmove = 0;
                pe->client->pers.cmd.upmove = 0;
                pe->client->pers.cmd.buttons = 0;
                pe->client->pers.pmoveFixed = qfalse;
                pe->client->ps.pm_flags &= ~PMF_FOLLOW;

                /* Clear duel state for collision system */
                pe->client->ps.duelInProgress = qfalse;
                pe->client->ps.duelIndex = ENTITYNUM_NONE;
                pe->client->ps.duelTime = 0;

                /* Clear teach bot flag and restore collision */
                pe->s.eFlags &= ~EF_TEACH_BOT;
                pe->r.contents = CONTENTS_BODY;
            }
        }
    }

    memset(&g_teachDuelPlay, 0, sizeof(g_teachDuelPlay));
    TPrintF("teach: duel playback stopped\n");
}

static void Teach_DuelPlayStart(const char *name, int clientA, int clientB, float rate, qboolean loop) {
    if (clientA < 0 || clientA >= MAX_CLIENTS || !g_entities[clientA].client) {
        TPrintF("teach: invalid client A %d\n", clientA);
        return;
    }
    if (clientB < 0 || clientB >= MAX_CLIENTS || !g_entities[clientB].client) {
        TPrintF("teach: invalid client B %d\n", clientB);
        return;
    }
    if (clientA == clientB) {
        TPrintF("teach: clients A and B must be different\n");
        return;
    }
    if (g_entities[clientA].client->sess.sessionTeam == TEAM_SPECTATOR) {
        TPrintF("teach: client A is spectator, cannot playback\n");
        return;
    }
    if (g_entities[clientB].client->sess.sessionTeam == TEAM_SPECTATOR) {
        TPrintF("teach: client B is spectator, cannot playback\n");
        return;
    }

    teach_duel_chunk_t *chunks = NULL;
    int totalFrames = 0;

    chunks = T_LoadDuel(name, &totalFrames);
    if (!chunks || totalFrames <= 0) {
        return;
    }

    Teach_DuelPlayStop();

    memset(&g_teachDuelPlay, 0, sizeof(g_teachDuelPlay));
    g_teachDuelPlay.active = qtrue;
    g_teachDuelPlay.clientNumA = clientA;
    g_teachDuelPlay.clientNumB = clientB;
    g_teachDuelPlay.chunks = chunks;
    g_teachDuelPlay.totalFrames = totalFrames;
    g_teachDuelPlay.rate = (rate > 0.f) ? rate : 1.f;
    g_teachDuelPlay.loop = loop ? qtrue : qfalse;
    g_teachDuelPlay.startTime = level.time;
    g_teachDuelPlay.last_ms = (totalFrames > 0) ? chunks->frames[0].t : 0;
    g_teachDuelPlay.last_idx = 0;
    g_teachDuelPlay.lastCmdServerTimeA = 0;
    g_teachDuelPlay.lastCmdServerTimeB = 0;
    g_teachDuelPlay.lastStyleA = -1;  /* Initialize style tracking */
    g_teachDuelPlay.lastStyleB = -1;
    g_teachDuelPlay.cachedChunk = chunks;
    g_teachDuelPlay.cachedChunkStartIdx = 0;
    Q_strncpyz(g_teachDuelPlay.name, name, sizeof(g_teachDuelPlay.name));

    /* Prime both clients */
    for (int i = 0; i < 2; i++) {
        int cnum = (i == 0) ? clientA : clientB;
        gentity_t *pe = &g_entities[cnum];
        if (pe && pe->client) {
            pe->client->ps.commandTime = g_teachDuelPlay.startTime;
            pe->client->ps.pm_flags |= PMF_FOLLOW;

            if (totalFrames > 0 && chunks->count > 0) {
                const teach_frame_t *first = (i == 0) ? &chunks->frames[0].A : &chunks->frames[0].B;

                /* Teleport to initial position if available */
                if (first->haveState) {
                    /* Use state data from first frame (new format) */
                    VectorCopy(first->origin, pe->client->ps.origin);
                    VectorCopy(first->origin, pe->r.currentOrigin);
                    VectorCopy(first->origin, pe->s.pos.trBase);
                    pe->s.pos.trType = TR_STATIONARY;
                    pe->s.pos.trTime = 0;
                    pe->s.pos.trDuration = 0;
                    VectorClear(pe->s.pos.trDelta);
                    VectorCopy(first->velocity, pe->client->ps.velocity);

                    /* Update entity spatial information */
                    trap->LinkEntity((sharedEntity_t *)pe);

                    TPrintF("teach: teleported client %d to start position (%.1f,%.1f,%.1f)\n",
                            cnum, first->origin[0], first->origin[1], first->origin[2]);
                } else if (chunks->frames[0].hasInitialState) {
                    /* Use legacy initial state (old dual format) */
                    vec3_t *origin = (i == 0) ? &chunks->frames[0].originA : &chunks->frames[0].originB;
                    VectorCopy(*origin, pe->client->ps.origin);
                    VectorCopy(*origin, pe->r.currentOrigin);
                    VectorCopy(*origin, pe->s.pos.trBase);
                    pe->s.pos.trType = TR_STATIONARY;
                    pe->s.pos.trTime = 0;
                    pe->s.pos.trDuration = 0;
                    VectorClear(pe->s.pos.trDelta);
                    VectorClear(pe->client->ps.velocity);

                    /* Update entity spatial information */
                    trap->LinkEntity((sharedEntity_t *)pe);

                    TPrintF("teach: teleported client %d to (%.1f,%.1f,%.1f)\n",
                            cnum, (*origin)[0], (*origin)[1], (*origin)[2]);
                }

                pe->client->ps.saberMove = LS_READY;
                pe->client->ps.saberBlocked = 0;
                pe->client->ps.saberBlocking = 0;

                if (first->style >= 0) {
                    pe->client->ps.fd.saberAnimLevel = first->style;
                    pe->client->ps.fd.saberAnimLevelBase = first->style;
                    pe->client->ps.fd.saberDrawAnimLevel = first->style;
                    pe->client->sess.saberLevel = first->style;
                    /* Initialize style tracking from first frame */
                    if (i == 0) {
                        g_teachDuelPlay.lastStyleA = first->style;
                    } else {
                        g_teachDuelPlay.lastStyleB = first->style;
                    }
                }

                /* Ensure full force power at playback start */
                pe->client->ps.fd.forcePower = pe->client->ps.fd.forcePowerMax;

                pe->client->buttons = 0;
                pe->client->oldbuttons = 0;

                if (first->haveWorldAngles) {
                    pe->client->ps.viewangles[YAW] = SHORT2ANGLE((short)first->wy);
                    pe->client->ps.viewangles[PITCH] = SHORT2ANGLE((short)first->wp);
                    pe->client->ps.viewangles[ROLL] = SHORT2ANGLE((short)first->wr);
                }
            }
        }
    }

    TPrintF("teach: playing duel '%s' on cid %d + %d (%d frames, rate=%.2f, loop=%d)\n",
            g_teachDuelPlay.name, clientA, clientB, g_teachDuelPlay.totalFrames,
            g_teachDuelPlay.rate, (int)g_teachDuelPlay.loop);
}

/* Helper: Get frame by global index from chunked storage */
static const teach_duel_frame_t* GetDuelFrameByIndex(int globalIdx, teach_duel_chunk_t **outChunk, int *outChunkStartIdx) {
    if (globalIdx < 0 || globalIdx >= g_teachDuelPlay.totalFrames) {
        return NULL;
    }

    /* Try cached chunk first for performance */
    if (g_teachDuelPlay.cachedChunk && globalIdx >= g_teachDuelPlay.cachedChunkStartIdx) {
        int offsetInChunk = globalIdx - g_teachDuelPlay.cachedChunkStartIdx;
        if (offsetInChunk < g_teachDuelPlay.cachedChunk->count) {
            if (outChunk) *outChunk = g_teachDuelPlay.cachedChunk;
            if (outChunkStartIdx) *outChunkStartIdx = g_teachDuelPlay.cachedChunkStartIdx;
            return &g_teachDuelPlay.cachedChunk->frames[offsetInChunk];
        }
    }

    /* Walk chunks to find the right one */
    teach_duel_chunk_t *chunk = g_teachDuelPlay.chunks;
    int startIdx = 0;
    while (chunk) {
        if (globalIdx < startIdx + chunk->count) {
            /* Found it - cache and return */
            g_teachDuelPlay.cachedChunk = chunk;
            g_teachDuelPlay.cachedChunkStartIdx = startIdx;
            if (outChunk) *outChunk = chunk;
            if (outChunkStartIdx) *outChunkStartIdx = startIdx;
            return &chunk->frames[globalIdx - startIdx];
        }
        startIdx += chunk->count;
        chunk = chunk->next;
    }

    return NULL;  /* Should never happen */
}

void Teach_DuelFilterOrPlayUcmd(gentity_t *ent, usercmd_t *ucmd) {
    if (!g_teachDuelPlay.active) return;
    if (!ent || !ent->client) return;
    if (!g_teachDuelPlay.chunks || g_teachDuelPlay.totalFrames <= 0) return;

    qboolean isA = (ent->s.number == g_teachDuelPlay.clientNumA);
    qboolean isB = (ent->s.number == g_teachDuelPlay.clientNumB);
    if (!isA && !isB) return;

    /* COLLISION SYSTEM: Simulate duel state so bots fight each other
     * This enables:
     * - Saber-to-saber collision (parries, blocks)
     * - Saber-to-body collision (damage)
     * - Duel isolation (only interact with partner, not live players)
     */
    gentity_t *partner = isA ? &g_entities[g_teachDuelPlay.clientNumB] : &g_entities[g_teachDuelPlay.clientNumA];
    if (partner && partner->client) {
        ent->client->ps.duelInProgress = qtrue;
        ent->client->ps.duelIndex = partner->s.number;
        ent->client->ps.duelTime = level.time + 999999;  /* Keep duel active */

        /* Mark as teach-controlled bot for collision filtering
         * Using EF_NOT_USED_1 as EF_TEACH_BOT flag */
        #define EF_TEACH_BOT EF_NOT_USED_1
        ent->s.eFlags |= EF_TEACH_BOT;
    }

    const int nowms = (int)((level.time - g_teachDuelPlay.startTime) * g_teachDuelPlay.rate);

    /* Find frame with timestamp closest to nowms */
    int idx = g_teachDuelPlay.last_idx;
    if (idx < 0) idx = 0;
    if (idx >= g_teachDuelPlay.totalFrames) idx = g_teachDuelPlay.totalFrames - 1;

    const teach_duel_frame_t *df = GetDuelFrameByIndex(idx, NULL, NULL);
    if (!df) return;

    /* Linear search forward/backward for correct timestamp */
    while (idx + 1 < g_teachDuelPlay.totalFrames) {
        const teach_duel_frame_t *next = GetDuelFrameByIndex(idx + 1, NULL, NULL);
        if (!next || next->t > nowms) break;
        idx++;
        df = next;
    }
    while (idx > 0) {
        if (df->t <= nowms) break;
        idx--;
        df = GetDuelFrameByIndex(idx, NULL, NULL);
        if (!df) return;
    }

    const teach_frame_t *fr = isA ? &df->A : &df->B;

    extern vmCvar_t pmove_msec;
    int step = pmove_msec.integer;
    if (step < 8) step = 8; else if (step > 33) step = 33;

    /* Each player needs independent serverTime tracking */
    int *lastTime = isA ? &g_teachDuelPlay.lastCmdServerTimeA : &g_teachDuelPlay.lastCmdServerTimeB;
    if (*lastTime <= 0) {
        *lastTime = ent->client->ps.commandTime;
    }
    ucmd->serverTime = *lastTime + step;
    *lastTime = ucmd->serverTime;

    ent->client->pers.pmoveFixed = qtrue;

    ucmd->buttons = fr->buttons;
    ucmd->forwardmove = fr->f;
    ucmd->rightmove = fr->r;
    ucmd->upmove = fr->u;
    ucmd->generic_cmd = fr->gc;

    /* Use recorded cmd angles for Pmove delta calculation */
    ucmd->angles[YAW] = (short)fr->ay;
    ucmd->angles[PITCH] = (short)fr->ap;
    ucmd->angles[ROLL] = (short)fr->ar;

    /* Apply saber style with proper switching logic (matches single-player sync) */
    if (fr->style >= 0) {
        int *lastStyle = isA ? &g_teachDuelPlay.lastStyleA : &g_teachDuelPlay.lastStyleB;
        int prevStyle = *lastStyle;
        int targetStyle = fr->style;

        /* Ensure player has the force power level for this style */
        if (ent->client->ps.fd.forcePowerLevel[FP_SABER_OFFENSE] < targetStyle) {
            ent->client->ps.fd.forcePowerLevel[FP_SABER_OFFENSE] = targetStyle;
        }

        /* Handle style changes with proper command injection */
        if (targetStyle != prevStyle) {
            if (fr->gc != 0) {
                /* Recording had a generic command, use it */
                ucmd->generic_cmd = fr->gc;
            } else if (prevStyle >= 0) {
                /* Force a style cycle to trigger the change */
                ucmd->generic_cmd = GENCMD_SABERATTACKCYCLE;
            }
        }

        /* Apply style directly if no generic command active */
        if (ucmd->generic_cmd == 0) {
            ent->client->ps.fd.saberAnimLevelBase = targetStyle;
            ent->client->ps.fd.saberAnimLevel = targetStyle;
            ent->client->ps.fd.saberDrawAnimLevel = targetStyle;
            ent->client->sess.saberLevel = targetStyle;
            ent->client->saberCycleQueue = 0;
        }

        *lastStyle = targetStyle;
    } else {
        int *lastStyle = isA ? &g_teachDuelPlay.lastStyleA : &g_teachDuelPlay.lastStyleB;
        *lastStyle = -1;
    }

    /* Apply angles via delta_angles */
    if (fr->haveWorldAngles) {
        for (int axis = 0; axis < 3; axis++) {
            int targetShort;
            if (axis == YAW) targetShort = (short)fr->wy;
            else if (axis == PITCH) targetShort = (short)fr->wp;
            else targetShort = (short)fr->wr;

            int cmdShort = (int)(short)ucmd->angles[axis];
            int deltaShort = T_ShortDelta(targetShort, cmdShort);
            ent->client->ps.delta_angles[axis] = deltaShort;
        }
    }

    /* Maintain force power during playback */
    if (ent->client) {
        ent->client->ps.fd.forcePower = ent->client->ps.fd.forcePowerMax;
    }

    g_teachDuelPlay.last_idx = idx;
    g_teachDuelPlay.last_ms = df->t;

    if (g_teachDuelPlay.last_idx >= g_teachDuelPlay.totalFrames - 1) {
        if (g_teachDuelPlay.loop) {
            g_teachDuelPlay.last_idx = 0;
            const teach_duel_frame_t *firstFrame = GetDuelFrameByIndex(0, NULL, NULL);
            g_teachDuelPlay.last_ms = firstFrame ? firstFrame->t : 0;
            g_teachDuelPlay.startTime = level.time;
        } else {
            Teach_DuelPlayStop();
        }
    }
}

void Teach_DuelPostPmove(gentity_t *ent) {
    if (!g_teachDuelPlay.active) return;
    if (!ent || !ent->client) return;
    if (!g_teachDuelPlay.chunks || g_teachDuelPlay.totalFrames <= 0) return;

    qboolean isA = (ent->s.number == g_teachDuelPlay.clientNumA);
    qboolean isB = (ent->s.number == g_teachDuelPlay.clientNumB);
    if (!isA && !isB) return;

    int idx = g_teachDuelPlay.last_idx;
    if (idx < 0 || idx >= g_teachDuelPlay.totalFrames) return;

    const teach_duel_frame_t *df = GetDuelFrameByIndex(idx, NULL, NULL);
    if (!df) return;
    const teach_frame_t *fr = isA ? &df->A : &df->B;

    /* Apply drift correction first */
    Teach_ApplyDriftCorrection(ent, fr);

    /* Force viewangles to recorded values after Pmove */
    if (fr->haveWorldAngles) {
        static int debugCounter = 0;
        if (debugCounter++ % 40 == 0) {
            TPrintF("teach: PostPmove cid=%d yaw=%.1f pitch=%.1f\n",
                    ent->s.number,
                    SHORT2ANGLE((short)fr->wy),
                    SHORT2ANGLE((short)fr->wp));
        }
        ent->client->ps.viewangles[YAW] = SHORT2ANGLE((short)fr->wy);
        ent->client->ps.viewangles[PITCH] = SHORT2ANGLE((short)fr->wp);
        ent->client->ps.viewangles[ROLL] = SHORT2ANGLE((short)fr->wr);
    }
}

void Teach_PlayPostPmove(gentity_t *ent) {
    if (!g_teachPlay.active) return;
    if (!ent || !ent->client) return;
    if (ent->s.number != g_teachPlay.clientNum) return;
    if (!g_teachPlay.chunks || g_teachPlay.count <= 0) return;

    int idx = g_teachPlay.last_idx;
    if (idx < 0 || idx >= g_teachPlay.count) return;

    const teach_frame_t *fr = Teach_GetFrame(idx);
    if (!fr) return;

    /* Apply drift correction for normal playback */
    Teach_ApplyDriftCorrection(ent, fr);

    /* Force viewangles to recorded values (for both normal and training bot) */
    if (fr->haveWorldAngles) {
        ent->client->ps.viewangles[YAW] = SHORT2ANGLE((short)fr->wy);
        ent->client->ps.viewangles[PITCH] = SHORT2ANGLE((short)fr->wp);
        ent->client->ps.viewangles[ROLL] = SHORT2ANGLE((short)fr->wr);
    }
}

/* ============================================================
   Drift Correction System
   ============================================================ */

/* Helper: Detect semantic anchor frames (stronger correction) */
static qboolean Teach_IsAnchorFrame(gentity_t *ent, const teach_frame_t *fr) {
    static int lastGround[MAX_GENTITIES] = {0};
    static int lastSaberMove[MAX_GENTITIES] = {0};

    if (!ent || !fr || !fr->haveState) return qfalse;

    int entNum = ent->s.number;
    if (entNum < 0 || entNum >= MAX_GENTITIES) return qfalse;

    /* Ground state change (jump/land) */
    if (fr->groundEntityNum != lastGround[entNum]) {
        lastGround[entNum] = fr->groundEntityNum;
        return qtrue;
    }

    /* Saber move start */
    if (fr->saberMove != lastSaberMove[entNum] && fr->saberMove != LS_READY) {
        lastSaberMove[entNum] = fr->saberMove;
        return qtrue;
    }

    return qfalse;
}

/* Helper: Taper correction strength as we approach target */
static float Teach_TaperFactor(float drift, float threshold) {
    if (drift <= threshold) return 0.0f;

    float excess = drift - threshold;
    /* Linear taper: 1.0 at 2x threshold, tapering down to 0 at threshold */
    return Com_Clamp(0.0f, 1.0f, excess / threshold);
}

/* Helper: Check if correction path is clear (no wall clipping) */
static qboolean Teach_TraceClear(vec3_t start, vec3_t delta, int skipEnt) {
    trace_t trace;
    vec3_t end;

    VectorAdd(start, delta, end);
    trap->Trace(&trace, start, NULL, NULL, end, skipEnt, MASK_PLAYERSOLID, qfalse, 0, 0);

    return (trace.fraction >= 0.95f);  /* Allow 95% of movement */
}

/* Helper: Project correction along ground plane */
static void Teach_ProjectToGroundPlane(gentity_t *ent, vec3_t correction) {
    trace_t trace;
    vec3_t start, down;

    VectorCopy(ent->client->ps.origin, start);
    VectorSet(down, start[0], start[1], start[2] - 64.0f);

    trap->Trace(&trace, start, NULL, NULL, down, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

    if (trace.fraction < 1.0f) {
        /* Project correction along surface normal */
        float dot = DotProduct(correction, trace.plane.normal);
        VectorMA(correction, -dot, trace.plane.normal, correction);
    }
}

/* Main drift correction (runs after Pmove) */
static void Teach_ApplyDriftCorrection(gentity_t *ent, const teach_frame_t *fr) {
    if (!fr || !fr->haveState || !ent || !ent->client) return;

    /* 1. Measure drift */
    vec3_t drift;
    VectorSubtract(fr->origin, ent->client->ps.origin, drift);
    float driftXY = sqrt(drift[0]*drift[0] + drift[1]*drift[1]);
    float driftZ = fabs(drift[2]);

    /* 2. Determine thresholds (ground-aware) */
    qboolean grounded = (ent->client->ps.groundEntityNum != -1);
    float thresholdXY = grounded ? 7.0f : 5.0f;
    float thresholdZ = grounded ? 2.0f : 3.0f;

    /* 3. Check if we're at a semantic anchor */
    qboolean isAnchor = Teach_IsAnchorFrame(ent, fr);
    float correctionStrength = isAnchor ? 0.35f : 0.20f;

    /* 4. Apply XY correction if needed */
    if (driftXY > thresholdXY) {
        vec3_t correctionXY;
        VectorSet(correctionXY, drift[0], drift[1], 0);

        if (grounded) {
            Teach_ProjectToGroundPlane(ent, correctionXY);
        }

        /* Feather with tapering */
        float taper = Teach_TaperFactor(driftXY, thresholdXY);
        VectorScale(correctionXY, correctionStrength * taper, correctionXY);

        /* Trace-guard: don't clip into walls */
        if (Teach_TraceClear(ent->client->ps.origin, correctionXY, ent->s.number)) {
            VectorAdd(ent->client->ps.origin, correctionXY, ent->client->ps.origin);
        }
    }

    /* 5. Apply Z correction separately */
    if (driftZ > thresholdZ) {
        float correctionZ = drift[2] * correctionStrength;
        float taper = Teach_TaperFactor(driftZ, thresholdZ);
        correctionZ *= taper;

        /* Gentler downward when grounded */
        if (correctionZ < 0 && grounded) {
            correctionZ *= 0.5f;
        }

        ent->client->ps.origin[2] += correctionZ;
    }

    /* 6. Velocity blending (only when we corrected position) */
    if (driftXY > thresholdXY || driftZ > thresholdZ) {
        vec3_t velBlend;
        VectorSubtract(fr->velocity, ent->client->ps.velocity, velBlend);
        VectorScale(velBlend, 0.25f, velBlend);  /* 25% blend - was 15% */
        VectorAdd(ent->client->ps.velocity, velBlend, ent->client->ps.velocity);
    }

    /* 7. Force animation state to prevent choppy playback (especially for bots) */
    if (fr->haveState) {
        ent->client->ps.saberMove = fr->saberMove;
        ent->client->ps.torsoAnim = fr->torsoAnim;
        ent->client->ps.legsAnim = fr->legsAnim;
        ent->client->ps.torsoTimer = fr->torsoTimer;
        ent->client->ps.legsTimer = fr->legsTimer;
        ent->client->ps.weaponTime = fr->weaponTime;
        /* Note: dualSabers is determined by saber[1].model, not a ps field */
        ent->client->ps.saberHolstered = fr->saberHolstered;
    }

    /* 8. Sync entity positions */
    VectorCopy(ent->client->ps.origin, ent->r.currentOrigin);
    VectorCopy(ent->client->ps.origin, ent->s.pos.trBase);
}

/* ============================================================
   Playback helpers
   ============================================================ */

static void Teach_PlayStop(void);

static teach_chunk_t* T_Load(const char *name, int *outCount) {
    char fname[MAX_QPATH];
    Com_sprintf(fname, sizeof(fname), "teach__%s.teach.jsonl", name);

    fileHandle_t fh = 0;
    const int len = trap->FS_Open(fname, &fh, FS_READ);
    if (!fh || len <= 0) {
        TPrintF("teach: play load failed: %s\n", fname);
        if (fh) trap->FS_Close(fh);
        return NULL;
    }

    /* Use chunked allocation to support unlimited recording length */
    teach_chunk_t *firstChunk = NULL;
    teach_chunk_t *currentChunk = NULL;
    int totalCount = 0;

    char line[512];
    while (T_ReadLine(fh, line, sizeof(line)) > 0) {
        teach_frame_t fr;
        int parsed = 0;
        int fwd = 0, rgt = 0, up = 0;

        memset(&fr, 0, sizeof(fr));
        fr.style = -1;
        fr.gc = 0;
        fr.haveWorldAngles = qfalse;
        fr.haveState = qfalse;

        /* Try parsing with dual saber state (newest format) */
        if (!parsed) {
            float ox=0, oy=0, oz=0, vx=0, vy=0, vz=0;
            int ground=0, pmf=0, pmt=0, sm=0, ta=0, la=0, tt=0, lt=0, wt=0, ds=0, sh=0;
            int hp=0, maxhp=0, fp=0, maxfp=0, sblk=0, sblking=0;
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d,"
                "\"ox\":%f,\"oy\":%f,\"oz\":%f,\"vx\":%f,\"vy\":%f,\"vz\":%f,\"ground\":%d,\"pmf\":%d,\"pmt\":%d,\"sm\":%d,"
                "\"ta\":%d,\"la\":%d,\"tt\":%d,\"lt\":%d,\"wt\":%d,\"ds\":%d,\"sh\":%d,"
                "\"hp\":%d,\"maxhp\":%d,\"fp\":%d,\"maxfp\":%d,\"sblk\":%d,\"sblking\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up, &fr.gc, &fr.style, &fr.wy, &fr.wp, &fr.wr,
                &ox, &oy, &oz, &vx, &vy, &vz, &ground, &pmf, &pmt, &sm,
                &ta, &la, &tt, &lt, &wt, &ds, &sh,
                &hp, &maxhp, &fp, &maxfp, &sblk, &sblking);
            if (n == 36) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                fr.haveWorldAngles = qtrue;
                fr.haveState = qtrue;
                fr.origin[0] = ox;
                fr.origin[1] = oy;
                fr.origin[2] = oz;
                fr.velocity[0] = vx;
                fr.velocity[1] = vy;
                fr.velocity[2] = vz;
                fr.groundEntityNum = ground;
                fr.pm_flags = pmf;
                fr.pm_time = pmt;
                fr.saberMove = sm;
                fr.torsoAnim = ta;
                fr.legsAnim = la;
                fr.torsoTimer = tt;
                fr.legsTimer = lt;
                fr.weaponTime = wt;
                fr.dualSabers = ds;
                fr.saberHolstered = sh;
                fr.health = hp;
                fr.maxHealth = maxhp;
                fr.forcePower = fp;
                fr.forcePowerMax = maxfp;
                fr.saberBlocked = sblk;
                fr.saberBlocking = sblking;
                parsed = 1;
            }
        }

        /* Fallback: Try parsing without dual saber state (old format with torsoAnim/legsAnim) */
        if (!parsed) {
            float ox=0, oy=0, oz=0, vx=0, vy=0, vz=0;
            int ground=0, pmf=0, pmt=0, sm=0, ta=0, la=0, tt=0, lt=0, wt=0;
            int hp=0, maxhp=0, fp=0, maxfp=0, sblk=0, sblking=0;
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d,"
                "\"ox\":%f,\"oy\":%f,\"oz\":%f,\"vx\":%f,\"vy\":%f,\"vz\":%f,\"ground\":%d,\"pmf\":%d,\"pmt\":%d,\"sm\":%d,"
                "\"ta\":%d,\"la\":%d,\"tt\":%d,\"lt\":%d,\"wt\":%d,"
                "\"hp\":%d,\"maxhp\":%d,\"fp\":%d,\"maxfp\":%d,\"sblk\":%d,\"sblking\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up, &fr.gc, &fr.style, &fr.wy, &fr.wp, &fr.wr,
                &ox, &oy, &oz, &vx, &vy, &vz, &ground, &pmf, &pmt, &sm,
                &ta, &la, &tt, &lt, &wt,
                &hp, &maxhp, &fp, &maxfp, &sblk, &sblking);
            if (n == 34) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                fr.haveWorldAngles = qtrue;
                fr.haveState = qtrue;
                fr.origin[0] = ox;
                fr.origin[1] = oy;
                fr.origin[2] = oz;
                fr.velocity[0] = vx;
                fr.velocity[1] = vy;
                fr.velocity[2] = vz;
                fr.groundEntityNum = ground;
                fr.pm_flags = pmf;
                fr.pm_time = pmt;
                fr.saberMove = sm;
                fr.torsoAnim = ta;
                fr.legsAnim = la;
                fr.torsoTimer = tt;
                fr.legsTimer = lt;
                fr.weaponTime = wt;
                fr.dualSabers = 0;  /* Default for old recordings */
                fr.saberHolstered = 0;
                fr.health = hp;
                fr.maxHealth = maxhp;
                fr.forcePower = fp;
                fr.forcePowerMax = maxfp;
                fr.saberBlocked = sblk;
                fr.saberBlocking = sblking;
                parsed = 1;
            }
        }

        /* Fallback: Try parsing with animation timers but no anim indices (old format) */
        if (!parsed) {
            float ox=0, oy=0, oz=0, vx=0, vy=0, vz=0;
            int ground=0, pmf=0, pmt=0, sm=0, tt=0, lt=0, wt=0;
            int hp=0, maxhp=0, fp=0, maxfp=0, sblk=0, sblking=0;
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d,"
                "\"ox\":%f,\"oy\":%f,\"oz\":%f,\"vx\":%f,\"vy\":%f,\"vz\":%f,\"ground\":%d,\"pmf\":%d,\"pmt\":%d,\"sm\":%d,"
                "\"tt\":%d,\"lt\":%d,\"wt\":%d,"
                "\"hp\":%d,\"maxhp\":%d,\"fp\":%d,\"maxfp\":%d,\"sblk\":%d,\"sblking\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up, &fr.gc, &fr.style, &fr.wy, &fr.wp, &fr.wr,
                &ox, &oy, &oz, &vx, &vy, &vz, &ground, &pmf, &pmt, &sm,
                &tt, &lt, &wt,
                &hp, &maxhp, &fp, &maxfp, &sblk, &sblking);
            if (n == 32) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                fr.haveWorldAngles = qtrue;
                fr.haveState = qtrue;
                fr.origin[0] = ox;
                fr.origin[1] = oy;
                fr.origin[2] = oz;
                fr.velocity[0] = vx;
                fr.velocity[1] = vy;
                fr.velocity[2] = vz;
                fr.groundEntityNum = ground;
                fr.pm_flags = pmf;
                fr.pm_time = pmt;
                fr.saberMove = sm;
                fr.torsoAnim = 0;  /* Default for old recordings */
                fr.legsAnim = 0;
                fr.torsoTimer = tt;
                fr.legsTimer = lt;
                fr.weaponTime = wt;
                fr.health = hp;
                fr.maxHealth = maxhp;
                fr.forcePower = fp;
                fr.forcePowerMax = maxfp;
                fr.saberBlocked = sblk;
                fr.saberBlocking = sblking;
                parsed = 1;
            }
        }

        /* Fallback: Try parsing with combat state but no animation timers (old format) */
        if (!parsed) {
            float ox=0, oy=0, oz=0, vx=0, vy=0, vz=0;
            int ground=0, pmf=0, pmt=0, sm=0;
            int hp=0, maxhp=0, fp=0, maxfp=0, sblk=0, sblking=0;
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d,"
                "\"ox\":%f,\"oy\":%f,\"oz\":%f,\"vx\":%f,\"vy\":%f,\"vz\":%f,\"ground\":%d,\"pmf\":%d,\"pmt\":%d,\"sm\":%d,"
                "\"hp\":%d,\"maxhp\":%d,\"fp\":%d,\"maxfp\":%d,\"sblk\":%d,\"sblking\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up, &fr.gc, &fr.style, &fr.wy, &fr.wp, &fr.wr,
                &ox, &oy, &oz, &vx, &vy, &vz, &ground, &pmf, &pmt, &sm,
                &hp, &maxhp, &fp, &maxfp, &sblk, &sblking);
            if (n == 29) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                fr.haveWorldAngles = qtrue;
                fr.haveState = qtrue;
                fr.origin[0] = ox;
                fr.origin[1] = oy;
                fr.origin[2] = oz;
                fr.velocity[0] = vx;
                fr.velocity[1] = vy;
                fr.velocity[2] = vz;
                fr.groundEntityNum = ground;
                fr.pm_flags = pmf;
                fr.pm_time = pmt;
                fr.saberMove = sm;
                fr.torsoTimer = 0;  /* Default for old recordings */
                fr.legsTimer = 0;
                fr.weaponTime = 0;
                fr.health = hp;
                fr.maxHealth = maxhp;
                fr.forcePower = fp;
                fr.forcePowerMax = maxfp;
                fr.saberBlocked = sblk;
                fr.saberBlocking = sblking;
                parsed = 1;
            }
        }

        /* Fallback: Try parsing with state data only (old format without combat) */
        if (!parsed) {
            float ox=0, oy=0, oz=0, vx=0, vy=0, vz=0;
            int ground=0, pmf=0, pmt=0, sm=0;
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d,"
                "\"ox\":%f,\"oy\":%f,\"oz\":%f,\"vx\":%f,\"vy\":%f,\"vz\":%f,\"ground\":%d,\"pmf\":%d,\"pmt\":%d,\"sm\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up, &fr.gc, &fr.style, &fr.wy, &fr.wp, &fr.wr,
                &ox, &oy, &oz, &vx, &vy, &vz, &ground, &pmf, &pmt, &sm);
            if (n == 23) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                fr.haveWorldAngles = qtrue;
                fr.haveState = qtrue;
                fr.origin[0] = ox;
                fr.origin[1] = oy;
                fr.origin[2] = oz;
                fr.velocity[0] = vx;
                fr.velocity[1] = vy;
                fr.velocity[2] = vz;
                fr.groundEntityNum = ground;
                fr.pm_flags = pmf;
                fr.pm_time = pmt;
                fr.saberMove = sm;
                /* Set defaults for missing combat state */
                fr.health = 100;
                fr.maxHealth = 100;
                fr.forcePower = 100;
                fr.forcePowerMax = 100;
                fr.saberBlocked = 0;
                fr.saberBlocking = 0;
                parsed = 1;
            }
        }

        /* Fallback: try parsing without state (old format compatibility) */
        if (!parsed) {
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d,\"wy\":%d,\"wp\":%d,\"wr\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up, &fr.gc, &fr.style, &fr.wy, &fr.wp, &fr.wr);
            if (n == 13) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                fr.haveWorldAngles = qtrue;
                parsed = 1;
            }
        }

        if (!parsed) {
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d,\"gc\":%d,\"style\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up, &fr.gc, &fr.style);
            if (n == 10) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                parsed = 1;
            }
        }

        if (!parsed) {
            int n = sscanf(line,
                "{\"ms\":%d,\"buttons\":%d,\"ay\":%d,\"ap\":%d,\"ar\":%d,\"f\":%d,\"r\":%d,\"u\":%d}",
                &fr.ms, &fr.buttons, &fr.ay, &fr.ap, &fr.ar,
                &fwd, &rgt, &up);
            if (n == 8) {
                fr.f = (signed char)fwd;
                fr.r = (signed char)rgt;
                fr.u = (signed char)up;
                parsed = 1;
            }
        }

        if (parsed) {
            /* Allocate new chunk if needed */
            if (!currentChunk || currentChunk->count >= FRAMES_PER_CHUNK) {
                teach_chunk_t *newChunk = (teach_chunk_t*)G_Alloc(sizeof(teach_chunk_t));
                if (!newChunk) {
                    TPrintF("teach: failed to allocate chunk (total frames so far: %d)\n", totalCount);
                    trap->FS_Close(fh);
                    return firstChunk;  /* Return what we have so far */
                }
                memset(newChunk, 0, sizeof(teach_chunk_t));
                newChunk->count = 0;
                newChunk->next = NULL;

                if (!firstChunk) {
                    firstChunk = newChunk;
                } else {
                    currentChunk->next = newChunk;
                }
                currentChunk = newChunk;
            }

            /* Store frame in current chunk */
            currentChunk->frames[currentChunk->count++] = fr;
            totalCount++;
        }
    }
    trap->FS_Close(fh);

    if (totalCount == 0) {
        TPrintF("teach: play load produced 0 frames\n");
        return NULL;
    }

    TPrintF("teach: loaded %d frames in %d chunks\n", totalCount,
            (totalCount + FRAMES_PER_CHUNK - 1) / FRAMES_PER_CHUNK);

    *outCount = totalCount;
    return firstChunk;
}

static void Teach_PlayStop(void) {
    if (!g_teachPlay.active) {
        return;
    }

    if (g_teachPlay.clientNum >= 0 && g_teachPlay.clientNum < MAX_GENTITIES) {
        gentity_t *pe = &g_entities[g_teachPlay.clientNum];
        if (pe && pe->client) {
            pe->client->buttons = 0;
            pe->client->oldbuttons = 0;
            pe->client->pers.cmd.forwardmove = 0;
            pe->client->pers.cmd.rightmove   = 0;
            pe->client->pers.cmd.upmove      = 0;
            pe->client->pers.cmd.buttons     = 0;
            pe->client->pers.pmoveFixed = qfalse;
            pe->client->ps.pm_flags &= ~PMF_FOLLOW;
        }
    }

    memset(&g_teachPlay, 0, sizeof(g_teachPlay));
    TPrintF("teach: playback stopped\n");
}

static void Teach_PlayStart(const char *name, int clientNum, float rate, qboolean loop) {
    if (clientNum < 0 || clientNum >= MAX_GENTITIES || !g_entities[clientNum].client) {
        TPrintF("teach: invalid target entity %d (no client)\n", clientNum);
        return;
    }
    if (clientNum < MAX_CLIENTS && g_entities[clientNum].client->sess.sessionTeam == TEAM_SPECTATOR) {
        TPrintF("teach: client is spectator, cannot playback\n");
        return;
    }

    teach_chunk_t *chunks = NULL;
    int count = 0;

    chunks = T_Load(name, &count);
    if (!chunks || count <= 0) {
        return;
    }

    Teach_PlayStop();

    memset(&g_teachPlay, 0, sizeof(g_teachPlay));
    g_teachPlay.haveAngleBase = qfalse;
    g_teachPlay.haveViewAngles = qfalse;
    g_teachPlay.haveCmdAngles = qfalse;
    g_teachPlay.inForcedSetView = qfalse;
    g_teachPlay.active    = qtrue;
    g_teachPlay.clientNum = clientNum;
    g_teachPlay.chunks    = chunks;
    g_teachPlay.count     = count;
    g_teachPlay.rate      = (rate > 0.f) ? rate : 1.f;
    g_teachPlay.loop      = loop ? qtrue : qfalse;
    g_teachPlay.startTime = level.time;
    g_teachPlay.last_ms   = (count > 0 && chunks) ? chunks->frames[0].ms : 0;
    g_teachPlay.last_idx  = 0;
    g_teachPlay.lastStyle = -1;
    g_teachPlay.lastCmdServerTime = 0;
    g_teachPlay.targetPlayerNum = -1;  /* Disabled by default */
    VectorClear(g_teachPlay.trainingOffset);
    Q_strncpyz(g_teachPlay.name, name, sizeof(g_teachPlay.name));

    // initialize the target entity's commandTime to our playback start for clean deltas
    if (g_teachPlay.clientNum >= 0 && g_teachPlay.clientNum < MAX_GENTITIES) {
        gentity_t *pe = &g_entities[g_teachPlay.clientNum];
        if (pe && pe->client) {
            pe->client->ps.commandTime = g_teachPlay.startTime;
            pe->client->ps.pm_flags |= PMF_FOLLOW;

            /* Prime initial state from first frame to avoid cold-start issues */
            if (count > 0 && chunks) {
                const teach_frame_t *first = &chunks->frames[0];

                /* Special bot initialization to prevent AI interference */
                if (pe->r.svFlags & SVF_BOT) {
                    /* Clear any bot movement state */
                    pe->client->ps.pm_type = PM_NORMAL;
                    pe->client->ps.pm_flags &= ~PMF_DUCKED; /* Clear crouch */
                    pe->client->ps.pm_flags &= ~PMF_JUMP_HELD;
                    pe->client->ps.eFlags &= ~EF_JETPACK_ACTIVE;
                    /* Clear any special states */
                    pe->client->ps.forceHandExtend = HANDEXTEND_NONE;
                    pe->client->ps.forceHandExtendTime = 0;

                    /* Initialize animation state from first frame if available */
                    if (first->haveState) {
                        pe->client->ps.saberMove = first->saberMove;
                        pe->client->ps.torsoAnim = first->torsoAnim;
                        pe->client->ps.legsAnim = first->legsAnim;
                        pe->client->ps.torsoTimer = first->torsoTimer;
                        pe->client->ps.legsTimer = first->legsTimer;
                        pe->client->ps.weaponTime = first->weaponTime;
                        /* Note: dualSabers is determined by saber[1].model, not a ps field */
                        pe->client->ps.saberHolstered = first->saberHolstered;
                    } else {
                        /* Fallback: clear timers if no state data */
                        pe->client->ps.torsoTimer = 0;
                        pe->client->ps.legsTimer = 0;
                        pe->client->ps.weaponTime = 0;
                    }
                }

                /* Teleport to exact starting position if state data available */
                if (first->haveState) {
                    TPrintF("teach: DEBUG - first frame haveState=%d, origin=(%.2f,%.2f,%.2f)\n",
                            (int)first->haveState, first->origin[0], first->origin[1], first->origin[2]);
                    TPrintF("teach: DEBUG - player current position before teleport=(%.2f,%.2f,%.2f)\n",
                            pe->client->ps.origin[0], pe->client->ps.origin[1], pe->client->ps.origin[2]);

                    VectorCopy(first->origin, pe->client->ps.origin);
                    VectorCopy(first->origin, pe->r.currentOrigin);
                    VectorCopy(first->origin, pe->s.pos.trBase);
                    pe->s.pos.trType = TR_STATIONARY;
                    pe->s.pos.trTime = 0;
                    pe->s.pos.trDuration = 0;
                    VectorClear(pe->s.pos.trDelta);
                    VectorCopy(first->velocity, pe->client->ps.velocity);

                    /* Update entity spatial information */
                    trap->LinkEntity((sharedEntity_t *)pe);

                    TPrintF("teach: teleported to start position (%.1f,%.1f,%.1f)\n",
                            first->origin[0], first->origin[1], first->origin[2]);
                    TPrintF("teach: DEBUG - player position after teleport=(%.2f,%.2f,%.2f)\n",
                            pe->client->ps.origin[0], pe->client->ps.origin[1], pe->client->ps.origin[2]);
                } else {
                    TPrintF("teach: WARNING - first frame has no state data, cannot teleport to start position\n");
                }

                /* Clear any active animations/moves */
                pe->client->ps.saberMove = LS_READY;
                pe->client->ps.saberBlocked = 0;
                pe->client->ps.saberBlocking = 0;

                /* Set initial saber style if recorded */
                if (first->style >= 0) {
                    pe->client->ps.fd.saberAnimLevel = first->style;
                    pe->client->ps.fd.saberAnimLevelBase = first->style;
                    pe->client->ps.fd.saberDrawAnimLevel = first->style;
                    pe->client->sess.saberLevel = first->style;
                }

                /* Ensure full force power at playback start */
                pe->client->ps.fd.forcePower = pe->client->ps.fd.forcePowerMax;

                /* Clear button states */
                pe->client->buttons = 0;
                pe->client->oldbuttons = 0;

                /* Set initial view angles if available */
                if (first->haveWorldAngles) {
                    pe->client->ps.viewangles[YAW] = SHORT2ANGLE((short)first->wy);
                    pe->client->ps.viewangles[PITCH] = SHORT2ANGLE((short)first->wp);
                    pe->client->ps.viewangles[ROLL] = SHORT2ANGLE((short)first->wr);
                }
            }
        }
    }

    TPrintF("teach: playing '%s' on cid %d (%d frames, rate=%.2f, loop=%d)\n",
            g_teachPlay.name, g_teachPlay.clientNum, g_teachPlay.count,
            g_teachPlay.rate, (int)g_teachPlay.loop);
}

/* Helper: Get frame by index from chunked storage */
static const teach_frame_t* Teach_GetFrame(int idx) {
    if (idx < 0 || idx >= g_teachPlay.count || !g_teachPlay.chunks) {
        return NULL;
    }

    int chunkIndex = idx / FRAMES_PER_CHUNK;
    int frameIndex = idx % FRAMES_PER_CHUNK;

    teach_chunk_t *chunk = g_teachPlay.chunks;
    for (int i = 0; i < chunkIndex && chunk; i++) {
        chunk = chunk->next;
    }

    if (!chunk || frameIndex >= chunk->count) {
        return NULL;
    }

    return &chunk->frames[frameIndex];
}

void Teach_FilterOrPlayUcmd(gentity_t *ent, usercmd_t *ucmd) {
    if (!g_teachPlay.active) return;
    if (!ent || !ent->client) return;
    if (ent->s.number != g_teachPlay.clientNum) return;
    if (!g_teachPlay.chunks || g_teachPlay.count <= 0) return;

    const int nowms = (int)((level.time - g_teachPlay.startTime) * g_teachPlay.rate);

    int idx = g_teachPlay.last_idx;
    if (idx < 0) {
        idx = 0;
    }

    const teach_frame_t *nextFr;
    while (idx + 1 < g_teachPlay.count && (nextFr = Teach_GetFrame(idx + 1)) && nextFr->ms <= nowms) {
        idx++;
    }
    const teach_frame_t *currFr;
    while (idx > 0 && (currFr = Teach_GetFrame(idx)) && currFr->ms > nowms) {
        idx--;
    }

    const teach_frame_t *fr = Teach_GetFrame(idx);
    if (!fr) return;
    vec3_t recordedAngles;
    vec3_t targetAngles;
    qboolean haveTargetAngles = qfalse;
    int axis;
    float delta;

    VectorClear(recordedAngles);
    VectorClear(targetAngles);

    recordedAngles[YAW]   = SHORT2ANGLE((short)fr->ay);
    recordedAngles[PITCH] = SHORT2ANGLE((short)fr->ap);
    recordedAngles[ROLL]  = SHORT2ANGLE((short)fr->ar);

    /* Debug logging (controlled by g_teachDebug cvar) */
    static int lastDebugTime = 0;
    if (level.time - lastDebugTime > 250) {  // Every 250ms
        TPrintF("teach: idx=%d/%d ms=%d buttons=0x%x f=%d r=%d u=%d gc=%d style=%d\n",
                idx, g_teachPlay.count, nowms, fr->buttons, fr->f, fr->r, fr->u, fr->gc, fr->style);
        if (fr->haveWorldAngles) {
            TPrintF("  recorded angles: y=%.1f p=%.1f | actual: y=%.1f p=%.1f\n",
                    SHORT2ANGLE((short)fr->wy), SHORT2ANGLE((short)fr->wp),
                    ent->client->ps.viewangles[YAW], ent->client->ps.viewangles[PITCH]);
        }
        lastDebugTime = level.time;
    }

    extern vmCvar_t pmove_msec;
    int step = pmove_msec.integer;
    if (step < 8) step = 8; else if (step > 33) step = 33;
    if (g_teachPlay.lastCmdServerTime <= 0) {
        g_teachPlay.lastCmdServerTime = ent->client->ps.commandTime;
    }
    ucmd->serverTime = g_teachPlay.lastCmdServerTime + step;
    g_teachPlay.lastCmdServerTime = ucmd->serverTime;

    ent->client->pers.pmoveFixed = qtrue;

    ucmd->buttons       = fr->buttons;
    ucmd->generic_cmd   = fr->gc;
    ucmd->upmove        = fr->u;

    /* Training bot mode: rotate movement to offset direction, keep original angles */
    if (g_teachPlay.targetPlayerNum >= 0 && g_teachPlay.targetPlayerNum < MAX_CLIENTS) {
        /* Use the stored yaw offset to rotate movement inputs only */
        float yawDelta = g_teachPlay.trainingOffset[0];
        float radDelta = yawDelta * M_PI / 180.0f;
        float cosAngle = cos(radDelta);
        float sinAngle = sin(radDelta);
        signed char newForward = (signed char)(fr->f * cosAngle - fr->r * sinAngle);
        signed char newRight = (signed char)(fr->f * sinAngle + fr->r * cosAngle);

        ucmd->forwardmove = newForward;
        ucmd->rightmove = newRight;

        /* Keep original recorded angles for exact combo reproduction */
        ucmd->angles[YAW]   = (short)fr->ay;
        ucmd->angles[PITCH] = (short)fr->ap;
        ucmd->angles[ROLL]  = (short)fr->ar;
    } else {
        /* Normal playback: use recorded values directly */
        ucmd->angles[YAW]   = (short)fr->ay;
        ucmd->angles[PITCH] = (short)fr->ap;
        ucmd->angles[ROLL]  = (short)fr->ar;
        ucmd->forwardmove   = fr->f;
        ucmd->rightmove     = fr->r;
    }

    g_teachPlay.haveCmdAngles = qtrue;
    g_teachPlay.lastCmdAngles[YAW]   = (int)(short)ucmd->angles[YAW];
    g_teachPlay.lastCmdAngles[PITCH] = (int)(short)ucmd->angles[PITCH];
    g_teachPlay.lastCmdAngles[ROLL]  = (int)(short)ucmd->angles[ROLL];

    if (fr->style >= 0) {
        int prevStyle = g_teachPlay.lastStyle;
        int targetStyle = fr->style;

        if (ent->client->ps.fd.forcePowerLevel[FP_SABER_OFFENSE] < targetStyle) {
            ent->client->ps.fd.forcePowerLevel[FP_SABER_OFFENSE] = targetStyle;
        }

        if (targetStyle != prevStyle) {
            if (fr->gc != 0) {
                ucmd->generic_cmd = fr->gc;
            } else if (prevStyle >= 0) {
                ucmd->generic_cmd = GENCMD_SABERATTACKCYCLE;
            }
        }

        if (ucmd->generic_cmd == 0) {
            ent->client->ps.fd.saberAnimLevelBase = targetStyle;
            ent->client->ps.fd.saberAnimLevel     = targetStyle;
            ent->client->ps.fd.saberDrawAnimLevel = targetStyle;
            ent->client->sess.saberLevel          = targetStyle;
            ent->client->saberCycleQueue          = 0;
        }

        g_teachPlay.lastStyle = targetStyle;
    } else {
        g_teachPlay.lastStyle = -1;
    }

    if (fr->haveWorldAngles) {
        targetAngles[YAW]   = SHORT2ANGLE((short)fr->wy);
        targetAngles[PITCH] = SHORT2ANGLE((short)fr->wp);
        targetAngles[ROLL]  = SHORT2ANGLE((short)fr->wr);
        haveTargetAngles = qtrue;

        if (!g_teachPlay.haveAngleBase) {
            VectorCopy(recordedAngles, g_teachPlay.baseRecordedAngles);
            VectorCopy(targetAngles, g_teachPlay.baseWorldAngles);
            g_teachPlay.haveAngleBase = qtrue;
        }
    } else {
        if (!g_teachPlay.haveAngleBase) {
            VectorCopy(recordedAngles, g_teachPlay.baseRecordedAngles);
            VectorCopy(ent->client->ps.viewangles, g_teachPlay.baseWorldAngles);
            g_teachPlay.haveAngleBase = qtrue;
        }
        if (g_teachPlay.haveAngleBase) {
            for (axis = 0; axis < 3; axis++) {
                delta = AngleNormalize180(recordedAngles[axis] - g_teachPlay.baseRecordedAngles[axis]);
                targetAngles[axis] = AngleNormalize180(g_teachPlay.baseWorldAngles[axis] + delta);
            }
            haveTargetAngles = qtrue;
        }
    }

    if (haveTargetAngles) {
        VectorCopy(targetAngles, g_teachPlay.lastViewAngles);
        g_teachPlay.haveViewAngles = qtrue;

        for (axis = 0; axis < 3; axis++) {
            int targetShort;
            if (fr->haveWorldAngles) {
                /* Use the raw SHORT values directly - no conversion! */
                if (axis == YAW) targetShort = (short)fr->wy;
                else if (axis == PITCH) targetShort = (short)fr->wp;
                else targetShort = (short)fr->wr;
            } else {
                /* Fallback for old recordings without world angles */
                targetShort = ANGLE2SHORT(targetAngles[axis]);
            }
            int cmdShort = (int)(short)ucmd->angles[axis];
            int deltaShort = T_ShortDelta(targetShort, cmdShort);
            ent->client->ps.delta_angles[axis] = deltaShort;
        }
    } else {
        g_teachPlay.haveViewAngles = qfalse;
    }

    /* Maintain force power during playback */
    if (ent->client) {
        ent->client->ps.fd.forcePower = ent->client->ps.fd.forcePowerMax;
    }

    g_teachPlay.last_idx = idx;
    g_teachPlay.last_ms  = fr->ms;

    if (g_teachPlay.last_idx >= g_teachPlay.count - 1) {
        if (g_teachPlay.loop) {
            g_teachPlay.last_idx  = 0;
            const teach_frame_t *firstFrame = Teach_GetFrame(0);
            g_teachPlay.last_ms   = firstFrame ? firstFrame->ms : 0;
            g_teachPlay.startTime = level.time;
            g_teachPlay.haveAngleBase = qfalse;
            g_teachPlay.haveViewAngles = qfalse;
            g_teachPlay.haveCmdAngles = qfalse;

            /* Training bot: reposition near target when loop restarts */
            if (g_teachPlay.targetPlayerNum >= 0 && g_teachPlay.targetPlayerNum < MAX_CLIENTS) {
                gentity_t *target = &g_entities[g_teachPlay.targetPlayerNum];
                gentity_t *bot = &g_entities[g_teachPlay.clientNum];
                if (target && target->client && target->client->pers.connected == CON_CONNECTED &&
                    bot && bot->client) {
                    vec3_t targetPos, botPos, forward, dir;
                    vec3_t targetAngles;
                    float distance, targetYaw, firstFrameYaw;

                    /* Get target position and facing */
                    VectorCopy(target->client->ps.origin, targetPos);
                    VectorCopy(bot->client->ps.origin, botPos);

                    /* Calculate yaw offset: where target is facing vs. where recording started */
                    VectorSubtract(targetPos, botPos, dir);
                    targetYaw = atan2(dir[1], dir[0]) * 180.0f / M_PI;

                    /* Get first frame's recorded yaw to calculate offset */
                    if (g_teachPlay.count > 0) {
                        const teach_frame_t *firstFrame = Teach_GetFrame(0);
                        if (firstFrame) {
                            firstFrameYaw = SHORT2ANGLE((short)firstFrame->ay);
                            g_teachPlay.trainingOffset[0] = AngleNormalize180(targetYaw - firstFrameYaw);
                        }
                    }

                    /* Check distance to target */
                    VectorSubtract(targetPos, botPos, dir);
                    distance = VectorLength(dir);

                    /* If too far (>300 units), teleport closer */
                    if (distance > 300.0f) {
                        VectorCopy(target->client->ps.viewangles, targetAngles);
                        targetAngles[PITCH] = 0;
                        AngleVectors(targetAngles, forward, NULL, NULL);

                        /* Position bot 150 units in front of target */
                        VectorMA(targetPos, 150.0f, forward, botPos);
                        botPos[2] = targetPos[2];

                        VectorCopy(botPos, bot->client->ps.origin);
                        VectorCopy(botPos, bot->r.currentOrigin);
                        VectorCopy(botPos, bot->s.pos.trBase);
                        VectorClear(bot->client->ps.velocity);
                        trap->LinkEntity((sharedEntity_t *)bot);

                        TPrintF("teach: training bot repositioned (dist was %.0f, yaw offset %.1f)\n",
                                distance, g_teachPlay.trainingOffset[0]);
                    }
                }
            }
        } else {
            Teach_PlayStop();
        }
    }
}

qboolean Teach_IsForcingViewFor(const gentity_t *ent) {
    if (!Teach_IsPlayingFor(ent)) {
        return qfalse;
    }
    return g_teachPlay.haveViewAngles;
}

qboolean Teach_HaveCurrentAnglesFor(const gentity_t *ent, vec3_t outAngles) {
    if (!Teach_IsForcingViewFor(ent)) {
        return qfalse;
    }
    if (outAngles) {
        VectorCopy(g_teachPlay.lastViewAngles, outAngles);
    }
    return qtrue;
}

qboolean Teach_GetTargetViewAngles(const gentity_t *ent, vec3_t outAngles) {
    return Teach_HaveCurrentAnglesFor(ent, outAngles);
}

void Teach_ApplyForcedView(gentity_t *ent) {
    if (!Teach_IsForcingViewFor(ent)) {
        return;
    }
    if (!ent || !ent->client) {
        return;
    }

    g_teachPlay.inForcedSetView = qtrue;

    /* NOTE: Do NOT write ps.viewangles directly here!
       The angles are already set via ps.delta_angles in Teach_FilterOrPlayUcmd
       before Pmove(), and Pmove computes ps.viewangles from delta_angles.
       Writing viewangles here (after Pmove) would overwrite the correct values
       and cause camera drift/stuck issues. */

    /* Update entity angles for rendering (non-authoritative) */
    VectorCopy(g_teachPlay.lastViewAngles, ent->s.angles);

    g_teachPlay.inForcedSetView = qfalse;
}

/* ============================================================
   Puppet (disabled placeholders to avoid crashes for now)
   ============================================================ */

qboolean Teach_PuppetIsActive(void) { return qfalse; }
qboolean Teach_PuppetSpawn(const char *name, int hereCid, float rate, qboolean loop) {
    TPrintF("teach: puppet is disabled in this build\n");
    return qfalse;
}
void Teach_PuppetKill(void) { /* no-op */ }

/* ============================================================
   Per-frame pump
   ============================================================ */
void Teach_RunFrame(void) {
	/* Dual-actor recording: capture commands from both clients once per frame */
	if (g_teachDuelRec.active) {
		gentity_t *entA = &g_entities[g_teachDuelRec.clientNumA];
		gentity_t *entB = &g_entities[g_teachDuelRec.clientNumB];
		if (entA && entA->client && entB && entB->client) {
			usercmd_t *ucmdA = &entA->client->pers.cmd;
			usercmd_t *ucmdB = &entB->client->pers.cmd;
			Teach_DuelRecordUsercmd(entA, ucmdA, entB, ucmdB);
		}
	}
}

/* ============================================================
   Console command dispatcher (void for OpenJK)
   Usage:
     teach where
     teach status
     teach testwrite
     teach record <cid> <name>
     teach stop
     teach play <name> <cid> [rate=1.0] [loop=0/1]
     teach stopplay
     teach puppet <name> here:<cid> [rate=1.0] [loop=0/1]
     teach killpuppet
   ============================================================ */

static void Teach_Dispatch(void) {
    if (trap->Argc() < 2) {
        TPrintF("teach: where|status|testwrite|record <cid> <name>|duelrec <cidA> <cidB> <name>|stop|play <name> <cid> [rate] [loop]|playduel <name> <cidA> <cidB> [rate] [loop]|stopplay|puppet <name> here:<cid> [rate] [loop]|killpuppet\n");
        return;
    }
    char cmd[64];
    trap->Argv(1, cmd, sizeof(cmd));

    if (!Q_stricmp(cmd, "where"))  { T_PrintWhere();  return; }
    if (!Q_stricmp(cmd, "status")) { T_PrintStatus(); return; }

    if (!Q_stricmp(cmd, "testwrite")) {
        fileHandle_t f = 0;
        trap->FS_Open("teach__probe.probe.txt", &f, FS_WRITE);
        if (f) {
            const char *s = "ok\n";
            trap->FS_Write((void*)s, (int)strlen(s), f);
            trap->FS_Close(f);
            TPrintF("teach: wrote 'teach__probe.probe.txt'\n");
        } else {
            TPrintF("teach: probe write failed\n");
        }
        return;
    }

    if (!Q_stricmp(cmd, "record")) {
        if (trap->Argc() < 4) { TPrintF("usage: teach record <cid> <name>\n"); return; }
        char a[16], name[64];
        trap->Argv(2, a, sizeof(a));
        trap->Argv(3, name, sizeof(name));
        Teach_RecordStart(atoi(a), name);
        return;
    }

    if (!Q_stricmp(cmd, "stop")) {
        Teach_RecordStop();
        Teach_DuelRecordStop();
        Teach_PlayStop();
        Teach_DuelPlayStop();
        return;
    }

    if (!Q_stricmp(cmd, "recordduel") || !Q_stricmp(cmd, "duelrec")) {
        if (trap->Argc() < 5) { TPrintF("usage: teach recordduel <cidA> <cidB> <name>\n"); return; }
        char a[16], b[16], name[64];
        trap->Argv(2, a, sizeof(a));
        trap->Argv(3, b, sizeof(b));
        trap->Argv(4, name, sizeof(name));
        Teach_DuelRecordStart(atoi(a), atoi(b), name);
        return;
    }

    if (!Q_stricmp(cmd, "play")) {
        if (trap->Argc() < 4) { TPrintF("usage: teach play <name> <cid> [rate=1.0] [loop=0/1]\n"); return; }
        char name[64], a[16], b[16];
        float rate = 1.0f; qboolean loop = qfalse;
        trap->Argv(2, name, sizeof(name));
        trap->Argv(3, a, sizeof(a));
        if (trap->Argc() > 4) { trap->Argv(4, b, sizeof(b)); rate = (float)atof(b); }
        if (trap->Argc() > 5) { trap->Argv(5, b, sizeof(b)); loop = (atoi(b) != 0); }
        Teach_PlayStart(name, atoi(a), rate, loop);
        return;
    }

    if (!Q_stricmp(cmd, "stopplay")) { Teach_PlayStop(); Teach_DuelPlayStop(); return; }

    if (!Q_stricmp(cmd, "playbot")) {
        if (trap->Argc() < 3) {
            TPrintF("usage: teach playbot <name> [rate=1.0] [loop=0/1]\n");
            TPrintF("  Auto-spawns a reborn bot and plays recording on it\n");
            TPrintF("  Alternative: 'addbot reborn' then 'teach play <name> <botClientNum>'\n");
            return;
        }
        char name[64], tmp[16];
        float rate = 1.0f; qboolean loop = qfalse;
        trap->Argv(2, name, sizeof(name));
        if (trap->Argc() > 3) { trap->Argv(3, tmp, sizeof(tmp)); rate = (float)atof(tmp); }
        if (trap->Argc() > 4) { trap->Argv(4, tmp, sizeof(tmp)); loop = (atoi(tmp) != 0); }

        /* Find any available bot that's not currently being controlled */
        int botNum = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if ((g_entities[i].r.svFlags & SVF_BOT) &&
                level.clients[i].pers.connected == CON_CONNECTED &&
                !Teach_IsControllingClient(i)) {
                botNum = i;
                break;
            }
        }

        /* If no free bot found, spawn one and tell user to retry */
        if (botNum < 0) {
            TPrintF("teach: no free bots found, spawning one...\n");
            TPrintF("teach: please run 'teach playbot %s' again after bot spawns\n", name);
            trap->SendConsoleCommand(EXEC_APPEND, "addbot reborn 1\n");
            return;
        }

        /* Found a free bot - use it */
        TPrintF("teach: using bot at slot %d, playing '%s'\n", botNum, name);
        Teach_PlayStart(name, botNum, rate, loop);

        /* Verify playback started */
        if (g_teachPlay.active && g_teachPlay.clientNum == botNum) {
            TPrintF("teach: playback active on bot %d\n", botNum);
        } else {
            TPrintF("teach: ERROR - playback failed (check file: teach__%s.teach.jsonl)\n", name);
        }
        return;
    }

    if (!Q_stricmp(cmd, "trainbot")) {
        if (trap->Argc() < 4) {
            TPrintF("usage: teach trainbot <recording> <targetPlayerID> [rate=1.0]\n");
            TPrintF("  Spawns a bot that loops the recording and chases the target player\n");
            TPrintF("  Use 'teach stopplay' to stop the training bot\n");
            return;
        }
        char name[64], targetStr[16], tmp[16];
        float rate = 1.0f;
        trap->Argv(2, name, sizeof(name));
        trap->Argv(3, targetStr, sizeof(targetStr));
        int targetPlayer = atoi(targetStr);
        if (trap->Argc() > 4) { trap->Argv(4, tmp, sizeof(tmp)); rate = (float)atof(tmp); }

        /* Validate target player */
        if (targetPlayer < 0 || targetPlayer >= MAX_CLIENTS || !g_entities[targetPlayer].client) {
            TPrintF("teach: invalid target player %d\n", targetPlayer);
            return;
        }

        /* Find or spawn a bot */
        int botNum = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if ((g_entities[i].r.svFlags & SVF_BOT) &&
                level.clients[i].pers.connected == CON_CONNECTED &&
                !Teach_IsControllingClient(i)) {
                botNum = i;
                break;
            }
        }

        if (botNum < 0) {
            TPrintF("teach: no free bots found, spawning one...\n");
            TPrintF("teach: please run 'teach trainbot %s %d' again after bot spawns\n", name, targetPlayer);
            trap->SendConsoleCommand(EXEC_APPEND, "addbot reborn 1\n");
            return;
        }

        /* Start looped playback on the bot */
        TPrintF("teach: training bot %d will loop '%s' and chase player %d\n", botNum, name, targetPlayer);
        Teach_PlayStart(name, botNum, rate, qtrue);  /* qtrue = loop */

        if (g_teachPlay.active && g_teachPlay.clientNum == botNum) {
            /* Enable training mode - bot will chase the target */
            g_teachPlay.targetPlayerNum = targetPlayer;
            /* Set initial offset (in front of target, facing them) */
            g_teachPlay.trainingOffset[0] = 100.0f;  /* Forward */
            g_teachPlay.trainingOffset[1] = 0.0f;    /* Right */
            g_teachPlay.trainingOffset[2] = 0.0f;    /* Up */
            TPrintF("teach: training bot active - will reposition to face player %d\n", targetPlayer);
            TPrintF("teach: use 'teach stopplay' to stop\n");
        } else {
            TPrintF("teach: ERROR - failed to start training bot (check file: teach__%s.teach.jsonl)\n", name);
        }
        return;
    }

    if (!Q_stricmp(cmd, "playduel")) {
        if (trap->Argc() < 5) { TPrintF("usage: teach playduel <name> <cidA> <cidB> [rate=1.0] [loop=0/1]\n"); return; }
        char name[64], a[16], b[16], tmp[16];
        float rate = 1.0f; qboolean loop = qfalse;
        trap->Argv(2, name, sizeof(name));
        trap->Argv(3, a, sizeof(a));
        trap->Argv(4, b, sizeof(b));
        if (trap->Argc() > 5) { trap->Argv(5, tmp, sizeof(tmp)); rate = (float)atof(tmp); }
        if (trap->Argc() > 6) { trap->Argv(6, tmp, sizeof(tmp)); loop = (atoi(tmp) != 0); }
        Teach_DuelPlayStart(name, atoi(a), atoi(b), rate, loop);
        return;
    }

    if (!Q_stricmp(cmd, "puppet")) {
        if (trap->Argc() < 4) { TPrintF("usage: teach puppet <name> here:<cid> [rate=1.0] [loop=0/1]\n"); return; }
        char name[64], hereArg[32], tmp[32];
        float rate = 1.0f; qboolean loop = qfalse; int cid = 0;
        trap->Argv(2, name, sizeof(name));
        trap->Argv(3, hereArg, sizeof(hereArg));
        if (Q_stricmpn(hereArg, "here:", 5)) { TPrintF("teach: need here:<cid>\n"); return; }
        cid = atoi(hereArg + 5);
        if (trap->Argc() > 4) { trap->Argv(4, tmp, sizeof(tmp)); rate = (float)atof(tmp); }
        if (trap->Argc() > 5) { trap->Argv(5, tmp, sizeof(tmp)); loop = (atoi(tmp) != 0); }
        Teach_PuppetSpawn(name, cid, rate, loop);
        return;
    }

    if (!Q_stricmp(cmd, "killpuppet")) { Teach_PuppetKill(); return; }

    TPrintF("teach: where|status|testwrite|record <cid> <name>|duelrec <cidA> <cidB> <name>|stop|play <name> <cid> [rate] [loop]|playduel <name> <cidA> <cidB> [rate] [loop]|stopplay|puppet <name> here:<cid> [rate] [loop]|killpuppet\n");
}

/* OpenJK expects a void handler for server commands */
void Svcmd_Teach_f(void) {
    Teach_Dispatch();
}

