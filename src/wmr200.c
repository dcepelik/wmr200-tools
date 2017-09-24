/*
 * Oregon Scientific WMR200 USB HID communication wrapper.
 *
 * Copyright (c) 2015-2017 David Čepelík <d@dcepelik.cz>
 */

#include "common.h"
#include "log.h"
#include "wmr200.h"
#include "wmrdata.h"

#include <assert.h>
#include <hidapi.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define	NTH_BIT(n, val)		(((val) >> (n)) & 0x01)
#define	LOW(b)			((b) & 0x0F)
#define	HIGH(b)			((b) & 0xF0)

/*
 * Although heartbeat is required every 30 seconds, using a little
 * less is reasonable. Otherwise the station will (often) switch to
 * logging mode and data which would otherwise be sent immediately
 * will be received as historic records.
 */
#define	HEARTBEAT_INTERVAL_SEC	25

/*
 * Although packet length is validated for each reading as it is
 * processed, packet length is checked against MAX_PACKET_LEN before
 * memory is allocated for the packet.
 *
 * 112 bytes is the maximum length of HISTORIC_DATA reading, which
 * is the length of the largest well-formed packet the station will
 * send with all external sensors attached.
 */
#define MAX_PACKET_LEN		112

#define	VENDOR_ID		0x0FDE
#define	PRODUCT_ID		0xCA01
#define	TENTH_OF_INCH		0.0254

/*
 * The following HIST_* constants are offsets into the HISTORIC_DATA
 * packets. Although offsets are generally hardcoded in the packet
 * processing logic, they are introduced as constants fo HISTORIC_DATA
 * packets, as it makes the code easier to understand.
 */
#define HIST_RAIN_OFFSET	7
#define HIST_WIND_OFFSET	20
#define HIST_UVI_OFFSET		27
#define HIST_BARO_OFFSET	28
#define HIST_NUM_EXT_OFFSET	32	/* offset of the number of external sensors */
#define HIST_SENSORS_OFFSET	33	/* external sensors data offset */
#define HIST_SENSOR_LEN		7	/* external sensor reading length in HISTORIC_DATA*/

/*
 * WMR200 connection and communication context.
 */
struct wmr200
{
	hid_device *dev;		/* HIDAPI device handle */
	struct wmr_handler *handler;	/* loggers TODO */
	pthread_t mainloop_thread;	/* main loop thread */
	pthread_t heartbeat_thread;	/* heartbeat loop thread */
	wmr_latest_data latest;		/* latest readings */
	wmr_meta meta;			/* system metadata packet (updated on the fly) */
	time_t conn_since;		/* time the connection was established */

	byte_t buf[WMR200_FRAME_SIZE];	/* RX buffer */
	size_t buf_avail;		/* number of bytes available in the buffer */
	size_t buf_pos;			/* read position within the buffer */

	byte_t *packet;			/* current packet */
	size_t packet_len;		/* length of the packet */
	byte_t packet_type;		/* type of the packet */
};

/*
 * Some kind of a wake-up command. The device won't talk to us unless
 * we send this first. I don't know why.
 */
static byte_t wakeup[8] = { 0x20, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00 };

/*
 * Packet lengths for certain packet types. If a nonzero value is
 * present for a given packet type, verify_packet will check that
 * packets of that type have the correct length.
 *
 * Length of HISTORIC_DATA packet has to be checked differently, as it
 * depends on the information present in the packet (external sensor count).
 */
static size_t packet_len[PACKET_TYPE_MAX] = {
	[WMR_WIND] = 16,
	[WMR_RAIN] = 22,
	[WMR_UVI] = 10,
	[WMR_BARO] = 13,
	[WMR_TEMP] = 16,
	[WMR_STATUS] = 8,
};

/*
 * A command to be sent to the station.
 */
enum command
{
	CMD_HEARTBEAT = 0xD0,		/* I'm alive, keep sending data */
	CMD_REQUEST_HISTDATA = 0xDA,	/* send me next record from internal logger */
	CMD_ERASE = 0xDB,	/* erase internal logger data */
	CMD_STOP = 0xDF			/* terminate commmunication */
};

/*
 * Sign to indicate positive/negative value.
 */
enum sign
{
	SIGN_POSITIVE = 0x0,
	SIGN_NEGATIVE = 0x8
};

struct wmr_handler
{
	wmr_handler_t handler;
	void *arg;
	struct wmr_handler *next;
};

/*
 * Signal level to string.
 */
static const char *level_string[] = {
	"ok",
	"low"
};

/*
 * Status to string.
 */
static const char *status_string[] = {
	"ok",
	"failed"
};

/*
 * Forecast to forecast string. Corresponds to "icons" drawn on the screen
 * of the console.
 */
static const char *forecast_string[] = {
	"partly_cloudy-day",
	"rainy",
	"cloudy",
	"sunny",
	"clear",
	"snowy",
	"partly_cloudy-night"
};

/*
 * Wind direction to string.
 */
static const char *wind_dir_string[] = {
	"N",
	"NNE",
	"NE",
	"ENE",
	"E",
	"ESE",
	"SE",
	"SSE",
	"S",
	"SSW",
	"SW",
	"WSW",
	"W",
	"WNW",
	"NW",
	"NNW"
};

static byte_t read_byte(struct wmr200 *wmr)
{
	ssize_t ret;

	if (wmr->buf_avail == 0) {
again:
		ret = hid_read(wmr->dev, wmr->buf, WMR200_FRAME_SIZE);
		if (ret < 0) {
			log_warning("hid_read: read error\n");
			goto again;
			/* TODO teardown instead */
		}

		wmr->meta.num_frames++;
		wmr->buf_avail = wmr->buf[0];
		wmr->buf_pos = 1;
	}

	wmr->meta.num_bytes++;
	wmr->buf_avail--;
	return wmr->buf[wmr->buf_pos++];
}

static void send_cmd(struct wmr200 *wmr, byte_t cmd)
{
	byte_t data[2] = { 0x01, cmd };
	int ret = hid_write(wmr->dev, data, sizeof(data));

	if (ret != sizeof(data)) {
		fprintf(stderr, "hid_write: cannot write command\n");
		exit(1);
		/* TODO teardown instead */
	}
}

static void send_heartbeat(struct wmr200 *wmr)
{
	log_debug("Sending heartbeat to WMR200");
	send_cmd(wmr, CMD_HEARTBEAT);
}

/*
 * data processing
 */

static time_t get_reading_time_from_packet(struct wmr200 *wmr)
{
	struct tm tm = {
		.tm_year	= (2000 + wmr->packet[6]) - 1900,
		.tm_mon		= wmr->packet[5],
		.tm_mday	= wmr->packet[4],
		.tm_hour	= wmr->packet[3],
		.tm_min		= wmr->packet[2],
		.tm_sec		= 0,
		.tm_isdst	= -1
	};

	return mktime(&tm);
}

static void invoke_handlers(struct wmr200 *wmr, wmr_reading *reading)
{
	struct wmr_handler *handler = wmr->handler;

	while (handler != NULL) {
		handler->handler(reading, handler->arg);
		handler = handler->next;
	}
}

static void update_if_newer(wmr_reading *old, wmr_reading *new)
{
	if (new->time >= old->time)
		*old = *new;
}

static void process_wind_data(struct wmr200 *wmr, byte_t *data)
{
	byte_t dir_flag = LOW(data[7]);
	float gust_speed = (256 * LOW(data[10]) + data[9]) / 10.0;
	float avg_speed	= (16 * LOW(data[11]) + HIGH(data[10])) / 10.0;
	float chill = data[12]; /* TODO verify the formula */

	assert(dir_flag < ARRAY_SIZE(wind_dir_string));

	wmr_reading reading = {
		.type = WMR_WIND,
		.time = get_reading_time_from_packet(wmr),
		.wind = {
			.dir = wind_dir_string[dir_flag],
			.gust_speed = gust_speed,
			.avg_speed = avg_speed,
			.chill = chill
		}
	};

	update_if_newer(&wmr->latest.wind, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_rain_data(struct wmr200 *wmr, byte_t *data)
{
	float rate = ((data[8] << 8) + data[7]) * TENTH_OF_INCH;
	float accum_hour = ((data[10] << 8) + data[9]) * TENTH_OF_INCH;
	float accum_24h	= ((data[12] << 8) + data[11]) * TENTH_OF_INCH;
	float accum_2007 = ((data[14] << 8) + data[13]) * TENTH_OF_INCH;

	wmr_reading reading = {
		.type = WMR_RAIN,
		.time = get_reading_time_from_packet(wmr),
		.rain = {
			.rate = rate,
			.accum_hour = accum_hour,
			.accum_24h = accum_24h,
			.accum_2007 = accum_2007
		}
	};

	update_if_newer(&wmr->latest.rain, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_uvi_data(struct wmr200 *wmr, byte_t *data)
{
	byte_t index = LOW(data[7]);

	wmr_reading reading = {
		.type = WMR_UVI,
		.time = get_reading_time_from_packet(wmr),
		.uvi = {
			.index = index
		}
	};

	update_if_newer(&wmr->latest.uvi, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_baro_data(struct wmr200 *wmr, byte_t *data)
{
	uint_t pressure = 256 * LOW(data[8]) + data[7];
	uint_t alt_pressure = 256 * LOW(data[10]) + data[9];
	byte_t forecast = HIGH(data[8]);

	assert(forecast < ARRAY_SIZE(forecast_string));

	wmr_reading reading = {
		.type = WMR_BARO,
		.time = get_reading_time_from_packet(wmr),
		.baro = {
			.pressure = pressure,
			.alt_pressure = alt_pressure,
			.forecast = forecast_string[forecast]
		}
	};

	update_if_newer(&wmr->latest.baro, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_temp_data(struct wmr200 *wmr, byte_t *data)
{
	int sensor_id = LOW(data[7]);

	/* TODO */
	if (sensor_id > 1) {
		fprintf(stderr, "Unknown sensor, ID: %i\n", sensor_id);
		exit(1);
		/* teardown instead */
	}

	byte_t humidity = data[10];
	byte_t heat_index = data[13];

	float temp = (256 * LOW(data[9]) + data[8]) / 10.0;
	if (HIGH(data[9]) == SIGN_NEGATIVE)
		temp = -temp;

	float dew_point = (256 * LOW(data[12]) + data[11]) / 10.0;
	if (HIGH(data[12]) == SIGN_NEGATIVE)
		dew_point = -dew_point;

	wmr_reading reading = {
		.type = WMR_TEMP,
		.time = get_reading_time_from_packet(wmr),
		.temp = {
			.humidity = humidity,
			.heat_index = heat_index,
			.temp = temp,
			.dew_point = dew_point,
			.sensor_id = sensor_id
		}
	};

	update_if_newer(&wmr->latest.temp[sensor_id], &reading);
	invoke_handlers(wmr, &reading);
}

static void process_status_data(struct wmr200 *wmr, byte_t *data)
{
	byte_t wind_bat = NTH_BIT(0, data[4]);
	byte_t temp_bat = NTH_BIT(1, data[4]);
	byte_t rain_bat = NTH_BIT(4, data[5]);
	byte_t uv_bat = NTH_BIT(5, data[5]);

	byte_t wind_status = NTH_BIT(0, data[2]);
	byte_t temp_status = NTH_BIT(1, data[2]);
	byte_t rain_status = NTH_BIT(4, data[3]);
	byte_t uv_status = NTH_BIT(5, data[3]);

	byte_t rtc_signal = NTH_BIT(8, data[4]);

	wmr_reading reading = {
		.type = WMR_STATUS,
		.time = get_reading_time_from_packet(wmr),
		.status = {
			.wind_bat = level_string[wind_bat],
			.temp_bat = level_string[temp_bat],
			.rain_bat = level_string[rain_bat],
			.uv_bat = level_string[uv_bat],
			.wind_sensor = status_string[wind_status],
			.temp_sensor = status_string[temp_status],
			.rain_sensor = status_string[rain_status],
			.uv_sensor = status_string[uv_status],
			.rtc_signal_level = level_string[rtc_signal]
		}
	};

	update_if_newer(&wmr->latest.status, &reading);
	invoke_handlers(wmr, &reading);
}

/*
 * Process HISTORIC_DATA packet data.
 *
 * The processing of this packet is a bit special. Normally, each reading contains
 * date and time information and individual checksum. With HISTORIC_DATA packets,
 * however, all readings are sent as a single HISTORIC_DATA packet with common
 * date/time and checksum.
 *
 * To reuse the logic used for processing of readings when sent individually
 * for HISTORIC_DATA processing, the packet data pointer is moved to point
 * into the appropriate places in the HISTORIC_DATA packet. Note that when
 * reading handlers such as process_temp_data extract reading date and time
 * information, they use wmr->packet as argument to get_reading_time_from_packet.
 */
static void process_historic_data(struct wmr200 *wmr, byte_t *data)
{
	size_t num_ext_sensors;
	size_t i;

	num_ext_sensors = data[HIST_NUM_EXT_OFFSET];

	process_rain_data(wmr, data +  HIST_RAIN_OFFSET);
	process_wind_data(wmr, data + HIST_WIND_OFFSET);
	process_uvi_data(wmr, data + HIST_UVI_OFFSET);
	process_baro_data(wmr, data + HIST_BARO_OFFSET);

	for (i = 0; i < num_ext_sensors; i++)
		process_temp_data(wmr, data + HIST_SENSORS_OFFSET + i * HIST_SENSOR_LEN);
}

static void emit_meta_packet(struct wmr200 *wmr)
{
	log_debug("Emitting system WMR_META packet");

	wmr->meta.uptime = time(NULL) - wmr->conn_since;
	wmr_reading reading = {
		.time = time(NULL),
		.type = WMR_META,
		.meta = wmr->meta,
	};
	wmr->latest.meta = reading;

	invoke_handlers(wmr, &reading);
}

static bool verify_packet(struct wmr200 *wmr)
{
	uint_t sum;
	uint_t checksum;
	size_t num_ext_sensors;
	size_t i;

	if (wmr->packet_len <= 2)
		return false;

	for (i = 0, sum = 0; i < wmr->packet_len - 2; i++)
		sum += wmr->packet[i];

	checksum = 256 * wmr->packet[wmr->packet_len - 1]
		+ wmr->packet[wmr->packet_len - 2];

	if (sum != checksum)
		return false;

	/*
	 * Validate packet length so that packet processing logic
	 * does not read invalid memory.
	 */
	if (packet_len[wmr->packet_type] > 0) {
		if (wmr->packet_len != packet_len[wmr->packet_type]) {
			log_error("Invalid %s packet length (%zu), dropping",
				packet_type_to_string(wmr->packet_type), wmr->packet_len);
			/* TODO teardown */
		}
	}

	/*
	 * Validate length of HISTORIC_DATA packet, which depends on the number
	 * of external sensors present in the reading.
	 */
	if (wmr->packet_type == HISTORIC_DATA) {
		assert(wmr->packet_len >= HIST_NUM_EXT_OFFSET);
		num_ext_sensors = wmr->packet[HIST_NUM_EXT_OFFSET];
		assert(wmr->packet_len == HIST_SENSORS_OFFSET + (1 + num_ext_sensors) * HIST_SENSOR_LEN + 2);
	}

	return true;
}

static void dispatch_packet(struct wmr200 *wmr)
{
	switch (wmr->packet_type) {
	case HISTORIC_DATA:
		process_historic_data(wmr, wmr->packet);
		break;
	case WMR_WIND:
		process_wind_data(wmr, wmr->packet);
		break;
	case WMR_RAIN:
		process_rain_data(wmr, wmr->packet);
		break;
	case WMR_UVI:
		process_uvi_data(wmr, wmr->packet);
		break;
	case WMR_BARO:
		process_baro_data(wmr, wmr->packet);
		break;
	case WMR_TEMP:
		process_temp_data(wmr, wmr->packet);
		break;
	case WMR_STATUS:
		process_status_data(wmr, wmr->packet);
		break;
	default:
		log_warning("Ignoring unknown packet 0x%02X", wmr->packet_type);
	}
}

static void mainloop(struct wmr200 *wmr)
{
	size_t i;

	while (1) {
		wmr->packet_type = read_byte(wmr);

handle_packet:
		switch (wmr->packet_type) {
		case PACKET_HISTDATA_NOTIF:
			log_info("Data logger contains some unprocessed "
				"historic records");
			log_info("Issuing CMD_REQUEST_HISTDATA command");

			send_cmd(wmr, CMD_REQUEST_HISTDATA);
			continue;

		case PACKET_ERASE_ACK:
			log_info("Data logger database purge successful");
			continue;

		case PACKET_STOP_ACK:
			/*
			 * Ignore, this is only a response to prev CMD_STOP packet.
			 * This packet may have been sent during previous session.
			 */
			log_debug("Ignoring CMD_STOP packet");
			continue;
		}

		wmr->packet_len = read_byte(wmr);

		log_debug("Received %s (type=0x%02X, len=%zu)",
			packet_type_to_string(wmr->packet_type), wmr->packet_type,
			wmr->packet_len);

		/*
		 * When communication gets out of sync, we may read a packet length
		 * value which is actually packet type. This is a rather trivial
		 * attempt to re-sync.
		 */
		if (wmr->packet_len >= 0xD0 && wmr->packet_len <= 0xDF) {
			wmr->packet_type = wmr->packet_len;
			goto handle_packet;
		}

		/*
		 * If a packet exceeds maximum size, drop it.
		 */
		if (wmr->packet_len > MAX_PACKET_LEN) {
			log_warning("Dropping oversize packet (%zu B)", wmr->packet_len);
			for (i = 0; i < wmr->packet_len; i++)
				(void) read_byte(wmr);
		}

		wmr->packet = malloc_safe(wmr->packet_len);
		wmr->packet[0] = wmr->packet_type;
		wmr->packet[1] = wmr->packet_len;

		for (i = 2; i < wmr->packet_len; i++)
			wmr->packet[i] = read_byte(wmr);

		wmr->meta.num_packets++;

		if (!verify_packet(wmr)) {
			log_warning("Received incorrect packet, dropping");
			wmr->meta.num_failed++;
			goto free_packet;
		}

		wmr->meta.latest_packet = time(NULL);
		dispatch_packet(wmr);

free_packet:
		free(wmr->packet);
	}
}

void *mainloop_pthread(void *arg)
{
	struct wmr200 *wmr = (struct wmr200 *)arg;
	mainloop(wmr); /* TODO register any cleanup handlers here? */

	return NULL;
}

static void heartbeat_loop(struct wmr200 *wmr)
{
	while (1) {
		send_heartbeat(wmr);
		emit_meta_packet(wmr);

		usleep(HEARTBEAT_INTERVAL_SEC * 1e6);
	}
}

static void *heartbeat_loop_pthread(void *arg)
{
	struct wmr200 *wmr = (struct wmr200 *)arg;
	heartbeat_loop(wmr);
	
	return NULL;
}

/*
 * public interface
 */

struct wmr200 *wmr_open(void)
{
	struct wmr200 *wmr = malloc_safe(sizeof(*wmr));

	wmr->dev = hid_open(VENDOR_ID, PRODUCT_ID, NULL);
	if (wmr->dev == NULL) {
		log_error("hid_open: cannot connect to WMR200");
		return NULL;
	}

	wmr->packet = NULL;
	wmr->buf_avail = wmr->buf_pos = 0;
	wmr->handler = NULL;
	wmr->conn_since = time(NULL);
	memset(&wmr->latest, 0, sizeof(wmr->latest));
	memset(&wmr->meta, 0, sizeof(wmr->meta));

	if (hid_write(wmr->dev, wakeup, sizeof(wakeup)) != sizeof(wakeup)) {
		log_error("hid_write: cannot write wakeup packet");
		goto out_free;
	}

	return wmr;

out_free:
	free(wmr);
	return NULL;
}

void wmr_close(struct wmr200 *wmr)
{
	if (wmr->dev != NULL) {
		send_cmd(wmr, CMD_STOP);
		hid_close(wmr->dev);
	}

	free(wmr);
}

void wmr_init(void)
{
	hid_init();
}

void wmr_end(void)
{
	hid_exit();
}

int wmr_start(struct wmr200 *wmr)
{
	if (pthread_create(&wmr->heartbeat_thread,
		NULL, heartbeat_loop_pthread, wmr) != 0) {
		log_error("Cannot start heartbeat loop thread");
		return -1;
	}

	log_debug("Started heartbeat thread");

	if (pthread_create(&wmr->mainloop_thread,
		NULL, mainloop_pthread, wmr) != 0) {
		log_error("Cannot start main communication loop thread");
		return -1;
	}

	log_debug("Started main thread");

	send_cmd(wmr, CMD_ERASE);
	return 0;
}

void wmr_stop(struct wmr200 *wmr)
{
	pthread_cancel(wmr->heartbeat_thread);
	pthread_cancel(wmr->mainloop_thread);
	pthread_join(wmr->heartbeat_thread, NULL);
	pthread_join(wmr->mainloop_thread, NULL);
}

void wmr_add_handler(struct wmr200 *wmr, wmr_handler_t func, void *arg)
{
	struct wmr_handler *handler = malloc_safe(sizeof (struct wmr_handler));
	handler->handler = func;
	handler->arg = arg;
	handler->next = wmr->handler;
	wmr->handler = handler;
}
