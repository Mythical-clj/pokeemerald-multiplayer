// Emulator side of the native Emerald multiplayer link.
//
// The ROM (src/remote_sio.c) exposes a mailbox in EWRAM when it sees mGBA.
// This file finds it and moves events between it and the relay. Networking
// runs on its own thread so a slow connection never stalls emulation; the
// per-frame hook only copies between the mailbox and two small rings.
//
// Transports (net/protocol.py of gen3-poke-multiplayer-overworld-sync):
//   relay=wss://host/path   WebSocket over TLS (port 443; works everywhere)
//   relay=ws://host:port/p  WebSocket in the clear
//   host=H and port=P       raw UDP datagrams
// Either way each message is one OWL1 datagram.
//
// Status goes to owl_status.txt next to owl.txt.
//
// NOT RUN ON A DEVICE YET. It compiles in CI; expect first-run fixes.

#include "owl_bridge.h"

#include <mgba/core/core.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef OWL_USE_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif

#define EWRAM_BASE   0x02000000u
#define EWRAM_SIZE   0x00040000u
#define MBOX_MAGIC   0x324D424Fu
#define MBOX_SLOTS   16
#define EVENT_SIZE   12

#define MB_MAGIC     0x00
#define MB_TXHEAD    0x04
#define MB_TXTAIL    0x08
#define MB_SLOTS     0x0C
#define MB_RXHEAD    0x10
#define MB_RXTAIL    0x14
#define MB_FRAMES    0x1C
#define MB_TX        0x20
#define MB_RX        (MB_TX + MBOX_SLOTS * EVENT_SIZE)

#define OWL_HEADER   11
#define T_HELLO      0
#define T_EVENT      1
#define T_PING       2
#define T_BYE        4

#define MAX_FRIENDS  3
#define RING_SLOTS   64

// ---- a single-producer single-consumer ring of events --------------------

struct Ring {
	uint8_t slot[RING_SLOTS][EVENT_SIZE];
	unsigned head; // written by the producer
	unsigned tail; // written by the consumer
};

static int ring_push(struct Ring* r, const uint8_t* ev) {
	unsigned head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
	unsigned next = (head + 1) % RING_SLOTS;
	if (next == __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE)) {
		return 0;
	}
	memcpy(r->slot[head], ev, EVENT_SIZE);
	__atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
	return 1;
}

static int ring_pop(struct Ring* r, uint8_t* ev) {
	unsigned tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
	if (tail == __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)) {
		return 0;
	}
	memcpy(ev, r->slot[tail], EVENT_SIZE);
	__atomic_store_n(&r->tail, (tail + 1) % RING_SLOTS, __ATOMIC_RELEASE);
	return 1;
}

// ---- state ---------------------------------------------------------------

static struct Ring sToNet, sFromNet;

static char sDir[400] = ".";
static char sRelay[300] = "";
static char sHost[128] = "";
static char sPort[16] = "9000";
static uint16_t sPeer = 1, sRoom = 1;

static pthread_t sThread;
static volatile int sRunning = 0;
static volatile int sConnected = 0;
static volatile int sMailboxFound = 0;
static volatile unsigned sGameFrames = 0;
static volatile unsigned sSent = 0, sGot = 0;
static char sNote[160] = "starting";

static uint32_t sBox = 0;
static uint32_t sLastFrames = 0;
static uint32_t sStale = 0;

// network-thread only
static uint32_t sCode;
static uint8_t sSeq = 0;
static uint16_t sFriends[MAX_FRIENDS];
static int sFriendCount = 0;

static void write_status(void) {
	char path[460];
	FILE* f;
	snprintf(path, sizeof(path), "%s/owl_status.txt", sDir);
	f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "relay: %s\nroom %u, player %u\nnetwork: %s (%s)\nmailbox in game: %s\ngame frames: %u\nevents sent: %u, received: %u\n",
	        sRelay[0] ? sRelay : sHost, sRoom, sPeer, sConnected ? "connected" : "NOT connected", sNote,
	        sMailboxFound ? "found" : "NOT found (is the multiplayer ROM running?)", sGameFrames, sSent, sGot);
	fclose(f);
}

static void note(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(sNote, sizeof(sNote), fmt, ap);
	va_end(ap);
}

static void putle16(uint8_t* p, uint16_t v) {
	p[0] = v;
	p[1] = v >> 8;
}
static uint16_t getle16(const uint8_t* p) {
	return p[0] | (p[1] << 8);
}

// ---- transport -----------------------------------------------------------

enum Mode { MODE_UDP, MODE_WS, MODE_WSS };

static enum Mode sMode = MODE_UDP;
static char sWsHost[128], sWsPort[8], sWsPath[200];
static int sFd = -1;
static uint8_t sWsBuf[2048];
static size_t sWsLen = 0;

#ifdef OWL_USE_TLS
static mbedtls_net_context sNet;
static mbedtls_ssl_context sSsl;
static mbedtls_ssl_config sConf;
static mbedtls_entropy_context sEntropy;
static mbedtls_ctr_drbg_context sDrbg;
static mbedtls_x509_crt sCa;
static int sTlsUp = 0;
#endif

static int raw_write(const uint8_t* p, size_t n) {
	while (n) {
		int w;
#ifdef OWL_USE_TLS
		if (sMode == MODE_WSS) {
			w = mbedtls_ssl_write(&sSsl, p, n);
			if (w == MBEDTLS_ERR_SSL_WANT_WRITE || w == MBEDTLS_ERR_SSL_WANT_READ) {
				continue;
			}
		} else
#endif
		{
			w = (int) send(sFd, p, n, MSG_NOSIGNAL);
		}
		if (w <= 0) {
			return -1;
		}
		p += w;
		n -= w;
	}
	return 0;
}

// Reads up to n bytes, waiting at most ~20 ms. Returns bytes, 0 for none, -1 for closed/error.
static int raw_read(uint8_t* p, size_t n) {
#ifdef OWL_USE_TLS
	if (sMode == MODE_WSS) {
		int r = mbedtls_ssl_read(&sSsl, p, n);
		if (r == MBEDTLS_ERR_SSL_TIMEOUT || r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
			return 0;
		}
		return r <= 0 ? -1 : r;
	}
#endif
	{
		struct pollfd pf = {sFd, POLLIN, 0};
		int r = poll(&pf, 1, 20);
		if (r <= 0) {
			return 0;
		}
		r = (int) recv(sFd, p, n, 0);
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
			return 0;
		}
		return r <= 0 ? -1 : r;
	}
}

static void transport_close(void) {
#ifdef OWL_USE_TLS
	if (sTlsUp) {
		mbedtls_ssl_close_notify(&sSsl);
		mbedtls_ssl_free(&sSsl);
		mbedtls_ssl_config_free(&sConf);
		mbedtls_ctr_drbg_free(&sDrbg);
		mbedtls_entropy_free(&sEntropy);
		mbedtls_x509_crt_free(&sCa);
		mbedtls_net_free(&sNet);
		sTlsUp = 0;
		sFd = -1;
	}
#endif
	if (sFd >= 0) {
		close(sFd);
	}
	sFd = -1;
	sWsLen = 0;
	sConnected = 0;
}

static void b64(const uint8_t* in, int n, char* out) {
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int i, o = 0;
	for (i = 0; i < n; i += 3) {
		uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
		out[o++] = t[v >> 18 & 63];
		out[o++] = t[v >> 12 & 63];
		out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
		out[o++] = i + 2 < n ? t[v & 63] : '=';
	}
	out[o] = 0;
}

static int tcp_connect(const char* host, const char* port) {
	struct addrinfo hints, *res, *a;
	int fd = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		note("cannot resolve %s", host);
		return -1;
	}
	for (a = res; a; a = a->ai_next) {
		struct timeval tv = {8, 0};
		fd = socket(a->ai_family, a->ai_socktype, 0);
		if (fd < 0) {
			continue;
		}
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
			break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		note("cannot reach %s:%s", host, port);
	}
	return fd;
}

static int ws_handshake(void) {
	uint8_t key[16];
	char keyb[32], req[600];
	uint8_t head[2048];
	size_t got = 0;
	int i, n;

	for (i = 0; i < 16; i++) {
		key[i] = rand();
	}
	b64(key, 16, keyb);
	n = snprintf(req, sizeof(req),
	             "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
	             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
	             sWsPath, sWsHost, keyb);
	if (raw_write((uint8_t*) req, n) < 0) {
		note("send failed during WebSocket upgrade");
		return -1;
	}
	for (i = 0; i < 200 && got < sizeof(head) - 1; i++) { // ~4 s
		int r = raw_read(head + got, sizeof(head) - 1 - got);
		if (r < 0) {
			note("connection closed during WebSocket upgrade");
			return -1;
		}
		got += r;
		head[got] = 0;
		if (strstr((char*) head, "\r\n\r\n")) {
			break;
		}
	}
	if (!strstr((char*) head, "\r\n\r\n")) {
		note("no answer to the WebSocket upgrade");
		return -1;
	}
	if (!strstr((char*) head, " 101")) {
		note("server refused WebSocket upgrade: check the relay address");
		return -1;
	}
	// Anything after the headers is already frame data.
	{
		char* end = strstr((char*) head, "\r\n\r\n") + 4;
		size_t extra = got - (size_t) ((uint8_t*) end - head);
		if (extra && extra <= sizeof(sWsBuf)) {
			memcpy(sWsBuf, end, extra);
			sWsLen = extra;
		}
	}
	return 0;
}

static void parse_relay(void) {
	const char* p = sRelay;
	const char* slash;
	const char* colon;
	size_t hl;
	sMode = MODE_UDP;
	if (!strncmp(p, "wss://", 6)) {
		sMode = MODE_WSS;
		p += 6;
		snprintf(sWsPort, sizeof(sWsPort), "443");
	} else if (!strncmp(p, "ws://", 5)) {
		sMode = MODE_WS;
		p += 5;
		snprintf(sWsPort, sizeof(sWsPort), "80");
	} else {
		return;
	}
	slash = strchr(p, '/');
	snprintf(sWsPath, sizeof(sWsPath), "%s", slash ? slash : "/");
	hl = slash ? (size_t) (slash - p) : strlen(p);
	if (hl >= sizeof(sWsHost)) {
		hl = sizeof(sWsHost) - 1;
	}
	memcpy(sWsHost, p, hl);
	sWsHost[hl] = 0;
	colon = strchr(sWsHost, ':');
	if (colon) {
		snprintf(sWsPort, sizeof(sWsPort), "%s", colon + 1);
		sWsHost[colon - sWsHost] = 0;
	}
}

static int transport_open(void) {
	transport_close();
	if (sMode == MODE_UDP) {
		struct addrinfo hints, *res;
		memset(&hints, 0, sizeof(hints));
		hints.ai_socktype = SOCK_DGRAM;
		if (getaddrinfo(sHost, sPort, &hints, &res) != 0) {
			note("cannot resolve %s", sHost);
			return -1;
		}
		sFd = socket(res->ai_family, res->ai_socktype, 0);
		if (sFd >= 0 && connect(sFd, res->ai_addr, res->ai_addrlen) != 0) {
			close(sFd);
			sFd = -1;
		}
		freeaddrinfo(res);
		if (sFd < 0) {
			note("cannot open UDP to %s", sHost);
			return -1;
		}
		return 0;
	}
	if (sMode == MODE_WS) {
		sFd = tcp_connect(sWsHost, sWsPort);
		if (sFd < 0) {
			return -1;
		}
		return ws_handshake();
	}
#ifdef OWL_USE_TLS
	{
		const char* caDirs[] = {"/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts", NULL};
		int i, r;

		mbedtls_net_init(&sNet);
		mbedtls_ssl_init(&sSsl);
		mbedtls_ssl_config_init(&sConf);
		mbedtls_entropy_init(&sEntropy);
		mbedtls_ctr_drbg_init(&sDrbg);
		mbedtls_x509_crt_init(&sCa);
		sTlsUp = 1;
		psa_crypto_init();
		if (mbedtls_ctr_drbg_seed(&sDrbg, mbedtls_entropy_func, &sEntropy, (const unsigned char*) "owl", 3) != 0) {
			note("TLS random generator failed");
			return -1;
		}
		for (i = 0; caDirs[i]; i++) {
			mbedtls_x509_crt_parse_path(&sCa, caDirs[i]);
		}
		sFd = tcp_connect(sWsHost, sWsPort);
		if (sFd < 0) {
			return -1;
		}
		sNet.fd = sFd;
		mbedtls_ssl_config_defaults(&sConf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
		                            MBEDTLS_SSL_PRESET_DEFAULT);
		// Game events only: a certificate problem is noted, not fatal.
		mbedtls_ssl_conf_authmode(&sConf, MBEDTLS_SSL_VERIFY_OPTIONAL);
		mbedtls_ssl_conf_ca_chain(&sConf, &sCa, NULL);
		mbedtls_ssl_conf_rng(&sConf, mbedtls_ctr_drbg_random, &sDrbg);
		mbedtls_ssl_conf_read_timeout(&sConf, 20);
		if (mbedtls_ssl_setup(&sSsl, &sConf) != 0) {
			note("TLS setup failed");
			return -1;
		}
		mbedtls_ssl_set_hostname(&sSsl, sWsHost);
		mbedtls_ssl_set_bio(&sSsl, &sNet, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);
		while ((r = mbedtls_ssl_handshake(&sSsl)) != 0) {
			if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE && r != MBEDTLS_ERR_SSL_TIMEOUT) {
				note("TLS handshake failed (%d)", r);
				return -1;
			}
		}
		return ws_handshake();
	}
#else
	note("this build has no TLS: use a ws:// or UDP relay");
	return -1;
#endif
}

// One OWL1 datagram out.
static int send_dgram(uint8_t kind, uint8_t seq, const uint8_t* body, size_t n) {
	uint8_t dg[OWL_HEADER + 64];
	if (n > 64) {
		return -1;
	}
	memcpy(dg, "OWL1", 4);
	dg[4] = 1;
	dg[5] = kind;
	putle16(dg + 6, sPeer);
	putle16(dg + 8, sRoom);
	dg[10] = seq;
	if (n) {
		memcpy(dg + OWL_HEADER, body, n);
	}
	n += OWL_HEADER;
	if (sMode == MODE_UDP) {
		return send(sFd, dg, n, MSG_NOSIGNAL) == (ssize_t) n ? 0 : -1;
	} else {
		uint8_t fr[2 + 4 + OWL_HEADER + 64];
		uint8_t mask[4];
		size_t i;
		for (i = 0; i < 4; i++) {
			mask[i] = rand();
		}
		fr[0] = 0x82; // FIN + binary
		fr[1] = 0x80 | (uint8_t) n;
		memcpy(fr + 2, mask, 4);
		for (i = 0; i < n; i++) {
			fr[6 + i] = dg[i] ^ mask[i & 3];
		}
		return raw_write(fr, 6 + n);
	}
}

static int slot_for(uint16_t peer) {
	int i;
	for (i = 0; i < sFriendCount; i++) {
		if (sFriends[i] == peer) {
			return i;
		}
	}
	if (sFriendCount >= MAX_FRIENDS) {
		return -1;
	}
	sFriends[sFriendCount] = peer;
	return sFriendCount++;
}

static void deliver(const uint8_t* dg, size_t n) {
	uint8_t ev[EVENT_SIZE];
	int slot;
	if (n < OWL_HEADER + EVENT_SIZE || memcmp(dg, "OWL1", 4) != 0 || dg[5] != T_EVENT) {
		return;
	}
	slot = slot_for(getle16(dg + 6));
	if (slot < 0) {
		return;
	}
	memcpy(ev, dg + OWL_HEADER, EVENT_SIZE);
	ev[0] = (ev[0] & 0x0F) | (slot << 4); // the receiver stamps the sender's slot
	if (ring_push(&sFromNet, ev)) {
		sGot++;
	}
}

// Returns -1 if the connection is gone.
static int pump_receive(void) {
	uint8_t tmp[1024];
	int r;
	if (sMode == MODE_UDP) {
		uint8_t dg[OWL_HEADER + 64];
		struct pollfd pf = {sFd, POLLIN, 0};
		if (poll(&pf, 1, 20) <= 0) {
			return 0;
		}
		while ((r = (int) recv(sFd, dg, sizeof(dg), MSG_DONTWAIT)) > 0) {
			deliver(dg, r);
		}
		return 0;
	}
	r = raw_read(tmp, sizeof(tmp));
	if (r < 0) {
		return -1;
	}
	if (r > 0 && sWsLen + r <= sizeof(sWsBuf)) {
		memcpy(sWsBuf + sWsLen, tmp, r);
		sWsLen += r;
	}
	for (;;) {
		uint8_t op;
		size_t len, hdr = 2;
		if (sWsLen < 2) {
			break;
		}
		op = sWsBuf[0] & 0x0F;
		len = sWsBuf[1] & 0x7F;
		if (len == 126) {
			if (sWsLen < 4) {
				break;
			}
			len = sWsBuf[2] << 8 | sWsBuf[3];
			hdr = 4;
		} else if (len == 127) {
			sWsLen = 0; // far larger than any game frame: resync
			return -1;
		}
		if (len > sizeof(sWsBuf) - hdr) {
			sWsLen = 0;
			return -1;
		}
		if (sWsLen < hdr + len) {
			break;
		}
		if (op == 2) {
			deliver(sWsBuf + hdr, len);
		} else if (op == 8) {
			return -1;
		} else if (op == 9) { // ping -> pong with the same payload
			uint8_t pong[2 + 4 + 125];
			size_t i;
			if (len <= 125) {
				pong[0] = 0x8A;
				pong[1] = 0x80 | (uint8_t) len;
				memset(pong + 2, 0, 4); // zero mask is a valid mask
				for (i = 0; i < len; i++) {
					pong[6 + i] = sWsBuf[hdr + i];
				}
				raw_write(pong, 6 + len);
			}
		}
		memmove(sWsBuf, sWsBuf + hdr + len, sWsLen - hdr - len);
		sWsLen -= hdr + len;
	}
	return 0;
}

static void* net_main(void* unused) {
	uint8_t ev[EVENT_SIZE];
	time_t lastPing = 0, lastStatus = 0;
	(void) unused;

	while (sRunning) {
		time_t now;
		if (!sConnected) {
			uint8_t code[4] = {sCode, sCode >> 8, sCode >> 16, sCode >> 24};
			note("connecting");
			write_status();
			if (transport_open() == 0 && send_dgram(T_HELLO, 0, code, 4) == 0) {
				sConnected = 1;
				note(sMode == MODE_UDP ? "UDP" : sMode == MODE_WS ? "WebSocket" : "WebSocket over TLS");
				lastPing = time(NULL);
				write_status();
			} else {
				transport_close();
				write_status();
				sleep(3);
				continue;
			}
		}
		now = time(NULL);
		if (now - lastPing >= 1) {
			uint8_t code[4] = {sCode, sCode >> 8, sCode >> 16, sCode >> 24};
			if (send_dgram(T_PING, sSeq++, code, 4) != 0) {
				transport_close();
				note("send failed, reconnecting");
				continue;
			}
			lastPing = now;
		}
		while (ring_pop(&sToNet, ev)) {
			if (send_dgram(T_EVENT, sSeq++, ev, EVENT_SIZE) != 0) {
				transport_close();
				note("send failed, reconnecting");
				break;
			}
			sSent++;
		}
		if (!sConnected) {
			continue;
		}
		if (pump_receive() < 0) {
			transport_close();
			note("connection lost, reconnecting");
			continue;
		}
		if (now != lastStatus) {
			lastStatus = now;
			write_status();
		}
	}
	return NULL;
}

// ---- config and per-frame hook ----------------------------------------------

static void load_config(void) {
	char path[460], line[300], v[300];
	FILE* f;
	snprintf(path, sizeof(path), "%s/owl.txt", sDir);
	f = fopen(path, "r");
	if (!f) {
		note("no owl.txt in %s", sDir);
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "relay=%299s", v) == 1) snprintf(sRelay, sizeof(sRelay), "%s", v);
		else if (sscanf(line, "host=%127s", v) == 1) snprintf(sHost, sizeof(sHost), "%s", v);
		else if (sscanf(line, "port=%15s", v) == 1) snprintf(sPort, sizeof(sPort), "%s", v);
		else if (sscanf(line, "room=%299s", v) == 1) sRoom = (uint16_t) atoi(v);
		else if (sscanf(line, "peer=%299s", v) == 1) sPeer = (uint16_t) atoi(v);
	}
	fclose(f);
}

void owl_bridge_attach(struct mCore* core, const char* configDir) {
	(void) core;
	if (sRunning) {
		owl_bridge_detach();
	}
	snprintf(sDir, sizeof(sDir), "%s", configDir ? configDir : ".");
	sRelay[0] = sHost[0] = 0;
	load_config();
	if (!sRelay[0] && !sHost[0]) {
		note("no relay set in owl.txt");
		write_status();
		return;
	}
	parse_relay();
	srand((unsigned) time(NULL) ^ (unsigned) getpid());
	sCode = ((uint32_t) rand() << 16) ^ (uint32_t) rand() ^ 1u;
	sFriendCount = 0;
	sBox = 0;
	sMailboxFound = 0;
	sToNet.head = sToNet.tail = sFromNet.head = sFromNet.tail = 0;
	sRunning = 1;
	if (pthread_create(&sThread, NULL, net_main, NULL) != 0) {
		sRunning = 0;
		note("cannot start network thread");
	}
	write_status();
}

void owl_bridge_detach(void) {
	if (!sRunning) {
		return;
	}
	sRunning = 0;
	pthread_join(sThread, NULL);
	if (sConnected) {
		send_dgram(4, sSeq++, NULL, 0); // T_BYE
	}
	transport_close();
	sMailboxFound = 0;
}

static void find_mailbox(struct mCore* core) {
	uint32_t a;
	for (a = EWRAM_BASE; a < EWRAM_BASE + EWRAM_SIZE; a += 4) {
		if (core->busRead32(core, a) == MBOX_MAGIC && core->busRead32(core, a + MB_SLOTS) == MBOX_SLOTS) {
			sBox = a;
			sLastFrames = 0;
			sStale = 0;
			sMailboxFound = 1;
			return;
		}
	}
}

void owl_bridge_frame(struct mCore* core) {
	uint32_t frames, head, tail;
	uint8_t ev[EVENT_SIZE];
	int i;

	if (!sRunning) {
		return;
	}
	if (!sBox || core->busRead32(core, sBox + MB_MAGIC) != MBOX_MAGIC) {
		sBox = 0;
		sMailboxFound = 0;
		find_mailbox(core);
		if (!sBox) {
			return;
		}
	}
	frames = core->busRead32(core, sBox + MB_FRAMES);
	if (frames == sLastFrames) {
		if (++sStale > 600) { // the game stopped counting: look again
			sBox = 0;
			return;
		}
	} else {
		sStale = 0;
		sLastFrames = frames;
	}
	sGameFrames = frames;

	// Game -> network
	head = core->busRead32(core, sBox + MB_TXHEAD);
	tail = core->busRead32(core, sBox + MB_TXTAIL);
	while (tail != head) {
		for (i = 0; i < EVENT_SIZE; i++) {
			ev[i] = core->busRead8(core, sBox + MB_TX + tail * EVENT_SIZE + i);
		}
		ring_push(&sToNet, ev);
		tail = (tail + 1) % MBOX_SLOTS;
	}
	core->busWrite32(core, sBox + MB_TXTAIL, tail);

	// Network -> game
	head = core->busRead32(core, sBox + MB_RXHEAD);
	tail = core->busRead32(core, sBox + MB_RXTAIL);
	while ((head + 1) % MBOX_SLOTS != tail && ring_pop(&sFromNet, ev)) {
		for (i = 0; i < EVENT_SIZE; i++) {
			core->busWrite8(core, sBox + MB_RX + head * EVENT_SIZE + i, ev[i]);
		}
		head = (head + 1) % MBOX_SLOTS;
		core->busWrite32(core, sBox + MB_RXHEAD, head);
		tail = core->busRead32(core, sBox + MB_RXTAIL);
	}
}
