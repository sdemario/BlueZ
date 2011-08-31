/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
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

#include <glib.h>
#include <dbus/dbus.h>

#include "adapter.h"
#include "plugin.h"
#include "log.h"
#include "gdbus.h"

/* from mce/mode-names.h */
#define MCE_RADIO_STATE_BLUETOOTH	(1 << 3)

/* from mce/dbus-names.h */
#define MCE_SERVICE			"com.nokia.mce"
#define MCE_REQUEST_IF			"com.nokia.mce.request"
#define MCE_SIGNAL_IF			"com.nokia.mce.signal"
#define MCE_REQUEST_PATH		"/com/nokia/mce/request"
#define MCE_SIGNAL_PATH			"/com/nokia/mce/signal"
#define MCE_RADIO_STATES_CHANGE_REQ	"req_radio_states_change"
#define MCE_RADIO_STATES_GET		"get_radio_states"
#define MCE_RADIO_STATES_SIG		"radio_states_ind"
#define MCE_TKLOCK_MODE_SIG		"tklock_mode_ind"

static guint watch_id;
static guint tklock_watch_id;
static DBusConnection *conn = NULL;
static gboolean mce_bt_set = FALSE;
static gboolean mce_bt_on = FALSE;

static gboolean mce_tklock_mode_cb(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	struct btd_adapter *adapter = user_data;
	DBusMessageIter args;
	const char *sigvalue;

	if (!dbus_message_iter_init(message, &args)) {
		error("message has no arguments");
	} else if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) {
		error("argument is not string");
	} else {

		dbus_message_iter_get_basic(&args, &sigvalue);
		DBG("got signal with value %s", sigvalue);

		if (g_strcmp0("unlocked", sigvalue) == 0)
			btd_adapter_enable_auto_connect(adapter);
	}

	return TRUE;
}

static gboolean mce_signal_callback(DBusConnection *connection,
					DBusMessage *message, void *user_data)
{
	DBusMessageIter args;
	uint32_t sigvalue;
	struct btd_adapter *adapter = user_data;
	int err;

	DBG("received mce signal");

	if (!dbus_message_iter_init(message, &args))
		error("message has no arguments");
	else if (DBUS_TYPE_UINT32 != dbus_message_iter_get_arg_type(&args))
		error("argument is not uint32");
	else {
		dbus_message_iter_get_basic(&args, &sigvalue);
		DBG("got signal with value %u", sigvalue);

		/* set the adapter according to the mce signal
		   and remember the value */
		mce_bt_on = sigvalue & MCE_RADIO_STATE_BLUETOOTH ? TRUE : FALSE;

		if (mce_bt_on)
			err = btd_adapter_switch_online(adapter);
		else
			err = btd_adapter_switch_offline(adapter);

		if (err == 0)
			mce_bt_set = TRUE;

	}

	return TRUE;
}

static void read_radio_states_cb(DBusPendingCall *call, void *user_data)
{
	DBusError derr;
	DBusMessage *reply;
	dbus_uint32_t radio_states;
	struct btd_adapter *adapter = user_data;
	int err;

	reply = dbus_pending_call_steal_reply(call);

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		error("mce replied with an error: %s, %s",
				derr.name, derr.message);
		dbus_error_free(&derr);
		goto done;
	}

	dbus_error_init(&derr);
	if (dbus_message_get_args(reply, &derr,
				DBUS_TYPE_UINT32, &radio_states,
				DBUS_TYPE_INVALID) == FALSE) {
		error("unable to parse get_radio_states reply: %s, %s",
							derr.name, derr.message);
		dbus_error_free(&derr);
		goto done;
	}

	DBG("radio_states: %d", radio_states);

	mce_bt_on = radio_states & MCE_RADIO_STATE_BLUETOOTH ? TRUE : FALSE;

	if (mce_bt_on)
		err = btd_adapter_switch_online(adapter);
	else
		err = btd_adapter_switch_offline(adapter);

	if (err == 0)
		mce_bt_set = TRUE;

done:
	dbus_message_unref(reply);
}

static void adapter_powered(struct btd_adapter *adapter, gboolean powered)
{
	DBusMessage *msg;
	static gboolean startup = TRUE;

	DBG("adapter_powered called with %d", powered);

	if (startup) {
		DBusPendingCall *call;

		/* Initialization: sync adapter state and MCE radio state */

		DBG("Startup: reading MCE Bluetooth radio state...");
		startup = FALSE;

		msg = dbus_message_new_method_call(MCE_SERVICE,
					MCE_REQUEST_PATH, MCE_REQUEST_IF,
					MCE_RADIO_STATES_GET);

		if (!dbus_connection_send_with_reply(conn, msg, &call, -1)) {
			error("calling %s failed", MCE_RADIO_STATES_GET);
			dbus_message_unref(msg);
			return;
		}

		dbus_pending_call_set_notify(call, read_radio_states_cb,
								adapter, NULL);
		dbus_pending_call_unref(call);
		dbus_message_unref(msg);
		return;
	}

	/* MCE initiated operation */
	if (mce_bt_set == TRUE) {
		mce_bt_set = FALSE;
		return;
	}

	/* Non MCE operation: set MCE according to adapter state */
	if (mce_bt_on != powered) {
		dbus_uint32_t radio_states;
		dbus_uint32_t radio_mask = MCE_RADIO_STATE_BLUETOOTH;

		msg = dbus_message_new_method_call(MCE_SERVICE,
					MCE_REQUEST_PATH, MCE_REQUEST_IF,
					MCE_RADIO_STATES_CHANGE_REQ);

		radio_states = (powered ? MCE_RADIO_STATE_BLUETOOTH : 0);

		DBG("Changing MCE Bluetooth radio state to: %d", radio_states);

		dbus_message_append_args(msg, DBUS_TYPE_UINT32, &radio_states,
					DBUS_TYPE_UINT32, &radio_mask,
					DBUS_TYPE_INVALID);

		if (dbus_connection_send(conn, msg, NULL))
			mce_bt_on = powered;
		else
			error("calling %s failed", MCE_RADIO_STATES_CHANGE_REQ);

		dbus_message_unref(msg);
	}
}

static int mce_probe(struct btd_adapter *adapter)
{

	DBG("path %s", adapter_get_path(adapter));

	watch_id = g_dbus_add_signal_watch(conn, NULL, MCE_SIGNAL_PATH,
					MCE_SIGNAL_IF, MCE_RADIO_STATES_SIG,
					mce_signal_callback, adapter, NULL);

	tklock_watch_id = g_dbus_add_signal_watch(conn, NULL, MCE_SIGNAL_PATH,
					MCE_SIGNAL_IF, MCE_TKLOCK_MODE_SIG,
					mce_tklock_mode_cb, adapter, NULL);

	btd_adapter_register_powered_callback(adapter, adapter_powered);

	return 0;
}

static void mce_remove(struct btd_adapter *adapter)
{
	DBG("path %s", adapter_get_path(adapter));

	if (watch_id > 0)
		g_dbus_remove_watch(conn, watch_id);

	if (tklock_watch_id > 0)
		g_dbus_remove_watch(conn, tklock_watch_id);

	btd_adapter_unregister_powered_callback(adapter, adapter_powered);
}

static struct btd_adapter_driver mce_driver = {
	.name	= "mce",
	.probe	= mce_probe,
	.remove	= mce_remove,
};

static int maemo6_init(void)
{
	DBG("init maemo6 plugin");

	conn = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);
	if (conn == NULL) {
		error("Unable to connect to D-Bus");
		return -1;
	}

	return btd_register_adapter_driver(&mce_driver);
}

static void maemo6_exit(void)
{
	DBG("exit maemo6 plugin");

	if (conn != NULL)
		dbus_connection_unref(conn);

	btd_unregister_adapter_driver(&mce_driver);
}

BLUETOOTH_PLUGIN_DEFINE(maemo6, VERSION,
		BLUETOOTH_PLUGIN_PRIORITY_DEFAULT, maemo6_init, maemo6_exit)
