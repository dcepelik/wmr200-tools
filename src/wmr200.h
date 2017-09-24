/*
 * wmr200.h:
 * Oregon Scientific WMR200 USB HID communication wrapper
 *
 * This software may be freely used and distributed according to the terms
 * of the GNU GPL version 2 or 3. See LICENSE for more information.
 *
 * Copyright (c) 2015 David Čepelík <cepelik@gymlit.cz>
 */

#ifndef WMR200_H
#define	WMR200_H

#include "wmrdata.h"
#include "common.h"

#include <stdio.h>
#include <hidapi.h>
#include <pthread.h>


#define	WMR200_FRAME_SIZE		8
#define	WMR200_MAX_TEMP_SENSORS		10

typedef void (*wmr_handler_t)(wmr_reading *reading, void *arg);

struct wmr_handler;

void wmr_init();
void wmr_end();
struct wmr200 *wmr_open();
int wmr_start(struct wmr200 *wmr);
void wmr_stop(struct wmr200 *wmr);
void wmr_close(struct wmr200 *wmr);
void wmr_add_handler(struct wmr200 *wmr, wmr_handler_t handler, void *arg);

#endif
