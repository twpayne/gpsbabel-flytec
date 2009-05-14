/*

    Braeuniger/Flytec serial protocol.

    Copyright (C) 2009  Tom Payne, twpayne@gmail.com
    Copyright (C) 2005  Robert Lipe, robertlipe@usa.net

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111 USA

 */

#include "defs.h"
#include <ctype.h>

#define MYNAME "flytec"


// Any arg in this list will appear in command line help and will be 
// populated for you.
// Values for ARGTYPE_xxx can be found in defs.h and are used to 
// select the type of option.
static
arglist_t flytec_args[] = {
// {"foo", &fooopt, "The text of the foo option in help", 
//   "default", ARGYTPE_STRING, ARG_NOMINMAX} , 
	ARG_TERMINATOR
};

/*******************************************************************************
* %%%        global callbacks called by gpsbabel main process              %%% *
*******************************************************************************/

static void
flytec_rd_init(const char *fname)
{
//	fin = gbfopen(fname, "r", MYNAME);
}

static void 
flytec_rd_deinit(void)
{
//	gbfclose(fin);
}

static void
flytec_read(void)
{
//	your special code to extract waypoint, route and track
//	information from gbfile "fin"
//
// Sample text-file read code:
//	char *s;
//	while ((s = gbfgetstr(fin))) {
//		do_anything(s);
//	}
//
//
// For waypoints:
//         while (have waypoints) {
//                 waypoint = waypt_new()
//                 populate waypoint
//                 waypt_add(waypoint);
//         }
// 
// For routes:
// 
//         route = route_head_alloc();
//         populate struct route_hdr
//	   route_add_head(route);      
//         while (have more routepoints) {
//                 waypoint = waypt_new()
//                 populate waypoint
//                 route_add_wpt(route, waypoint)
//         }
// 
// Tracks are just like routes, except the word "track" replaces "routes".
//
}

static void
flytec_wr_init(const char *fname)
{
//	fout = gbfopen(fname, "w", MYNAME);
}

static void
flytec_wr_deinit(void)
{
//	gbfclose(fout);
}

static void
flytec_write(void)
{
// Here is how you register callbacks for all waypoints, routes, tracks.
// waypt_disp_all(waypt)
// route_disp_all(head, tail, rtept);
// track_disp_all(head, tail, trkpt);
}

static void
flytec_exit(void)		/* optional */
{
}

/**************************************************************************/

// capabilities below means: we can only read and write waypoints
// please change this depending on your new module 

ff_vecs_t flytec_vecs = {
	ff_type_file,
	{ 
		ff_cap_read | ff_cap_write 	/* waypoints */, 
	  	ff_cap_none 			/* tracks */, 
	  	ff_cap_none 			/* routes */
	},
	flytec_rd_init,	
	flytec_wr_init,	
	flytec_rd_deinit,	
	flytec_wr_deinit,	
	flytec_read,
	flytec_write,
	flytec_exit,
	flytec_args,
	CET_CHARSET_ASCII, 0			/* ascii is the expected character set */
						/* not fixed, can be changed through command line parameter */
};
/**************************************************************************/
