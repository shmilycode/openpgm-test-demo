#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <pgm/pgm.h>
#include <fstream>

#define N_ELEMENTS(arr)    (sizeof (arr) / sizeof ((arr)[0]))

int save_to_file(struct pgm_msgv_t*  msgv, size_t len, std::ofstream& file) {
  unsigned int i = 0;
  /* for each apdu */
  char buf[2048], tsi[PGM_TSISTRLEN];
  do {
    size_t apdu_len = 0;
    printf("msgv[i].msgv_len = %d\n", msgv[i].msgv_len);
    for (unsigned j = 0; j < msgv[i].msgv_len; j++) {
      struct pgm_sk_buff_t* pskb = msgv[i].msgv_skb[j];
      apdu_len += pskb->len;
      file.write((const char*)pskb->data, pskb->len);
    }
    i++;
    len -= apdu_len;
  } while (len);

  return 0;
}

int on_msgv (
  struct pgm_msgv_t*  msgv,    /* an array of msgvs */
  size_t      len
  )
{
  printf ("(%u bytes)\n", (unsigned)len);
  unsigned int i = 0;
/* for each apdu */
  do {
    const struct pgm_sk_buff_t* pskb = msgv[i].msgv_skb[0];
    size_t apdu_len = 0;
    for (unsigned j = 0; j < msgv[i].msgv_len; j++)
      apdu_len += msgv[i].msgv_skb[j]->len;
/* truncate to first fragment to make GLib printing happy */
    char buf[2048], tsi[PGM_TSISTRLEN];
    const size_t buflen = MIN(sizeof(buf) - 1, pskb->len);
    strncpy (buf, (const char*)pskb->data, buflen);
    buf[buflen] = '\0';
    pgm_tsi_print_r (&pskb->tsi, tsi, sizeof(tsi));
    if (msgv[i].msgv_len > 1)
      printf ("\t%u: %s ... (%lu bytes from %s)\n",
           i, buf, apdu_len, tsi);
    else
      printf ("\t%u: %s (%lu bytes from %s)\n",
           i, buf, apdu_len, tsi);
    i++;
    len -= apdu_len;
  } while (len);

  return 0;
}

int main(int argc, char** argv) {
  pgm_sock_t*  g_sock = NULL;
  int g_multicast_loop = true;

  struct pgm_addrinfo_t* res = NULL;
  pgm_error_t* pgm_err = NULL;
  sa_family_t sa_family = AF_UNSPEC;

  if (!pgm_init (&pgm_err)) {
    printf ("Unable to start PGM engine: %s", (pgm_err && pgm_err->message) ? pgm_err->message : "(null)");
    pgm_error_free (pgm_err);
    return 0;
  }
  /* parse network parameter into transport address structure */
//  const char*  g_network = "ens33;239.0.0.100";
  const char*  g_network = argv[1];
  int g_udp_encap_port = std::stoi(argv[2]);
  if (!pgm_getaddrinfo (g_network, NULL, &res, &pgm_err)) {
    printf("parsing network parameter: %s", pgm_err->message);
    return 0;
  }

  sa_family = res->ai_send_addrs[0].gsr_group.ss_family;

  if (g_udp_encap_port) {
    printf ("create PGM/UDP socket.");
    if (!pgm_socket (&g_sock, sa_family, SOCK_SEQPACKET, IPPROTO_UDP, &pgm_err)) {
      printf ("socket: %s", pgm_err->message);
      return 0;
    }
    pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_UDP_ENCAP_UCAST_PORT, &g_udp_encap_port, sizeof(g_udp_encap_port));
    pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_UDP_ENCAP_MCAST_PORT, &g_udp_encap_port, sizeof(g_udp_encap_port));
  }

  /* Use RFC 2113 tagging for PGM Router Assist */
  const int no_router_assist = 0;
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_IP_ROUTER_ALERT, &no_router_assist, sizeof(no_router_assist));

  /* set PGM parameters */
  const int recv_only = 1,
      passive = 0,
      peer_expiry = pgm_secs (300),
      spmr_expiry = pgm_msecs (250),
      nak_bo_ivl = pgm_msecs (50),
      nak_rpt_ivl = pgm_secs (2),
      nak_rdata_ivl = pgm_secs (2),
      nak_data_retries = 50,
      nak_ncf_retries = 50,
      g_max_tpdu = 1500,
      g_sqns = 100;

  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_RECV_ONLY, &recv_only, sizeof(recv_only));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_PASSIVE, &passive, sizeof(passive));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_MTU, &g_max_tpdu, sizeof(g_max_tpdu));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_RXW_SQNS, &g_sqns, sizeof(g_sqns));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_PEER_EXPIRY, &peer_expiry, sizeof(peer_expiry));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_SPMR_EXPIRY, &spmr_expiry, sizeof(spmr_expiry));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_BO_IVL, &nak_bo_ivl, sizeof(nak_bo_ivl));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_RPT_IVL, &nak_rpt_ivl, sizeof(nak_rpt_ivl));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_RDATA_IVL, &nak_rdata_ivl, sizeof(nak_rdata_ivl));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_DATA_RETRIES, &nak_data_retries, sizeof(nak_data_retries));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_NCF_RETRIES, &nak_ncf_retries, sizeof(nak_ncf_retries));

/* create global session identifier */
  struct pgm_sockaddr_t addr;
  memset (&addr, 0, sizeof(addr));
  addr.sa_port = DEFAULT_DATA_DESTINATION_PORT;
  addr.sa_addr.sport = DEFAULT_DATA_SOURCE_PORT;
  if (!pgm_gsi_create_from_hostname (&addr.sa_addr.gsi, &pgm_err)) {
    printf("creating GSI: %s", pgm_err->message);
    return 0;
  }

/* assign socket to specified address */
  struct pgm_interface_req_t if_req;
  memset (&if_req, 0, sizeof(if_req));
  if_req.ir_interface = res->ai_recv_addrs[0].gsr_interface;
  if_req.ir_scope_id  = 0;
  if (AF_INET6 == sa_family) {
    struct sockaddr_in6 sa6;
    memcpy (&sa6, &res->ai_recv_addrs[0].gsr_group, sizeof(sa6));
    if_req.ir_scope_id = sa6.sin6_scope_id;
  }
  if (!pgm_bind3 (g_sock,
      &addr, sizeof(addr),
      &if_req, sizeof(if_req),  /* tx interface */
      &if_req, sizeof(if_req),  /* rx interface */
      &pgm_err))
  {
    printf ("binding PGM socket: %s", pgm_err->message);
    return 0;
  }

/* join IP multicast groups */
  for (unsigned i = 0; i < res->ai_recv_addrs_len; i++)
    pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_JOIN_GROUP, &res->ai_recv_addrs[i], sizeof(struct group_req));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_SEND_GROUP, &res->ai_send_addrs[0], sizeof(struct group_req));
  pgm_freeaddrinfo (res);

/* set IP parameters */
  const int nonblocking = 1,
      multicast_loop = g_multicast_loop ? 1 : 0,
      multicast_hops = 16,
      dscp = 0x2e << 2;    /* Expedited Forwarding PHB for network elements, no ECN. */

  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_MULTICAST_LOOP, &multicast_loop, sizeof(multicast_loop));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_MULTICAST_HOPS, &multicast_hops, sizeof(multicast_hops));
  if (AF_INET6 != sa_family)
    pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_TOS, &dscp, sizeof(dscp));
  pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NOBLOCK, &nonblocking, sizeof(nonblocking));

  if (!pgm_connect (g_sock, &pgm_err)) {
    printf ("connecting PGM socket: %s", pgm_err->message);
    return 0;
  }

  std::ofstream saved_file(argv[3], std::ios::out | std::ios::binary);
  if (!saved_file.is_open()) {
    printf("Open file %s failed", argv[3]);
    return 0;
  }

  int n_fds = 0;
  fd_set readfds;
  const long iov_len = 20;
  const long ev_len  = 1;
  struct pgm_msgv_t msgv[iov_len];
  do {
    struct timeval tv;
    int timeout;
    size_t len;
    pgm_error_t* pgm_err = NULL;
    const int status = pgm_recvmsgv (g_sock,
                   msgv,
                   N_ELEMENTS(msgv),
                   0,
                   &len,
                   &pgm_err);
    switch (status) {
    case PGM_IO_STATUS_NORMAL:
      save_to_file(msgv, len, saved_file);
//      on_msgv (msgv, len);
      break;
    case PGM_IO_STATUS_TIMER_PENDING:
      {
        socklen_t optlen = sizeof (tv);
        pgm_getsockopt (g_sock, IPPROTO_PGM, PGM_TIME_REMAIN, &tv, &optlen);
      }
      goto block;
    case PGM_IO_STATUS_RATE_LIMITED:
      {
        socklen_t optlen = sizeof (tv);
        pgm_getsockopt (g_sock, IPPROTO_PGM, PGM_RATE_REMAIN, &tv, &optlen);
      }
/* fall through */
    case PGM_IO_STATUS_WOULD_BLOCK:
block:
      FD_ZERO(&readfds);
      pgm_select_info (g_sock, &readfds, NULL, &n_fds);
      select (n_fds, &readfds, NULL, NULL, PGM_IO_STATUS_RATE_LIMITED == status ? &tv : NULL);
      break;

    default:
      if (pgm_err) {
        printf ("%s", pgm_err->message);
        pgm_error_free (pgm_err);
        pgm_err = NULL;
      }
      if (PGM_IO_STATUS_ERROR == status)
        break;
    }
  } while (true);
  return 0;
}