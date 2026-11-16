/* Serves src/proto.c on a pty and writes every HID report to a log, so a host client can test the protocol. */
#include "../src/proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

static FILE *reports;
static int host_connected;

static int ready(void)
{
	return host_connected ? 0 : -ENOTCONN;
}

static int send_report(enum proto_report report, const uint8_t *data, size_t len)
{
	fprintf(reports, "%d", report);
	for (size_t i = 0; i < len; i++) {
		fprintf(reports, " %u", data[i]);
	}
	fprintf(reports, "\n");
	fflush(reports);
	return 0;
}

static void sleep_ms(uint32_t ms)
{
	(void)ms;
}

static int unpair(void)
{
	return 0;
}

static void bootloader(void)
{
	fprintf(reports, "bootloader\n");
	fflush(reports);
}

static void status(char *out, size_t len)
{
	snprintf(out, len, "conn=%s", host_connected ? "yes" : "no");
}

int main(int argc, char **argv)
{
	struct proto_ops ops = {ready, send_report, sleep_ms, unpair, status, bootloader, "HOST", 0};
	char name[128], line[PROTO_LINE_MAX + 1], chunk[512];
	size_t len = 0;
	int master, slave;

	if (argc != 3) {
		fprintf(stderr, "usage: fake_radio <report log> <host connected 0|1>\n");
		return 2;
	}
	reports = fopen(argv[1], "a");
	host_connected = atoi(argv[2]);
	if (!reports || openpty(&master, &slave, name, NULL, NULL) != 0) {
		perror("fake_radio");
		return 1;
	}
	printf("%s\n", name);
	fflush(stdout);
	for (;;) {
		ssize_t n = read(master, chunk, sizeof(chunk));

		if (n <= 0) {
			return 0;
		}
		for (ssize_t i = 0; i < n; i++) {
			char c = chunk[i];

			if (c == '\r') {
				continue;
			}
			if (c != '\n') {
				if (len < PROTO_LINE_MAX) {
					line[len++] = c;
				}
				continue;
			}
			line[len] = '\0';
			len = 0;
			const char *reply = proto_handle(&ops, line);

			if (*reply) {
				dprintf(master, "%s\n", reply);
			}
		}
	}
}
