/*
 * canwsserver.c
 *
 * Copyright (c) 2012 Tim Trampedach
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Volkswagen nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Alternatively, provided that this notice is retained in full, this
 * software may be distributed under the terms of the GNU General
 * Public License ("GPL") version 2, in which case the provisions of the
 * GPL apply INSTEAD OF those given above.
 *
 * The provided data structures and external interfaces from this code
 * are not restricted to be used by modules with a GPL compatible license.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * Send feedback to <socketcan-users@lists.berlios.de>
 *
 */

#include "canwsserver.h"

#include "lib/websockets/libwebsockets.h"
#include "helpers.h"
#include "wsserver.h"

extern int optind, opterr, optopt;

static volatile int running = 1;
static char devname[MAXDEV][IFNAMSIZ+1];
static int  dindex[MAXDEV];
static int  max_devname_len;

void print_usage(char *prg)
{
    fprintf(stderr, "\nUsage: %s [options] <CAN interface>+\n", prg);
    fprintf(stderr, "  (use CTRL-C to terminate %s)\n\n", prg);
	fprintf(stderr, "Options:  -p <port>   (listen on port <port>. Default: %d)\n", DEFPORT);
	fprintf(stderr, "\n");
	fprintf(stderr, "\nUse interface name '%s' to receive from all CAN interfaces.\n\n", ANYDEV);
}

int idx2dindex(int ifidx, int socket)
{
	int i;
	struct ifreq ifr;

	for (i=0; i<MAXDEV; i++) {
		if (dindex[i] == ifidx)
			return i;
	}

	/* remove index cache zombies first */
	for (i=0; i < MAXDEV; i++) {
		if (dindex[i]) {
			ifr.ifr_ifindex = dindex[i];
			if (ioctl(socket, SIOCGIFNAME, &ifr) < 0)
				dindex[i] = 0;
		}
	}

	for (i=0; i < MAXDEV; i++)
		if (!dindex[i]) /* free entry */
			break;

	if (i == MAXDEV) {
		printf("Interface index cache only supports %d interfaces.\n", MAXDEV);
		exit(1);
	}

	dindex[i] = ifidx;

	ifr.ifr_ifindex = ifidx;
	if (ioctl(socket, SIOCGIFNAME, &ifr) < 0)
		perror("SIOCGIFNAME");

	if (max_devname_len < strlen(ifr.ifr_name))
		max_devname_len = strlen(ifr.ifr_name);

	strcpy(devname[i], ifr.ifr_name);

	return i;
}

/* 
 * This is a Signalhandler. When we get a signal, that a child
 * terminated, we wait for it, so the zombie will disappear.
 */
void childdied(int i)
{
	wait(NULL);
}

/*
 * This is a Signalhandler for a cought SIGTERM
 */
void shutdown_gra(int i)
{
	exit(0);
}


int main(int argc, char **argv)
{
	struct sigaction signalaction;
	sigset_t sigset;
	fd_set rdfs;
	int s[MAXDEV];
	canid_t mask[MAXDEV] = {0};
	canid_t value[MAXDEV] = {0};
	int inv_filter[MAXDEV] = {0};
	can_err_mask_t err_mask[MAXDEV] = {0};
	int opt, ret;
	int currmax = 1; /* we assume at least one can bus ;-) */
	struct sockaddr_can addr;
	struct can_filter rfilter;
	struct can_frame frame;
	int nbytes, i, j;
	struct ifreq ifr;
	struct timeval tv;
	char temp[DATA_BUFLEN];
	
	int n = 0;
	const char *cert_path =
			    LOCAL_RESOURCE_PATH"/libwebsockets-test-server.pem";
	const char *key_path =
			LOCAL_RESOURCE_PATH"/libwebsockets-test-server.key.pem";
	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + DATA_BUFLEN +
						  LWS_SEND_BUFFER_POST_PADDING];
	int port = DEFPORT;
	int use_ssl = 0;
	struct libwebsocket_context *context;
	int opts = 0;
	//char interface_name[128] = "";
	const char * interface = NULL;

	sigemptyset(&sigset);
	signalaction.sa_handler = &childdied;
	signalaction.sa_mask = sigset;
	signalaction.sa_flags = 0;
	sigaction(SIGCHLD, &signalaction, NULL);  /* install signal for dying child */
	signalaction.sa_handler = &shutdown_gra;
	signalaction.sa_mask = sigset;
	signalaction.sa_flags = 0;
	sigaction(SIGTERM, &signalaction, NULL); /* install Signal for termination */
	sigaction(SIGINT, &signalaction, NULL); /* install Signal for termination */

	if (!use_ssl)
		cert_path = key_path = NULL;

	context = libwebsocket_create_context(port, interface, protocols, cert_path, key_path, -1, -1, opts);

	if (context == NULL) {
		fprintf(stderr, "libwebsocket init failed\n");
		return -1;
	}

	while ((opt = getopt(argc, argv, "p:?")) != -1) {

		switch (opt) {
		case 'p':
			port = atoi(optarg);
			break;

		case 's':
			use_ssl = 1;
			break;

		default:
			print_usage(basename(argv[0]));
			exit(1);
			break;
		}
	}

	if (optind == argc) {
		print_usage(basename(argv[0]));
		exit(0);
	}

	/* count in options higher than device count ? */
	if (optind + currmax > argc) {
		printf("low count of CAN devices!\n");
		return 1;
	}

	currmax = argc - optind; /* find real number of CAN devices */

	if (currmax > MAXDEV) {
		printf("More than %d CAN devices!\n", MAXDEV);
		return 1;
	}

	/* fork the websockets service loop */
	n = libwebsockets_fork_service_loop(context);
	if (n < 0) {
		fprintf(stderr, "Unable to fork service loop %d\n", n);
		return 1;
	}

	for (i=0; i<currmax; i++) {

#ifdef DEBUG
		printf("open %d '%s' m%08X v%08X i%d e%d.\n",
		       i, argv[optind+i], mask[i], value[i],
		       inv_filter[i], err_mask[i]);
#endif

		if ((s[i] = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
			perror("socket");
			return 1;
		}

		if (mask[i] || value[i]) {

			printf("CAN ID filter[%d] for %s set to "
			       "mask = %08X, value = %08X %s\n",
			       i, argv[optind+i], mask[i], value[i],
			       (inv_filter[i]) ? "(inv_filter)" : "");

			rfilter.can_id   = value[i];
			rfilter.can_mask = mask[i];
			if (inv_filter[i])
				rfilter.can_id |= CAN_INV_FILTER;

			setsockopt(s[i], SOL_CAN_RAW, CAN_RAW_FILTER,
				   &rfilter, sizeof(rfilter));
		}

		if (err_mask[i])
			setsockopt(s[i], SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
				   &err_mask[i], sizeof(err_mask[i]));

		j = strlen(argv[optind+i]);

		if (!(j < IFNAMSIZ)) {
			printf("name of CAN device '%s' is too long!\n", argv[optind+i]);
			return 1;
		}

		if (j > max_devname_len)
			max_devname_len = j; /* for nice printing */

		addr.can_family = AF_CAN;

		if (strcmp(ANYDEV, argv[optind+i])) {
			strcpy(ifr.ifr_name, argv[optind+i]);
			if (ioctl(s[i], SIOCGIFINDEX, &ifr) < 0) {
				perror("SIOCGIFINDEX");
				exit(1);
			}
			addr.can_ifindex = ifr.ifr_ifindex;
		}
		else
			addr.can_ifindex = 0; /* any can interface */

		if (bind(s[i], (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			perror("bindcan");
			return 1;
		}
	}

	while (running) {

		FD_ZERO(&rdfs);
		for (i=0; i<currmax; i++)
			FD_SET(s[i], &rdfs);

		if ((ret = select(s[currmax-1]+1, &rdfs, NULL, NULL, NULL)) < 0) {
			//perror("select");
			running = 0;
			continue;
		}

		for (i=0; i<currmax; i++) {  /* check all CAN RAW sockets */

			if (FD_ISSET(s[i], &rdfs)) {

				socklen_t len = sizeof(addr);
				int idx;
										  
				if ((nbytes = recvfrom(s[i], &frame,
						       sizeof(struct can_frame), 0,
						       (struct sockaddr*)&addr, &len)) < 0) {
					perror("read");
					return 1;
				}

				if (nbytes < sizeof(struct can_frame)) {
					fprintf(stderr, "read: incomplete CAN frame\n");
					return 1;
				}

				if (ioctl(s[i], SIOCGSTAMP, &tv) < 0)
					perror("SIOCGSTAMP");


				idx = idx2dindex(addr.can_ifindex, s[i]);

				sprintf(temp, "(%ld.%06ld) %*s ", tv.tv_sec, tv.tv_usec, max_devname_len, devname[idx]);
				sprint_canframe(temp+strlen(temp), &frame, 0); 

				//printf("%s\n",temp);
                //printf("0x%x ", frame.can_id);
                
				sprintf(&buf[LWS_SEND_BUFFER_PRE_PADDING], "%s", temp);

				libwebsockets_broadcast(&protocols[PROTOCOL_CAN_RAW_RELAY], &buf[LWS_SEND_BUFFER_PRE_PADDING], DATA_BUFLEN);
                libwebsockets_broadcast(&protocols[PROTOCOL_CAN_RAW_DELTA], &buf[LWS_SEND_BUFFER_PRE_PADDING], DATA_BUFLEN);

#if 0
				/* print CAN frame in log file style to stdout */
				printf("(%ld.%06ld) ", tv.tv_sec, tv.tv_usec);
				printf("%*s ", max_devname_len, devname[idx]);
				fprint_canframe(stdout, &frame, "\n", 0);
#endif
			}

		}
	}

	for (i=0; i<currmax; i++)
		close(s[i]);

	libwebsocket_context_destroy(context);

	return 0;
}
