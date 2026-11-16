/*
 * The serial protocol, free of Zephyr so the host tests can build it.
 * A request is "<id> <verb> [args]" and gets exactly one reply, "<id> ok [data]" or "<id> err <reason>".
 */
#ifndef AIRHAND_PROTO_H
#define AIRHAND_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define PROTO_LINE_MAX 256
#define PROTO_REPLY_MAX 160
#define PROTO_VERSION "0.1.2"

enum proto_report { REP_KEYS, REP_MOUSE, REP_CONSUMER, REP_POINTER, REP_TOUCH };

#define KEYS_LEN 8
#define MOUSE_LEN 5
#define CONSUMER_LEN 2
#define POINTER_LEN 5
#define TOUCH_LEN 7
#define POINTER_MAX 0x7FFF

struct proto_ops {
	/* 0 when a host can take reports, -ENOTCONN without a host, -EACCES on a link that is not encrypted. */
	int (*ready)(void);
	/* 0 when the stack queued the report, a negative errno otherwise. */
	int (*send)(enum proto_report report, const uint8_t *data, size_t len);
	void (*sleep_ms)(uint32_t ms);
	int (*unpair)(void);
	void (*status)(char *out, size_t len);
	const char *chip_id;
	int hid_init_err;
};

/* Runs one line; returns the reply line without its newline. Empty lines get no reply (an empty string). */
const char *proto_handle(const struct proto_ops *ops, const char *line);

#endif
