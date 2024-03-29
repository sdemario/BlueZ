/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2009-2010  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2009-2010  Nokia Corporation
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
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */
#ifndef BT_IO_H
#define BT_IO_H

#include <glib.h>

typedef enum {
	BT_IO_ERROR_DISCONNECTED,
	BT_IO_ERROR_CONNECT_FAILED,
	BT_IO_ERROR_FAILED,
	BT_IO_ERROR_INVALID_ARGS,
} BtIOError;

#define BT_IO_ERROR bt_io_error_quark()

GQuark bt_io_error_quark(void);

typedef enum {
	BT_IO_L2RAW,
	BT_IO_L2CAP,
	BT_IO_RFCOMM,
	BT_IO_SCO,
} BtIOType;

typedef enum {
	BT_IO_OPT_INVALID = 0,
	BT_IO_OPT_SOURCE,
	BT_IO_OPT_SOURCE_BDADDR,
	BT_IO_OPT_DEST,
	BT_IO_OPT_DEST_BDADDR,
	BT_IO_OPT_DEFER_TIMEOUT,
	BT_IO_OPT_SEC_LEVEL,
	BT_IO_OPT_CHANNEL,
	BT_IO_OPT_SOURCE_CHANNEL,
	BT_IO_OPT_DEST_CHANNEL,
	BT_IO_OPT_PSM,
	BT_IO_OPT_CID,
	BT_IO_OPT_MTU,
	BT_IO_OPT_OMTU,
	BT_IO_OPT_IMTU,
	BT_IO_OPT_MASTER,
	BT_IO_OPT_HANDLE,
	BT_IO_OPT_CLASS,
	BT_IO_OPT_MODE,
	BT_IO_OPT_FLUSHABLE,
	BT_IO_OPT_PRIORITY,
} BtIOOption;

typedef enum {
	BT_IO_SEC_SDP = 0,
	BT_IO_SEC_LOW,
	BT_IO_SEC_MEDIUM,
	BT_IO_SEC_HIGH,
} BtIOSecLevel;

typedef void (*BtIOConfirm)(GIOChannel *io, gpointer user_data);

typedef void (*BtIOConnect)(GIOChannel *io, GError *err, gpointer user_data);

gboolean bt_io_accept(GIOChannel *io, BtIOConnect connect, gpointer user_data,
					GDestroyNotify destroy, GError **err);

gboolean bt_io_set(GIOChannel *io, BtIOType type, GError **err,
						BtIOOption opt1, ...);

gboolean bt_io_get(GIOChannel *io, BtIOType type, GError **err,
						BtIOOption opt1, ...);

GIOChannel *bt_io_connect(BtIOType type, BtIOConnect connect,
				gpointer user_data, GDestroyNotify destroy,
				GError **err, BtIOOption opt1, ...);

GIOChannel *bt_io_listen(BtIOType type, BtIOConnect connect,
				BtIOConfirm confirm, gpointer user_data,
				GDestroyNotify destroy, GError **err,
				BtIOOption opt1, ...);

#endif
