/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * airhand on an nRF52840: protocol lines arrive on the USB serial console, proto.c turns them into HID
 * reports, and this file sends those reports to the connected Bluetooth host over HID-over-GATT.
 * All output goes through say(), so an event line never lands inside a reply line.
 */

#include <errno.h>
#include <stdarg.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/retention/bootmode.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/services/hids.h>

#include "proto.h"

enum report_id { ID_KEYS = 1, ID_MOUSE, ID_CONSUMER, ID_POINTER, ID_TOUCH, ID_WIN8 };

#define SEND_TRIES 100
#define SEND_RETRY_MS 2
/* Lets the reply to update leave before USB goes. */
#define UPDATE_DELAY_MS 200
/* A soft reset with USB still attached can leave the port dead until a replug, so the host first sees it go. */
#define DETACH_MS 500
/* A new image that no client has spoken to by then is rolled back. */
#define TRIAL_S 60
/* Longer than the longest single wait in a verb: a 10 s hold. */
#define WATCHDOG_MS 30000

/*
 * Report descriptor, from the HID 1.11 specification and its usage tables. The order of the reports and
 * their sizes must match enum proto_report and the *_LEN constants in proto.h. A host caches this
 * descriptor when it pairs, so any change here means pairing again.
 */
static const uint8_t descriptor[] = {
	/* Keyboard as in HID 1.11 appendix B.1, without the LED output: modifiers, a reserved byte, 6 keys up to F24. */
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, ID_KEYS,
	0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
	0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x73, 0x05, 0x07, 0x19, 0x00, 0x29, 0x73, 0x81, 0x00,
	0xC0,

	/*
	 * Relative mouse: 3 buttons and 5 bits of padding, then X and Y moves, the wheel and horizontal pan,
	 * -127..127 each. iOS stops following the absolute pointer when a second mouse collection is added.
	 */
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, ID_MOUSE, 0x09, 0x01, 0xA1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06,
	0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,
	0xC0, 0xC0,

	/* Consumer control: one 16-bit usage up to 0x3FF, for keys such as AC Home and Volume Up. */
	0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, ID_CONSUMER,
	0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
	0xC0,

	/* Absolute pointer: 3 buttons and 5 bits of padding, then X and Y as 0..32767 across the screen. */
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, ID_POINTER, 0x09, 0x01, 0xA1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
	0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
	0xC0, 0xC0,

	/*
	 * Touch screen with one finger: tip switch and in range, then contact id, X and Y as 0..32767, and contact
	 * count. The contact id makes Linux bind hid-multitouch, so Android gets a direct touch screen.
	 */
	0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, ID_TOUCH, 0x09, 0x22, 0xA1, 0x02,
	0x09, 0x42, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
	0x95, 0x06, 0x81, 0x01,
	0x09, 0x51, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
	0xC0,
	0x05, 0x0D, 0x09, 0x54, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
	/*
	 * Vendor feature 0xFF0000C6, one byte: Linux then takes the touch screen as a Windows 8 one, whose class
	 * keeps the mouse, so Android shows a cursor. Without it hid-multitouch drops every mouse collection.
	 */
	0x85, ID_WIN8, 0x06, 0x00, 0xFF, 0x09, 0xC6, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0xB1, 0x02,
	0xC0,
};

static const struct {
	uint8_t id;
	uint8_t size;
} reports[] = {
	[REP_KEYS] = {ID_KEYS, KEYS_LEN},
	[REP_MOUSE] = {ID_MOUSE, MOUSE_LEN},
	[REP_CONSUMER] = {ID_CONSUMER, CONSUMER_LEN},
	[REP_POINTER] = {ID_POINTER, POINTER_LEN},
	[REP_TOUCH] = {ID_TOUCH, TOUCH_LEN},
};

BT_HIDS_DEF(hid_service, KEYS_LEN, MOUSE_LEN, CONSUMER_LEN, POINTER_LEN, TOUCH_LEN, WIN8_LEN);

static K_MUTEX_DEFINE(host_lock);
static struct bt_conn *host;
static uint16_t interval_units;
static K_MUTEX_DEFINE(say_lock);
static int service_err;
static const struct device *const watchdog = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int watchdog_channel = -1;

static void say(const char *fmt, ...)
{
	char line[PROTO_REPLY_MAX + 16];
	va_list args;

	va_start(args, fmt);
	vsnprintk(line, sizeof(line), fmt, args);
	va_end(args);
	k_mutex_lock(&say_lock, K_FOREVER);
	printk("%s\n", line);
	k_mutex_unlock(&say_lock);
}

static struct bt_conn *host_conn(void)
{
	struct bt_conn *conn;

	k_mutex_lock(&host_lock, K_FOREVER);
	conn = host ? bt_conn_ref(host) : NULL;
	k_mutex_unlock(&host_lock);
	return conn;
}

static void set_host(struct bt_conn *conn)
{
	k_mutex_lock(&host_lock, K_FOREVER);
	if (host) {
		bt_conn_unref(host);
	}
	host = conn ? bt_conn_ref(conn) : NULL;
	k_mutex_unlock(&host_lock);
}

static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, CONFIG_BT_DEVICE_APPEARANCE & 0xFF, CONFIG_BT_DEVICE_APPEARANCE >> 8),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
};

static const struct bt_data scan_data[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void start_advertising(struct k_work *work)
{
	ARG_UNUSED(work);
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, adv_data, ARRAY_SIZE(adv_data), scan_data,
				  ARRAY_SIZE(scan_data));

	if (err) {
		say("evt ready adv=failed err=%d", err);
	} else {
		say("evt ready adv=on");
	}
}

/* Advertising cannot restart from inside the disconnected callback. */
static K_WORK_DEFINE(advertising, start_advertising);

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		say("evt connect_failed hci=0x%02x", err);
		k_work_submit(&advertising);
		return;
	}
	bt_hids_connected(&hid_service, conn);
	set_host(conn);
	say("evt connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	bt_hids_disconnected(&hid_service, conn);
	set_host(NULL);
	say("evt disconnected reason=0x%02x", reason);
	k_work_submit(&advertising);
}

static void on_security(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	ARG_UNUSED(conn);
	if (err) {
		say("evt security_failed sec=%u err=%d", level, err);
	} else {
		say("evt security sec=%u", level);
	}
}

static void on_params(struct bt_conn *conn, uint16_t interval, uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);
	interval_units = interval;
	say("evt params interval_ms=%u.%02u latency=%u timeout_ms=%u", interval * 125 / 100, interval * 125 % 100,
	    latency, timeout * 10);
}

BT_CONN_CB_DEFINE(link_events) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security,
	.le_param_updated = on_params,
};

static void on_paired(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);
	say("evt paired bonded=%s", bonded ? "yes" : "no");
}

static void on_pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	ARG_UNUSED(conn);
	say("evt pairing_failed reason=%d", reason);
}

static struct bt_conn_auth_info_cb pairing_events = {
	.pairing_complete = on_paired,
	.pairing_failed = on_pairing_failed,
};

static int start_hid_service(void)
{
	struct bt_hids_init_param params = {
		.rep_map = {.data = descriptor, .size = sizeof(descriptor)},
		.info = {.bcd_hid = 0x0101, .flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE},
	};

	for (size_t i = 0; i < ARRAY_SIZE(reports); i++) {
		params.inp_rep_group_init.reports[i].id = reports[i].id;
		params.inp_rep_group_init.reports[i].size = reports[i].size;
	}
	params.inp_rep_group_init.cnt = ARRAY_SIZE(reports);
	params.feat_rep_group_init.reports[0].id = ID_WIN8;
	params.feat_rep_group_init.reports[0].size = WIN8_LEN;
	params.feat_rep_group_init.cnt = 1;
	return bt_hids_init(&hid_service, &params);
}

static int host_ready(void)
{
	struct bt_conn *conn = host_conn();

	if (!conn) {
		return -ENOTCONN;
	}
	bt_security_t level = bt_conn_get_security(conn);

	bt_conn_unref(conn);
	return level < BT_SECURITY_L2 ? -EACCES : 0;
}

static void feed(void)
{
	if (watchdog_channel >= 0) {
		wdt_feed(watchdog, watchdog_channel);
	}
}

static void wait_ms(uint32_t ms)
{
	k_msleep(ms);
	feed();
}

static int send_report(enum proto_report report, const uint8_t *data, size_t len)
{
	struct bt_conn *conn = host_conn();
	int err = -ENOTCONN;

	if (!conn) {
		return err;
	}
	/* Reports that come faster than the connection interval use up the stack's buffers. */
	for (int try = 0; try < SEND_TRIES; try++) {
		err = bt_hids_inp_rep_send(&hid_service, conn, report, data, len, NULL);
		if (err != -ENOMEM && err != -EAGAIN) {
			break;
		}
		wait_ms(SEND_RETRY_MS);
	}
	bt_conn_unref(conn);
	return err;
}

/* A new image that hangs is reset, and MCUboot, which feeds the watchdog while it swaps, rolls it back. */
static void start_watchdog(void)
{
	const struct wdt_timeout_cfg timeout = {.window.max = WATCHDOG_MS, .flags = WDT_FLAG_RESET_SOC};

	watchdog_channel = wdt_install_timeout(watchdog, &timeout);
	if (watchdog_channel < 0 || wdt_setup(watchdog, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
		say("evt watchdog_failed err=%d", watchdog_channel);
		watchdog_channel = -1;
	}
}

static void restart(struct k_work *work)
{
	ARG_UNUSED(work);
	STRUCT_SECTION_FOREACH(usbd_context, usb) {
		usbd_disable(usb);
	}
	k_msleep(DETACH_MS);
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(update_restart, restart);
static K_WORK_DELAYABLE_DEFINE(trial_end, restart);

static int enter_update(void)
{
	int err = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);

	if (!err) {
		k_work_schedule(&update_restart, K_MSEC(UPDATE_DELAY_MS));
	}
	return err;
}

static void confirm_image(void)
{
	if (mcuboot_swap_type() != BOOT_SWAP_TYPE_REVERT) {
		return;
	}
	int err = boot_write_img_confirmed();

	if (err) {
		say("evt confirm_failed err=%d", err);
	} else {
		k_work_cancel_delayable(&trial_end);
		say("evt confirmed");
	}
}

static int forget_hosts(void)
{
	return bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
}

static void describe(char *out, size_t len)
{
	struct bt_conn *conn = host_conn();
	unsigned int level = 0;

	if (conn) {
		level = bt_conn_get_security(conn);
		bt_conn_unref(conn);
	}
	snprintk(out, len, "conn=%s sec=%u interval_ms=%u hid_init=%d uptime_s=%u", conn ? "yes" : "no", level,
		 interval_units * 125 / 100, service_err, (unsigned int)(k_uptime_get() / 1000));
}

static void read_chip_id(char *out, size_t len)
{
	uint8_t raw[8];
	ssize_t count = hwinfo_get_device_id(raw, sizeof(raw));
	size_t at = 0;

	out[0] = '\0';
	for (ssize_t i = 0; i < count && at + 2 < len; i++) {
		at += snprintk(out + at, len - at, "%02X", raw[i]);
	}
}

/* A longer line is cut at PROTO_LINE_MAX bytes, which proto_handle still refuses as too long. */
static const char *next_line(const struct device *console)
{
	static char line[PROTO_LINE_MAX + 1];
	size_t len = 0;
	unsigned char c;

	for (;;) {
		feed();
		if (uart_poll_in(console, &c) != 0) {
			k_msleep(1);
		} else if (c == '\n') {
			line[len] = '\0';
			return line;
		} else if (c != '\r' && len < PROTO_LINE_MAX) {
			line[len++] = c;
		}
	}
}

int main(void)
{
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	static char chip_id[17];
	static struct proto_ops ops = {
		.ready = host_ready,
		.send = send_report,
		.sleep_ms = wait_ms,
		.unpair = forget_hosts,
		.status = describe,
		.confirm = confirm_image,
		.update = enter_update,
		.chip_id = chip_id,
	};

	start_watchdog();
	if (mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT) {
		k_work_schedule(&trial_end, K_SECONDS(TRIAL_S));
	}
	read_chip_id(chip_id, sizeof(chip_id));
	service_err = start_hid_service();
	ops.hid_init_err = service_err;
	bt_conn_auth_info_cb_register(&pairing_events);

	int err = bt_enable(NULL);

	if (err) {
		say("evt bt_failed err=%d", err);
	} else {
		settings_load();
		k_work_submit(&advertising);
	}

	for (;;) {
		const char *reply = proto_handle(&ops, next_line(console));

		if (*reply) {
			say("%s", reply);
		}
	}
}
