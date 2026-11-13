#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define OP_INIT_KEY 0x800

static int http_post(const char *host, int port, const char *path,
		     const char *json, char *out, size_t outlen)
{
	int fd, n;
	struct sockaddr_in sa;
	char req[1024];
	char buf[2048];
	size_t off = 0;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((unsigned short)port);
	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}

	n = snprintf(req, sizeof(req),
		     "POST %s HTTP/1.1\r\nHost: %s\r\n"
		     "Content-Type: application/json\r\n"
		     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
		     path, host, strlen(json), json);
	if (write(fd, req, (size_t)n) != n) {
		close(fd);
		return -1;
	}

	while ((n = (int)read(fd, buf + off, sizeof(buf) - 1 - off)) > 0) {
		off += (size_t)n;
		if (off >= sizeof(buf) - 1)
			break;
	}
	close(fd);
	buf[off] = '\0';

	{
		char *p = strstr(buf, "\r\n\r\n");
		if (!p)
			return -1;
		p += 4;
		if (strlen(p) >= outlen)
			return -1;
		strcpy(out, p);
	}
	return 0;
}

static int get_device_id(char *out, size_t len)
{
	FILE *f = fopen("/sys/class/net/wlan0/address", "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

static int extract_ticket(const char *json, char *out, size_t len)
{
	const char *p = strstr(json, "\"ticket\"");
	const char *q;
	size_t n;

	if (!p)
		return -1;
	p = strchr(p, ':');
	if (!p)
		return -1;
	p = strchr(p, '"');
	if (!p)
		return -1;
	p++;
	q = strchr(p, '"');
	if (!q)
		return -1;
	n = (size_t)(q - p);
	if (n >= len)
		return -1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 0;
}

int main(int argc, char **argv)
{
	char device[128];
	char json[512];
	char resp[2048];
	char ticket[512];
	int fd;

	if (argc < 5) {
		fprintf(stderr, "usage: %s <card> <host> <port> <device>\n", argv[0]);
		return 2;
	}
	if (get_device_id(device, sizeof(device)) != 0)
		snprintf(device, sizeof(device), "unknown");

	snprintf(json, sizeof(json),
		 "{\"card\":\"%s\",\"device\":\"%s\"}", argv[1], device);

	if (http_post(argv[2], atoi(argv[3]), "/v1/ticket", json,
		      resp, sizeof(resp)) != 0) {
		fprintf(stderr, "request failed\n");
		return 1;
	}
	if (extract_ticket(resp, ticket, sizeof(ticket)) != 0) {
		fprintf(stderr, "no ticket: %s\n", resp);
		return 1;
	}
	fprintf(stderr, "ticket: %s\n", ticket);

	fd = open(argv[4], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", argv[4], strerror(errno));
		return 1;
	}
	if (ioctl(fd, OP_INIT_KEY, ticket) != 0) {
		fprintf(stderr, "ioctl: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("authorized\n");
	close(fd);
	return 0;
}
