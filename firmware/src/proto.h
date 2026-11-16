/*
 * Serial protocol v2, free of Zephyr so the host tests can build it.
 * A request is "<id> <verb> [args]" and gets exactly one reply, "<id> ok [data]" or "<id> err <reason>".
 */
#ifndef HIDWIRE_PROTO_H
#define HIDWIRE_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_LINE_MAX 256
#define PROTO_REPLY_MAX 160
#define PROTO_VERSION "2.2.0"

enum proto_report { REP_KEYS, REP_MOUSE, REP_CONSUMER, REP_POINTER, REP_WHEEL };

#define KEYS_LEN 8
#define MOUSE_LEN 3
#define CONSUMER_LEN 2
#define POINTER_LEN 5
#define WHEEL_LEN 5
#define POINTER_MAX 0x7FFF

struct proto_ops {
	/* 0 when a host can take reports, -ENOTCONN without a host, -EACCES on a link that is not encrypted. */
	int (*ready)(void);
	/* 0 when the stack queued the report, a negative errno otherwise. */
	int (*send)(enum proto_report report, const uint8_t *data, size_t len);
	void (*sleep_ms)(uint32_t ms);
	int (*unpair)(void);
	void (*status)(char *out, size_t len);
	/* Restarts into the USB bootloader a moment after the reply has gone out. */
	void (*bootloader)(void);
	const char *chip_id;
	int hid_init_err;
};

/* Runs one line; returns the reply line without its newline. Empty lines get no reply (an empty string). */
const char *proto_handle(const struct proto_ops *ops, const char *line);

#endif
