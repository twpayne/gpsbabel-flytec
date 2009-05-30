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
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MYNAME "flytec"


static char *flytec_debug = 0;
static
arglist_t flytec_args[] = {
	{ "debug", &flytec_debug, "Enable debugging", NULL, ARGTYPE_BOOL, ARG_NOMINMAX },
	ARG_TERMINATOR
};

#define DIE(syscall, _errno) die(__FILE__, __LINE__, __FUNCTION__, (syscall), (_errno))

#define DATE_NEW(tm) (((tm).tm_year << 9) + ((tm).tm_mon << 5) + (tm).tm_mday)
#define DATE_YEAR(date) ((date) >> 9)
#define DATE_MON(date) (((date) >> 5) & 0xf)
#define DATE_MDAY(date) ((date) & 0x1f)

enum {
	XON = '\x11',
	XOFF = '\x13',
};

static const char base36[36] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

typedef struct {
	char *instrument_id;
	char *pilot_name;
	int serial_number;
	char *software_version;
} snp_t;

typedef struct {
	int count;
	int index;
	int date;
	int day_index;
	time_t time;
	int duration;
	char *igc_filename;
} track_t;

typedef struct {
	const char *device;
	int fd;
	FILE *logfile;
	snp_t *snp;
	const char *manufacturer;
	char *pilot_name;
	int serial_number;
	int trackc;
	track_t **trackv;
	int waypoint_count;
	int waypoint_index;
	char *next;
	char *end;
	char buf[128];
} flytec_t;

	static void
die(const char *file, int line, const char *function, const char *message, int _errno)
{
	if (_errno)
		fatal(MYNAME "%s:%d: %s: %s: %s", file, line, function, message, strerror(_errno));
	else
		fatal(MYNAME "%s:%d: %s: %s", file, line, function, message);
}

	static char *
rstrip(char *s)
{
	char *p = s + strlen(s) - 1;
	while (p >= s && *p == ' ')
		*p-- = '\0';
	return s;
}

	static inline const char *
match_char(const char *p, char c)
{
	if (!p)
		return 0;
	return *p == c ? ++p : 0;
}

	static inline const char *
match_literal(const char *p, const char *s)
{
	if (!p)
		return 0;
	while (*p && *s && *p == *s) {
		++p;
		++s;
	}
	return *s ? 0 : p;
}

	static inline const char *
match_n_digits(const char *p, int n, int *result)
{
	if (!p)
		return 0;
	*result = 0;
	for (; n > 0; --n) {
		if ('0' <= *p && *p <= '9') {
			*result = 10 * *result + *p - '0';
			++p;
		} else {
			return 0;
		}
	}
	return p;
}

	static inline const char *
match_unsigned(const char *p, int *result)
{
	if (!p)
		return 0;
	if (!isdigit(*p))
		return 0;
	*result = *p - '0';
	++p;
	while (isdigit(*p)) {
		*result = 10 * *result + *p - '0';
		++p;
	}
	return p;
}

	static inline const char *
match_one_of(const char *p, const char *s, char *result)
{
	if (!p)
		return 0;
	for (; *s; ++s)
		if (*p == *s) {
			*result = *p;
			return ++p;
		}
	return 0;
}

	static inline const char *
match_string_until(const char *p, char c, int consume, char **result)
{
	if (!p)
		return 0;
	const char *start = p;
	while (*p && *p != c)
		++p;
	if (!p)
		return 0;
	if (result) {
		*result = xmalloc(p - start + 1);
		memcpy(*result, start, p - start);
		(*result)[p - start] = '\0';
	}
	return consume ? ++p : p;
}

	static inline const char *
match_until_eol(const char *p)
{
	if (!p)
		return 0;
	while (*p && *p != '\r')
		++p;
	if (*p != '\r')
		return 0;
	++p;
	return *p == '\n' ? ++p : 0;
}

	static inline const char *
match_eos(const char *p)
{
	if (!p)
		return 0;
	return *p ? 0 : p;
}

	static const char *
match_b_record(const char *p, struct tm *tm, route_head **track)
{
	waypoint *w = waypt_new();
	p = match_char(p, 'B');
	struct tm _tm = *tm;
	p = match_n_digits(p, 2, &_tm.tm_hour);
	p = match_n_digits(p, 2, &_tm.tm_min);
	p = match_n_digits(p, 2, &_tm.tm_sec);
	w->creation_time = mktime(&_tm);
	int lat = 0, lat_min = 0, lat_mmin = 0;
	p = match_n_digits(p, 2, &lat);
	p = match_n_digits(p, 2, &lat_min);
	p = match_n_digits(p, 3, &lat_mmin);
	w->latitude = lat + lat_min / 60.0 + lat_mmin / 60000.0;
	char lat_hemi = 0;
	p = match_one_of(p, "NS", &lat_hemi);
	if (lat_hemi == 'S')
		w->latitude = -w->latitude;
	int lon = 0, lon_min = 0, lon_mmin = 0;
	p = match_n_digits(p, 3, &lon);
	p = match_n_digits(p, 2, &lon_min);
	p = match_n_digits(p, 3, &lon_mmin);
	w->longitude = lon + lon_min / 60.0 + lon_mmin / 60000.0;
	char lon_hemi = 0;
	p = match_one_of(p, "EW", &lon_hemi);
	if (lon_hemi == 'W')
		w->longitude = -w->longitude;
	char av = 0;
	p = match_one_of(p, "AV", &av);
	switch (av) {
		case 'A':
			w->fix = fix_3d;
			break;
		case 'V':
			w->fix = fix_2d;
			break;
	}
	int alt = 0, ele = 0;
	p = match_n_digits(p, 5, &alt);
	w->altitude = alt;
	p = match_n_digits(p, 5, &ele);
	p = match_until_eol(p);
	if (!p)
		goto error;
	if (!*track) {
		*track = route_head_alloc();
		track_add_head(*track);
	}
	track_add_wpt(*track, w);
	return p;
error:
	waypt_del(w);
	return 0;
}

	static const char *
match_hfdte_record(const char *p, struct tm *tm)
{
	int mday = 0, mon = 0, year = 0;
	p = match_literal(p, "HFDTE");
	if (!p)
		return 0;
	p = match_n_digits(p, 2, &mday);
	p = match_n_digits(p, 2, &mon);
	p = match_n_digits(p, 2, &year);
	p = match_literal(p, "\r\n");
	if (!p)
		return 0;
	tm->tm_year = year + 2000 - 1900;
	tm->tm_mon = mon - 1;
	tm->tm_mday = mday;
	return p;
}

	static const char *
manufacturer_new(const char *instrument_id)
{
	if (
			!strcmp(instrument_id, "5020") ||
			!strcmp(instrument_id, "5030") ||
			!strcmp(instrument_id, "6020") ||
			!strcmp(instrument_id, "6030")
	   )
		return "FLY";
	else if (
			!strcmp(instrument_id, "COMPEO") ||
			!strcmp(instrument_id, "COMPEO+") ||
			!strcmp(instrument_id, "COMPETINO") ||
			!strcmp(instrument_id, "COMPETINO+") ||
			!strcmp(instrument_id, "GALILEO")
			)
		return "BRA";
	else
		return "XXX";
}

	static void
snp_delete(snp_t *snp)
{
	if (snp) {
		free(snp->instrument_id);
		free(snp->pilot_name);
		free(snp->software_version);
		free(snp);
	}
}

	static snp_t *
snp_new(const char *p)
{
	snp_t *snp = xmalloc(sizeof(snp_t));
	memset(snp, 0, sizeof *snp);
	p = match_literal(p, "PBRSNP,");
	p = match_string_until(p, ',', 1, &snp->instrument_id);
	p = match_string_until(p, ',', 1, &snp->pilot_name);
	p = match_unsigned(p, &snp->serial_number);
	p = match_char(p, ',');
	p = match_string_until(p, '\0', 0, &snp->software_version);
	p = match_eos(p);
	if (!p) {
		snp_delete(snp);
		return 0;
	}
	return snp;
}

	static void
track_delete(track_t *track)
{
	if (track) {
		free(track->igc_filename);
		free(track);
	}
}

	static track_t *
track_new(const char *p)
{
	track_t *track = xmalloc(sizeof(track_t));
	memset(track, 0, sizeof *track);
	p = match_literal(p, "PBRTL,");
	p = match_unsigned(p, &track->count);
	p = match_char(p, ',');
	p = match_unsigned(p, &track->index);
	p = match_char(p, ',');
	struct tm tm;
	memset(&tm, 0, sizeof tm);
	p = match_unsigned(p, &tm.tm_mday);
	p = match_char(p, '.');
	p = match_unsigned(p, &tm.tm_mon);
	p = match_char(p, '.');
	p = match_unsigned(p, &tm.tm_year);
	p = match_char(p, ',');
	p = match_unsigned(p, &tm.tm_hour);
	p = match_char(p, ':');
	p = match_unsigned(p, &tm.tm_min);
	p = match_char(p, ':');
	p = match_unsigned(p, &tm.tm_sec);
	p = match_char(p, ',');
	int duration_hour = 0, duration_min = 0, duration_sec = 0;
	p = match_unsigned(p, &duration_hour);
	p = match_char(p, ':');
	p = match_unsigned(p, &duration_min);
	p = match_char(p, ':');
	p = match_unsigned(p, &duration_sec);
	p = match_eos(p);
	if (!p) {
		track_delete(track);
		return 0;
	}
	tm.tm_mon -= 1;
	tm.tm_year += 2000 - 1900;
	track->date = DATE_NEW(tm);
	track->time = mktime(&tm);
	if (track->time == (time_t) -1)
		DIE("mktime", errno);
	track->duration = 3600 * duration_hour + 60 * duration_min + duration_sec;
	return track;
}

	static flytec_t *
flytec_new(const char *device, FILE *logfile)
{
	flytec_t *flytec = xmalloc(sizeof(flytec_t));
	memset(flytec, 0, sizeof *flytec);
	flytec->device = device;
	flytec->fd = open(flytec->device, O_NOCTTY | O_NONBLOCK | O_RDWR);
	if (flytec->fd == -1)
		fatal(MYNAME "open: %s: %s", flytec->device, strerror(errno));
	if (tcflush(flytec->fd, TCIOFLUSH) == -1)
		fatal(MYNAME "tcflush: %s: %s", flytec->device, strerror(errno));
	struct termios termios;
	memset(&termios, 0, sizeof termios);
	termios.c_iflag = IGNPAR;
	termios.c_cflag = CLOCAL | CREAD | CS8;
	cfsetispeed(&termios, B57600);
	cfsetospeed(&termios, B57600);
	if (tcsetattr(flytec->fd, TCSANOW, &termios) == -1)
		fatal(MYNAME "tcsetattr: %s: %s", flytec->device, strerror(errno));
	flytec->logfile = logfile;
	return flytec;
}

	static void
flytec_delete(flytec_t *flytec)
{
	if (flytec) {
		snp_delete(flytec->snp);
		if (flytec->trackv) {
			track_t **track;
			for (track = flytec->trackv; *track; ++track)
				track_delete(*track);
			free(flytec->trackv);
		}
		if (close(flytec->fd) == -1)
			DIE("close", errno);
		free(flytec->pilot_name);
		free(flytec);
	}
}

	static void
flytec_fill(flytec_t *flytec)
{
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(flytec->fd, &readfds);
	int rc;
	do {
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 250 * 1000;
		rc = select(flytec->fd + 1, &readfds, 0, 0, &timeout);
	} while (rc == -1 && errno == EINTR);
	if (rc == -1)
		DIE("select", errno);
	else if (rc == 0)
		fatal(MYNAME "%s: timeout waiting for data", flytec->device);
	else if (!FD_ISSET(flytec->fd, &readfds))
		DIE("select", 0);
	int n;
	do {
		n = read(flytec->fd, flytec->buf, sizeof flytec->buf);
	} while (n == -1 && errno == EINTR);
	if (n == -1)
		DIE("read", errno);
	else if (n == 0)
		DIE("read", 0);
	flytec->next = flytec->buf;
	flytec->end = flytec->buf + n;
}

	static int
flytec_getc(flytec_t *flytec)
{
	if (flytec->next == flytec->end)
		flytec_fill(flytec);
	if (flytec->next == flytec->end)
		return EOF;
	return *flytec->next++;
}

	static void
flytec_expectc(flytec_t *flytec, char c)
{
	if (flytec_getc(flytec) != c)
		fatal(MYNAME "%s: unexpected character", flytec->device);
}

	static void
flytec_puts_nmea(flytec_t *flytec, char *s)
{
	int checksum = 0;
	char *p;
	for (p = s; *p; ++p)
		checksum ^= (unsigned char) *p;
	int len = strlen(s) + 7;
	char *buf = xmalloc(len);
	memset(buf, 0, len);
	if (snprintf(buf, len, "$%s*%02X\r\n", s, checksum) != len - 1)
		DIE("snprintf", 0);
	if (flytec->logfile)
		fprintf(flytec->logfile, "> %s", buf);
	int rc;
	do {
		rc = write(flytec->fd, buf, len);
	} while (rc == -1 && errno == EINTR);
	if (rc == -1)
		DIE("write", errno);
	free(buf);
}

	static char *
flytec_gets(flytec_t *flytec, char *buf, int size)
{
	if (flytec->next == flytec->end)
		flytec_fill(flytec);
	if (*flytec->next == XON)
		return 0;
	int len = size;
	char *p = buf;
	while (1) {
		if (--len <= 0)
			DIE(__FUNCTION__, 0);
		if ((*p++ = *flytec->next++) == '\n') {
			*p = '\0';
			if (flytec->logfile)
				fprintf(flytec->logfile, "< %s", buf);
			return buf;
		}
		if (flytec->next == flytec->end)
			flytec_fill(flytec);
	}
}

	static char *
flytec_gets_nmea(flytec_t *flytec, char *buf, int size)
{
	buf = flytec_gets(flytec, buf, size);
	if (!buf)
		return 0;
	int len = strlen(buf);
	if (len < 6)
		goto _error;
	if (buf[0] != '$' || buf[len - 5] != '*' || buf[len - 2] != '\r' || buf[len - 1] != '\n')
		goto _error;
	int checksum = 0;
	char *p;
	for (p = buf + 1; p != buf + len - 5; ++p)
		checksum ^= (unsigned char) *p;
	int result = 0;
	char xdigit = buf[len - 4];
	if ('0' <= xdigit && xdigit <= '9')
		result = (xdigit - '0') << 4;
	else if ('A' <= xdigit && xdigit <= 'F')
		result = (xdigit - 'A' + 0xa) << 4;
	else
		goto _error;
	xdigit = buf[len - 3];
	if ('0' <= xdigit && xdigit <= '9')
		result += xdigit - '0';
	else if ('A' <= xdigit && xdigit <= 'F')
		result += xdigit - 'A' + 0xa;
	else
		goto _error;
	if (checksum != result)
		goto _error;
	memmove(buf, buf + 1, len - 5);
	buf[len - 6] = '\0';
	return buf;
_error:
	fatal(MYNAME "%s: invalid NMEA response", flytec->device);
	return 0;
}

	static void
flytec_pbrigc(flytec_t *flytec)
{
	flytec_puts_nmea(flytec, "PBRIGC,");
	flytec_expectc(flytec, XOFF);
	char line[128];
	route_head *track = 0;
	struct tm tm;
	memset(&tm, 0, sizeof tm);
	while (flytec_gets(flytec, line, sizeof line)) {
		switch (line[0]) {
			case 'B':
				match_b_record(line, &tm, &track);
				break;
			case 'H':
				match_hfdte_record(line, &tm);
				break;
		}
	}
	flytec_expectc(flytec, XON);
}

	static void
flytec_pbrrts(flytec_t *flytec)
{
	flytec_puts_nmea(flytec, "PBRRTS,");
	flytec_expectc(flytec, XOFF);
	route_head *route = 0;
	char line[128];
	while (flytec_gets_nmea(flytec, line, sizeof line)) {
		const char *p = line;
		p = match_literal(p, "PBRRTS,");
		int index = 0, count = 0, routepoint_index = 0;
		p = match_unsigned(p, &index);
		p = match_char(p, ',');
		p = match_unsigned(p, &count);
		p = match_char(p, ',');
		p = match_unsigned(p, &routepoint_index);
		p = match_char(p, ',');
		if (!p)
			continue;
		if (routepoint_index == 0) {
			char *name = 0;
			p = match_string_until(p, '\0', 0, &name);
			p = match_eos(p);
			if (p) {
				route = route_head_alloc();
				route->rte_num = index + 1;
				route->rte_name = rstrip(name);
				route_add_head(route);
			} else {
				free(name);
			}
		} else {
			char *name = 0;
			p = match_string_until(p, ',', 1, 0);
			p = match_string_until(p, '\0', 0, &name);
			p = match_eos(p);
			if (p) {
				const waypoint *w = find_waypt_by_name(rstrip(name));
				if (w)
					route_add_wpt(route, waypt_dupe(w));
			}
			free(name);
		}
	}
	flytec_expectc(flytec, XON);
}

	static snp_t *
flytec_pbrsnp(flytec_t *flytec)
{
	if (flytec->snp)
		return flytec->snp;
	flytec_puts_nmea(flytec, "PBRSNP,");
	flytec_expectc(flytec, XOFF);
	char line[128];
	if (!flytec_gets_nmea(flytec, line, sizeof line))
		DIE("flytec_gets_nmea", 0);
	flytec->snp = snp_new(line);
	if (!flytec->snp)
		fatal(MYNAME "%s: invalid response", flytec->device);
	flytec_expectc(flytec, XON);
	/* strip leading and trailing spaces from pilot name */
	char *pilot_name = flytec->snp->pilot_name;
	while (*pilot_name == ' ')
		++pilot_name;
	char *pilot_name_end = pilot_name + 1;
	char *p;
	for (p = pilot_name; *p; ++p)
		if (*p != ' ')
			pilot_name_end = p + 1;
	flytec->pilot_name = xmalloc(pilot_name_end - pilot_name + 1);
	memcpy(flytec->pilot_name, pilot_name, pilot_name_end - pilot_name);
	flytec->pilot_name[pilot_name_end - pilot_name] = '\0';
	flytec->serial_number = flytec->snp->serial_number;
	return flytec->snp;
}

	static track_t **
flytec_pbrtl(flytec_t *flytec, const char *manufacturer, int filename_format)
{
	if (flytec->trackv)
		return flytec->trackv;
	manufacturer = manufacturer ? manufacturer : flytec->manufacturer;
	flytec_pbrsnp(flytec);
	flytec_puts_nmea(flytec, "PBRTL,");
	flytec_expectc(flytec, XOFF);
	char line[128];
	int index = 0;
	while (flytec_gets_nmea(flytec, line, sizeof line)) {
		track_t *track = track_new(line);
		if (!track)
			fatal(MYNAME "%s: invalid response", flytec->device);
		if (track->index != index++)
			fatal(MYNAME "%s: inconsistent data", flytec->device);
		if (flytec->trackv) {
			if (track->count != flytec->trackc)
				fatal(MYNAME "%s: inconsistent data", flytec->device);
		} else {
			flytec->trackc = track->count;
			flytec->trackv = xmalloc((flytec->trackc + 1) * sizeof(track_t *));
			memset(flytec->trackv, 0, (flytec->trackc + 1) * sizeof(track_t *));
		}
		flytec->trackv[track->index] = track;
	}
	if (flytec->trackc) {
		/* calculate daily flight indexes */
		int i;
		flytec->trackv[flytec->trackc - 1]->day_index = 1;
		for (i = flytec->trackc - 2; i >= 0; --i) {
			if (flytec->trackv[i]->date == flytec->trackv[i + 1]->date)
				flytec->trackv[i]->day_index = flytec->trackv[i + 1]->day_index + 1;
			else
				flytec->trackv[i]->day_index = 1;
		}
		/* calculate igc filenames */
		for (i = 0; i < flytec->trackc; ++i) {
			track_t *track = flytec->trackv[i];
			char serial_number[4];
			int rc;
			switch (filename_format) {
				case 0:
					track->igc_filename = xmalloc(128);
					memset(track->igc_filename, 0, 128);
					rc = snprintf(track->igc_filename, 128, "%04d-%02d-%02d-%s-%d-%02d.IGC", DATE_YEAR(track->date) + 1900, DATE_MON(track->date) + 1, DATE_MDAY(track->date), manufacturer, flytec->serial_number, track->day_index);
					if (rc < 0 || rc > 128)
						fatal(MYNAME "snprintf");
					break;
				case 1:
					track->igc_filename = xmalloc(16);
					memset(track->igc_filename, 0, 16);
					serial_number[0] = base36[flytec->serial_number % 36];
					serial_number[1] = base36[(flytec->serial_number / 36) % 36];
					serial_number[2] = base36[(flytec->serial_number / 36 / 36) % 36];
					serial_number[3] = '\0';
					rc = snprintf(track->igc_filename, 16, "%c%c%c%c%s%c.IGC", base36[DATE_YEAR(track->date) % 10], base36[DATE_MON(track->date) + 1], base36[DATE_MDAY(track->date)], manufacturer[0], serial_number, base36[track->day_index]);
					if (rc < 0 || rc > 16)
						fatal(MYNAME "snprintf");
					break;
			}
		}
	} else {
		flytec->trackv = xmalloc(sizeof(track_t *));
		memset(flytec->trackv, 0, sizeof *flytec->trackv);
	}
	flytec_expectc(flytec, XON);
	return flytec->trackv;
}

	static void
flytec_pbrtr(flytec_t *flytec, track_t *track, void (*callback)(void *, const char *), void *data)
{
	char buf[9];
	if (snprintf(buf, sizeof buf, "PBRTR,%02d", track->index) != 8)
		DIE("sprintf", 0);
	flytec_puts_nmea(flytec, buf);
	flytec_expectc(flytec, XOFF);
	char line[1024];
	while (flytec_gets(flytec, line, sizeof line))
		callback(data, line);
	flytec_expectc(flytec, XON);
}

	static void
flytec_pbrwps(flytec_t *flytec)
{
	flytec_puts_nmea(flytec, "PBRWPS,");
	flytec_expectc(flytec, XOFF);
	char line[128];
	while (flytec_gets_nmea(flytec, line, sizeof line)) {
		const char *p = line;
		p = match_literal(p, "PBRWPS,");
		int lat_deg = 0, lat_min = 0, lat_mmin = 0;
		p = match_n_digits(p, 2, &lat_deg);
		p = match_n_digits(p, 2, &lat_min);
		p = match_char(p, '.');
		p = match_n_digits(p, 3, &lat_mmin);
		p = match_char(p, ',');
		char lat_hemi = '\0';
		p = match_one_of(p, "NS", &lat_hemi);
		p = match_char(p, ',');
		int lon_deg = 0, lon_min = 0, lon_mmin = 0;
		p = match_n_digits(p, 3, &lon_deg);
		p = match_n_digits(p, 2, &lon_min);
		p = match_char(p, '.');
		p = match_n_digits(p, 3, &lon_mmin);
		p = match_char(p, ',');
		char lon_hemi = '\0';
		p = match_one_of(p, "EW", &lon_hemi);
		p = match_char(p, ',');
		char *name = 0;
		p = match_string_until(p, ',', 1, 0);
		p = match_string_until(p, ',', 1, &name);
		int ele = 0;
		p = match_unsigned(p, &ele);
		p = match_eos(p);
		if (p) {
			waypoint *w = waypt_new();
			w->latitude = lat_deg + lat_min / 60.0 + lat_mmin / 60000.0;
			if (lat_hemi == 'S')
				w->latitude = -w->latitude;
			w->longitude = lon_deg + lon_min / 60.0 + lon_mmin / 60000.0;
			if (lon_hemi == 'W')
				w->longitude = -w->longitude;
			w->altitude = ele;
			w->shortname = rstrip(name);
			waypt_add(w);
		} else {
			free(name);
		}
	}
	flytec_expectc(flytec, XON);
}

/*******************************************************************************
 * %%%		 global callbacks called by gpsbabel main process			   %%% *
 *******************************************************************************/

static flytec_t *flytec_rd = 0;

	static void
flytec_rd_init(const char *fname)
{
	flytec_rd = flytec_new(fname, flytec_debug ? stderr : 0);
}

	static void 
flytec_rd_deinit(void)
{
	flytec_delete(flytec_rd);
}

	static void
flytec_read(void)
{
	flytec_pbrwps(flytec_rd);
	flytec_pbrrts(flytec_rd);
	char *tz = getenv("TZ");
	setenv("TZ", "", 1);
	tzset();
	flytec_pbrigc(flytec_rd);
	if (tz)
		setenv("TZ", tz, 1);
	else
		unsetenv("TZ");
	tzset();
}

static flytec_t *flytec_wr = 0;

	static void
flytec_wr_init(const char *fname)
{
	flytec_wr = flytec_new(fname, flytec_debug ? stderr : 0);
}

	static void
flytec_wr_deinit(void)
{
	flytec_delete(flytec_wr);
}

	static void
flytec_route_write_head(const route_head *r)
{
	flytec_wr->waypoint_count = r->rte_waypt_ct;
	flytec_wr->waypoint_index = 0;
	char name[18];
	strncpy(name, r->rte_name, sizeof name);
	name[sizeof name - 1] = '\0';
	char buffer[128];
	if (snprintf(buffer, sizeof buffer, "PBRRTR,99,%02d,00,%-17s", flytec_wr->waypoint_count + 1, name) != 33)
		fatal(MYNAME "snprintf");
	flytec_puts_nmea(flytec_wr, buffer);
	flytec_expectc(flytec_wr, XOFF);
	flytec_expectc(flytec_wr, XON);
}

	static void
flytec_route_write_waypoint(const waypoint *w)
{
	char name[18];
	strncpy(name, w->shortname, sizeof name);
	name[sizeof name - 1] = '\0';
	char buffer[128];
	if (snprintf(buffer, sizeof buffer, "PBRRTR,99,%02d,%02d,,%-17s", flytec_wr->waypoint_count + 1, ++flytec_wr->waypoint_index, name) != 34)
		fatal(MYNAME "snprintf");
	flytec_puts_nmea(flytec_wr, buffer);
	flytec_expectc(flytec_wr, XOFF);
	flytec_expectc(flytec_wr, XON);
}

	static void
flytec_waypoint_write(const waypoint *w)
{
	long lat = abs(60000.0 * w->latitude) + 0.5;
	long lat_deg = lat / 60000;
	double lat_min = (lat % 60000) / 1000.0;
	char lat_hemi = w->latitude < 0.0 ? 'S' : 'N';
	long lon = abs(60000.0 * w->longitude) + 0.5;
	long lon_deg = lon / 60000;
	double lon_min = (lon % 60000) / 1000.0;
	char lon_hemi = w->longitude < 0.0 ? 'W' : 'E';
	char name[18];
	strncpy(name, w->shortname, sizeof name);
	name[sizeof name - 1] = '\0';
	long ele = w->altitude;
	if (ele > 9999) {
		ele = 9999;
	} else if (ele < -999) {
		ele  = -999;
	}
	char buffer[64];
	if (snprintf(buffer, sizeof buffer, "PBRWPR,%02ld%06.3f,%c,%03ld%06.3f,%c,,%-17s,%04ld", lat_deg, lat_min, lat_hemi, lon_deg, lon_min, lon_hemi, name, ele) != 53)
		DIE("snprintf", errno);
	flytec_puts_nmea(flytec_wr, buffer);
	flytec_expectc(flytec_wr, XOFF);
	flytec_expectc(flytec_wr, XON);
}

	static void
flytec_write(void)
{
	waypt_disp_all(flytec_waypoint_write);
	route_disp_all(flytec_route_write_head, 0, flytec_route_write_waypoint);
}

	static void
flytec_exit(void)		/* optional */
{
}

/**************************************************************************/

// capabilities below means: we can only read and write waypoints
// please change this depending on your new module 

ff_vecs_t flytec_vecs = {
	ff_type_serial,
	{ 
		ff_cap_read | ff_cap_write /* waypoints */, 
		ff_cap_read                /* tracks */, 
		ff_cap_read | ff_cap_write /* routes */
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
