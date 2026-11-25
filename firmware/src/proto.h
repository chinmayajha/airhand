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
#ifndef PROTO_VERSION
#error "PROTO_VERSION comes from firmware/VERSION"
#endif

enum proto_report { REP_KEYS, REP_MOUSE, REP_CONSUMER, REP_POINTER, REP_TOUCH };

#define KEYS_LEN 8
#define MOUSE_LEN 5
#define CONSUMER_LEN 2
#define POINTER_LEN 5
#define TOUCH_LEN 7
#define WIN8_LEN 1
#define POINTER_MAX 0x7FFF
/* CONFIG_BT_MAX_PAIRED */
#define PROTO_HOSTS_MAX 4
#define PROTO_ADDR_LEN 17
#define PROTO_NAME_MAX 32

struct proto_host {
	char addr[PROTO_ADDR_LEN + 1];
	char name[PROTO_NAME_MAX + 1];
};

/* chosen and connected are indexes into list, -1 for any host and for none. */
struct proto_hosts {
	int count;
	int chosen;
	int connected;
	struct proto_host list[PROTO_HOSTS_MAX];
};

struct proto_ops {
	/* 0 when a host can take reports, -ENOTCONN without a host, -EACCES on a link that is not encrypted. */
	int (*ready)(void);
	/* 0 when the stack queued the report, a negative errno otherwise. */
	int (*send)(enum proto_report report, const uint8_t *data, size_t len);
	void (*sleep_ms)(uint32_t ms);
	int (*unpair)(void);
	void (*status)(char *out, size_t len);
	/* Marks the running image good, so MCUboot keeps it; called on every version request. */
	void (*confirm)(void);
	/* Reboots into the bootloader's update mode once the reply is out; 0, or a negative errno. */
	int (*update)(void);
	const char *chip_id;
	int hid_init_err;
	void (*hosts)(struct proto_hosts *out);
	/* Lets only list[index] of hosts() connect and drops any other; -1 lets every paired host connect again. */
	int (*use)(int index);
};

/* Runs one line; returns the reply line without its newline. Empty lines get no reply (an empty string). */
const char *proto_handle(const struct proto_ops *ops, const char *line);

#endif
