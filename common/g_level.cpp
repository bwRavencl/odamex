// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id$
//
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2006 by Randy Heit (ZDoom).
// Copyright (C) 2006-2020 by The Odamex Team.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Common level routines
//
//-----------------------------------------------------------------------------

#include "g_level.h"

#include <set>

#include "c_console.h"
#include "c_dispatch.h"
#include "d_event.h"
#include "d_main.h"
#include "doomstat.h"
#include "g_level.h"
#include "g_game.h"
#include "gstrings.h"
#include "gi.h"
#include "i_system.h"
#include "m_alloc.h"
#include "m_fileio.h"
#include "minilzo.h"
#include "p_acs.h"
#include "p_local.h"
#include "p_saveg.h"
#include "p_unlag.h"
#include "r_data.h"
#include "r_sky.h"
#include "s_sound.h"
#include "sc_man.h"
#include "v_video.h"
#include "w_wad.h"
#include "w_ident.h"
#include "z_zone.h"
#include "stringenums.h"

#define lioffset(x)		offsetof(level_pwad_info_t,x)
#define cioffset(x)		offsetof(cluster_info_t,x)

level_locals_t level;			// info about current level

typedef std::vector<level_pwad_info_t> WadLevelInfos;
WadLevelInfos wadlevelinfos;

typedef std::vector<cluster_info_t> WadClusterInfos;
WadClusterInfos wadclusterinfos;

// A tagged union that represents all possible infos that we can pass to
// the "lower" MAPINFO parser.
struct tagged_info_t {
	enum tags {
		LEVEL,
		CLUSTER,
		EPISODE,
	};
	tags tag;
	union {
		level_pwad_info_t* level;
		cluster_info_t* cluster;
		void* episode;
	};
};

BOOL HexenHack;

enum EMIType
{
	MITYPE_IGNORE,
	MITYPE_EATNEXT,
	MITYPE_INT,
	MITYPE_FLOAT,
	MITYPE_COLOR,
	MITYPE_MAPNAME,
	MITYPE_LUMPNAME,
	MITYPE_$LUMPNAME,
	MITYPE_MUSICLUMPNAME,
	MITYPE_SKY,
	MITYPE_SETFLAG,
	MITYPE_SCFLAGS,
	MITYPE_CLUSTER,
	MITYPE_STRING,
	MITYPE_CSTRING,
	MITYPE_STRING_OR_LOOKUP,
};

struct MapInfoHandler
{
    EMIType type;
    DWORD data1, data2;
};

static const char *MapInfoTopLevel[] =
{
	"map",
	"defaultmap",
	"cluster",
	"clusterdef",
	"episode",
	"clearepisodes",
	"gameinfo",
	"intermission",
	NULL
};

enum
{
	// map <maplump> <nice name>
	// map <maplump> lookup <keyword>
	MITL_MAP,

	// defaultmap
	MITL_DEFAULTMAP,

	// cluster <value>
	MITL_CLUSTER,

	// clusterdef <value>
	MITL_CLUSTERDEF,

	// episode <maplump>
	// episode <maplump> teaser <maplump> // New MAPINFO only
	MITL_EPISODE,

	// clearepisodes
	MITL_CLEAREPISODES,

	// gameinfo // New MAPINFO only
	MITL_GAMEINFO,

	// intermission // New MAPINFO only
	MITL_INTERMISSION
};

static const char *MapInfoMapLevel[] =
{
	"levelnum",
	"next",
	"secretnext",
	"cluster",
	"sky1",
	"sky2",
	"fade",
	"outsidefog",
	"titlepatch",
	"par",
	"music",
	"nointermission",
	"doublesky",
	"nosoundclipping",
	"allowmonstertelefrags",
	"map07special",
	"baronspecial",
	"cyberdemonspecial",
	"spidermastermindspecial",
	"specialaction_exitlevel",
	"specialaction_opendoor",
	"specialaction_lowerfloor",
	"lightning",
	"fadetable",
	"evenlighting",
	"noautosequences",
	"forcenoskystretch",
	"allowfreelook",
	"nofreelook",
	"allowjump",
	"nojump",
	"cdtrack",
	"cd_start_track",
	"cd_end1_track",
	"cd_end2_track",
	"cd_end3_track",
	"cd_intermission_track",
	"cd_title_track",
	"warptrans",
	"gravity",
	"aircontrol",
	"islobby",
	"lobby",
	"nocrouch",
	"intermusic",
	"par",
	"sucktime",
	NULL
};

MapInfoHandler MapHandlers[] =
{
	// levelnum <levelnum>
	{ MITYPE_INT, lioffset(levelnum), 0 },
	// next <maplump>
	{ MITYPE_MAPNAME, lioffset(nextmap), 0 },
	// secretnext <maplump>
	{ MITYPE_MAPNAME, lioffset(secretmap), 0 },
	// cluster <number>
	{ MITYPE_CLUSTER, lioffset(cluster), 0 },
	// sky1 <texture> <scrollspeed>
	{ MITYPE_SKY, lioffset(skypic), 0 },
	// sky2 <texture> <scrollspeed>
	{ MITYPE_SKY, lioffset(skypic2), 0 },
	// fade <color>
	{ MITYPE_COLOR, lioffset(fadeto_color), 0 },
	// outsidefog <color>
	{ MITYPE_COLOR, lioffset(outsidefog_color), 0 },
	// titlepatch <patch>
	{ MITYPE_LUMPNAME, lioffset(pname), 0 },
	// par <partime>
	{ MITYPE_INT, lioffset(partime), 0 },
	// music <musiclump>
	{ MITYPE_MUSICLUMPNAME, lioffset(music), 0 },
	// nointermission
	{ MITYPE_SETFLAG, LEVEL_NOINTERMISSION, 0 },
	// doublesky
	{ MITYPE_SETFLAG, LEVEL_DOUBLESKY, 0 },
	// nosoundclipping
	{ MITYPE_SETFLAG, LEVEL_NOSOUNDCLIPPING, 0 },
	// allowmonstertelefrags
	{ MITYPE_SETFLAG, LEVEL_MONSTERSTELEFRAG, 0 },
	// map07special
	{ MITYPE_SETFLAG, LEVEL_MAP07SPECIAL, 0 },
	// baronspecial
	{ MITYPE_SETFLAG, LEVEL_BRUISERSPECIAL, 0 },
	// cyberdemonspecial
	{ MITYPE_SETFLAG, LEVEL_CYBORGSPECIAL, 0 },
	// spidermastermindspecial
	{ MITYPE_SETFLAG, LEVEL_SPIDERSPECIAL, 0 },
	// specialaction_exitlevel
	{ MITYPE_SCFLAGS, 0, ~LEVEL_SPECACTIONSMASK },
	// specialaction_opendoor
	{ MITYPE_SCFLAGS, LEVEL_SPECOPENDOOR, ~LEVEL_SPECACTIONSMASK },
	// specialaction_lowerfloor
	{ MITYPE_SCFLAGS, LEVEL_SPECLOWERFLOOR, ~LEVEL_SPECACTIONSMASK },
	// lightning
	{ MITYPE_IGNORE, 0, 0 },
	// fadetable <colormap>
	{ MITYPE_LUMPNAME, lioffset(fadetable), 0 },
	// evenlighting
	{ MITYPE_SETFLAG, LEVEL_EVENLIGHTING, 0 },
	// noautosequences
	{ MITYPE_SETFLAG, LEVEL_SNDSEQTOTALCTRL, 0 },
	// forcenoskystretch
	{ MITYPE_SETFLAG, LEVEL_FORCENOSKYSTRETCH, 0 },
	// allowfreelook
	{ MITYPE_SCFLAGS, LEVEL_FREELOOK_YES, ~LEVEL_FREELOOK_NO },
	// nofreelook
	{ MITYPE_SCFLAGS, LEVEL_FREELOOK_NO, ~LEVEL_FREELOOK_YES },
	// allowjump
	{ MITYPE_SCFLAGS, LEVEL_JUMP_YES, ~LEVEL_JUMP_NO },
	// nojump
	{ MITYPE_SCFLAGS, LEVEL_JUMP_NO, ~LEVEL_JUMP_YES },
	// cdtrack <track number>
	{ MITYPE_EATNEXT, 0, 0 },
	// cd_start_track ???
	{ MITYPE_EATNEXT, 0, 0 },
	// cd_end1_track ???
	{ MITYPE_EATNEXT, 0, 0 },
	// cd_end2_track ???
	{ MITYPE_EATNEXT, 0, 0 },
	// cd_end3_track ???
	{ MITYPE_EATNEXT, 0, 0 },
	// cd_intermission_track ???
	{ MITYPE_EATNEXT, 0, 0 },
	// cd_title_track ???
	{ MITYPE_EATNEXT, 0, 0 },
	// warptrans ???
	{ MITYPE_EATNEXT, 0, 0 },
	// gravity <amount>
	{ MITYPE_FLOAT, lioffset(gravity), 0 },
	// aircontrol <amount>
	{ MITYPE_FLOAT, lioffset(aircontrol), 0 },
	// islobby
	{ MITYPE_SETFLAG, LEVEL_LOBBYSPECIAL, 0},
	// lobby
	{ MITYPE_SETFLAG, LEVEL_LOBBYSPECIAL, 0},
	// nocrouch
	{ MITYPE_IGNORE, 0, 0 },
	// intermusic <musicname>
	{ MITYPE_EATNEXT, 0, 0 },
	// par <partime>
	{ MITYPE_EATNEXT, 0, 0 },
	// sucktime <value>
	{ MITYPE_EATNEXT, 0, 0 },
};

static const char *MapInfoClusterLevel[] =
{
	"entertext",
	"exittext",
	"music",
	"flat",
	"hub",
	NULL
};

MapInfoHandler ClusterHandlers[] =
{
	// entertext <message>
	{ MITYPE_STRING_OR_LOOKUP, cioffset(entertext), 0 },
	// exittext <message>
	{ MITYPE_STRING_OR_LOOKUP, cioffset(exittext), 0 },
	// messagemusic <musiclump>
	{ MITYPE_MUSICLUMPNAME, cioffset(messagemusic), 8 },
	// flat <flatlump>
	{ MITYPE_$LUMPNAME, cioffset(finaleflat), 0 },
	// hub
	{ MITYPE_SETFLAG, CLUSTER_HUB, 0 }
};

static const char* MapInfoEpisodeLevel[] =
{
	"name",
	"lookup",
	"picname",
	"key",
	"remove",
	"noskillmenu",
	"optional",
	NULL
};

MapInfoHandler EpisodeHandlers[] =
{
	// name <nice name>
	{ MITYPE_EATNEXT, 0, 0 },
	// lookup <keyword>
	{ MITYPE_EATNEXT, 0, 0 },
	// picname <piclump>
	{ MITYPE_EATNEXT, 0, 0 },
	// remove
	{ MITYPE_IGNORE, 0, 0 },
	// noskillmenu
	{ MITYPE_IGNORE, 0, 0 },
	// optional
	{ MITYPE_IGNORE, 0, 0 }
};

int FindWadLevelInfo(char *name)
{
	for (size_t i = 0; i < wadlevelinfos.size(); i++)
	{
		if (!strnicmp(name, wadlevelinfos[i].mapname, 8))
		{
			return i;
		}
	}
	return -1;
}

int FindWadClusterInfo(int cluster)
{
	for (size_t i = 0; i < wadclusterinfos.size(); i++)
	{
		if (wadclusterinfos[i].cluster == cluster)
		{
			return i;
		}
	}
	return -1;
}

static void SetLevelDefaults(level_pwad_info_t* levelinfo)
{
	memset (levelinfo, 0, sizeof(*levelinfo));
	levelinfo->snapshot = NULL;
	levelinfo->outsidefog_color[0] = 255; 
	levelinfo->outsidefog_color[1] = 0; 
	levelinfo->outsidefog_color[2] = 0; 
	levelinfo->outsidefog_color[3] = 0; 
	strncpy(levelinfo->fadetable, "COLORMAP", 8);
}

//
// Assumes that you have munched the last parameter you know how to handle,
// but have not yet munched a comma.
//
static void SkipUnknownParams()
{
	// Every loop, try to burn a comma.
	while (SC_GetString())
	{
		if (!SC_Compare(","))
		{
			SC_UnGet();
			return;
		}

		// Burn the parameter.
		SC_GetString();
	}
}

//
// Assumes that you have already munched the unknown type name, and just need
// to much parameters, if any.
//
static void SkipUnknownType()
{
	SC_GetString();
	if (!SC_Compare("="))
	{
		SC_UnGet();
		return;
	}

	SC_GetString(); // Get the first parameter
	SkipUnknownParams();
}

//
// Assumes you have already munched the first opening brace.
//
// This function does not work with old-school ZDoom MAPINFO.
//
static void SkipUnknownBlock()
{
	int stack = 0;

	while (SC_GetString())
	{
		if (SC_Compare("{"))
		{
			// Found another block
			stack++;
			continue;
		}
		else if (SC_Compare("}"))
		{
			stack--;
			if (stack <= 0)
			{
				// Done with all blocks
				break;
			}
		}
	}
}

//
// Parse a MAPINFO block
//
// NULL pointers can be passed if the block is unimplemented.  However, if
// the block you want to stub out is compatible with old MAPINFO, you need
// to parse the block anyway, even if you throw away the values.  This is
// done by passing in a strings pointer, and leaving the others NULL.
//
static void ParseMapInfoLower(
	MapInfoHandler* handlers, const char** strings, tagged_info_t* tinfo, DWORD flags
)
{
	// 0 if old mapinfo, positive number if new MAPINFO, the exact
	// number represents current brace depth.
	int newMapinfoStack = 0;

	byte* info = NULL;
	if (tinfo)
	{
		// The union pointer is always the same, regardless of the tag.
		info = reinterpret_cast<byte*>(tinfo->level);
	}

	while (SC_GetString())
	{
		if (SC_Compare("{"))
		{
			// Detected new-style MAPINFO
			newMapinfoStack++;
			continue;
		}
		else if (SC_Compare("}"))
		{
			newMapinfoStack--;
			if (newMapinfoStack <= 0)
			{
				// MAPINFO block is done
				break;
			}
		}

		if (
			newMapinfoStack <= 0 &&
			SC_MatchString(MapInfoTopLevel) != SC_NOMATCH &&
			// "cluster" is a valid map block type and is also
			// a valid top-level type.
			!SC_Compare("cluster")
		)
		{
			// Old-style MAPINFO is done
			SC_UnGet();
			break;
		}

		int entry = SC_MatchString(strings);
		if (entry == SC_NOMATCH)
		{
			if (newMapinfoStack <= 0)
			{
				// Old MAPINFO is up a creek, we need to be
				// able to parse all types even if we can't
				// do anything with them.
				SC_ScriptError("Unknown MAPINFO token \"%s\"", sc_String);
			}

			// New MAPINFO is capable of skipping past unknown
			// types.
			SkipUnknownType();
			continue;
		}

		MapInfoHandler* handler = handlers + entry;

		switch (handler->type)
		{
		case MITYPE_IGNORE:
			break;

		case MITYPE_EATNEXT:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			break;

		case MITYPE_INT:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetNumber();
			*((int*)(info + handler->data1)) = sc_Number;
			break;

		case MITYPE_FLOAT:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetFloat();
			*((float*)(info + handler->data1)) = sc_Float;
			break;

		case MITYPE_COLOR:
		{
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			argb_t color(V_GetColorFromString(sc_String));
			uint8_t* ptr = (uint8_t*)(info + handler->data1);
			ptr[0] = color.geta(); ptr[1] = color.getr(); ptr[2] = color.getg(); ptr[3] = color.getb();
			break;
		}
		case MITYPE_MAPNAME:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			if (IsNum(sc_String))
			{
				int map = atoi(sc_String);
				sprintf(sc_String, "MAP%02d", map);
			}
			strncpy((char*)(info + handler->data1), sc_String, 8);
			break;

		case MITYPE_LUMPNAME:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			uppercopy((char*)(info + handler->data1), sc_String);
			break;

		case MITYPE_$LUMPNAME:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			if (sc_String[0] == '$')
			{
				// It is possible to pass a DeHackEd string
				// prefixed by a $.
				char* s = sc_String + 1;
				int i = GStrings.FindString(s);
				if (i == -1)
				{
					SC_ScriptError("Unknown lookup string \"%s\"", s);
				}
				uppercopy((char*)(info + handler->data1), GStrings(i));
			}
			else
			{
				uppercopy((char*)(info + handler->data1), sc_String);
			}
			break;

		case MITYPE_MUSICLUMPNAME:
		{
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			if (sc_String[0] == '$')
			{
				// It is possible to pass a DeHackEd string
				// prefixed by a $.
				char* s = sc_String + 1;
				int i = GStrings.FindString(s);
				if (i == -1)
				{
					SC_ScriptError("Unknown lookup string \"%s\"", s);
				}

				// Music lumps in the stringtable do not begin
				// with a D_, so we must add it.
				char lumpname[9];
				snprintf(lumpname, ARRAY_LENGTH(lumpname), "D_%s", GStrings(i));
				uppercopy((char*)(info + handler->data1), lumpname);
			}
			else
			{
				uppercopy((char*)(info + handler->data1), sc_String);
			}
			break;
		}
		case MITYPE_SKY:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
				SC_MustGetString(); // Texture name
				uppercopy((char*)(info + handler->data1), sc_String);
				SkipUnknownParams();
			}
			else
			{
				SC_MustGetString();	// get texture name;
				uppercopy((char*)(info + handler->data1), sc_String);
				SC_MustGetFloat();		// get scroll speed
				//if (HexenHack)
				//{
				//	*((fixed_t *)(info + handler->data2)) = sc_Number << 8;
				//}
				//else
				//{
				//	*((fixed_t *)(info + handler->data2)) = (fixed_t)(sc_Float * 65536.0f);
				//}
			}
			break;

		case MITYPE_SETFLAG:
			flags |= handler->data1;
			break;

		case MITYPE_SCFLAGS:
			flags = (flags & handler->data2) | handler->data1;
			break;

		case MITYPE_CLUSTER:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetNumber();
			*((int*)(info + handler->data1)) = sc_Number;
			if (HexenHack)
			{
				cluster_info_t* clusterH = FindClusterInfo(sc_Number);
				if (clusterH)
					clusterH->flags |= CLUSTER_HUB;
			}
			break;

		case MITYPE_STRING:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			ReplaceString((char**)(info + handler->data1), sc_String);
			break;

		case MITYPE_CSTRING:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			strncpy((char*)(info + handler->data1), sc_String, handler->data2);
			*((char*)(info + handler->data1 + handler->data2)) = '\0';
			break;

		case MITYPE_STRING_OR_LOOKUP:
			if (newMapinfoStack > 0)
			{
				SC_MustGetStringName("=");
			}

			SC_MustGetString();
			if (SC_Compare("lookup"))
			{
				if (newMapinfoStack > 0)
				{
					SC_MustGetStringName(",");
				}

				SC_MustGetString();
				int i = GStrings.FindString(sc_String);
				if (i == -1)
				{
					SC_ScriptError("Unknown lookup string \"%s\"", sc_String);
				}
				ReplaceString((char**)(info + handler->data1), GStrings(i));
			}
			else
			{
				ReplaceString((char**)(info + handler->data1), sc_String);
			}
			break;
		}
	}

	if (tinfo == NULL)
	{
		return;
	}

	switch (tinfo->tag)
	{
	case tagged_info_t::LEVEL:
		tinfo->level->flags = flags;
		break;
	case tagged_info_t::CLUSTER:
		tinfo->cluster->flags = flags;
		break;
	}
}

static void ParseMapInfoLump(int lump, const char* lumpname)
{
	level_pwad_info_t defaultinfo;
	level_pwad_info_t* levelinfo;
	int levelindex;
	cluster_info_t* clusterinfo;
	int clusterindex;
	DWORD levelflags;

	SetLevelDefaults (&defaultinfo);
	SC_OpenLumpNum (lump, lumpname);

	while (SC_GetString ())
	{
		switch (SC_MustMatchString (MapInfoTopLevel))
		{
		case MITL_DEFAULTMAP:
		{
			SetLevelDefaults(&defaultinfo);
			tagged_info_t tinfo;
			tinfo.tag = tagged_info_t::LEVEL;
			tinfo.level = &defaultinfo;
			ParseMapInfoLower(MapHandlers, MapInfoMapLevel, &tinfo, 0);
			break;
		}
		case MITL_MAP:
		{
			levelflags = defaultinfo.flags;
			SC_MustGetString ();
			if (IsNum (sc_String))
			{	// MAPNAME is a number, assume a Hexen wad
				int map = atoi (sc_String);
				sprintf (sc_String, "MAP%02d", map);
				SKYFLATNAME[5] = 0;
				HexenHack = true;
				// Hexen levels are automatically nointermission
				// and even lighting and no auto sound sequences
				levelflags |= LEVEL_NOINTERMISSION
							| LEVEL_EVENLIGHTING
							| LEVEL_SNDSEQTOTALCTRL;
			}
			levelindex = FindWadLevelInfo (sc_String);
			if (levelindex == -1)
			{
				wadlevelinfos.push_back(level_pwad_info_t());
				levelindex = wadlevelinfos.size() - 1;
			}
			levelinfo = &wadlevelinfos[levelindex];
			memcpy (levelinfo, &defaultinfo, sizeof(level_pwad_info_t));
			uppercopy (levelinfo->mapname, sc_String);

			// Map name.
			SC_MustGetString();
			if (SC_Compare("lookup"))
			{
				SC_MustGetString();
				int i = GStrings.FindString(sc_String);
				if (i == -1)
				{
					SC_ScriptError("Unknown lookup string \"%s\"", sc_String);
				}
				ReplaceString(&levelinfo->level_name, GStrings(i));
			}
			else
			{
				ReplaceString(&levelinfo->level_name, sc_String);
			}

			// Set up levelnum now so that the Teleport_NewMap specials
			// in hexen.wad work without modification.
			if (!strnicmp (levelinfo->mapname, "MAP", 3) && levelinfo->mapname[5] == 0)
			{
				int mapnum = atoi (levelinfo->mapname + 3);

				if (mapnum >= 1 && mapnum <= 99)
					levelinfo->levelnum = mapnum;
			}
			tagged_info_t tinfo;
			tinfo.tag = tagged_info_t::LEVEL;
			tinfo.level = levelinfo;
			ParseMapInfoLower (MapHandlers, MapInfoMapLevel, &tinfo, levelflags);
			break;
		}
		case MITL_CLUSTER:
		case MITL_CLUSTERDEF:
		{
			SC_MustGetNumber ();
			clusterindex = FindWadClusterInfo (sc_Number);
			if (clusterindex == -1)
			{
				wadclusterinfos.push_back(cluster_info_t());
				clusterindex = wadclusterinfos.size() - 1;
				memset(&wadclusterinfos[clusterindex], 0, sizeof(cluster_info_t));
			}
			clusterinfo = &wadclusterinfos[clusterindex];
			clusterinfo->cluster = sc_Number;
			tagged_info_t tinfo;
			tinfo.tag = tagged_info_t::CLUSTER;
			tinfo.cluster = clusterinfo;
			ParseMapInfoLower (ClusterHandlers, MapInfoClusterLevel, &tinfo, 0);
			break;
		}
		case MITL_EPISODE:
		{
			// Not implemented
			SC_MustGetString(); // Map lump
			SC_GetString();
			if (SC_Compare("teaser"))
			{
				SC_MustGetString(); // Teaser lump
			}
			else
			{
				SC_UnGet();
			}

			tagged_info_t tinfo;
			tinfo.tag = tagged_info_t::EPISODE;
			tinfo.episode = NULL;
			ParseMapInfoLower(EpisodeHandlers, MapInfoEpisodeLevel, &tinfo, 0);
			break;
		}
		case MITL_CLEAREPISODES:
			// Not implemented
			break;

		case MITL_GAMEINFO:
			// Not implemented
			ParseMapInfoLower(NULL, NULL, NULL, 0);
			break;

		case MITL_INTERMISSION:
			// Not implemented
			SC_MustGetString(); // Name
			ParseMapInfoLower(NULL, NULL, NULL, 0);
			break;

		default:
			SC_ScriptError("Unimplemented top-level type \"%s\"", sc_String);
		}
	}
	SC_Close ();
}

//
// G_ParseMapInfo
// Parses the MAPINFO lumps of all loaded WADs and generates
// data for wadlevelinfos and wadclusterinfos.
//
void G_ParseMapInfo (void)
{
	// First load a specific MAPINFO based on the game we're playing.
	const char* baseinfoname = NULL;
	switch (gamemission)
	{
	case doom:
		baseinfoname = "_D1NFO";
		break;
	case doom2:
		baseinfoname = "_D2NFO";
		break;
	case pack_tnt:
		baseinfoname = "_TNTNFO";
		break;
	case pack_plut:
		baseinfoname = "_PLUTNFO";
		break;
	case chex:
		baseinfoname = "_CHEXNFO";
		break;
	}
	int lump = W_GetNumForName(baseinfoname);
	ParseMapInfoLump(lump, baseinfoname);

	BOOL found_zmapinfo = false;
	lump = -1;
	while ((lump = W_FindLump("ZMAPINFO", lump)) != -1)
	{
		found_zmapinfo = true;
		ParseMapInfoLump(lump, "ZMAPINFO");
	}

	// If ZMAPINFO exists, we don't parse a normal MAPINFO
	if (found_zmapinfo == true)
	{
		return;
	}

	lump = -1;
	while ((lump = W_FindLump("MAPINFO", lump)) != -1)
	{
		ParseMapInfoLump(lump, "MAPINFO");
	}
}

static void zapDefereds (acsdefered_t *def)
{
	while (def) {
		acsdefered_t *next = def->next;
		delete def;
		def = next;
	}
}

void P_RemoveDefereds (void)
{
	// Remove any existing defereds
	for (WadLevelInfos::iterator it = wadlevelinfos.begin(); it != wadlevelinfos.end(); ++it)
	{
		if (it->defered)
		{
			zapDefereds(it->defered);
			it->defered = NULL;
		}
	}
}

// [ML] Not sure where to put this for now...
// 	G_ParseMusInfo
void G_ParseMusInfo(void)
{
	// Nothing yet...
}

//
// G_LoadWad
//
// Determines if the vectors of wad & patch filenames differs from the currently
// loaded ones and calls D_DoomWadReboot if so.
//
bool G_LoadWad(	const std::vector<std::string> &newwadfiles,
				const std::vector<std::string> &newpatchfiles,
				const std::vector<std::string> &newwadhashes,
				const std::vector<std::string> &newpatchhashes,
				const std::string &mapname)
{
	bool AddedIWAD = false;
	bool Reboot = false;
	size_t i, j;

	// Did we pass an IWAD?
	if (!newwadfiles.empty() && W_IsIWAD(newwadfiles[0]))
		AddedIWAD = true;

	// Check our environment, if the same WADs are used, ignore this command.

	// Did we switch IWAD files?
	if (AddedIWAD && !wadfiles.empty())
	{
		if (!iequals(M_ExtractFileName(newwadfiles[0]), M_ExtractFileName(wadfiles[1])))
			Reboot = true;
	}

	// Do the sizes of the WAD lists not match up?
	if (!Reboot)
	{
		if (wadfiles.size() - 2 != newwadfiles.size() - (AddedIWAD ? 1 : 0))
			Reboot = true;
	}

	// Do our WAD lists match up exactly?
	if (!Reboot)
	{
		for (i = 2, j = (AddedIWAD ? 1 : 0); i < wadfiles.size() && j < newwadfiles.size(); i++, j++)
		{
			if (!iequals(M_ExtractFileName(newwadfiles[j]), M_ExtractFileName(wadfiles[i])))
			{
				Reboot = true;
				break;
			}
		}
	}

	// Do the sizes of the patch lists not match up?
	if (!Reboot)
	{
		if (patchfiles.size() != newpatchfiles.size())
			Reboot = true;
	}

	// Do our patchfile lists match up exactly?
	if (!Reboot)
	{
		for (i = 0, j = 0; i < patchfiles.size() && j < newpatchfiles.size(); i++, j++)
		{
			if (!iequals(M_ExtractFileName(newpatchfiles[j]), M_ExtractFileName(patchfiles[i])))
			{
				Reboot = true;
				break;
			}
		}
	}

	if (Reboot)
	{
		unnatural_level_progression = true;

		// [SL] Stop any playing/recording demos before D_DoomWadReboot wipes out
		// the zone memory heap and takes the demo data with it.
#ifdef CLIENT_APP
		{
			G_CheckDemoStatus();
		}
#endif
		D_DoomWadReboot(newwadfiles, newpatchfiles, newwadhashes, newpatchhashes);
		if (!missingfiles.empty())
		{
			G_DeferedInitNew(startmap);
			return false;
		}
	}

	if (mapname.length())
	{
		if (W_CheckNumForName(mapname.c_str()) != -1)
            G_DeferedInitNew((char *)mapname.c_str());
        else
        {
            Printf(PRINT_HIGH, "map %s not found, loading start map instead", mapname.c_str());
            G_DeferedInitNew(startmap);
        }
	}
	else
		G_DeferedInitNew(startmap);

	return true;
}

const char *ParseString2(const char *data);

//
// G_LoadWad
//
// Takes a space-separated string list of wad and patch names, which is parsed
// into a vector of wad filenames and patch filenames and then calls
// D_DoomWadReboot.
//
bool G_LoadWad(const std::string &str, const std::string &mapname)
{
	std::vector<std::string> newwadfiles;
	std::vector<std::string> newpatchfiles;
	std::vector<std::string> nohashes;	// intentionally empty

	const char *data = str.c_str();

	for (size_t argv = 0; (data = ParseString2(data)); argv++)
	{
		std::string ext;

		if (argv == 0 && W_IsIWAD(com_token))
		{
			// Add an IWAD
			std::string iwad_name(com_token);

			// The first argument in the string can be the name of an IWAD
			// with the WAD extension omitted
			M_AppendExtension(iwad_name, ".wad");

			newwadfiles.push_back(iwad_name);
		}
		else if (M_ExtractFileExtension(com_token, ext))
		{
			if (iequals(ext, "wad") && !W_IsIWAD(com_token))
				newwadfiles.push_back(com_token);
			else if (iequals(ext, "deh") || iequals(ext, "bex"))
				newpatchfiles.push_back(com_token);		// Patch file
		}
	}

	return G_LoadWad(newwadfiles, newpatchfiles, nohashes, nohashes, mapname);
}


BEGIN_COMMAND (map)
{
	if (argc > 1)
	{
		char mapname[32];

		// [Dash|RD] -- We can make a safe assumption that the user might not specify
		//              the whole lumpname for the level, and might opt for just the
		//              number. This makes sense, so why isn't there any code for it?
		if (W_CheckNumForName (argv[1]) == -1 && isdigit(argv[1][0]))
		{ // The map name isn't valid, so lets try to make some assumptions for the user.

			// If argc is 2, we assume Doom 2/Final Doom. If it's 3, Ultimate Doom.
            // [Russell] - gamemode is always the better option compared to above
			if ( argc == 2 )
			{
				if ((gameinfo.flags & GI_MAPxx))
                    sprintf( mapname, "MAP%02i", atoi( argv[1] ) );
                else
                    sprintf( mapname, "E%cM%c", argv[1][0], argv[1][1]);

			}

			if (W_CheckNumForName (mapname) == -1)
			{ // Still no luck, oh well.
				Printf (PRINT_HIGH, "Map %s not found.\n", argv[1]);
			}
			else
			{ // Success
				unnatural_level_progression = true;
				G_DeferedInitNew (mapname);
			}

		}
		else
		{
			// Ch0wW - Map was still not found, so don't bother trying loading the map.
			if (W_CheckNumForName (argv[1]) == -1)
			{
				Printf (PRINT_HIGH, "Map %s not found.\n", argv[1]);
			}
			else
			{
				unnatural_level_progression = true;
				uppercopy(mapname, argv[1]); // uppercase the mapname
				G_DeferedInitNew (mapname);
			}
		}
	}
	else
	{
		Printf (PRINT_HIGH, "The current map is %s: \"%s\"\n", level.mapname, level.level_name);
	}
}
END_COMMAND (map)

char *CalcMapName (int episode, int level)
{
	static char lumpname[9];

	if (gameinfo.flags & GI_MAPxx)
	{
		sprintf (lumpname, "MAP%02d", level);
	}
	else
	{
		lumpname[0] = 'E';
		lumpname[1] = '0' + episode;
		lumpname[2] = 'M';
		lumpname[3] = '0' + level;
		lumpname[4] = 0;
	}
	return lumpname;
}

level_info_t* FindLevelInfo(char* mapname)
{
	int i;

	if ((i = FindWadLevelInfo(mapname)) > -1)
	{
		return(level_info_t*)(&wadlevelinfos[i]);
	}

	I_Error("Could not find level info for %s\n", mapname);
}

level_info_t* FindLevelByNum(int num)
{
	for (size_t i = 0; i < wadlevelinfos.size(); i++)
	{
		if (wadlevelinfos[i].levelnum == num)
		{
			return (level_info_t*)(&wadlevelinfos[i]);
		}
	}

	I_Error("Could not find level info for level number %d\n", num);
}

cluster_info_t* FindClusterInfo(int cluster)
{
	int i;

	if ((i = FindWadClusterInfo(cluster)) > -1)
	{
		return &wadclusterinfos[i];
	}

	I_Error("Could not find culster info for culster number %d\n", i);
}

void G_AirControlChanged ()
{
	if (level.aircontrol <= 256)
	{
		level.airfriction = FRACUNIT;
	}
	else
	{
		// Friction is inversely proportional to the amount of control
		float fric = ((float)level.aircontrol/65536.f) * -0.0941f + 1.0004f;
		level.airfriction = (fixed_t)(fric * 65536.f);
	}
}

// Serialize or unserialize the state of the level depending on the state of
// the first parameter.  Second parameter is true if you need to deal with hub
// playerstate.  Third parameter is true if you want to handle playerstate
// yourself (map resets), just make sure you set it the same for both
// serialization and unserialization.
void G_SerializeLevel(FArchive &arc, bool hubLoad, bool noStorePlayers)
{
	if (arc.IsStoring ())
	{
		unsigned int playernum = players.size();
		arc << level.flags
			<< level.fadeto_color[0] << level.fadeto_color[1] << level.fadeto_color[2] << level.fadeto_color[3]
			<< level.found_secrets
			<< level.found_items
			<< level.killed_monsters
			<< level.gravity
			<< level.aircontrol;

		G_AirControlChanged();

		for (int i = 0; i < NUM_MAPVARS; i++)
			arc << level.vars[i];

		if (!noStorePlayers)
			arc << playernum;
	}
	else
	{
		unsigned int playernum;
		arc >> level.flags
			>> level.fadeto_color[0] >> level.fadeto_color[1] >> level.fadeto_color[2] >> level.fadeto_color[3]
			>> level.found_secrets
			>> level.found_items
			>> level.killed_monsters
			>> level.gravity
			>> level.aircontrol;

		G_AirControlChanged();

		for (int i = 0; i < NUM_MAPVARS; i++)
			arc >> level.vars[i];

		if (!noStorePlayers)
		{
			arc >> playernum;
			players.resize(playernum);
		}
	}

	if (!hubLoad && !noStorePlayers)
		P_SerializePlayers(arc);

	P_SerializeThinkers(arc, hubLoad, noStorePlayers);
	P_SerializeWorld(arc);
	P_SerializePolyobjs(arc);
	P_SerializeSounds(arc);
}

// Archives the current level
void G_SnapshotLevel ()
{
	delete level.info->snapshot;

	level.info->snapshot = new FLZOMemFile;
	level.info->snapshot->Open ();

	FArchive arc (*level.info->snapshot);

	G_SerializeLevel (arc, false, false);
}

// Unarchives the current level based on its snapshot
// The level should have already been loaded and setup.
void G_UnSnapshotLevel (bool hubLoad)
{
	if (level.info->snapshot == NULL)
		return;

	level.info->snapshot->Reopen ();
	FArchive arc (*level.info->snapshot);
	if (hubLoad)
		arc.SetHubTravel (); // denis - hexen?
	G_SerializeLevel (arc, hubLoad, false);
	arc.Close ();
	// No reason to keep the snapshot around once the level's been entered.
	delete level.info->snapshot;
	level.info->snapshot = NULL;
}

void G_ClearSnapshots (void)
{
	for (size_t i = 0; i < wadlevelinfos.size(); i++)
	{
		if (wadlevelinfos[i].snapshot)
		{
			delete wadlevelinfos[i].snapshot;
			wadlevelinfos[i].snapshot = NULL;
		}
	}
}

static void writeSnapShot (FArchive &arc, level_info_t *i)
{
	arc.Write (i->mapname, 8);
	i->snapshot->Serialize (arc);
}

void G_SerializeSnapshots (FArchive &arc)
{
	if (arc.IsStoring())
	{
		for (size_t i = 0; i < wadlevelinfos.size(); i++)
		{
			if (wadlevelinfos[i].snapshot)
			{
				writeSnapShot(arc, (level_info_t*)&wadlevelinfos[i]);
			}
		}

		// Signal end of snapshots
		arc << (char)0;
	}
	else
	{
		char mapname[8];

		G_ClearSnapshots ();

		arc >> mapname[0];
		while (mapname[0])
		{
			arc.Read(&mapname[1], 7);
			level_info_t* i = FindLevelInfo(mapname);
			i->snapshot = new FLZOMemFile;
			i->snapshot->Serialize(arc);
			arc >> mapname[0];
		}
	}
}

static void writeDefereds (FArchive &arc, level_info_t *i)
{
	arc.Write (i->mapname, 8);
	arc << i->defered;
}

void P_SerializeACSDefereds(FArchive &arc)
{
	if (arc.IsStoring())
	{
		for (size_t i = 0; i < wadlevelinfos.size(); i++)
		{
			if (wadlevelinfos[i].defered)
			{
				writeDefereds(arc, (level_info_t*)&wadlevelinfos[i]);
			}
		}

		// Signal end of defereds
		arc << (byte)0;
	}
	else
	{
		char mapname[8];

		P_RemoveDefereds();

		arc >> mapname[0];
		while (mapname[0])
		{
			arc.Read(&mapname[1], 7);
			level_info_t* i = FindLevelInfo(mapname);
			if (i == NULL)
			{
				char name[9];

				strncpy(name, mapname, 8);
				name[8] = 0;
				I_Error("Unknown map '%s' in savegame", name);
			}
			arc >> i->defered;
			arc >> mapname[0];
		}
	}
}

static int		startpos;	// [RH] Support for multiple starts per level

void G_DoWorldDone (void)
{
	gamestate = GS_LEVEL;
	if (wminfo.next[0] == 0) {
		// Don't die if no next map is given,
		// just repeat the current one.
		Printf (PRINT_HIGH, "No next map specified.\n");
	} else {
		strncpy (level.mapname, wminfo.next, 8);
	}
	G_DoLoadLevel (startpos);
	startpos = 0;
	gameaction = ga_nothing;
	viewactive = true;
}


extern dyncolormap_t NormalLight;

EXTERN_CVAR (sv_gravity)
EXTERN_CVAR (sv_aircontrol)
EXTERN_CVAR (sv_allowjump)
EXTERN_CVAR (sv_freelook)

void G_InitLevelLocals()
{
	byte old_fadeto_color[4];
	memcpy(old_fadeto_color, level.fadeto_color, 4);

	level_info_t *info;
	int i;

	R_ExitLevel();

	NormalLight.maps = shaderef_t(&realcolormaps, 0);
	//NormalLight.maps = shaderef_t(&DefaultPalette->maps, 0);

	level.gravity = sv_gravity;
	level.aircontrol = (fixed_t)(sv_aircontrol * 65536.f);
	G_AirControlChanged();

	// clear all ACS variables
	memset(level.vars, 0, sizeof(level.vars));

	if ((i = FindWadLevelInfo(level.mapname)) == -1)
	{
		I_Error("Could not find level info for %s\n");
	}
	level_pwad_info_t* pinfo = &wadlevelinfos[i];

	// [ML] 5/11/06 - Remove sky scrolling and sky2
	// [SL] 2012-03-19 - Add sky2 back
	level.info = (level_info_t*)pinfo;
	info = (level_info_t*)pinfo;
	strncpy (level.skypic2, pinfo->skypic2, 8);

	memcpy(level.fadeto_color, pinfo->fadeto_color, 4);
	
	if (level.fadeto_color[0] || level.fadeto_color[1] || level.fadeto_color[2] || level.fadeto_color[3])
		NormalLight.maps = shaderef_t(&V_GetDefaultPalette()->maps, 0);
	else
		R_ForceDefaultColormap(pinfo->fadetable);

	memcpy(level.outsidefog_color, pinfo->outsidefog_color, 4);

	level.flags |= LEVEL_DEFINEDINMAPINFO;
	if (pinfo->gravity != 0.f)
		level.gravity = pinfo->gravity;
	if (pinfo->aircontrol != 0.f)
		level.aircontrol = (fixed_t)(pinfo->aircontrol * 65536.f);

	if (info->level_name)
	{
		level.partime = info->partime;
		level.cluster = info->cluster;
		level.flags = info->flags;
		level.levelnum = info->levelnum;

		strncpy(level.level_name, info->level_name, 63);
		strncpy(level.nextmap, info->nextmap, 8);
		strncpy(level.secretmap, info->secretmap, 8);
		strncpy(level.music, info->music, 8);
		strncpy(level.skypic, info->skypic, 8);
		if (!level.skypic2[0])
			strncpy(level.skypic2, level.skypic, 8);
	}
	else
	{
		level.partime = level.cluster = 0;
		strcpy(level.level_name, "Unnamed");
		level.nextmap[0] = level.secretmap[0] = level.music[0] = 0;
		strncpy(level.skypic, "SKY1", 8);
		strncpy(level.skypic2, "SKY1", 8);
		level.flags = 0;
		level.levelnum = 1;
	}
	
	if (level.flags & LEVEL_JUMP_YES)
		sv_allowjump = 1;
	if (level.flags & LEVEL_JUMP_NO)
		sv_allowjump = 0.0;
	if (level.flags & LEVEL_FREELOOK_YES)
		sv_freelook = 1;
	if (level.flags & LEVEL_FREELOOK_NO)
		sv_freelook = 0.0;

//	memset (level.vars, 0, sizeof(level.vars));

	if (memcmp(level.fadeto_color, old_fadeto_color, 4) != 0)
		V_RefreshColormaps();

	movingsectors.clear();
}

VERSION_CONTROL (g_level_cpp, "$Id$")
