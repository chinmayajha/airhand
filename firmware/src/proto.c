#include "proto.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD_CTRL  0x01
#define MOD_SHIFT 0x02
#define MOD_ALT   0x04
#define MOD_GUI   0x08

/* iOS AssistiveTouch applies pointer reports about one connection interval late: it presses where its cursor was,
 * and a move sent right after a release turns a tap into a drag. */
#define SETTLE_MS     100
#define TAP_HOLD_MS   60
#define SWIPE_STEP_MS 15
#define WHEEL_STEP_MS 15
#define SWIPE_MAX_STEPS 300
#define CLICK_GAP_MS  80
#define MAX_CLICKS    5
#define MAX_HOLD_MS   10000
#define RESULT_SLOTS 8
#define ID_MAX 10

struct key {
	uint8_t mod;
	uint8_t code;
};

struct named {
	const char *name;
	uint8_t value;
};

static const struct named key_names[] = {
	{"enter", 40}, {"return", 40}, {"escape", 41}, {"esc", 41}, {"backspace", 42}, {"tab", 43},
	{"space", 44}, {"insert", 73}, {"home", 74}, {"pageup", 75}, {"delete", 76}, {"end", 77},
	{"pagedown", 78}, {"right", 79}, {"left", 80}, {"down", 81}, {"up", 82},
	{"f1", 58}, {"f2", 59}, {"f3", 60}, {"f4", 61}, {"f5", 62}, {"f6", 63},
	{"f7", 64}, {"f8", 65}, {"f9", 66}, {"f10", 67}, {"f11", 68}, {"f12", 69},
	{"printscreen", 70}, {"scrolllock", 71}, {"pause", 72}, {"numlock", 83},
	{"numpaddivide", 84}, {"numpadmultiply", 85}, {"numpadminus", 86}, {"numpadplus", 87},
	{"numpadenter", 88}, {"numpad1", 89}, {"numpad2", 90}, {"numpad3", 91}, {"numpad4", 92},
	{"numpad5", 93}, {"numpad6", 94}, {"numpad7", 95}, {"numpad8", 96}, {"numpad9", 97},
	{"numpad0", 98}, {"numpaddecimal", 99}, {"numpadperiod", 99}, {"capslock", 57},
	{"menu", 101}, {"application", 101}, {"f13", 104}, {"f14", 105}, {"f15", 106}, {"f16", 107},
	{"f17", 108}, {"f18", 109}, {"f19", 110}, {"f20", 111}, {"f21", 112}, {"f22", 113}, {"f23", 114},
	{"f24", 115},
};

static const struct named modifier_names[] = {
	{"ctrl", MOD_CTRL}, {"control", MOD_CTRL}, {"shift", MOD_SHIFT}, {"alt", MOD_ALT},
	{"option", MOD_ALT}, {"cmd", MOD_GUI}, {"command", MOD_GUI}, {"gui", MOD_GUI},
	{"meta", MOD_GUI}, {"win", MOD_GUI},
};

static const struct {
	const char *name;
	uint16_t usage;
} media_names[] = {
	{"home", 0x223}, {"back", 0x224}, {"search", 0x221}, {"volup", 0xE9}, {"voldown", 0xEA},
	{"mute", 0xE2}, {"playpause", 0xCD}, {"next", 0xB5}, {"previous", 0xB6}, {"stop", 0xB7},
	{"brightnessup", 0x6F}, {"brightnessdown", 0x70},
};

static const struct named button_names[] = {{"left", 1}, {"right", 2}, {"middle", 4}};

/* US layout: the characters on the keys from 0x2D to 0x38; 0x32 is the non-US hash key, which a US layout lacks. */
static const char punct[] = "-=[]\\\x01;'`,./";
static const char punct_shifted[] = "_+{}|\x01:\"~<>?";
static const char digit_shifted[] = "!@#$%^&*()";

static struct {
	char id[ID_MAX + 1];
	char reply[PROTO_REPLY_MAX];
} results[RESULT_SLOTS];
static unsigned int next_slot;

static uint16_t pointer_x, pointer_y;
static char reply[PROTO_REPLY_MAX];

struct run {
	const struct proto_ops *ops;
	int done;
	int total;
	int err;
};

static bool char_key(char c, struct key *k)
{
	const char *p;

	k->mod = 0;
	if (c >= 'a' && c <= 'z') {
		k->code = 4 + (c - 'a');
	} else if (c >= 'A' && c <= 'Z') {
		k->mod = MOD_SHIFT;
		k->code = 4 + (c - 'A');
	} else if (c >= '1' && c <= '9') {
		k->code = 30 + (c - '1');
	} else if (c == '0') {
		k->code = 39;
	} else if (c == '\n') {
		k->code = 40;
	} else if (c == '\t') {
		k->code = 43;
	} else if (c == ' ') {
		k->code = 44;
	} else if (c > 0x01 && (p = strchr(digit_shifted, c)) != NULL) {
		k->mod = MOD_SHIFT;
		k->code = 30 + (p - digit_shifted);
	} else if (c > 0x01 && (p = strchr(punct, c)) != NULL) {
		k->code = 0x2D + (p - punct);
	} else if (c > 0x01 && (p = strchr(punct_shifted, c)) != NULL) {
		k->mod = MOD_SHIFT;
		k->code = 0x2D + (p - punct_shifted);
	} else {
		return false;
	}
	return true;
}

static int lookup(const struct named *table, size_t count, const char *name)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(table[i].name, name) == 0) {
			return table[i].value;
		}
	}
	return -1;
}

#define LOOKUP(table, name) lookup(table, sizeof(table) / sizeof(table[0]), name)

static bool parse_combo(const char *combo, struct key *k)
{
	char buf[48];
	char *part, *save = NULL;
	char *last = NULL;
	uint8_t mod = 0;

	if (strlen(combo) >= sizeof(buf) || !*combo) {
		return false;
	}
	strcpy(buf, combo);
	for (char *c = buf; *c; c++) {
		*c = tolower((unsigned char)*c);
	}
	for (part = strtok_r(buf, "+", &save); part; part = strtok_r(NULL, "+", &save)) {
		if (last) {
			int m = LOOKUP(modifier_names, last);

			if (m < 0) {
				return false;
			}
			mod |= m;
		}
		last = part;
	}
	if (!last) {
		return false;
	}
	int named = LOOKUP(key_names, last);

	if (named >= 0) {
		k->mod = mod;
		k->code = named;
		return true;
	}
	if (strlen(last) == 1 && char_key(last[0], k)) {
		k->mod |= mod;
		return true;
	}
	return false;
}

/* Decodes a JSON string into ASCII; false on bad JSON or a character outside ASCII. */
static bool decode_json(const char *in, char *out, size_t out_len, bool *unsupported)
{
	size_t n = 0;

	*unsupported = false;
	if (*in++ != '"') {
		return false;
	}
	while (*in && *in != '"') {
		unsigned int c = (unsigned char)*in++;

		if (c == '\\') {
			char e = *in++;

			switch (e) {
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u': {
				char hex[5] = {0};

				for (int i = 0; i < 4; i++) {
					if (!isxdigit((unsigned char)in[i])) {
						return false;
					}
					hex[i] = in[i];
				}
				in += 4;
				c = strtoul(hex, NULL, 16);
				break;
			}
			default:
				return false;
			}
		}
		if (c > 0x7F) {
			*unsupported = true;
			c = '?';
		}
		if (n + 1 >= out_len) {
			return false;
		}
		out[n++] = c;
	}
	if (*in != '"' || in[1] != '\0') {
		return false;
	}
	out[n] = '\0';
	return true;
}

static bool parse_fraction(char **p, uint16_t *out)
{
	char *end;
	double v = strtod(*p, &end);

	if (end == *p || v < 0.0 || v > 1.0) {
		return false;
	}
	*p = end;
	*out = (uint16_t)(v * POINTER_MAX + 0.5);
	return true;
}

static bool parse_long(char **p, long *out)
{
	char *end;

	*out = strtol(*p, &end, 10);
	if (end == *p) {
		return false;
	}
	*p = end;
	return true;
}

static bool at_end(const char *p)
{
	while (*p == ' ') {
		p++;
	}
	return *p == '\0';
}

static void put(struct run *r, enum proto_report report, const uint8_t *data, size_t len)
{
	if (r->err) {
		return;
	}
	int err = r->ops->send(report, data, len);

	if (err) {
		r->err = err;
		return;
	}
	r->done++;
}

static void pointer(struct run *r, uint8_t buttons, uint16_t x, uint16_t y)
{
	uint8_t rep[POINTER_LEN] = {buttons, x & 0xFF, x >> 8, y & 0xFF, y >> 8};

	put(r, REP_POINTER, rep, sizeof(rep));
	if (!r->err) {
		pointer_x = x;
		pointer_y = y;
	}
}

static void press_key(struct run *r, struct key k)
{
	uint8_t down[KEYS_LEN] = {k.mod, 0, k.code};
	uint8_t up[KEYS_LEN] = {0};

	put(r, REP_KEYS, down, sizeof(down));
	put(r, REP_KEYS, up, sizeof(up));
}

static void consumer(struct run *r, uint16_t usage)
{
	uint8_t rep[CONSUMER_LEN] = {usage & 0xFF, usage >> 8};

	put(r, REP_CONSUMER, rep, sizeof(rep));
}

static void wait_ms(struct run *r, uint32_t ms)
{
	if (!r->err) {
		r->ops->sleep_ms(ms);
	}
}

/* The checks every report command passes before its first report; NULL when it may run. */
static const char *refusal(const struct proto_ops *ops)
{
	static char why[48];

	if (ops->hid_init_err) {
		snprintf(why, sizeof(why), "err hid init failed %d", ops->hid_init_err);
		return why;
	}
	int ready = ops->ready();

	if (ready == -ENOTCONN) {
		return "err not connected";
	}
	if (ready == -EACCES) {
		return "err not secure";
	}
	if (ready) {
		snprintf(why, sizeof(why), "err not ready %d", ready);
		return why;
	}
	return NULL;
}

static const char *outcome(const struct run *r)
{
	if (!r->err) {
		return "ok";
	}
	if (r->done == 0) {
		snprintf(reply, sizeof(reply), "err %s %d", r->err == -ENOMEM ? "busy" : "failed", r->err);
	} else {
		snprintf(reply, sizeof(reply), "err partial %d/%d failed %d", r->done, r->total, r->err);
	}
	return reply;
}

/* Optional trailing words: a button name, then a count of clicks. */
static bool parse_button(char **p, uint8_t *button)
{
	char word[8];
	int n = 0;

	*button = 1;
	while (**p == ' ') {
		(*p)++;
	}
	while (isalpha((unsigned char)**p) && n < (int)sizeof(word) - 1) {
		word[n++] = *(*p)++;
	}
	if (n == 0) {
		return true;
	}
	word[n] = '\0';
	int found = LOOKUP(button_names, word);

	if (found < 0) {
		return false;
	}
	*button = found;
	return true;
}

static void click(struct run *r, uint8_t button, uint16_t x, uint16_t y, uint32_t hold_ms)
{
	pointer(r, button, x, y);
	wait_ms(r, hold_ms);
	pointer(r, 0, x, y);
}

static const char *cmd_tap(struct run *r, char *args)
{
	uint16_t x, y;
	uint8_t button;
	long count = 1;

	if (!parse_fraction(&args, &x) || !parse_fraction(&args, &y) || !parse_button(&args, &button)) {
		return "err bad args";
	}
	if (!at_end(args) && (!parse_long(&args, &count) || count < 1 || count > MAX_CLICKS)) {
		return "err bad args";
	}
	if (!at_end(args)) {
		return "err bad args";
	}
	r->total = 1 + 2 * count;
	pointer(r, 0, x, y);
	wait_ms(r, SETTLE_MS);
	for (long i = 0; i < count; i++) {
		if (i) {
			wait_ms(r, CLICK_GAP_MS);
		}
		click(r, button, x, y, TAP_HOLD_MS);
	}
	wait_ms(r, SETTLE_MS);
	return outcome(r);
}

static const char *cmd_hold(struct run *r, char *args)
{
	uint16_t x, y;
	long ms;
	uint8_t button;

	if (!parse_fraction(&args, &x) || !parse_fraction(&args, &y) || !parse_long(&args, &ms) || ms < 0 ||
	    ms > MAX_HOLD_MS || !parse_button(&args, &button) || !at_end(args)) {
		return "err bad args";
	}
	r->total = 3;
	pointer(r, 0, x, y);
	wait_ms(r, SETTLE_MS);
	click(r, button, x, y, ms);
	wait_ms(r, SETTLE_MS);
	return outcome(r);
}

static const char *cmd_move(struct run *r, char *args)
{
	uint16_t x, y;

	if (!parse_fraction(&args, &x) || !parse_fraction(&args, &y) || !at_end(args)) {
		return "err bad args";
	}
	r->total = 1;
	pointer(r, 0, x, y);
	return outcome(r);
}

/* x1 y1 x2 y2 ms [hold_ms]: hold_ms keeps the button down before the move starts, as a drag that begins with a long press. */
static const char *cmd_swipe(struct run *r, char *args)
{
	uint16_t x1, y1, x2, y2;
	long ms, hold = 0;

	if (!parse_fraction(&args, &x1) || !parse_fraction(&args, &y1) || !parse_fraction(&args, &x2) ||
	    !parse_fraction(&args, &y2) || !parse_long(&args, &ms) || ms < 0) {
		return "err bad args";
	}
	if (!at_end(args) && (!parse_long(&args, &hold) || hold < 0 || hold > MAX_HOLD_MS)) {
		return "err bad args";
	}
	if (!at_end(args)) {
		return "err bad args";
	}
	long steps = ms / SWIPE_STEP_MS;

	steps = steps < 1 ? 1 : steps > SWIPE_MAX_STEPS ? SWIPE_MAX_STEPS : steps;
	r->total = 3 + steps;
	pointer(r, 0, x1, y1);
	wait_ms(r, SETTLE_MS);
	pointer(r, 1, x1, y1);
	wait_ms(r, hold);
	for (long i = 1; i <= steps; i++) {
		wait_ms(r, ms / steps);
		pointer(r, 1, x1 + ((long)x2 - x1) * i / steps, y1 + ((long)y2 - y1) * i / steps);
	}
	pointer(r, 0, x2, y2);
	wait_ms(r, SETTLE_MS);
	return outcome(r);
}

static int8_t clamp8(long v)
{
	return v < -127 ? -127 : v > 127 ? 127 : v;
}

/* lines [pan]: positive lines scroll up, positive pan scrolls right. */
static const char *cmd_wheel(struct run *r, char *args)
{
	long lines, pan = 0;

	if (!parse_long(&args, &lines) || (!at_end(args) && !parse_long(&args, &pan)) || !at_end(args)) {
		return "err bad args";
	}
	lines = clamp8(lines);
	pan = clamp8(pan);
	long steps = labs(lines) > labs(pan) ? labs(lines) : labs(pan);

	/* iOS ignores the size of a wheel report and a lone report, so a turn goes out as single detents. */
	r->total = steps;
	for (long i = 0; i < steps; i++) {
		int8_t v = i < labs(lines) ? (lines > 0 ? 1 : -1) : 0;
		int8_t h = i < labs(pan) ? (pan > 0 ? 1 : -1) : 0;
		uint8_t rep[MOUSE_LEN] = {0, 0, 0, (uint8_t)v, (uint8_t)h};

		if (i) {
			wait_ms(r, WHEEL_STEP_MS);
		}
		put(r, REP_MOUSE, rep, sizeof(rep));
	}
	return outcome(r);
}

static const char *cmd_text(struct run *r, char *args)
{
	char text[PROTO_LINE_MAX];
	bool unsupported;
	struct key k;

	if (!decode_json(args, text, sizeof(text), &unsupported)) {
		return "err bad json";
	}
	for (const char *c = text; *c; c++) {
		if (unsupported || !char_key(*c, &k)) {
			return "err unsupported char";
		}
	}
	r->total = 2 * strlen(text);
	for (const char *c = text; *c && !r->err; c++) {
		char_key(*c, &k);
		press_key(r, k);
	}
	return outcome(r);
}

static const char *cmd_key(struct run *r, char *args)
{
	struct key k;

	if (!parse_combo(args, &k)) {
		return "err unsupported key";
	}
	r->total = 2;
	press_key(r, k);
	return outcome(r);
}

static const char *cmd_media(struct run *r, char *args)
{
	for (size_t i = 0; i < sizeof(media_names) / sizeof(media_names[0]); i++) {
		if (strcmp(media_names[i].name, args) == 0) {
			r->total = 2;
			consumer(r, media_names[i].usage);
			consumer(r, 0);
			return outcome(r);
		}
	}
	return "err unsupported media";
}

static const char *cmd_pointer(struct run *r, char *args)
{
	long b, x, y;

	if (!parse_long(&args, &b) || !parse_long(&args, &x) || !parse_long(&args, &y) || !at_end(args)) {
		return "err bad args";
	}
	r->total = 1;
	pointer(r, b, x < 0 ? 0 : x > POINTER_MAX ? POINTER_MAX : x, y < 0 ? 0 : y > POINTER_MAX ? POINTER_MAX : y);
	return outcome(r);
}

/* A v1 line of numbers: 1 is a consumer usage, 3 a relative mouse report, 8 a keyboard report. */
static const char *cmd_numbers(struct run *r, char *args)
{
	long v[8];
	int n = 0;

	while (n < 8 && parse_long(&args, &v[n])) {
		n++;
	}
	if (!at_end(args)) {
		return "err bad args";
	}
	r->total = 1;
	if (n == 1) {
		consumer(r, v[0] < 0 ? 0 : v[0] > 0x3FF ? 0x3FF : v[0]);
	} else if (n == 3) {
		uint8_t rep[MOUSE_LEN] = {v[0], (uint8_t)clamp8(v[1]), (uint8_t)clamp8(v[2])};

		put(r, REP_MOUSE, rep, sizeof(rep));
	} else if (n == 8) {
		uint8_t rep[KEYS_LEN];

		for (int i = 0; i < KEYS_LEN; i++) {
			rep[i] = v[i];
		}
		put(r, REP_KEYS, rep, sizeof(rep));
	} else {
		return "err bad args";
	}
	return outcome(r);
}

static const char *cmd_reset(const struct proto_ops *ops)
{
	if (ops->hid_init_err || ops->ready()) {
		return "ok";
	}
	struct run r = {.ops = ops, .total = 4};
	uint8_t keys[KEYS_LEN] = {0};
	uint8_t mouse[MOUSE_LEN] = {0};

	put(&r, REP_KEYS, keys, sizeof(keys));
	put(&r, REP_MOUSE, mouse, sizeof(mouse));
	consumer(&r, 0);
	pointer(&r, 0, pointer_x, pointer_y);
	return outcome(&r);
}

static const char *cmd_result(char *args)
{
	for (int i = 0; i < RESULT_SLOTS; i++) {
		if (results[i].id[0] && strcmp(results[i].id, args) == 0) {
			return results[i].reply;
		}
	}
	return "err unknown id";
}

static const struct {
	const char *verb;
	const char *(*run)(struct run *r, char *args);
} report_verbs[] = {
	{"tap", cmd_tap}, {"hold", cmd_hold}, {"move", cmd_move}, {"swipe", cmd_swipe}, {"wheel", cmd_wheel},
	{"text", cmd_text}, {"key", cmd_key}, {"media", cmd_media}, {"pointer", cmd_pointer}, {"a", cmd_pointer},
	{"", cmd_numbers},
};

static const char *dispatch(const struct proto_ops *ops, const char *verb, char *args)
{
	static char data[PROTO_REPLY_MAX];

	if (strcmp(verb, "version") == 0) {
		snprintf(data, sizeof(data), "ok airhand %s id=%s", PROTO_VERSION, ops->chip_id);
		return data;
	}
	if (strcmp(verb, "status") == 0) {
		char status[PROTO_REPLY_MAX - 4];

		ops->status(status, sizeof(status));
		snprintf(data, sizeof(data), "ok %s", status);
		return data;
	}
	if (strcmp(verb, "unpair") == 0) {
		int err = ops->unpair();

		if (!err) {
			return "ok";
		}
		snprintf(data, sizeof(data), "err failed %d", err);
		return data;
	}
	if (strcmp(verb, "reset") == 0) {
		return cmd_reset(ops);
	}
	if (strcmp(verb, "result") == 0) {
		return cmd_result(args);
	}
	for (size_t i = 0; i < sizeof(report_verbs) / sizeof(report_verbs[0]); i++) {
		if (strcmp(report_verbs[i].verb, verb) == 0) {
			const char *refused = refusal(ops);
			struct run r = {.ops = ops};

			return refused ? refused : report_verbs[i].run(&r, args);
		}
	}
	return "err unknown verb";
}

static bool is_id(const char *word, size_t len)
{
	if (len == 0 || len > ID_MAX) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		if (!isdigit((unsigned char)word[i])) {
			return false;
		}
	}
	return true;
}

static void remember(const char *id, const char *status)
{
	strncpy(results[next_slot].id, id, ID_MAX);
	results[next_slot].id[ID_MAX] = '\0';
	strncpy(results[next_slot].reply, status, PROTO_REPLY_MAX - 1);
	results[next_slot].reply[PROTO_REPLY_MAX - 1] = '\0';
	next_slot = (next_slot + 1) % RESULT_SLOTS;
}

const char *proto_handle(const struct proto_ops *ops, const char *line)
{
	static char out[PROTO_REPLY_MAX + ID_MAX + 2];
	char buf[PROTO_LINE_MAX];
	char id[ID_MAX + 1] = "0";
	char *verb = buf;
	char *args;
	size_t first;

	if (strlen(line) >= sizeof(buf)) {
		const char *space = strchr(line, ' ');

		if (space && is_id(line, space - line)) {
			snprintf(out, sizeof(out), "%.*s err line too long", (int)(space - line), line);
		} else {
			snprintf(out, sizeof(out), "0 err line too long");
		}
		return out;
	}
	strcpy(buf, line);
	while (*verb == ' ') {
		verb++;
	}
	if (!*verb) {
		return "";
	}
	first = strcspn(verb, " ");
	if (verb[first] == ' ' && is_id(verb, first) && isalpha((unsigned char)verb[first + 1])) {
		memcpy(id, verb, first);
		id[first] = '\0';
		verb += first + 1;
	}
	if (isalpha((unsigned char)*verb)) {
		size_t len = strcspn(verb, " ");

		args = verb + len;
		if (*args) {
			*args++ = '\0';
		}
	} else {
		args = verb;
		verb = "";
	}

	const char *status = dispatch(ops, verb, args);

	if (strcmp(id, "0") != 0 && strcmp(verb, "result") != 0) {
		remember(id, status);
	}
	snprintf(out, sizeof(out), "%s %s", id, status);
	return out;
}
