/*
===========================================================================
Copyright (C) 2025 Master Mod contributors

In-game entity diagnostics overlay for builders. Provides live counts,
highlight rendering, and a focus inspector fed from the current snapshot.
===========================================================================
*/

#include "cg_local.h"

#include <math.h>

typedef enum {
	ENTINFO_CAT_GENERAL = 0,
	ENTINFO_CAT_PLAYERS,
	ENTINFO_CAT_NPCS,
	ENTINFO_CAT_ITEMS,
	ENTINFO_CAT_MISSILES,
	ENTINFO_CAT_MOVERS,
	ENTINFO_CAT_FX,
	ENTINFO_CAT_EVENTS,
	ENTINFO_CAT_MAX
} cgEntInfoCategory_t;

typedef struct cgEntInfoStats_s {
	int limit;
	int snapshotCount;
	int totalCount;
	int categoryCounts[ENTINFO_CAT_MAX];
} cgEntInfoStats_t;

typedef struct cgEntInfoLine_s {
	cgEntInfoCategory_t cat;
	const char *label;
} cgEntInfoLine_t;

static const cgEntInfoLine_t cg_entInfoCategoryLines[] = {
	{ ENTINFO_CAT_PLAYERS, "Players" },
	{ ENTINFO_CAT_NPCS, "NPCs" },
	{ ENTINFO_CAT_ITEMS, "Items / Holocrons" },
	{ ENTINFO_CAT_MISSILES, "Projectiles" },
	{ ENTINFO_CAT_MOVERS, "World Movers" },
	{ ENTINFO_CAT_FX, "FX / Beams" },
	{ ENTINFO_CAT_GENERAL, "General / Other" },
	{ ENTINFO_CAT_EVENTS, "Event Temp" }
};

static const char *const cg_entInfoTypeNames[] = {
	"GENERAL",
	"PLAYER",
	"ITEM",
	"MISSILE",
	"SPECIAL",
	"HOLOCRON",
	"MOVER",
	"BEAM",
	"PORTAL",
	"SPEAKER",
	"PUSH_TRIGGER",
	"TELEPORT_TRIG",
	"INVISIBLE",
	"NPC",
	"TEAM",
	"BODY",
	"TERRAIN",
	"FX"
};

static cgEntInfoStats_t cg_entInfoStats;
static int cg_entInfoFrameStamp = -1;
static int cg_entInfoHighlightCount = 0;

static int cg_entInfoFocusFrame = -1;
static int cg_entInfoFocusEnt = ENTITYNUM_NONE;
static vec3_t cg_entInfoFocusOrigin = { 0.0f, 0.0f, 0.0f };

static qboolean CG_EntInfo_IsEnabled( void ) {
	return ( cg_entInfoPanel.integer ||
			 cg_entInfoHighlight.integer ||
			 cg_entInfoLook.integer );
}

static const char *CG_EntInfo_CategoryLabel( cgEntInfoCategory_t cat ) {
	for ( size_t i = 0; i < ARRAY_LEN( cg_entInfoCategoryLines ); ++i ) {
		if ( cg_entInfoCategoryLines[i].cat == cat ) {
			return cg_entInfoCategoryLines[i].label;
		}
	}
	return "Unknown";
}

static void CG_EntInfo_CategoryColor( cgEntInfoCategory_t cat, vec4_t color ) {
	static const vec4_t table[ENTINFO_CAT_MAX] = {
		{ 0.60f, 0.65f, 0.68f, 0.75f }, // general
		{ 0.20f, 0.75f, 1.00f, 0.85f }, // players
		{ 1.00f, 0.60f, 0.25f, 0.90f }, // npcs
		{ 0.35f, 0.95f, 0.45f, 0.90f }, // items
		{ 1.00f, 0.35f, 0.35f, 0.90f }, // missiles
		{ 1.00f, 0.95f, 0.40f, 0.85f }, // movers/triggers
		{ 0.70f, 0.50f, 1.00f, 0.85f }, // fx
		{ 0.90f, 0.90f, 0.90f, 0.90f }  // events
	};

	if ( cat < 0 || cat >= ENTINFO_CAT_MAX ) {
		color[0] = 0.60f;
		color[1] = 0.65f;
		color[2] = 0.68f;
		color[3] = 0.75f;
		return;
	}

	color[0] = table[cat][0];
	color[1] = table[cat][1];
	color[2] = table[cat][2];
	color[3] = table[cat][3];
}

static const char *CG_EntInfo_TypeName( const entityState_t *es ) {
	static char buffer[32];
	const int eType = es->eType;

	if ( eType >= 0 && eType < (int)ARRAY_LEN( cg_entInfoTypeNames ) ) {
		return cg_entInfoTypeNames[eType];
	}

	if ( eType >= ET_EVENTS ) {
		Com_sprintf( buffer, sizeof( buffer ), "EVENT+%d", eType - ET_EVENTS );
	} else {
		Com_sprintf( buffer, sizeof( buffer ), "%d", eType );
	}
	return buffer;
}

static cgEntInfoCategory_t CG_EntInfo_Classify( const entityState_t *es ) {
	switch ( es->eType ) {
	case ET_PLAYER:
		return ENTINFO_CAT_PLAYERS;
	case ET_NPC:
		return ENTINFO_CAT_NPCS;
	case ET_ITEM:
	case ET_HOLOCRON:
		return ENTINFO_CAT_ITEMS;
	case ET_MISSILE:
		return ENTINFO_CAT_MISSILES;
	case ET_MOVER:
	case ET_SPECIAL:
	case ET_PORTAL:
	case ET_SPEAKER:
	case ET_PUSH_TRIGGER:
	case ET_TELEPORT_TRIGGER:
	case ET_INVISIBLE:
	case ET_TEAM:
	case ET_BODY:
	case ET_TERRAIN:
		return ENTINFO_CAT_MOVERS;
	case ET_BEAM:
	case ET_FX:
		return ENTINFO_CAT_FX;
	default:
		if ( es->eType >= ET_EVENTS ) {
			return ENTINFO_CAT_EVENTS;
		}
		return ENTINFO_CAT_GENERAL;
	}
}

static void CG_EntInfo_ResetStats( void ) {
	memset( &cg_entInfoStats, 0, sizeof( cg_entInfoStats ) );
	cg_entInfoStats.limit = MAX_GENTITIES;
}

static void CG_EntInfo_EnsureStats( void ) {
	if ( cg_entInfoFrameStamp == cg.clientFrame ) {
		return;
	}

	cg_entInfoFrameStamp = cg.clientFrame;
	CG_EntInfo_ResetStats();

	if ( !cg.snap ) {
		return;
	}

	cg_entInfoStats.snapshotCount = cg.snap->numEntities;
	cg_entInfoStats.totalCount = cg.snap->numEntities;

	for ( int i = 0; i < cg.snap->numEntities; ++i ) {
		const entityState_t *es = &cg.snap->entities[i];
		const cgEntInfoCategory_t cat = CG_EntInfo_Classify( es );

		if ( cat >= 0 && cat < ENTINFO_CAT_MAX ) {
			cg_entInfoStats.categoryCounts[cat]++;
		}
	}
}

static void CG_EntInfo_UpdateFocus( void ) {
	if ( cg_entInfoFocusFrame == cg.clientFrame ) {
		return;
	}

	cg_entInfoFocusFrame = cg.clientFrame;
	cg_entInfoFocusEnt = ENTITYNUM_NONE;
	VectorClear( cg_entInfoFocusOrigin );

	if ( !cg.snap ) {
		return;
	}

	vec3_t eye;
	vec3_t viewDir;

	VectorCopy( cg.refdef.vieworg, eye );
	VectorCopy( cg.refdef.viewaxis[0], viewDir );

	float bestLateral = 999999.0f;

	for ( int i = 0; i < cg.snap->numEntities; ++i ) {
		const entityState_t *es = &cg.snap->entities[i];
		const int entNum = es->number;

		if ( entNum < 0 || entNum >= MAX_GENTITIES ) {
			continue;
		}

		const centity_t *cent = &cg_entities[entNum];
		if ( !cent->currentValid ) {
			continue;
		}

		vec3_t delta;
		VectorSubtract( cent->lerpOrigin, eye, delta );

		const float distSq = VectorLengthSquared( delta );
		if ( distSq < 1.0f ) {
			continue;
		}

		const float dist = sqrtf( distSq );
		const float forward = DotProduct( delta, viewDir );
		if ( forward <= 0.0f ) {
			continue;
		}

		float lateralSq = distSq - ( forward * forward );
		if ( lateralSq < 0.0f ) {
			lateralSq = 0.0f;
		}
		const float lateral = sqrtf( lateralSq );
		const float angular = lateral / dist; // radians approximation

		if ( angular > 0.10f ) { // roughly 5.7 degrees
			continue;
		}

		if ( lateral < bestLateral ) {
			bestLateral = lateral;
			cg_entInfoFocusEnt = entNum;
			VectorCopy( cent->lerpOrigin, cg_entInfoFocusOrigin );
		}
	}
}

static void CG_EntInfo_DrawPanel( void ) {
	if ( !cg_entInfoPanel.integer || !cg.snap ) {
		return;
	}

	const float baseX = 12.0f;
	float baseY = 116.0f;
	const float lineHeight = (float)SMALLCHAR_HEIGHT + 3.0f;
	const float panelWidth = 236.0f;
	const int headerLines = 3;
	const int bodyLines = (int)ARRAY_LEN( cg_entInfoCategoryLines );
	const qboolean showHighlightLine = ( cg_entInfoHighlight.integer != 0 );
	const qboolean showFocusLine = ( cg_entInfoLook.integer && cg_entInfoFocusEnt != ENTITYNUM_NONE );
	const int extraLines = ( showHighlightLine ? 1 : 0 ) + ( showFocusLine ? 1 : 0 );
	const float panelHeight = ( headerLines + bodyLines + extraLines ) * lineHeight + 12.0f;

	vec4_t bgColor = { 0.05f, 0.06f, 0.09f, 0.72f };
	vec4_t borderColor = { 0.30f, 0.35f, 0.46f, 0.90f };
	vec4_t textColor = { 0.86f, 0.88f, 0.90f, 1.0f };
	vec4_t cautionColor = { 1.00f, 0.78f, 0.35f, 1.0f };
	vec4_t warnColor = { 1.00f, 0.45f, 0.45f, 1.0f };

	CG_FillRect( baseX, baseY, panelWidth, panelHeight, bgColor );
	CG_DrawRect( baseX, baseY, panelWidth, panelHeight, 1.0f, borderColor );

	char buffer[128];
	float textY = baseY + 6.0f;

	const int limit = cg_entInfoStats.limit > 0 ? cg_entInfoStats.limit : MAX_GENTITIES;
	const int total = cg_entInfoStats.totalCount;
	int percent = 0;
	if ( limit > 0 ) {
		percent = (int)( ( (float)total / (float)limit ) * 100.0f + 0.5f );
	}
	if ( percent < 0 ) {
		percent = 0;
	}

	const float *usageColor = textColor;
	if ( percent >= 95 ) {
		usageColor = warnColor;
	} else if ( percent >= 85 ) {
		usageColor = cautionColor;
	}

	Com_sprintf( buffer, sizeof( buffer ), "Entities: %d / %d (%d%%)", total, limit, percent );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, usageColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Snapshot ents: %d", cg_entInfoStats.snapshotCount );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Client frame: %d", cg.clientFrame );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	for ( size_t i = 0; i < ARRAY_LEN( cg_entInfoCategoryLines ); ++i ) {
		const cgEntInfoCategory_t cat = cg_entInfoCategoryLines[i].cat;
		const int count = cg_entInfoStats.categoryCounts[cat];
		vec4_t swatchColor;

		CG_EntInfo_CategoryColor( cat, swatchColor );

		vec4_t textTint = {
			swatchColor[0],
			swatchColor[1],
			swatchColor[2],
			1.0f
		};

		CG_FillRect( baseX + 10.0f, textY - 1.0f, 8.0f, (float)SMALLCHAR_HEIGHT, swatchColor );
		CG_DrawStringExt( (int)( baseX + 24.0f ), (int)textY,
			va( "%s: %d", CG_EntInfo_CategoryLabel( cat ), count ),
			textTint, qfalse, qfalse, SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
		textY += lineHeight;
	}

	if ( showHighlightLine ) {
		const int highlightMax = Com_Clampi( 0, MAX_GENTITIES, cg_entInfoHighlightLimit.integer );
		Com_sprintf( buffer, sizeof( buffer ), "Highlighting: %d / %d", cg_entInfoHighlightCount, highlightMax );
		CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
			SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
		textY += lineHeight;
	}

	if ( showFocusLine ) {
		const centity_t *focus = &cg_entities[cg_entInfoFocusEnt];
		const entityState_t *es = &focus->currentState;
		vec4_t focusColor;

		CG_EntInfo_CategoryColor( CG_EntInfo_Classify( es ), focusColor );
		focusColor[3] = 1.0f;

		Com_sprintf( buffer, sizeof( buffer ), "Focus: #%d %s", es->number, CG_EntInfo_TypeName( es ) );
		CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, focusColor, qfalse, qfalse,
			SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	}
}

static void CG_EntInfo_DrawInspector( void ) {
	if ( !cg_entInfoLook.integer || cg_entInfoFocusEnt == ENTITYNUM_NONE ) {
		return;
	}

	if ( cg_entInfoFocusEnt < 0 || cg_entInfoFocusEnt >= MAX_GENTITIES ) {
		return;
	}

	const centity_t *cent = &cg_entities[cg_entInfoFocusEnt];
	if ( !cent->currentValid ) {
		return;
	}

	const entityState_t *es = &cent->currentState;
	const cgEntInfoCategory_t cat = CG_EntInfo_Classify( es );

	vec4_t catColor;
	CG_EntInfo_CategoryColor( cat, catColor );
	catColor[3] = 1.0f;

	vec4_t bgColor = { 0.10f, 0.12f, 0.18f, 0.85f };
	vec4_t borderColor = { 0.50f, 0.55f, 0.70f, 0.95f };
	vec4_t textColor = { 0.88f, 0.90f, 0.94f, 1.0f };

	const float lineHeight = (float)SMALLCHAR_HEIGHT + 3.0f;
	const int lineCount = 7;
	const float panelWidth = 260.0f;
	const float panelHeight = lineCount * lineHeight + 12.0f;

	float baseX = ( SCREEN_WIDTH * 0.5f ) + 30.0f;
	float baseY = ( SCREEN_HEIGHT * 0.5f ) - ( panelHeight * 0.5f );

	if ( baseX + panelWidth > SCREEN_WIDTH - 8.0f ) {
		baseX = SCREEN_WIDTH - panelWidth - 8.0f;
	}
	if ( baseY < 24.0f ) {
		baseY = 24.0f;
	} else if ( baseY + panelHeight > SCREEN_HEIGHT - 8.0f ) {
		baseY = SCREEN_HEIGHT - panelHeight - 8.0f;
	}

	CG_FillRect( baseX, baseY, panelWidth, panelHeight, bgColor );
	CG_DrawRect( baseX, baseY, panelWidth, panelHeight, 1.0f, borderColor );

	char buffer[160];
	float textY = baseY + 6.0f;

	Com_sprintf( buffer, sizeof( buffer ), "Entity #%d  %s", es->number, CG_EntInfo_TypeName( es ) );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, catColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Category: %s", CG_EntInfo_CategoryLabel( cat ) );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Origin: %.1f %.1f %.1f",
		cg_entInfoFocusOrigin[0], cg_entInfoFocusOrigin[1], cg_entInfoFocusOrigin[2] );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Angles: %.1f %.1f %.1f",
		cent->lerpAngles[0], cent->lerpAngles[1], cent->lerpAngles[2] );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Model: %d  Skin: %d",
		es->modelindex, es->modelindex2 );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Owner: %d  Team: %d  Other: %d",
		es->owner, es->teamowner, es->otherEntityNum );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Solid: 0x%X  Flags: 0x%X",
		es->solid, es->eFlags );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
	textY += lineHeight;

	Com_sprintf( buffer, sizeof( buffer ), "Event: %d  Time: %d",
		es->event, es->time );
	CG_DrawStringExt( (int)( baseX + 10.0f ), (int)textY, buffer, textColor, qfalse, qfalse,
		SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT, 0 );
}

void CG_EntInfo_AddHighlights( void ) {
	cg_entInfoHighlightCount = 0;

	CG_EntInfo_EnsureStats();

	if ( !cg_entInfoHighlight.integer || !cg.snap ) {
		return;
	}

	CG_EntInfo_UpdateFocus();

	const int highlightMax = Com_Clampi( 0, MAX_GENTITIES, cg_entInfoHighlightLimit.integer );
	if ( highlightMax <= 0 ) {
		return;
	}

	for ( int i = 0; i < cg.snap->numEntities && cg_entInfoHighlightCount < highlightMax; ++i ) {
		const entityState_t *es = &cg.snap->entities[i];
		const int entNum = es->number;

		if ( entNum < 0 || entNum >= MAX_GENTITIES ) {
			continue;
		}

		const centity_t *cent = &cg_entities[entNum];
		if ( !cent->currentValid ) {
			continue;
		}

		refEntity_t re;
		memset( &re, 0, sizeof( re ) );
		re.reType = RT_SPRITE;
		re.customShader = cgs.media.whiteShader;
		re.radius = ( entNum == cg_entInfoFocusEnt ) ? 14.0f : 9.0f;
		VectorCopy( cent->lerpOrigin, re.origin );
		re.origin[2] += 12.0f;
		AxisClear( re.axis );

		vec4_t color;
		CG_EntInfo_CategoryColor( CG_EntInfo_Classify( es ), color );

		if ( entNum == cg_entInfoFocusEnt ) {
		color[0] = 1.0f;
		color[1] = 1.0f;
		color[2] = 1.0f;
		color[3] = 0.95f;
		}

		re.shaderRGBA[0] = (byte)Com_Clampi( 0, 255, (int)( color[0] * 255.0f ) );
		re.shaderRGBA[1] = (byte)Com_Clampi( 0, 255, (int)( color[1] * 255.0f ) );
		re.shaderRGBA[2] = (byte)Com_Clampi( 0, 255, (int)( color[2] * 255.0f ) );
		re.shaderRGBA[3] = (byte)Com_Clampi( 32, 255, (int)( color[3] * 255.0f ) );
		re.renderfx = RF_RGB_TINT;

		trap->R_AddRefEntityToScene( &re );
		++cg_entInfoHighlightCount;
	}
}

void CG_EntInfo_DrawOverlay( void ) {
	if ( !CG_EntInfo_IsEnabled() || !cg.snap ) {
		return;
	}

	CG_EntInfo_EnsureStats();
	CG_EntInfo_UpdateFocus();

	CG_EntInfo_DrawPanel();
	CG_EntInfo_DrawInspector();
}
