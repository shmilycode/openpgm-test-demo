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
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pgm/pgm.h>
#include <fstream>
#include <thread>
#include <poll.h>

static int		g_port = 0;
static const char*	g_network = "";
static int		g_udp_encap_port = 0;

static int		g_odata_rate = 0;
static int		g_odata_interval = 0;
static unsigned int		g_payload = 0;
static int		g_max_tpdu = 1500;
static int		g_max_rte = 16*1000*1000;
static int		g_odata_rte = 0;	/* 0 = disabled */
static int		g_rdata_rte = 0;	/* 0 = disabled */
static int		g_sqns = 200;

static bool		g_use_pgmcc = FALSE;
static sa_family_t	g_pgmcc_family = 0;	/* 0 = disabled */

static bool		g_use_fec = FALSE;
static int		g_rs_k = 8;
static int		g_rs_n = 255;
static pgm_sock_t*	g_sock = NULL;
static bool g_send_only = false;
static bool g_receive_only = false;
char* g_filename = "";

#define N_ELEMENTS(arr)    (sizeof (arr) / sizeof ((arr)[0]))

void sender_thread (pgm_sock_t*	sock, const char* filename)
{
	pgm_sock_t* tx_sock = sock;

  std::ifstream inputfile(filename, std::ios::in | std::ios::binary);
  if (!inputfile.is_open()) {
    printf("can't open file %s", filename);
    return;
  }

  #define MAX_BUFFER_SIZE (1024)
  char* buffer = new char[MAX_BUFFER_SIZE];
  while (!inputfile.eof()) {
    inputfile.read(buffer, MAX_BUFFER_SIZE);
    unsigned int size_read = inputfile.gcount();
    if (size_read <= 0) {
      printf("read failed!");
      break;
    }

		struct timeval tv;
		int n_fds = 0;
		fd_set readfds, writefds;
		size_t bytes_written;
		int status;
again:
    status = pgm_send (g_sock, buffer, size_read, &bytes_written);
		switch (status) {
/* rate control */
		case PGM_IO_STATUS_RATE_LIMITED:
		{
			socklen_t optlen = sizeof (tv);
			const bool status = pgm_getsockopt (tx_sock, IPPROTO_PGM, PGM_RATE_REMAIN, &tv, &optlen);
			if (!status) {
				printf ("getting PGM_RATE_REMAIN failed");
				break;
			}
			FD_ZERO(&readfds);
      pgm_select_info (g_sock, &readfds, NULL, &n_fds);
			n_fds = select (n_fds, &readfds, NULL, NULL, PGM_IO_STATUS_WOULD_BLOCK == status ? NULL : &tv);
			goto again;
		}
/* congestion control */
		case PGM_IO_STATUS_CONGESTION:
/* kernel feedback */
		case PGM_IO_STATUS_WOULD_BLOCK:
		{
      FD_ZERO(&readfds);
      pgm_select_info (g_sock, &readfds, &writefds, &n_fds);
			n_fds = select (n_fds, &readfds, &writefds, NULL, NULL);

			goto again;
		}
/* successful delivery */
		case PGM_IO_STATUS_NORMAL:
//			g_message ("sent payload: %s", ping.DebugString().c_str());
//			g_message ("sent %u bytes", (unsigned)bytes_written);
			break;
		default:
			printf ("pgm_send failed, status:%i", status);
      return;
		}
	}
}

static
void receiver_thread (pgm_sock_t* sock)
{
	pgm_sock_t* rx_sock = sock;
	const long iov_len = 20;
	struct pgm_msgv_t msgv[iov_len];
  unsigned int  total_receive = 0;

	do {
		struct timeval tv;
    int n_fds = 0;
		fd_set readfds;
		size_t len;
		pgm_error_t* pgm_err;
		int status;
again:
		pgm_err = NULL;
		status = pgm_recvmsgv (rx_sock,
				       msgv,
				       N_ELEMENTS(msgv),
				       MSG_ERRQUEUE,
				       &len,
				       &pgm_err);

		switch (status) {
		case PGM_IO_STATUS_NORMAL:
      total_receive += len;
      printf("total receive: %d\n", total_receive);
//			on_msgv (msgv, len);
			break;
		case PGM_IO_STATUS_TIMER_PENDING:
			{
				socklen_t optlen = sizeof (tv);
				const bool status = pgm_getsockopt (g_sock, IPPROTO_PGM, PGM_TIME_REMAIN, &tv, &optlen);
				if (!status) {
					printf ("getting PGM_TIME_REMAIN failed");
					break;
				}
			}
			goto block;
		case PGM_IO_STATUS_RATE_LIMITED:
			{
				socklen_t optlen = sizeof (tv);
				const bool status = pgm_getsockopt (g_sock, IPPROTO_PGM, PGM_RATE_REMAIN, &tv, &optlen);
				if (!status) {
					printf ("getting PGM_RATE_REMAIN failed");
					break;
				}
			}
/* fall through */
		case PGM_IO_STATUS_WOULD_BLOCK:
block:
      FD_ZERO(&readfds);
      pgm_select_info (g_sock, &readfds, NULL, &n_fds);
			n_fds = select (n_fds, &readfds, NULL, NULL, PGM_IO_STATUS_WOULD_BLOCK == status ? NULL : &tv);
			break;

		case PGM_IO_STATUS_RESET:
		{
      printf("???? PGM_IO_STATUS_RESET");
			break;
		}
		default:
			if (pgm_err) {
				printf ("%s", pgm_err->message);
				pgm_error_free (pgm_err);
				pgm_err = NULL;
			}
			break;
		}
	} while (true);
}

bool on_startup() {
	struct pgm_addrinfo_t* res = NULL;
	pgm_error_t* pgm_err = NULL;
	sa_family_t sa_family = AF_UNSPEC;
/* parse network parameter into transport address structure */
	if (!pgm_getaddrinfo (g_network, NULL, &res, &pgm_err)) {
		printf ("parsing network parameter: %s", pgm_err->message);
    return false;
	}

	sa_family = res->ai_send_addrs[0].gsr_group.ss_family;
	if (g_use_pgmcc)
		g_pgmcc_family = sa_family;

	if (g_udp_encap_port) {
		printf ("create PGM/UDP socket.");
		if (!pgm_socket (&g_sock, sa_family, SOCK_SEQPACKET, IPPROTO_UDP, &pgm_err)) {
			printf ("socket: %s", pgm_err->message);
      return false;
		}

		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_UDP_ENCAP_UCAST_PORT, &g_udp_encap_port, sizeof(g_udp_encap_port))) {
			printf ("setting PGM_UDP_ENCAP_UCAST_PORT = %d", g_udp_encap_port);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_UDP_ENCAP_MCAST_PORT, &g_udp_encap_port, sizeof(g_udp_encap_port))) {
			printf ("setting PGM_UDP_ENCAP_MCAST_PORT = %d", g_udp_encap_port);
      return false;
		}
	} else {
		printf ("create PGM/IP socket.");
		if (!pgm_socket (&g_sock, sa_family, SOCK_SEQPACKET, IPPROTO_PGM, &pgm_err)) {
			printf ("socket: %s", pgm_err->message);
      return false;
		}
	}

/* Use RFC 2113 tagging for PGM Router Assist */
	{
		const int no_router_assist = 0;
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_IP_ROUTER_ALERT, &no_router_assist, sizeof(no_router_assist))) {
			printf ("setting PGM_IP_ROUTER_ALERT = %d", no_router_assist);
      return false;
		}
	}


/* set PGM parameters */
/* common */
	{
		const int txbufsize = 1024 * 1024, rxbufsize = 1024 * 1024;
	  int max_tpdu = g_max_tpdu;

		if (!pgm_setsockopt (g_sock, SOL_SOCKET, SO_RCVBUF, &rxbufsize, sizeof(rxbufsize))) {
			printf ("setting SO_RCVBUF = %d", rxbufsize);
      return false;
		}
		if (!pgm_setsockopt (g_sock, SOL_SOCKET, SO_SNDBUF, &txbufsize, sizeof(txbufsize))) {
			printf ("setting SO_SNDBUF = %d", txbufsize);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_MTU, &max_tpdu, sizeof(max_tpdu))) {
			printf ("setting PGM_MTU = %d", max_tpdu);
      return false;
		}
	}

/* send side */
	{
		const int send_only	  = g_send_only,
			  txw_sqns	  = g_sqns * 4,
			  txw_max_rte	  = g_max_rte,
			  odata_max_rte	  = g_odata_rte,
			  rdata_max_rte	  = g_rdata_rte,
			  ambient_spm	  = pgm_secs (30),
			  heartbeat_spm[] = { pgm_msecs (100),
					      pgm_msecs (100),
					      pgm_msecs (100),
					      pgm_msecs (100),
					      pgm_msecs (1300),
					      pgm_secs  (7),
					      pgm_secs  (16),
					      pgm_secs  (25),
					      pgm_secs  (30) };

		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_SEND_ONLY, &send_only, sizeof(send_only))) {
			printf ("setting PGM_SEND_ONLY = %d", send_only);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_TXW_SQNS, &txw_sqns, sizeof(txw_sqns))) {
			printf ("setting PGM_TXW_SQNS = %d", txw_sqns);
      return false;
		}
		if (txw_max_rte > 0 &&
		    !pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_TXW_MAX_RTE, &txw_max_rte, sizeof(txw_max_rte))) {
			printf ("setting PGM_TXW_MAX_RTE = %d", txw_max_rte);
      return false;
		}
		if (odata_max_rte > 0 &&
		    !pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_ODATA_MAX_RTE, &odata_max_rte, sizeof(odata_max_rte))) {
			printf ("setting PGM_ODATA_MAX_RTE = %d", odata_max_rte);
      return false;
		}
		if (rdata_max_rte > 0 &&
		    !pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_RDATA_MAX_RTE, &rdata_max_rte, sizeof(rdata_max_rte))) {
			printf ("setting PGM_RDATA_MAX_RTE = %d", rdata_max_rte);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_AMBIENT_SPM, &ambient_spm, sizeof(ambient_spm))) {
			printf ("setting PGM_AMBIENT_SPM = %d", ambient_spm);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_HEARTBEAT_SPM, &heartbeat_spm, sizeof(heartbeat_spm))) {
	                char buffer[1024];
	                sprintf (buffer, "%d", heartbeat_spm[0]);
	                for (unsigned i = 1; i < N_ELEMENTS(heartbeat_spm); i++) {
	                        char t[1024];
	                        sprintf (t, ", %d", heartbeat_spm[i]);
	                        strcat (buffer, t);
	                }
	                printf ("setting HEARTBEAT_SPM = { %s }", buffer);
                  return 0;
	        }

	}

/* receive side */
	{
		const int recv_only	   = g_receive_only,
			  not_passive	   = 0,
			  rxw_sqns	   = g_sqns,
			  peer_expiry	   = pgm_secs (300),
			  spmr_expiry	   = pgm_msecs (250),
			  nak_bo_ivl	   = pgm_msecs (50),
			  nak_rpt_ivl	   = pgm_msecs (200), //pgm_secs (2),
			  nak_rdata_ivl    = pgm_msecs (200), //pgm_secs (2),
			  nak_data_retries = 50,
			  nak_ncf_retries  = 50;

		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_RECV_ONLY, &recv_only, sizeof(recv_only))) {
			printf ("setting PGM_RECV_ONLY = %d", recv_only);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_PASSIVE, &not_passive, sizeof(not_passive))) {
			printf ("setting PGM_PASSIVE = %d", not_passive);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_RXW_SQNS, &rxw_sqns, sizeof(rxw_sqns))) {
			printf ("setting PGM_RXW_SQNS = %d", rxw_sqns);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_PEER_EXPIRY, &peer_expiry, sizeof(peer_expiry))) {
			printf ("setting PGM_PEER_EXPIRY = %d", peer_expiry);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_SPMR_EXPIRY, &spmr_expiry, sizeof(spmr_expiry))) {
			printf ("setting PGM_SPMR_EXPIRY = %d", spmr_expiry);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_BO_IVL, &nak_bo_ivl, sizeof(nak_bo_ivl))) {
			printf ("setting PGM_NAK_BO_IVL = %d", nak_bo_ivl);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_RPT_IVL, &nak_rpt_ivl, sizeof(nak_rpt_ivl))) {
			printf ("setting PGM_NAK_RPT_IVL = %d", nak_rpt_ivl);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_RDATA_IVL, &nak_rdata_ivl, sizeof(nak_rdata_ivl))) {
			printf ("setting PGM_NAK_RDATA_IVL = %d", nak_rdata_ivl);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_DATA_RETRIES, &nak_data_retries, sizeof(nak_data_retries))) {
			printf ("setting PGM_NAK_DATA_RETRIES = %d", nak_data_retries);
      return false;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NAK_NCF_RETRIES, &nak_ncf_retries, sizeof(nak_ncf_retries))) {
			printf ("setting PGM_NAK_NCF_RETRIES = %d", nak_ncf_retries);
      return false;
		}
	}

/* create global session identifier */
	struct pgm_sockaddr_t addr;
	memset (&addr, 0, sizeof(addr));
	addr.sa_port = (0 != g_port) ? g_port : DEFAULT_DATA_DESTINATION_PORT;
	addr.sa_addr.sport = DEFAULT_DATA_SOURCE_PORT;
	if (!pgm_gsi_create_from_hostname (&addr.sa_addr.gsi, &pgm_err)) {
		printf ("creating GSI: %s", pgm_err->message);
    return false;
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
			&if_req, sizeof(if_req),	/* tx interface */
			&if_req, sizeof(if_req),	/* rx interface */
			&pgm_err))
	{
		printf ("binding PGM socket: %s", pgm_err->message);
    return 0;
	}

/* join IP multicast groups */
	for (unsigned i = 0; i < res->ai_recv_addrs_len; i++)
	{
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_JOIN_GROUP, &res->ai_recv_addrs[i], sizeof(struct group_req))) {
			char group[INET6_ADDRSTRLEN];
			getnameinfo ((struct sockaddr*)&res->ai_recv_addrs[i].gsr_group, sizeof(struct sockaddr_in),
                                        group, sizeof(group),
                                        NULL, 0,
                                        NI_NUMERICHOST);
			printf ("setting PGM_JOIN_GROUP = { #%u %s }",
				(unsigned)res->ai_recv_addrs[i].gsr_interface,
				group);
      return 0;
		}
	}
	if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_SEND_GROUP, &res->ai_send_addrs[0], sizeof(struct group_req))) {
                char group[INET6_ADDRSTRLEN];
                getnameinfo ((struct sockaddr*)&res->ai_send_addrs[0].gsr_group, sizeof(struct sockaddr_in),
				group, sizeof(group),
                                NULL, 0,
                                NI_NUMERICHOST);
		printf ("setting PGM_SEND_GROUP = { #%u %s }",
			(unsigned)res->ai_send_addrs[0].gsr_interface,
			group);
    return 0;
	}
	pgm_freeaddrinfo (res);

/* set IP parameters */
	{
		const int nonblocking	   = 1,
			  multicast_direct = 0,
			  multicast_hops   = 16,
			  dscp		   = 0x2e << 2;	/* Expedited Forwarding PHB for network elements, no ECN. */

		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_MULTICAST_LOOP, &multicast_direct, sizeof(multicast_direct))) {
			printf ("setting PGM_MULTICAST_LOOP = %d", multicast_direct);
      return 0;
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_MULTICAST_HOPS, &multicast_hops, sizeof(multicast_hops))) {
			printf ("setting PGM_MULTICAST_HOPS = %d", multicast_hops);
      return 0;
		}
		if (AF_INET6 != sa_family) {
			if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_TOS, &dscp, sizeof(dscp))) {
				printf ("setting PGM_TOS = 0x%x", dscp);
        return 0;
			}
		}
		if (!pgm_setsockopt (g_sock, IPPROTO_PGM, PGM_NOBLOCK, &nonblocking, sizeof(nonblocking))) {
			printf ("setting PGM_NOBLOCK = %d", nonblocking);
      return 0;
		}
	}

	if (!pgm_connect (g_sock, &pgm_err)) {
		printf ("connecting PGM socket: %s", pgm_err->message);
    return 0;
	}

	std::thread sthread = std::thread(sender_thread, g_sock, g_filename);

	std::thread rthread = std::thread (receiver_thread, g_sock);

	printf ("startup complete.");
  sthread.join();
  rthread.join();
	return true;
}

int main(int argc, char** argv) {
	pgm_error_t* pgm_err = NULL;
	if (!pgm_init (&pgm_err)) {
		printf ("Unable to start PGM engine: %s", pgm_err->message);
		pgm_error_free (pgm_err);
		return 0;
	}

  g_network = argv[1];
  g_udp_encap_port = std::stoi(argv[2]);
  g_filename = argv[3];
  on_startup();
}