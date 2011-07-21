/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation
 *  Copyright (C) 2011  Marcel Holtmann <marcel@holtmann.org>
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <gdbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/uuid.h>

#include "dbus-common.h"
#include "adapter.h"
#include "device.h"
#include "error.h"
#include "log.h"
#include "att.h"
#include "gattrib.h"
#include "gatt.h"
#include "attio.h"
#include "monitor.h"
#include "textfile.h"

#define PROXIMITY_INTERFACE "org.bluez.Proximity"

#define ALERT_LEVEL_CHR_UUID 0x2A06
#define POWER_LEVEL_CHR_UUID 0x2A07

struct monitor {
	struct btd_device *device;
	GAttrib *attrib;
	struct att_range *linkloss;
	struct att_range *txpower;
	struct att_range *immediate;
	struct enabled enabled;
	char *linklosslevel;		/* Link Loss Alert Level */
	char *immediatelevel;		/* Immediate Alert Level */
	char *signallevel;		/* Path Loss RSSI level */
	uint16_t linklosshandle;	/* Link Loss Characteristic Value Handle */
	uint16_t txpowerhandle;		/* Tx Power Characteristic Value Handle */
	guint attioid;
};

static inline int create_filename(char *buf, size_t size,
				const bdaddr_t *bdaddr, const char *name)
{
	char addr[18];

	ba2str(bdaddr, addr);

	return create_name(buf, size, STORAGEDIR, addr, name);
}

static int write_proximity_config(bdaddr_t *sba, bdaddr_t *dba,
					const char *alert, const char *level)
{
	char filename[PATH_MAX + 1], addr[18], key[38];

	create_filename(filename, PATH_MAX, sba, "proximity");

	create_file(filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	ba2str(dba, addr);

	snprintf(key, sizeof(key), "%17s#%s", addr, alert);

	return textfile_put(filename, key, level);
}

static char *read_proximity_config(bdaddr_t *sba, bdaddr_t *dba,
							const char *alert)
{
	char filename[PATH_MAX + 1], addr[18], key[38];

	create_filename(filename, PATH_MAX, sba, "proximity");

	ba2str(dba, addr);
	snprintf(key, sizeof(key), "%17s#%s", addr, alert);

	return textfile_caseget(filename, key);
}

static uint8_t str2level(const char *level)
{
	if (g_strcmp0("high", level) == 0)
		return 0x02;
	else if (g_strcmp0("mild", level) == 0)
		return 0x01;

	return 0x00;
}

static void char_discovered_cb(GSList *characteristics, guint8 status,
							gpointer user_data)
{
	struct monitor *monitor = user_data;
	struct att_char *chr;
	uint8_t value = str2level(monitor->linklosslevel);

	if (status) {
		error("Discover Link Loss handle: %s", att_ecode2str(status));
		return;
	}

	DBG("Setting alert level \"%s\" on Reporter", monitor->linklosslevel);

	/* Assume there is a single Alert Level characteristic */
	chr = characteristics->data;
	monitor->linklosshandle = chr->value_handle;

	gatt_write_cmd(monitor->attrib, monitor->linklosshandle, &value, 1,
								NULL, NULL);
}

static int write_alert_level(struct monitor *monitor)
{
	struct att_range *linkloss = monitor->linkloss;
	bt_uuid_t uuid;

	if (monitor->linklosshandle) {
		uint8_t value = str2level(monitor->linklosslevel);

		gatt_write_cmd(monitor->attrib, monitor->linklosshandle,
							&value, 1, NULL, NULL);
		return 0;
	}

	bt_uuid16_create(&uuid, ALERT_LEVEL_CHR_UUID);

	/* FIXME: use cache (requires service changed support) ? */
	gatt_discover_char(monitor->attrib, linkloss->start, linkloss->end,
					&uuid, char_discovered_cb, monitor);

	return 0;
}

static void tx_power_read_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	uint8_t value;
	int vlen;

	if (status != 0) {
		DBG("Tx Power Level read failed: %s", att_ecode2str(status));
		return;
	}

	if (!dec_read_resp(pdu, plen, &value, &vlen)) {
		DBG("Protocol error");
		return;
	}

	DBG("Tx Power Level: %02x", (int8_t) value);
}

static void tx_power_handle_cb(GSList *characteristics, guint8 status,
							gpointer user_data)
{
	struct monitor *monitor = user_data;
	struct att_char *chr;

	if (status) {
		error("Discover Tx Power handle: %s", att_ecode2str(status));
		return;
	}

	chr = characteristics->data;
	monitor->txpowerhandle = chr->value_handle;

	DBG("Tx Power handle: 0x%04x", monitor->txpowerhandle);

	gatt_read_char(monitor->attrib, monitor->txpowerhandle, 0,
							tx_power_read_cb, monitor);
}

static void read_tx_power(struct monitor *monitor)
{
	struct att_range *txpower = monitor->txpower;
	bt_uuid_t uuid;

	if (monitor->txpowerhandle != 0) {
		gatt_read_char(monitor->attrib, monitor->txpowerhandle, 0,
						tx_power_read_cb, monitor);
		return;
	}

	bt_uuid16_create(&uuid, POWER_LEVEL_CHR_UUID);

	gatt_discover_char(monitor->attrib, txpower->start, txpower->end,
				&uuid, tx_power_handle_cb, monitor);
}

static void attio_connected_cb(GAttrib *attrib, gpointer user_data)
{
	struct monitor *monitor = user_data;

	monitor->attrib = g_attrib_ref(attrib);

	if (monitor->enabled.linkloss)
		write_alert_level(monitor);

	if (monitor->enabled.pathloss)
		read_tx_power(monitor);
}

static void attio_disconnected_cb(gpointer user_data)
{
	struct monitor *monitor = user_data;

	g_attrib_unref(monitor->attrib);
	monitor->attrib = NULL;
}

static DBusMessage *set_link_loss_alert(DBusConnection *conn, DBusMessage *msg,
						const char *level, void *data)
{
	struct monitor *monitor = data;
	struct btd_device *device = monitor->device;
	const char *path = device_get_path(device);
	bdaddr_t sba, dba;

	if (!g_str_equal("none", level) && !g_str_equal("mild", level) &&
			!g_str_equal("high", level))
		return btd_error_invalid_args(msg);

	if (g_strcmp0(monitor->linklosslevel, level) == 0)
		return dbus_message_new_method_return(msg);

	g_free(monitor->linklosslevel);
	monitor->linklosslevel = g_strdup(level);

	adapter_get_address(device_get_adapter(device), &sba);
	device_get_address(device, &dba);

	write_proximity_config(&sba, &dba, "LinkLossAlertLevel", level);

	emit_property_changed(conn, path,
				PROXIMITY_INTERFACE, "LinkLossAlertLevel",
				DBUS_TYPE_STRING, &monitor->linklosslevel);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_immediate_alert(DBusConnection *conn, DBusMessage *msg,
						const char *level, void *data)
{
	struct monitor *monitor = data;
	const gchar *path = device_get_path(monitor->device);

	if (!g_str_equal("none", level) && !g_str_equal("mild", level) &&
			!g_str_equal("high", level))
		return btd_error_invalid_args(msg);

	if (g_strcmp0(monitor->immediatelevel, level) == 0)
		return dbus_message_new_method_return(msg);

	g_free(monitor->immediatelevel);
	monitor->immediatelevel = g_strdup(level);

	emit_property_changed(conn, path, PROXIMITY_INTERFACE,
					"ImmediateAlertLevel",
					DBUS_TYPE_STRING, &level);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct monitor *monitor = data;
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	if (monitor->enabled.linkloss)
		dict_append_entry(&dict, "LinkLossAlertLevel",
				DBUS_TYPE_STRING, &monitor->linklosslevel);

	if (monitor->enabled.pathloss)
		dict_append_entry(&dict, "SignalLevel",
				DBUS_TYPE_STRING, &monitor->signallevel);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct monitor *monitor = data;
	const char *property;
	DBusMessageIter iter;
	DBusMessageIter sub;
	const char *level;

	if (!dbus_message_iter_init(msg, &iter))
		return btd_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return btd_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &sub);

	if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&sub, &level);

	if (g_str_equal("ImmediateAlertLevel", property)) {
		if (monitor->enabled.findme == FALSE &&
				monitor->enabled.pathloss == FALSE)
			return btd_error_not_available(msg);

		return set_immediate_alert(conn, msg, level, data);
	} else if (g_str_equal("LinkLossAlertLevel", property)) {
		if (monitor->enabled.linkloss == FALSE)
			return btd_error_not_available(msg);

		return set_link_loss_alert(conn, msg, level, data);
	}

	return btd_error_invalid_args(msg);
}

static GDBusMethodTable monitor_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	get_properties	},
	{ "SetProperty",	"sv",	"",		set_property,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ }
};

static GDBusSignalTable monitor_signals[] = {
	{ "PropertyChanged",	"sv"	},
	{ }
};

static void monitor_destroy(gpointer user_data)
{
	struct monitor *monitor = user_data;

	if (monitor->attioid)
		btd_device_remove_attio_callback(monitor->device,
						monitor->attioid);
	if (monitor->attrib)
		g_attrib_unref(monitor->attrib);

	btd_device_unref(monitor->device);
	g_free(monitor->linkloss);
	g_free(monitor->immediate);
	g_free(monitor->txpower);
	g_free(monitor->linklosslevel);
	g_free(monitor->immediatelevel);
	g_free(monitor->signallevel);
	g_free(monitor);
}

int monitor_register(DBusConnection *conn, struct btd_device *device,
		struct att_primary *linkloss, struct att_primary *txpower,
		struct att_primary *immediate, struct enabled *enabled)
{
	const char *path = device_get_path(device);
	struct monitor *monitor;
	bdaddr_t sba, dba;
	char *level;

	adapter_get_address(device_get_adapter(device), &sba);
	device_get_address(device, &dba);

	level = read_proximity_config(&sba, &dba, "LinkLossAlertLevel");

	monitor = g_new0(struct monitor, 1);
	monitor->device = btd_device_ref(device);
	monitor->linklosslevel = (level ? : g_strdup("none"));
	monitor->signallevel = g_strdup("unknown");

	if (g_dbus_register_interface(conn, path,
				PROXIMITY_INTERFACE,
				monitor_methods, monitor_signals,
				NULL, monitor, monitor_destroy) == FALSE) {
		error("D-Bus failed to register %s interface",
						PROXIMITY_INTERFACE);
		monitor_destroy(monitor);
		return -1;
	}

	DBG("Registered interface %s on path %s", PROXIMITY_INTERFACE, path);

	if (linkloss && enabled->linkloss) {
		monitor->linkloss = g_new0(struct att_range, 1);
		monitor->linkloss->start = linkloss->start;
		monitor->linkloss->end = linkloss->end;

		monitor->enabled.linkloss = TRUE;
	}

	if (immediate) {
		if (txpower && enabled->pathloss) {
			monitor->txpower = g_new0(struct att_range, 1);
			monitor->txpower->start = txpower->start;
			monitor->txpower->end = txpower->end;

			monitor->enabled.pathloss = TRUE;
		}

		if (enabled->pathloss || enabled->findme) {
			monitor->immediate = g_new0(struct att_range, 1);
			monitor->immediate->start = immediate->start;
			monitor->immediate->end = immediate->end;
		}

		monitor->enabled.findme = enabled->findme;
	}

	DBG("Link Loss: %s, Path Loss: %s, FindMe: %s",
				monitor->enabled.linkloss ? "TRUE" : "FALSE",
				monitor->enabled.pathloss ? "TRUE" : "FALSE",
				monitor->enabled.findme ? "TRUE" : "FALSE");

	if (monitor->enabled.linkloss || monitor->enabled.pathloss)
		monitor->attioid = btd_device_add_attio_callback(device,
							attio_connected_cb,
							attio_disconnected_cb,
							monitor);

	return 0;
}

void monitor_unregister(DBusConnection *conn, struct btd_device *device)
{
	const char *path = device_get_path(device);

	g_dbus_unregister_interface(conn, path, PROXIMITY_INTERFACE);
}
