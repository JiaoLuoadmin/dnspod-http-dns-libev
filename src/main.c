// Simple UDP-to-HTTPS DNS Proxy
//
// (C) 2016 Aaron Drew
//
// Intended for use with Google's Public-DNS over HTTPS service
// (https://developers.google.com/speed/public-dns/docs/dns-over-https)
#include <sys/socket.h>
#include <sys/types.h>

#include <ares.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <ev.h>
#include <grp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dns_poller.h"
#include "dns_server.h"
#include "https_client.h"
#include "json_to_dns.h"
#include "text_to_dns.h"
#include "logging.h"
#include "options.h"

// Holds app state required for dns_server_cb.
typedef struct {
  https_client_t *https_client;
  struct curl_slist *resolv;
  // currently only used for edns_client_subnet, if specified.
  const char *extra_request_args;
} app_state_t;

typedef struct {
  uint16_t tx_id;
  struct sockaddr_in raddr;
  dns_server_t *dns_server;
  char name[254]; // The full domain name may not exceed the length of 253 characters
} request_t;

static void sigint_cb(struct ev_loop *loop, ev_signal *w, int revents) {
  ev_break(loop, EVBREAK_ALL);
}

static void sigpipe_cb(struct ev_loop *loop, ev_signal *w, int revents) {
  ELOG("Received SIGPIPE. Ignoring.");
}

static void https_resp_cb(void *data, unsigned char *buf, unsigned int buflen) {
  DLOG("buflen %u", buflen);
  if (buf == NULL) { // Timeout, DNS failure, or something similar.
    return;
  }
  request_t *req = (request_t *)data;
  if (req == NULL) {
    FLOG("data NULL");
  }
  unsigned int namelen = strlen(req->name);
  unsigned int datalen = 0;
  for (; ';' != buf[datalen] && datalen < buflen; ++datalen);
  char *bufcpy = (char *)calloc(1, namelen + datalen + 2);
  if (bufcpy == NULL) {
    FLOG("Out of mem");
  }
  memcpy(bufcpy, req->name, namelen);
  memset(bufcpy + namelen, ':', 1);
  memcpy(bufcpy + namelen + 1, buf, datalen);

  DLOG("Received response for id %04x: %.*s", req->tx_id, namelen + 1 + datalen, bufcpy);

  const int obuf_size = 1500;
  char obuf[obuf_size];
  int r;
  if ((r = text_to_dns(req->tx_id, bufcpy,
                       (unsigned char *)obuf, obuf_size)) <= 0) {
    ELOG("Failed to decode JSON.");
  } else {
    dns_server_respond(req->dns_server, req->raddr, obuf, r);
  }
  free(bufcpy);
  free(req);
}

static void dns_server_cb(dns_server_t *dns_server, void *data,
                          struct sockaddr_in addr, uint16_t tx_id,
                          uint16_t flags, const char *name, int type) {
  app_state_t *app = (app_state_t *)data;

  DLOG("Received request for '%s' id: %04x, type %d, flags %04x", name, tx_id,
       type, flags);

  if (type != 1 || strlen(name) > 253) {
    DLOG("Drop Received request for '%s' id: %04x, type %d", name, tx_id, type);
    return;
  }

  // Build URL
  int cd_bit = flags & (1 << 4);
  char *escaped_name = curl_escape(name, strlen(name));
  char url[1500] = "";
  snprintf(url, sizeof(url) - 1,
           "http://119.29.29.29/d?dn=%s%s",
           escaped_name, app->extra_request_args);

  request_t *req = (request_t *)calloc(1, sizeof(request_t));
  if (!req) {
    FLOG("Out of mem");
  }
  req->tx_id = tx_id;
  req->raddr = addr;
  req->dns_server = dns_server;
  memcpy(req->name, escaped_name, strlen(escaped_name));
  curl_free(escaped_name);

  https_client_fetch(app->https_client, url, app->resolv, https_resp_cb, req);
}

int main(int argc, char *argv[]) {
  struct Options opt;
  options_init(&opt);
  if (options_parse_args(&opt, argc, argv)) {
    options_show_usage(argc, argv);
    exit(1);
  }

  logging_init(opt.logfd, opt.loglevel);

  ILOG("Built "__DATE__" "__TIME__".");
  ILOG("System c-ares: %s", ares_version(NULL));
  ILOG("System libcurl: %s", curl_version());

  // Note: curl intentionally uses uninitialized stack variables and similar
  // tricks to increase it's entropy pool. This confuses valgrind and leaks
  // through to errors about use of uninitialized values in our code. :(
  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Note: This calls ev_default_loop(0) which never cleans up.
  //       valgrind will report a leak. :(
  struct ev_loop *loop = EV_DEFAULT;

  https_client_t https_client;
  https_client_init(&https_client, &opt, loop);

  app_state_t app;
  app.https_client = &https_client;
  app.resolv = NULL;
  if (opt.edns_client_subnet[0]) {
    static char buf[200];
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf)-1, "&ip=%s",
             opt.edns_client_subnet);
    app.extra_request_args = buf;
  } else {
    app.extra_request_args = "";
  }

  dns_server_t dns_server;
  dns_server_init(&dns_server, loop, opt.listen_addr, opt.listen_port,
                  dns_server_cb, &app);

  if (opt.daemonize) {
    if (setgid(opt.gid)) {
      FLOG("Failed to set gid.");
    }
    if (setuid(opt.uid)) {
      FLOG("Failed to set uid.");
    }
    // daemon() is non-standard. If needed, see OpenSSH openbsd-compat/daemon.c
    daemon(0, 0);
  }

  ev_signal sigpipe;
  ev_signal_init(&sigpipe, sigpipe_cb, SIGPIPE);
  ev_signal_start(loop, &sigpipe);

  ev_signal sigint;
  ev_signal_init(&sigint, sigint_cb, SIGINT);
  ev_signal_start(loop, &sigint);

  logging_flush_init(loop);

  dns_poller_t dns_poller;

  ev_run(loop, 0);

  curl_slist_free_all(app.resolv);

  ev_signal_stop(loop, &sigint);
  dns_server_cleanup(&dns_server);
  https_client_cleanup(&https_client);

  ev_loop_destroy(loop);

  curl_global_cleanup();
  logging_cleanup();
  options_cleanup(&opt);

  return EXIT_SUCCESS;
}
