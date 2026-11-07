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
 * Delivery settings of the client requests: packed requests are decoded
 * again and checked field by field. No SSC service is involved.
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
test_default_is_wakeup_without_batching (void)
{
	SSCDeliveryConfig config;
	const guint32 ids[] = { SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS,
				SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE,
				SSC_MSG_REQUEST_DISABLE_REPORT,
				SSC_MSG_REQUEST_GET_ATTRIBUTES,
				SSC_MSG_REQUEST_SUID };

	config_from (NULL, NULL, NULL, &config);
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
	g_assert_cmpuint (config.batch_period_us, ==, 0);
	g_assert_cmpuint (config.flush_period_us, ==, 0);

	for (gsize i = 0; i < G_N_ELEMENTS (ids); i++) {
		SscClientRequest *msg = pack_and_unpack (&config, ids[i]);

		g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
		g_assert_null (msg->request->batching);
		ssc_client_request__free_unpacked (msg, NULL);
	}
}

static void
test_nowakeup_applies_to_stream_requests_only (void)
{
	SSCDeliveryConfig config;
	SscClientRequest *msg;

	config_from ("nowakeup", NULL, NULL, &config);
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);

	msg = pack_and_unpack (&config, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	g_assert_null (msg->request->batching);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&config, SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&config, SSC_MSG_REQUEST_GET_ATTRIBUTES);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&config, SSC_MSG_REQUEST_DISABLE_REPORT);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
	ssc_client_request__free_unpacked (msg, NULL);
}

static void
test_batching_applies_to_continuous_streams (void)
{
	SSCDeliveryConfig config;
	SscClientRequest *msg;

	config_from ("wakeup", "2000000", "4000000", &config);
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);

	msg = pack_and_unpack (&config, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
	g_assert_nonnull (msg->request->batching);
	g_assert_cmpuint (msg->request->batching->batch_period, ==, 2000000);
	g_assert_true (msg->request->batching->has_flush_period);
	g_assert_cmpuint (msg->request->batching->flush_period, ==, 4000000);
	ssc_client_request__free_unpacked (msg, NULL);

	msg = pack_and_unpack (&config, SSC_MSG_REQUEST_ENABLE_REPORT_ON_CHANGE);
	g_assert_null (msg->request->batching);
	ssc_client_request__free_unpacked (msg, NULL);

	config_from ("nowakeup", "200000", NULL, &config);
	msg = pack_and_unpack (&config, SSC_MSG_REQUEST_ENABLE_REPORT_CONTINUOUS);
	g_assert_cmpint (msg->config->suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	g_assert_cmpuint (msg->request->batching->batch_period, ==, 200000);
	g_assert_false (msg->request->batching->has_flush_period);
	ssc_client_request__free_unpacked (msg, NULL);
}

static void
test_invalid_values_are_ignored (void)
{
	SSCDeliveryConfig config;

	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_SUSPEND_MODE=sleep*");
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_BATCH_PERIOD_US=0*");
	config_from ("sleep", "0", NULL, &config);
	g_test_assert_expected_messages ();
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_WAKEUP);
	g_assert_cmpuint (config.batch_period_us, ==, 0);

	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_BATCH_PERIOD_US=4294967296*");
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_FLUSH_PERIOD_US without*");
	config_from ("nowakeup", "4294967296", "1000", &config);
	g_test_assert_expected_messages ();
	g_assert_cmpint (config.suspend_mode, ==, SSC_SUSPEND_MODE_NO_WAKEUP);
	g_assert_cmpuint (config.batch_period_us, ==, 0);
	g_assert_cmpuint (config.flush_period_us, ==, 0);

	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Ignoring SSC_BATCH_PERIOD_US=2s*");
	config_from ("wakeup", "2s", NULL, &config);
	g_test_assert_expected_messages ();
	g_assert_cmpuint (config.batch_period_us, ==, 0);
}

int
main (int argc, char *argv[])
{
	setlocale (LC_ALL, "");
	g_test_init (&argc, &argv, NULL);
	g_test_add_func ("/libssc/delivery/default", test_default_is_wakeup_without_batching);
	g_test_add_func ("/libssc/delivery/nowakeup", test_nowakeup_applies_to_stream_requests_only);
	g_test_add_func ("/libssc/delivery/batching", test_batching_applies_to_continuous_streams);
	g_test_add_func ("/libssc/delivery/invalid", test_invalid_values_are_ignored);
	return g_test_run ();
}
