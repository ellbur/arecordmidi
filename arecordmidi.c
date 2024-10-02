/*
 * arecordmidi.c - record standard MIDI files from sequencer ports
 *
 * Copyright (c) 2004-2005 Clemens Ladisch <clemens@ladisch.de>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

/* TODO: sequencer queue timer selection */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <sys/poll.h>
#include <alsa/asoundlib.h>
#include "version.h"
#include <stdbool.h>

#define BUFFER_SIZE 4088

/* linked list of buffers, stores data as in the .mid file */
struct buffer {
	struct buffer *next;
	unsigned char buf[BUFFER_SIZE];
};

struct smf_track {
	int size;			/* size of entire data */
	int cur_buf_size;		/* size of cur_buf */
	struct buffer *cur_buf;
	snd_seq_tick_time_t last_tick;	/* end of track */
	unsigned char last_command;	/* used for running status */
	struct buffer first_buf;	/* list head */
};

static snd_seq_t *seq;
static int client;
static bool got_a_port;
static snd_seq_addr_t port;
static int queue;
static int smpte_timing = 0;
static int beats = 120;
static int frames;
static int ticks = 0;
static int timeout = 0;
static FILE *file;
static long size_offset;
static struct smf_track track = { };
static volatile sig_atomic_t stop = 0;
static int ts_num = 4; /* time signature: numerator */
static int ts_div = 4; /* time signature: denominator */
static int ts_dd = 2; /* time signature: denominator as a power of two */
static snd_seq_tick_time_t t_start = 0;


/* prints an error message to stderr, and dies */
static void fatal(const char *msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	vfprintf(stderr, msg, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

/* error handling for ALSA functions */
static void check_snd(const char *operation, int err)
{
	if (err < 0)
		fatal("Cannot %s - %s", operation, snd_strerror(err));
}

static void init_seq(void)
{
	int err;

	/* open sequencer */
	err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	check_snd("open sequencer", err);

	/* find out our client's id */
	client = snd_seq_client_id(seq);
	check_snd("get client id", client);

	/* set our client's name */
	err = snd_seq_set_client_name(seq, "arecordmidi");
	check_snd("set client name", err);
}

/* parses one or more port addresses from the string */
static void parse_port(const char *arg)
{
	const char *port_name;
	int err;

	port_name = arg;
	
	if (strchr(port_name, ',') != NULL) {
		fatal("Only 1 port allowed (this differs from standard ALSA arecordmidi)");
	}
	
	err = snd_seq_parse_address(seq, &port, port_name);
	if (err < 0)
		fatal("Invalid port %s - %s", port_name, snd_strerror(err));
	
	got_a_port = true;
}

/* parses time signature specification */
static void time_signature(const char *arg)
{
	long x = 0;
	char *sep;

	x = strtol(arg, &sep, 10);
	if (x < 1 || x > 64 || *sep != ':')
		fatal("Invalid time signature (%s)", arg);
	ts_num = x;
	x = strtol(++sep, NULL, 10);
	if (x < 1 || x > 64)
		fatal("Invalid time signature (%s)", arg);
	ts_div = x;
	for (ts_dd = 0; x > 1; x /= 2)
		++ts_dd;
}

static void init_tracks(void)
{
	track.cur_buf = &track.first_buf;
}

static void create_queue(void)
{
	snd_seq_queue_tempo_t *tempo;
	int err;

	queue = snd_seq_alloc_named_queue(seq, "arecordmidi");
	check_snd("create queue", queue);

	snd_seq_queue_tempo_alloca(&tempo);
	if (!smpte_timing) {
		snd_seq_queue_tempo_set_tempo(tempo, 60000000 / beats);
		snd_seq_queue_tempo_set_ppq(tempo, ticks);
	} else {
		/*
		 * ALSA doesn't know about the SMPTE time divisions, so
		 * we pretend to have a musical tempo with the equivalent
		 * number of ticks/s.
		 */
		switch (frames) {
		case 24:
			snd_seq_queue_tempo_set_tempo(tempo, 500000);
			snd_seq_queue_tempo_set_ppq(tempo, 12 * ticks);
			break;
		case 25:
			snd_seq_queue_tempo_set_tempo(tempo, 400000);
			snd_seq_queue_tempo_set_ppq(tempo, 10 * ticks);
			break;
		case 29:
			snd_seq_queue_tempo_set_tempo(tempo, 100000000);
			snd_seq_queue_tempo_set_ppq(tempo, 2997 * ticks);
			break;
		case 30:
			snd_seq_queue_tempo_set_tempo(tempo, 500000);
			snd_seq_queue_tempo_set_ppq(tempo, 15 * ticks);
			break;
		default:
			fatal("Invalid SMPTE frames %d", frames);
		}
	}
	err = snd_seq_set_queue_tempo(seq, queue, tempo);
	if (err < 0)
		fatal("Cannot set queue tempo (%u/%i)",
		      snd_seq_queue_tempo_get_tempo(tempo),
		      snd_seq_queue_tempo_get_ppq(tempo));
}

static void create_port(void)
{
	snd_seq_port_info_t *pinfo;
	int err;
	char name[32];

	snd_seq_port_info_alloca(&pinfo);

	/* common information for all our one port */
	snd_seq_port_info_set_capability(pinfo,
					 SND_SEQ_PORT_CAP_WRITE |
					 SND_SEQ_PORT_CAP_SUBS_WRITE);
	snd_seq_port_info_set_type(pinfo,
				   SND_SEQ_PORT_TYPE_MIDI_GENERIC |
				   SND_SEQ_PORT_TYPE_APPLICATION);
	snd_seq_port_info_set_midi_channels(pinfo, 16);

	/* we want to know when the events got delivered to us */
	snd_seq_port_info_set_timestamping(pinfo, 1);
	snd_seq_port_info_set_timestamp_queue(pinfo, queue);

	/* our port number is the same as our port index */
	snd_seq_port_info_set_port_specified(pinfo, 1);
	
	snd_seq_port_info_set_port(pinfo, 0);

	sprintf(name, "arecordmidi port %i", 0);
	snd_seq_port_info_set_name(pinfo, name);

	err = snd_seq_create_port(seq, pinfo);
	check_snd("create port", err);
}

static void connect_port(void)
{
	int err;

	err = snd_seq_connect_from(seq, 0, port.client, port.port);
	if (err < 0)
		fatal("Cannot connect from port %d:%d - %s",
				port.client, port.port, snd_strerror(err));
}

/* records a byte to be written to the .mid file */
static void add_byte(struct smf_track *track, unsigned char byte)
{
	/* make sure we have enough room in the current buffer */
	if (track->cur_buf_size >= BUFFER_SIZE) {
		track->cur_buf->next = calloc(1, sizeof(struct buffer));
		if (!track->cur_buf->next)
			fatal("out of memory");
		track->cur_buf = track->cur_buf->next;
		track->cur_buf_size = 0;
	}

	track->cur_buf->buf[track->cur_buf_size++] = byte;
	track->size++;
}

/* record a variable-length quantity */
static void var_value(struct smf_track *track, int v)
{
	if (v >= (1 << 28))
		add_byte(track, 0x80 | ((v >> 28) & 0x03));
	if (v >= (1 << 21))
		add_byte(track, 0x80 | ((v >> 21) & 0x7f));
	if (v >= (1 << 14))
		add_byte(track, 0x80 | ((v >> 14) & 0x7f));
	if (v >= (1 << 7))
		add_byte(track, 0x80 | ((v >> 7) & 0x7f));
	add_byte(track, v & 0x7f);
}

/* record the delta time from the last event */
static void delta_time(struct smf_track *track, const snd_seq_event_t *ev)
{
	snd_seq_tick_time_t tick = ev->time.tick - t_start;
	int diff = tick - track->last_tick;
	if (diff < 0)
		diff = 0;
	var_value(track, diff);
	track->last_tick = tick;
}

/* record a status byte (or not if we can use running status) */
static void command(struct smf_track *track, unsigned char cmd)
{
	if (cmd != track->last_command)
		add_byte(track, cmd);
	track->last_command = cmd < 0xf0 ? cmd : 0;
}

static void record_event(const snd_seq_event_t *ev)
{
	/* ignore events without proper timestamps */
	if (ev->queue != queue || !snd_seq_ev_is_tick(ev))
		return;

	if (t_start==0)
		t_start = ev->time.tick;

	/* determine which track to record to */
	// Our one port and one track
	if (ev->dest.port != 0)
		return;
	
	switch (ev->type) {
	case SND_SEQ_EVENT_NOTEON:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_NOTE_ON | (ev->data.note.channel & 0xf));
		add_byte(&track, ev->data.note.note & 0x7f);
		add_byte(&track, ev->data.note.velocity & 0x7f);
		break;
	case SND_SEQ_EVENT_NOTEOFF:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_NOTE_OFF | (ev->data.note.channel & 0xf));
		add_byte(&track, ev->data.note.note & 0x7f);
		add_byte(&track, ev->data.note.velocity & 0x7f);
		break;
	case SND_SEQ_EVENT_KEYPRESS:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_NOTE_PRESSURE | (ev->data.note.channel & 0xf));
		add_byte(&track, ev->data.note.note & 0x7f);
		add_byte(&track, ev->data.note.velocity & 0x7f);
		break;
	case SND_SEQ_EVENT_CONTROLLER:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_CONTROL | (ev->data.control.channel & 0xf));
		add_byte(&track, ev->data.control.param & 0x7f);
		add_byte(&track, ev->data.control.value & 0x7f);
		break;
	case SND_SEQ_EVENT_PGMCHANGE:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_PGM_CHANGE | (ev->data.control.channel & 0xf));
		add_byte(&track, ev->data.control.value & 0x7f);
		break;
	case SND_SEQ_EVENT_CHANPRESS:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_CHANNEL_PRESSURE | (ev->data.control.channel & 0xf));
		add_byte(&track, ev->data.control.value & 0x7f);
		break;
	case SND_SEQ_EVENT_PITCHBEND:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_BENDER | (ev->data.control.channel & 0xf));
		add_byte(&track, (ev->data.control.value + 8192) & 0x7f);
		add_byte(&track, ((ev->data.control.value + 8192) >> 7) & 0x7f);
		break;
	case SND_SEQ_EVENT_CONTROL14:
		/* create two commands for MSB and LSB */
		delta_time(&track, ev);
		command(&track, MIDI_CMD_CONTROL | (ev->data.control.channel & 0xf));
		add_byte(&track, ev->data.control.param & 0x7f);
		add_byte(&track, (ev->data.control.value >> 7) & 0x7f);
		if ((ev->data.control.param & 0x7f) < 0x20) {
			delta_time(&track, ev);
			/* running status */
			add_byte(&track, (ev->data.control.param & 0x7f) + 0x20);
			add_byte(&track, ev->data.control.value & 0x7f);
		}
		break;
	case SND_SEQ_EVENT_NONREGPARAM:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_CONTROL | (ev->data.control.channel & 0xf));
		add_byte(&track, MIDI_CTL_NONREG_PARM_NUM_LSB);
		add_byte(&track, ev->data.control.param & 0x7f);
		delta_time(&track, ev);
		add_byte(&track, MIDI_CTL_NONREG_PARM_NUM_MSB);
		add_byte(&track, (ev->data.control.param >> 7) & 0x7f);
		delta_time(&track, ev);
		add_byte(&track, MIDI_CTL_MSB_DATA_ENTRY);
		add_byte(&track, (ev->data.control.value >> 7) & 0x7f);
		delta_time(&track, ev);
		add_byte(&track, MIDI_CTL_LSB_DATA_ENTRY);
		add_byte(&track, ev->data.control.value & 0x7f);
		break;
	case SND_SEQ_EVENT_REGPARAM:
		delta_time(&track, ev);
		command(&track, MIDI_CMD_CONTROL | (ev->data.control.channel & 0xf));
		add_byte(&track, MIDI_CTL_REGIST_PARM_NUM_LSB);
		add_byte(&track, ev->data.control.param & 0x7f);
		delta_time(&track, ev);
		add_byte(&track, MIDI_CTL_REGIST_PARM_NUM_MSB);
		add_byte(&track, (ev->data.control.param >> 7) & 0x7f);
		delta_time(&track, ev);
		add_byte(&track, MIDI_CTL_MSB_DATA_ENTRY);
		add_byte(&track, (ev->data.control.value >> 7) & 0x7f);
		delta_time(&track, ev);
		add_byte(&track, MIDI_CTL_LSB_DATA_ENTRY);
		add_byte(&track, ev->data.control.value & 0x7f);
		break;
#if 0	/* ignore */
	case SND_SEQ_EVENT_SONGPOS:
	case SND_SEQ_EVENT_SONGSEL:
	case SND_SEQ_EVENT_QFRAME:
	case SND_SEQ_EVENT_START:
	case SND_SEQ_EVENT_CONTINUE:
	case SND_SEQ_EVENT_STOP:
	case SND_SEQ_EVENT_TUNE_REQUEST:
	case SND_SEQ_EVENT_RESET:
	case SND_SEQ_EVENT_SENSING:
		break;
#endif
	case SND_SEQ_EVENT_SYSEX:
		if (ev->data.ext.len == 0)
			break;
		delta_time(&track, ev);
		if (*(unsigned char*)ev->data.ext.ptr == 0xf0)
			command(&track, 0xf0);
		else
			command(&track, 0xf7);
		var_value(&track, ev->data.ext.len);
		for (int i = 0; i < ev->data.ext.len; ++i)
			add_byte(&track, ((unsigned char*)ev->data.ext.ptr)[i]);
		break;
	default:
		return;
	}
}

static void write_header(void)
{
	int time_division;

	/* header id and length */
	fwrite("MThd\0\0\0\6", 1, 8, file);
	/* type 0 or 1 */
	fputc(0, file);
	fputc(false, file);
	/* number of tracks */
	fputc((1 >> 8) & 0xff, file);
	fputc(1 & 0xff, file);
	/* time division */
	time_division = ticks;
	if (smpte_timing)
		time_division |= (0x100 - frames) << 8;
	fputc(time_division >> 8, file);
	fputc(time_division & 0xff, file);

	/* track id */
	fwrite("MTrk", 1, 4, file);
	
	/* data length */
	
	// Record where the length is stored, so we can update it
	// when data is added to the file.
	size_offset = ftell(file);
	
	fputc((track.size >> 24) & 0xff, file);
	fputc((track.size >> 16) & 0xff, file);
	fputc((track.size >> 8) & 0xff, file);
	fputc(track.size & 0xff, file);
}

/* record a variable-length quantity directly to file */
static int var_value_direct(int v)
{
	int extra_size = 0;
	
	if (v >= (1 << 28)) {
		fputc(0x80 | ((v >> 28) & 0x03), file);
		extra_size += 1;
	}
	if (v >= (1 << 21)) {
		fputc(0x80 | ((v >> 21) & 0x7f), file);
		extra_size += 1;
	}
	if (v >= (1 << 14)) {
		fputc(0x80 | ((v >> 14) & 0x7f), file);
		extra_size += 1;
	}
	if (v >= (1 << 7)) {
		fputc(0x80 | ((v >> 7) & 0x7f), file);
		extra_size += 1;
	}
	fputc(v & 0x7f, file);
	extra_size += 1;
	return extra_size;
}

static void flush_buffer(void)
{
	struct buffer *buf;
	
	/* track contents */
	for (buf = &track.first_buf; buf; buf = buf->next)
		fwrite(buf->buf, 1, buf == track.cur_buf
				? track.cur_buf_size : BUFFER_SIZE, file);
}

static void update_length(int extra_size)
{
	// Save position
	long saved_pos = ftell(file);
	
	// Jump back to where we recorded the length
	fseek(file, size_offset, SEEK_SET);
	
	int size = track.size += extra_size;
	
	fputc((size >> 24) & 0xff, file);
	fputc((size >> 16) & 0xff, file);
	fputc((size >> 8) & 0xff, file);
	fputc(size & 0xff, file);
	
	// Jump back to where we were
	fseek(file, saved_pos, SEEK_SET);
}

static int write_track_end(void)
{
	snd_seq_queue_status_t *queue_status;
	int tick, err;
	int extra_size = 0;

	snd_seq_queue_status_alloca(&queue_status);

	err = snd_seq_get_queue_status(seq, queue, queue_status);
	check_snd("get queue status", err);
	tick = snd_seq_queue_status_get_tick_time(queue_status);

	/* make length of first (and only) track the recording length */
	extra_size += var_value_direct(tick - track.last_tick);
	fputc(0xff, file);
	fputc(0x2f, file);
	extra_size += 2;
	extra_size += var_value_direct(0);
	
	return extra_size;
}

// TODO
/*
static int write_temporary_track_end(void)
{
	long saved_pos = ftell(file);
	int extra_size = write_track_end();
	fseek(file, saved_pos, SEEK_SET);
	return extra_size;
}
*/

static void list_ports(void)
{
	snd_seq_client_info_t *cinfo;
	snd_seq_port_info_t *pinfo;

	snd_seq_client_info_alloca(&cinfo);
	snd_seq_port_info_alloca(&pinfo);

	puts(" Port    Client name                      Port name");

	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(seq, cinfo) >= 0) {
		int client = snd_seq_client_info_get_client(cinfo);

		if (client == SND_SEQ_CLIENT_SYSTEM)
			continue; /* don't show system timer and announce ports */
		snd_seq_port_info_set_client(pinfo, client);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq, pinfo) >= 0) {
			/* port must understand MIDI messages */
			if (!(snd_seq_port_info_get_type(pinfo)
			      & SND_SEQ_PORT_TYPE_MIDI_GENERIC))
				continue;
			/* we need both READ and SUBS_READ */
			if ((snd_seq_port_info_get_capability(pinfo)
			     & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
			    != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
				continue;
			printf("%3d:%-3d  %-32.32s %s\n",
			       snd_seq_port_info_get_client(pinfo),
			       snd_seq_port_info_get_port(pinfo),
			       snd_seq_client_info_get_name(cinfo),
			       snd_seq_port_info_get_name(pinfo));
		}
	}
}

static void help(const char *argv0)
{
	fprintf(stderr, "Usage: %s [options] outputfile\n"
		"\nAvailable options:\n"
		"  -h,--help                  this help\n"
		"  -V,--version               show version\n"
		"  -l,--list                  list input ports\n"
		"  -p,--port=client:port,...  source port(s)\n"
		"  -b,--bpm=beats             tempo in beats per minute\n"
		"  -f,--fps=frames            resolution in frames per second (SMPTE)\n"
		"  -t,--ticks=ticks           resolution in ticks per beat or frame\n"
		"  -s,--split-channels        create a track for each channel\n"
		"  -i,--timesig=nn:dd         time signature\n"
		"  -T,--timeout=n             stop recording n milliseconds after the last event\n",
		argv0);
}

static void version(void)
{
	fputs("arecordmidi version " SND_UTIL_VERSION_STR "\n", stderr);
}

static void sighandler(int sig)
{
	stop = 1;
}

int main(int argc, char *argv[])
{
	static const char short_options[] = "hVlp:b:f:t:T:sdm:i:";
	static const struct option long_options[] = {
		{"help", 0, NULL, 'h'},
		{"version", 0, NULL, 'V'},
		{"list", 0, NULL, 'l'},
		{"port", 1, NULL, 'p'},
		{"bpm", 1, NULL, 'b'},
		{"fps", 1, NULL, 'f'},
		{"ticks", 1, NULL, 't'},
		{"split-channels", 0, NULL, 's'},
		{"dump", 0, NULL, 'd'},
		{"timesig", 1, NULL, 'i'},
		{"timeout", 1, NULL, 'T'},
		{ }
	};

	char *filename = NULL;
	int do_list = 0;
	struct pollfd *pfds;
	int npfds;
	int c, err;
	int no_events = 0;

	init_seq();

	while ((c = getopt_long(argc, argv, short_options,
				long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			help(argv[0]);
			return 0;
		case 'V':
			version();
			return 0;
		case 'l':
			do_list = 1;
			break;
		case 'p':
			parse_port(optarg);
			break;
		case 'b':
			beats = atoi(optarg);
			if (beats < 4 || beats > 6000)
				fatal("Invalid tempo");
			smpte_timing = 0;
			break;
		case 'f':
			frames = atoi(optarg);
			if (frames != 24 && frames != 25 &&
			    frames != 29 && frames != 30)
				fatal("Invalid number of frames/s");
			smpte_timing = 1;
			break;
		case 't':
			ticks = atoi(optarg);
			if (ticks < 1 || ticks > 0x7fff)
				fatal("Invalid number of ticks");
			break;
		case 'd':
			fputs("The --dump option isn't supported anymore, use aseqdump instead.\n", stderr);
			break;
		case 'i':
			time_signature(optarg);
			break;
		case 'T':
			timeout = atoi(optarg);
			if (timeout < 0)
				fatal("Timout must be 0(=disabled) or a positive value in milliseconds.");
			break;
		default:
			help(argv[0]);
			return 1;
		}
	}

	if (do_list) {
		list_ports();
		return 0;
	}

	if (!got_a_port) {
		fputs("Pleast specify a source port with --port.\n", stderr);
		return 1;
	}

	if (!ticks)
		ticks = smpte_timing ? 40 : 384;
	if (smpte_timing && ticks > 0xff)
		ticks = 0xff;

	if (optind >= argc) {
		fputs("Please specify a file to record to.\n", stderr);
		return 1;
	}
	filename = argv[optind];

	init_tracks();
	create_queue();
	create_port();
	connect_port();

	/* record tempo */
	if (!smpte_timing) {
		int usecs_per_quarter = 60000000 / beats;
		var_value(&track, 0); /* delta time */
		add_byte(&track, 0xff);
		add_byte(&track, 0x51);
		var_value(&track, 3);
		add_byte(&track, usecs_per_quarter >> 16);
		add_byte(&track, usecs_per_quarter >> 8);
		add_byte(&track, usecs_per_quarter);

		/* time signature */
		var_value(&track, 0); /* delta time */
		add_byte(&track, 0xff);
		add_byte(&track, 0x58);
		var_value(&track, 4);
		add_byte(&track, ts_num);
		add_byte(&track, ts_dd);
		add_byte(&track, 24); /* MIDI clocks per metronome click */
		add_byte(&track, 8); /* notated 32nd-notes per MIDI quarter note */
	}
	
	file = fopen(filename, "wb");
	if (!file)
		fatal("Cannot open %s - %s", filename, strerror(errno));

	err = snd_seq_start_queue(seq, queue, NULL);
	check_snd("start queue", err);
	snd_seq_drain_output(seq);

	err = snd_seq_nonblock(seq, 1);
	check_snd("set nonblock mode", err);
	
	write_header();
	
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	npfds = snd_seq_poll_descriptors_count(seq, POLLIN);
	pfds = alloca(sizeof(*pfds) * npfds);
	for (;;) {
		snd_seq_poll_descriptors(seq, pfds, npfds, POLLIN);
		err = poll(pfds, npfds, (timeout==0) ? -1 : timeout);
		if (err == 0) { // timeout occured
			if (no_events>0)
				break;
		} else if (err < 0) {
			break;
		}
		do {
			snd_seq_event_t *event;
			err = snd_seq_event_input(seq, &event);
			if (err < 0)
				break;
			if (event) {
				record_event(event);
				no_events++;
			}
		} while (err > 0);
		if (stop)
			break;
	}

	flush_buffer();
	int extra_size = write_track_end();
	update_length(extra_size);

	fclose(file);
	snd_seq_close(seq);
	return 0;
}
