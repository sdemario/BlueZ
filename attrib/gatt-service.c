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

#include <glib.h>
#include <bluetooth/uuid.h>
#include <bluetooth/sdp.h>

#include "att.h"
#include "gattrib.h"
#include "attrib-server.h"
#include "gatt-service.h"
#include "log.h"
#include "glib-compat.h"

struct gatt_info {
	bt_uuid_t uuid;
	uint8_t props;
	int authentication;
	int authorization;
	GSList *callbacks;
	unsigned int num_attrs;
	uint16_t *value_handle;
	uint16_t *ccc_handle;
};

struct attrib_cb {
	attrib_event_t event;
	void *fn;
};

static GSList *parse_opts(gatt_option opt1, va_list args)
{
	gatt_option opt = opt1;
	struct gatt_info *info;
	struct attrib_cb *cb;
	GSList *l = NULL;

	info = g_new0(struct gatt_info, 1);
	l = g_slist_append(l, info);

	while (opt != GATT_OPT_INVALID) {
		switch (opt) {
		case GATT_OPT_CHR_UUID:
			bt_uuid16_create(&info->uuid, va_arg(args, int));
			/* characteristic declaration and value */
			info->num_attrs += 2;
			break;
		case GATT_OPT_CHR_PROPS:
			info->props = va_arg(args, int);

			if (info->props & (ATT_CHAR_PROPER_NOTIFY |
						ATT_CHAR_PROPER_INDICATE))
				/* client characteristic configuration */
				info->num_attrs += 1;

			/* TODO: "Extended Properties" property requires a
			 * descriptor, but it is not supported yet. */
			break;
		case GATT_OPT_CHR_VALUE_CB:
			cb = g_new0(struct attrib_cb, 1);
			cb->event = va_arg(args, attrib_event_t);
			cb->fn = va_arg(args, void *);
			info->callbacks = g_slist_append(info->callbacks, cb);
			break;
		case GATT_OPT_CHR_VALUE_GET_HANDLE:
			info->value_handle = va_arg(args, void *);
			break;
		case GATT_OPT_CCC_GET_HANDLE:
			info->ccc_handle = va_arg(args, void *);
			break;
		case GATT_OPT_CHR_AUTHENTICATION:
			info->authentication = va_arg(args, gatt_option);
			break;
		case GATT_OPT_CHR_AUTHORIZATION:
			info->authorization = va_arg(args, gatt_option);
			break;
		default:
			error("Invalid option: %d", opt);
		}

		opt = va_arg(args, gatt_option);
		if (opt == GATT_OPT_CHR_UUID) {
			info = g_new0(struct gatt_info, 1);
			l = g_slist_append(l, info);
		}
	}

	return l;
}

static int att_read_reqs(int authorization, int authentication, uint8_t props)
{
	if (authorization == GATT_CHR_VALUE_READ ||
				authorization == GATT_CHR_VALUE_BOTH)
		return ATT_AUTHORIZATION;
	else if (authentication == GATT_CHR_VALUE_READ ||
				authentication == GATT_CHR_VALUE_BOTH)
		return ATT_AUTHENTICATION;
	else if (!(props & ATT_CHAR_PROPER_READ))
		return ATT_NOT_PERMITTED;

	return ATT_NONE;
}

static int att_write_reqs(int authorization, int authentication, uint8_t props)
{
	if (authorization == GATT_CHR_VALUE_WRITE ||
				authorization == GATT_CHR_VALUE_BOTH)
		return ATT_AUTHORIZATION;
	else if (authentication == GATT_CHR_VALUE_WRITE ||
				authentication == GATT_CHR_VALUE_BOTH)
		return ATT_AUTHENTICATION;
	else if (!(props & (ATT_CHAR_PROPER_WRITE |
					ATT_CHAR_PROPER_WRITE_WITHOUT_RESP)))
		return ATT_NOT_PERMITTED;

	return ATT_NONE;
}

static gint find_callback(gconstpointer a, gconstpointer b)
{
	const struct attrib_cb *cb = a;
	unsigned int event = GPOINTER_TO_UINT(b);

	return cb->event - event;
}

static gboolean add_characteristic(uint16_t *handle, struct gatt_info *info)
{
	int read_reqs, write_reqs;
	uint16_t h = *handle;
	struct attribute *a;
	bt_uuid_t bt_uuid;
	uint8_t atval[5];
	GSList *l;

	if (!info->uuid.value.u16 || !info->props) {
		error("Characteristic UUID or properties are missing");
		return FALSE;
	}

	read_reqs = att_read_reqs(info->authorization, info->authentication,
								info->props);
	write_reqs = att_write_reqs(info->authorization, info->authentication,
								info->props);

	/* TODO: static characteristic values are not supported, therefore a
	 * callback must be always provided if a read/write property is set */
	if (read_reqs != ATT_NOT_PERMITTED) {
		gpointer reqs = GUINT_TO_POINTER(ATTRIB_READ);

		if (!g_slist_find_custom(info->callbacks, reqs,
							find_callback)) {
			error("Callback for read required");
			return FALSE;
		}
	}

	if (write_reqs != ATT_NOT_PERMITTED) {
		gpointer reqs = GUINT_TO_POINTER(ATTRIB_WRITE);

		if (!g_slist_find_custom(info->callbacks, reqs,
							find_callback)) {
			error("Callback for write required");
			return FALSE;
		}
	}

	/* characteristic declaration */
	bt_uuid16_create(&bt_uuid, GATT_CHARAC_UUID);
	atval[0] = info->props;
	att_put_u16(h + 1, &atval[1]);
	att_put_u16(info->uuid.value.u16, &atval[3]);
	attrib_db_add(h++, &bt_uuid, ATT_NONE, ATT_NOT_PERMITTED, atval,
								sizeof(atval));

	/* characteristic value */
	a = attrib_db_add(h++, &info->uuid, read_reqs, write_reqs, NULL, 0);
	for (l = info->callbacks; l != NULL; l = l->next) {
		struct attrib_cb *cb = l->data;

		switch (cb->event) {
		case ATTRIB_READ:
			a->read_cb = cb->fn;
			break;
		case ATTRIB_WRITE:
			a->write_cb = cb->fn;
			break;
		}
	}

	if (info->value_handle != NULL)
		*info->value_handle = a->handle;

	/* client characteristic configuration descriptor */
	if (info->props & (ATT_CHAR_PROPER_NOTIFY | ATT_CHAR_PROPER_INDICATE)) {
		uint8_t cfg_val[2];

		bt_uuid16_create(&bt_uuid, GATT_CLIENT_CHARAC_CFG_UUID);
		cfg_val[0] = 0x00;
		cfg_val[1] = 0x00;
		a = attrib_db_add(h++, &bt_uuid, ATT_NONE, ATT_AUTHENTICATION,
						cfg_val, sizeof(cfg_val));

		if (info->ccc_handle != NULL)
			*info->ccc_handle = a->handle;
	}

	*handle = h;

	return TRUE;
}

static void free_gatt_info(void *data)
{
	struct gatt_info *info = data;

	g_slist_free_full(info->callbacks, g_free);
	g_free(info);
}

void gatt_service_add(uint16_t uuid, uint16_t svc_uuid, gatt_option opt1, ...)
{
	uint16_t start_handle, h;
	unsigned int size;
	bt_uuid_t bt_uuid;
	uint8_t atval[2];
	va_list args;
	GSList *chrs, *l;

	va_start(args, opt1);
	chrs = parse_opts(opt1, args);
	/* calculate how many attributes are necessary for this service */
	for (l = chrs, size = 1; l != NULL; l = l->next) {
		struct gatt_info *info = l->data;
		size += info->num_attrs;
	}
	va_end(args);

	start_handle = attrib_db_find_avail(size);
	if (start_handle == 0) {
		error("Not enough free handles to register service");
		goto done;
	}

	DBG("New service: handle 0x%04x, UUID 0x%04x, %d attributes",
						start_handle, svc_uuid, size);

	/* service declaration */
	h = start_handle;
	bt_uuid16_create(&bt_uuid, uuid);
	att_put_u16(svc_uuid, &atval[0]);
	attrib_db_add(h++, &bt_uuid, ATT_NONE, ATT_NOT_PERMITTED, atval,
								sizeof(atval));

	for (l = chrs; l != NULL; l = l->next) {
		struct gatt_info *info = l->data;

		DBG("New characteristic: handle 0x%04x", h);
		if (!add_characteristic(&h, info))
			goto done;
	}

	g_assert(size < USHRT_MAX);
	g_assert(h - start_handle == (uint16_t) size);

done:
	g_slist_free_full(chrs, free_gatt_info);
}
