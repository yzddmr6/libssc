/*
 * libssc: Library to expose Qualcomm Sensor Core sensors
 * Copyright (C) 2026 yzddmr6
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Delivery settings of the client requests, per sensor type and with the
 * environment overrides: packed requests are decoded again and checked
 * field by field. No SSC service is involved.
 */

#include <glib.h>
#include <locale.h>
#include "libssc-client-private.h"

#define UID_HIGH 0x1122334455667788UL
#define UID_LOW  0x99aabbccddeeff00UL

static SscClientRequest *
pack_and_unpack (const SSCDeliveryConfig *delivery, guint32 message_id)
{
	g_autoptr (GArray) payload = g_array_new (FALSE, FALSE, 1);
	g_autoptr (GArray) buf = NULL;
	SscClientRequest *msg;
	guint8 byte = 0x0d;

	g_array_append_val (payload, byte);
	buf = ssc_client_pack_request (delivery, UID_HIGH, UID_LOW, message_id, payload);
	msg = ssc_client_request__unpack (NULL, buf->len, (const uint8_t *) buf->data);
	g_assert_nonnull (msg);
	g_assert_cmphex (msg->uid->high, ==, UID_HIGH);
	g_assert_cmphex (msg->uid->low, ==, UID_LOW);
	g_assert_cmpuint (msg->msg_id, ==, message_id);
	g_assert_cmpint (msg->config->processor, ==, SSC_PROCESSOR_APSS);
	g_assert_true (msg->request->has_msg);
	g_assert_cmpuint (msg->request->msg.len, ==, 1);
	return msg;
}

static void
config_from (const gchar *mode, const gchar *batch, const gchar *flush, SSCDeliveryConfig *config)
{
	const gchar *names[] = { "SSC_SUSPEND_MODE", "SSC_BATCH_PERIOD_US", "SSC_FLUSH_PERIOD_US" };
	const gchar *values[] = { mode, batch, flush };

	for (gsize i = 0; i < G_N_ELEMENTS (names); i++) {
		if (values[i])
			g_setenv (names[i], values[i], TRUE);
		else
			g_unsetenv (names[i]);
	}
	ssc_client_delivery_config_from_env (config);
}

static void
resolve (const gchar *mode, const gchar *batch, const gchar *flush, const gchar *data_type, SSCDeliveryConfig *delivery)
{
	SSCDeliveryConfig overrides;

	config_from (mode, batch, flush, &overrides);
	ssc_client_delivery_for_sensor (&overrides, data_type, delivery);
}

static const gchar * const motion_types[] = { "accel", "gyro", "mag", "rotv" };
static const gchar * const wakeup_types[] = { "proximity", "ambient_light", "sig_motion", "hall", "suid", "" };

static void
test_default_follows_sensor_type (void)
{
	SSCDeliveryConfig config;

	config_from (NULL, NULL, NULL, &config);
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_BY_SENSOR_TYPE);
	g_assert_cmpuint (config.batch_period_us, ==, 0);
	g_assert_cmpuint (config.flush_period_us, ==, 0);

	for (gsize i = 0; i < G_N_ELEMENTS (motion_types); i++)
		g_assert_cmpint (ssc_client_default_suspend_mode (motion_types[i]), ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	for (gsize i = 0; i < G_N_ELEMENTS (wakeup_types); i++)
		g_assert_cmpint (ssc_client_default_suspend_mode (wakeup_types[i]), ==, SSC_SUSPEND_MODE_WAKEUP);
	g_assert_cmpint (ssc_client_default_suspend_mode (NULL), ==, SSC_SUSPEND_MODE_WAKEUP);
	/* Exact names only */
	g_assert_cmpint (ssc_client_default_suspend_mode ("ACCEL"), ==, SSC_SUSPEND_MODE_WAKEUP);
	g_assert_cmpint (ssc_client_default_suspend_mode ("accel_cal"), ==, SSC_SUSPEND_MODE_WAKEUP);
}

static void
test_motion_streams_default_to_nowakeup (void)
{
	const guint32 others[] = { SSC_MSG_REQUEST_DISABLE_REPORT,
				   SSC_MSG_REQUEST_GET_ATTRIBUTES,
				   SSC_MSG_REQUEST_SUID };

	for (gsize i = 0; i < G_N_ELEMENTS (motion_types); i++) {
		SSCDeliveryConfig delivery;
		SscClientRequest *msg;

		resolve (NULL, NULL, NULL, motion_types[i], &delivery);
		g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
		g_assert_cmpuint (delivery.batch_period_us, ==, 0);

		msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
		g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
		g_assert_null (msg->request->batching);
		ssc_client_request__free_unpacked (msg, NULL);

		msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE);
		g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
		ssc_client_request__free_unpacked (msg, NULL);

		/* Only the stream itself: the other requests keep wake-up delivery */
		for (gsize j = 0; j < G_N_ELEMENTS (others); j++) {
			msg = pack_and_unpack (&delivery, others[j]);
			g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
			g_assert_null (msg->request->batching);
			ssc_client_request__free_unpacked (msg, NULL);
		}
	}
}

static void
test_wakeup_types_keep_wakeup (void)
{
	const guint32 ids[] = { SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS,
				SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE,
				SSC_MSG_REQUEST_DISABLE_REPORT,
				SSC_MSG_REQUEST_GET_ATTRIBUTES,
				SSC_MSG_REQUEST_SUID };

	for (gsize i = 0; i <= G_N_ELEMENTS (wakeup_types); i++) {
		/* The last round is a request without a sensor type */
		const gchar *type = i < G_N_ELEMENTS (wakeup_types) ? wakeup_types[i] : NULL;
		SSCDeliveryConfig delivery;

		resolve (NULL, NULL, NULL, type, &delivery);
		g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
		for (gsize j = 0; j < G_N_ELEMENTS (ids); j++) {
			SscClientRequest *msg = pack_and_unpack (&delivery, ids[j]);

			g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
			g_assert_null (msg->request->batching);
			ssc_client_request__free_unpacked (msg, NULL);
		}
	}
}

/*
 * The wire bytes: config (field 3) is {processor 1, delivery type}, the body
 * (field 4) has no batching (field 1). A wake-up request is what libssc 0.4.4
 * always sent.
 */
static void
test_wire_encoding (void)
{
	static const guint8 wakeup[] = {
		0x0a, 0x12, 0x09, 0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99,
		0x11, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
		0x15, 0x01, 0x02, 0x00, 0x00,
		0x1a, 0x04, 0x08, 0x01, 0x10, 0x00,
		0x22, 0x03, 0x12, 0x01, 0x0d,
	};
	g_autoptr (GArray) payload = g_array_new (FALSE, FALSE, 1);
	g_autoptr (GArray) proximity = NULL;
	g_autoptr (GArray) accel = NULL;
	SSCDeliveryConfig delivery;
	guint8 byte = 0x0d;

	g_array_append_val (payload, byte);

	resolve (NULL, NULL, NULL, "proximity", &delivery);
	proximity = ssc_client_pack_request (&delivery, UID_HIGH, UID_LOW, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS, payload);
	g_assert_cmpmem (proximity->data, proximity->len, wakeup, sizeof (wakeup));

	resolve (NULL, NULL, NULL, "accel", &delivery);
	accel = ssc_client_pack_request (&delivery, UID_HIGH, UID_LOW, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS, payload);
	g_assert_cmpuint (accel->len, ==, sizeof (wakeup));
	/* Only the delivery type differs */
	g_assert_cmpmem (accel->data, 30, wakeup, 30);
	g_assert_cmphex (((guint8 *) accel->data)[30], ==, 0x01);
	g_assert_cmpmem (accel->data + 31, accel->len - 31, wakeup + 31, sizeof (wakeup) - 31);
}

static void
test_environment_overrides_every_type (void)
{
	SSCDeliveryConfig delivery;
	SscClientRequest *msg;

	/* wakeup brings back the 0.4.4 requests for the motion types */
	for (gsize i = 0; i < G_N_ELEMENTS (motion_types); i++) {
		resolve ("wakeup", NULL, NULL, motion_types[i], &delivery);
		g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
		msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
		g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
		g_assert_null (msg->request->batching);
		ssc_client_request__free_unpacked (msg, NULL);
	}

	/* nowakeup is process wide, as before: it applies to every type */
	resolve ("nowakeup", NULL, NULL, "proximity", &delivery);
	g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_GET_ATTRIBUTES);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_DISABLE_REPORT);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
	ssc_client_request__free_unpacked (msg, NULL);

	/* An empty value is the same as none */
	resolve ("", NULL, NULL, "accel", &delivery);
	g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	resolve ("", NULL, NULL, "proximity", &delivery);
	g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
}

static void
test_batching_applies_to_continuous_streams (void)
{
	SSCDeliveryConfig delivery;
	SscClientRequest *msg;

	resolve ("wakeup", "2000000", "4000000", "accel", &delivery);
	g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);

	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
	g_assert_nonnull (msg->request->batching);
	g_assert_cmpuint (msg->request->batching->batch_period, ==, 2000000);
	g_assert_true (msg->request->batching->has_flush_period);
	g_assert_cmpuint (msg->request->batching->flush_period, ==, 4000000);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE);
	g_assert_null (msg->request->batching);
	ssc_client_request__free_unpacked (msg, NULL);

	/* A batch period alone keeps the per-type delivery */
	resolve (NULL, "200000", NULL, "accel", &delivery);
	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	g_assert_cmpuint (msg->request->batching->batch_period, ==, 200000);
	g_assert_false (msg->request->batching->has_flush_period);
	ssc_client_request__free_unpacked (msg, NULL);

	resolve (NULL, "200000", NULL, "proximity", &delivery);
	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
	g_assert_cmpuint (msg->request->batching->batch_period, ==, 200000);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&delivery, SSC_MSG_REQUEST_GET_ATTRIBUTES);
	g_assert_null (msg->request->batching);
	ssc_client_request__free_unpacked (msg, NULL);
}

static void
test_invalid_values_are_ignored (void)
{
	SSCDeliveryConfig config;
	SSCDeliveryConfig delivery;

	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_SUSPEND_MODE=sleep*");
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_BATCH_PERIOD_US=0*");
	config_from ("sleep", "0", NULL, &config);
	g_test_assert_expected_messages ();
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_BY_SENSOR_TYPE);
	g_assert_cmpuint (config.batch_period_us, ==, 0);
	ssc_client_delivery_for_sensor (&config, "accel", &delivery);
	g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	ssc_client_delivery_for_sensor (&config, "proximity", &delivery);
	g_assert_cmpint (delivery.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);

	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_BATCH_PERIOD_US=4294967296*");
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_FLUSH_PERIOD_US without*");
	config_from ("nowakeup", "4294967296", "1000", &config);
	g_test_assert_expected_messages ();
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	g_assert_cmpuint (config.batch_period_us, ==, 0);
	g_assert_cmpuint (config.flush_period_us, ==, 0);

	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_SUSPEND_MODE=default*");
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_BATCH_PERIOD_US=2s*");
	config_from ("default", "2s", NULL, &config);
	g_test_assert_expected_messages ();
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_BY_SENSOR_TYPE);
	g_assert_cmpuint (config.batch_period_us, ==, 0);
}

int
main (int argc, char *argv[])
{
	setlocale (LC_ALL, "");
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/libssc/delivery/default", test_default_follows_sensor_type);
	g_test_add_func ("/libssc/delivery/motion", test_motion_streams_default_to_nowakeup);
	g_test_add_func ("/libssc/delivery/wakeup-types", test_wakeup_types_keep_wakeup);
	g_test_add_func ("/libssc/delivery/wire", test_wire_encoding);
	g_test_add_func ("/libssc/delivery/override", test_environment_overrides_every_type);
	g_test_add_func ("/libssc/delivery/batching", test_batching_applies_to_continuous_streams);
	g_test_add_func ("/libssc/delivery/invalid", test_invalid_values_are_ignored);
	return g_test_run ();
}
