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

static const char *program_name = "gpsbabel";

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
	char *next;
	char *end;
	char buf[128];
} flytec_t;

	static void
error(const char *message, ...)
{
    fprintf(stderr, "%s: ", program_name);
    va_list ap;
    va_start(ap, message);
    vfprintf(stderr, message, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

	static void
die(const char *file, int line, const char *function, const char *message, int _errno)
{
	if (_errno)
		error("%s:%d: %s: %s: %s", file, line, function, message, strerror(_errno));
	else
		error("%s:%d: %s: %s", file, line, function, message);
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
	if (!p) return 0;
	*result = xmalloc(p - start + 1);
	memcpy(*result, start, p - start);
	(*result)[p - start] = '\0';
	return consume ? ++p : p;
}

	static inline const char *
match_until_eol(const char *p)
{
	if (!p)
		return 0;
	while (*p && *p != '\r')
		++p;
	if (*p != '\r') return 0;
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
match_b_record(const char *p, struct tm *tm)
{
	p = match_char(p, 'B');
	if (!p)
		return 0;
	int hour = 0, min = 0, sec = 0;
	p = match_n_digits(p, 2, &hour);
	p = match_n_digits(p, 2, &min);
	p = match_n_digits(p, 2, &sec);
	if (!p)
		return 0;
	p = match_until_eol(p);
	if (!p)
		return 0;
	tm->tm_hour = hour;
	tm->tm_min = min;
	tm->tm_sec = sec;
	return p;
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

	static waypoint *
waypoint_new(const char *p)
{
	waypoint *w = waypt_new();
	p = match_literal(p, "PBRWPS,");
	int lat_deg = 0, lat_min = 0, lat_mmin = 0;
	p = match_n_digits(p, 2, &lat_deg);
	p = match_n_digits(p, 2, &lat_min);
	p = match_char(p, '.');
	p = match_n_digits(p, 3, &lat_mmin);
	w->latitude = (60000 * lat_deg + 1000 * lat_min + lat_mmin) / 60000.0;
	p = match_char(p, ',');
	char lat_hemi = 'N';
	p = match_one_of(p, "NS", &lat_hemi);
	if (lat_hemi == 'S')
		w->latitude = -w->latitude;
	p = match_char(p, ',');
	int lon_deg = 0, lon_min = 0, lon_mmin = 0;
	p = match_n_digits(p, 3, &lon_deg);
	p = match_n_digits(p, 2, &lon_min);
	p = match_char(p, '.');
	p = match_n_digits(p, 3, &lon_mmin);
	w->longitude = (60000 * lon_deg + 1000 * lon_min + lon_mmin) / 60000.0;
	p = match_char(p, ',');
	char lon_hemi = 'E';
	p = match_one_of(p, "EW", &lon_hemi);
	if (lon_hemi == 'W')
		w->longitude = -w->longitude;
	p = match_char(p, ',');
	p = match_string_until(p, ',', 1, &w->shortname);
	char *c = w->shortname + strlen(w->shortname) - 1;
	while (c > w->shortname && *c == ' ')
		*c-- = '\0';
	p = match_string_until(p, ',', 1, &w->description);
	c = w->description + strlen(w->description) - 1;
	while (c > w->description && *c == ' ')
		*c-- = '\0';
	int ele = 0;
	p = match_unsigned(p, &ele);
	w->altitude = ele;
	return w;
}

	static flytec_t *
flytec_new(const char *device, FILE *logfile)
{
	flytec_t *flytec = xmalloc(sizeof(flytec_t));
	memset(flytec, 0, sizeof *flytec);
	flytec->device = device;
	flytec->fd = open(flytec->device, O_NOCTTY | O_NONBLOCK | O_RDWR);
	if (flytec->fd == -1)
		error("open: %s: %s", flytec->device, strerror(errno));
	if (tcflush(flytec->fd, TCIOFLUSH) == -1)
		error("tcflush: %s: %s", flytec->device, strerror(errno));
	struct termios termios;
	memset(&termios, 0, sizeof termios);
	termios.c_iflag = IGNPAR;
	termios.c_cflag = CLOCAL | CREAD | CS8;
	cfsetispeed(&termios, B57600);
	cfsetospeed(&termios, B57600);
	if (tcsetattr(flytec->fd, TCSANOW, &termios) == -1)
		error("tcsetattr: %s: %s", flytec->device, strerror(errno));
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
		error("%s: timeout waiting for data", flytec->device);
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
		error("%s: unexpected character", flytec->device);
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
	error("%s: invalid NMEA response", flytec->device);
	return 0;
}

	static void
flytec_pbrigc(flytec_t *flytec, void (*callback)(void *, const char *), void *data)
{
	flytec_puts_nmea(flytec, "PBRIGC,");
	flytec_expectc(flytec, XOFF);
	char line[128];
	while (flytec_gets(flytec, line, sizeof line))
		callback(data, line);
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
		error("%s: invalid response", flytec->device);
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
			error("%s: invalid response", flytec->device);
		if (track->index != index++)
			error("%s: inconsistent data", flytec->device);
		if (flytec->trackv) {
			if (track->count != flytec->trackc)
				error("%s: inconsistent data", flytec->device);
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
						error("snprintf");
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
						error("snprintf");
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
flytec_pbrwpr(flytec_t *flytec, const waypoint *w)
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
	int result = snprintf(name, sizeof name, "%s %s", w->shortname, w->description);
	if (result < 0)
		DIE("snprintf", errno);
	long ele = abs(w->altitude) + 0.5;
	if (ele > 9999) {
		ele = 9999;
	} else if (ele < -999) {
		ele  = -999;
	}
	char buffer[64];
	result = snprintf(buffer, sizeof buffer, "PBRWPR,%02ld%06.3f,%c,%03ld%06.3f,%c,,%-17s,%04ld", lat_deg, lat_min, lat_hemi, lon_deg, lon_min, lon_hemi, name, ele);
	if (result < 0 || sizeof buffer <= result)
		DIE("snprintf", errno);
	flytec_puts_nmea(flytec, buffer);
	flytec_expectc(flytec, XOFF);
	flytec_expectc(flytec, XON);
}

	static void flytec_pbrwps(flytec_t *flytec)
{
	flytec_puts_nmea(flytec, "PBRWPS,");
	flytec_expectc(flytec, XOFF);
	char line[128];
	while (flytec_gets_nmea(flytec, line, sizeof line)) {
		waypt_add(waypoint_new(line));
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
	flytec_rd = flytec_new(fname, 0);
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
}

static flytec_t *flytec_wr = 0;

static void
flytec_wr_init(const char *fname)
{
	flytec_wr = flytec_new(fname, 0);
}

static void
flytec_wr_deinit(void)
{
	flytec_delete(flytec_wr);
}

static void
flytec_waypoint_write(const waypoint *w)
{
	flytec_pbrwpr(flytec_wr, w);
}

static void
flytec_write(void)
{
	waypt_disp_all(flytec_waypoint_write);
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
		ff_cap_read | ff_cap_write	/* waypoints */, 
		ff_cap_none				/* tracks */, 
		ff_cap_none				/* routes */
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
