/*(c) Copyright [first_year] – [latest_year] Xilinx, Inc. All rights reserved.
 *
 * This file contains confidential and proprietary information
 * of Xilinx, Inc. and is protected under U.S. and
 * international copyright and other intellectual property
 * laws.
 *
 * DISCLAIMER
 * This disclaimer is not a license and does not grant any
 * rights to the materials distributed herewith. Except as
 * otherwise provided in a valid license issued to you by
 * Xilinx, and to the maximum extent permitted by applicable
 * law: (1) THESE MATERIALS ARE MADE AVAILABLE "AS IS" AND
 * WITH ALL FAULTS, AND XILINX HEREBY DISCLAIMS ALL WARRANTIES
 * AND CONDITIONS, EXPRESS, IMPLIED, OR STATUTORY, INCLUDING
 * BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, NON-
 * INFRINGEMENT, OR FITNESS FOR ANY PARTICULAR PURPOSE; and
 * (2) Xilinx shall not be liable (whether in contract or tort,
 * including negligence, or under any other theory of
 * liability) for any loss or damage of any kind or nature
 * related to, arising under or in connection with these
 * materials, including for any direct, or any indirect,
 * special, incidental, or consequential loss or damage
 * (including loss of data, profits, goodwill, or any type of
 * loss or damage suffered as a result of any action brought
 * by a third party) even if such damage or loss was
 * reasonably foreseeable or Xilinx had been advised of the
 * possibility of the same.
 *
 * CRITICAL APPLICATIONS
 * Xilinx products are not designed or intended to be fail-
 * safe, or for use in any application requiring fail-safe
 * performance, such as life-support or safety devices or
 * systems, Class III medical devices, nuclear facilities,
 * applications related to the deployment of airbags, or any
 * other applications that could lead to death, personal
 * injury, or severe property or environmental damage
 * (individually and collectively, "Critical
 * Applications"). Customer assumes the sole risk and
 * liability of any use of Xilinx products in Critical
 * Applications, subject only to applicable laws and
 * regulations governing limitations on product liability.
 *
 * THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS
 * PART OF THIS FILE AT ALL TIMES.
 *
 * xvcClient.c
 *
 *  Created On  : Oct 6, 2014
 *
 *  Author      : Alvin Clark
 *
 *  Description : Xilinx Virtual Cable Server for Linux
 */


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#include <sys/mman.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>

#define MAP_SIZE      0x10000
#define LENGTH_OFFSET 0x00
#define TMS_OFFSET    0x04
#define TDI_OFFSET    0x08
#define TDO_OFFSET    0x0C
#define CTRL_OFFSET   0x10

static int jtag_state;
static int verbose;
static volatile void *ptr;
static int fd_uio;
static int commandStage = 0;

enum {
   test_logic_reset,
   run_test_idle,

   select_dr_scan,
   capture_dr,
   shift_dr,
   exit1_dr,
   pause_dr,
   exit2_dr,
   update_dr,

   select_ir_scan,
   capture_ir,
   shift_ir,
   exit1_ir,
   pause_ir,
   exit2_ir,
   update_ir,

   num_states
};

static int jtag_step(int state, int tms) {
   static const int next_state[num_states][2] =
   { [test_logic_reset] = { run_test_idle , test_logic_reset },
     [run_test_idle]    = { run_test_idle , select_dr_scan   },
     [select_dr_scan]   = { capture_dr    , select_ir_scan   },
     [capture_dr]       = { shift_dr      , exit1_dr         },
     [shift_dr]         = { shift_dr      , exit1_dr         },
     [exit1_dr]         = { pause_dr      , update_dr        },
     [pause_dr]         = { pause_dr      , exit2_dr         },
     [exit2_dr]         = { shift_dr      , update_dr        },
     [update_dr]        = { run_test_idle , select_dr_scan   },
     [select_ir_scan]   = { capture_ir    , test_logic_reset },
     [capture_ir]       = { shift_ir      , exit1_ir         },
     [shift_ir]         = { shift_ir      , exit1_ir         },
     [exit1_ir]         = { pause_ir      , update_ir        },
     [pause_ir]         = { pause_ir      , exit2_ir         },
     [exit2_ir]         = { shift_ir      , update_ir        },
     [update_ir]        = { run_test_idle , select_dr_scan   } };

   return next_state[state][tms];
}

static int sread(int fd, void *target, int len) {
   unsigned char *t = target;
   while (len) {
      int r = read(fd, t, len);
      if (r <= 0)
         return r;
      t += r;
      len -= r;
   }
   return 1;
}

int handle_data(int fd) {
	int i, n;
	int seen_tlr = 0;
	const char xvcInfo[] = "xvcServer_v1.0:2048\n";

	do {
		char cmd[16];
		unsigned char buffer[2048], result[1024];
		memset(cmd, 0, 16);

		if (sread(fd, cmd, 2) != 1)
			return 1;

		if (memcmp(cmd, "ge", 2) == 0) {
			if (sread(fd, cmd, 6) != 1)
				return 1;
			memcpy(result, xvcInfo, strlen(xvcInfo));
			if (write(fd, result, strlen(xvcInfo)) != strlen(xvcInfo)) {
				perror("write");
				return 1;
			}
			if (verbose) {
				printf("%u : Received command: 'getinfo'\n", (int)time(NULL));
				printf("\t Replied with %s\n", xvcInfo);
			}
			break;
		} else if (memcmp(cmd, "se", 2) == 0) {
			if (sread(fd, cmd, 9) != 1)
				return 1;
			memcpy(result, cmd + 5, 4);
			if (write(fd, result, 4) != 4) {
				perror("write");
				return 1;
			}
			if (verbose) {
				printf("%u : Received command: 'settck'\n", (int)time(NULL));
				printf("\t Replied with '%.*s'\n\n", 4, cmd + 5);
			}
			break;
		} else if (memcmp(cmd, "sh", 2) == 0) {
			if (sread(fd, cmd, 4) != 1)
				return 1;
//			if (verbose) {
				printf("%u : Received command: 'shift'\n", (int)time(NULL));
//			}
		} else {

			fprintf(stderr, "invalid cmd '%s'\n", cmd);
			return 1;
		}

		int len;
		if (sread(fd, &len, 4) != 1) {
			fprintf(stderr, "reading length failed\n");
			return 1;
		}

		int nr_bytes = (len + 7) / 8;
		if (nr_bytes * 2 > sizeof(buffer)) {
			fprintf(stderr, "buffer size exceeded\n");
			return 1;
		}

		if (sread(fd, buffer, nr_bytes * 2) != 1) {
			fprintf(stderr, "reading data failed\n");
			return 1;
		}
		memset(result, 0, nr_bytes);

		if (verbose) {
			printf("\tNumber of Bits  : %d\n", len);
			printf("\tNumber of Bytes : %d \n", nr_bytes);
//			for (i = 0; i < nr_bytes * 2; ++i)
//				printf("\t0x%02x \n", buffer[i]);
			printf("\n");
		}
//
		seen_tlr = (seen_tlr || jtag_state == test_logic_reset)
				&& (jtag_state != capture_dr) && (jtag_state != capture_ir);

		if ((jtag_state == exit1_ir && len == 5 && buffer[0] == 0x17)
				|| (jtag_state == exit1_dr && len == 4 && buffer[0] == 0x0b)) {
			if (verbose)
				printf("ignoring bogus jtag state movement in jtag_state %d\n",
						jtag_state);
		} else {
//

			int bytesLeft = nr_bytes;
			int bitsLeft = len;
			int byteIndex = 0;
			int tdi, tms, tdo;

			volatile ptr = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
					fd_uio, 0);

			if (ptr == MAP_FAILED)
				fprintf(stderr, "MMAP Failed\n");

			while (bytesLeft > 0) {
				tms = 0;
				tdi = 0;
				tdo = 0;
				if (bytesLeft >= 4) {
					memcpy(&tms, &buffer[byteIndex], 4);
					memcpy(&tdi, &buffer[byteIndex + nr_bytes], 4);

					*((unsigned *) (ptr + LENGTH_OFFSET)) = 32;
					*((unsigned *) (ptr + TMS_OFFSET)) = tms;
					*((unsigned *) (ptr + TDI_OFFSET)) = tdi;
					*((unsigned *) (ptr + CTRL_OFFSET)) = 0x01;

					/* Switch this to interrupt in next revision */
					while (*((unsigned *) (ptr + CTRL_OFFSET)))
						;

					tdo = *((unsigned *) (ptr + TDO_OFFSET));
					memcpy(&result[byteIndex], &tdo, 4);

					bytesLeft -= 4;
					bitsLeft -= 32;
					byteIndex += 4;

					if (verbose) {
						printf("LEN : 0x%08x\n", 32);
						printf("TMS : 0x%08x\n", tms);
						printf("TDI : 0x%08x\n", tdi);
						printf("TDO : 0x%08x\n", tdo);
					}

				} else {
					memcpy(&tms, &buffer[byteIndex], bytesLeft);
					memcpy(&tdi, &buffer[byteIndex + nr_bytes], bytesLeft);
					*((unsigned *) (ptr + LENGTH_OFFSET)) = bitsLeft;
					*((unsigned *) (ptr + TMS_OFFSET)) = tms;
					*((unsigned *) (ptr + TDI_OFFSET)) = tdi;
					*((unsigned *) (ptr + CTRL_OFFSET)) = 0x01;
					/* Switch this to interrupt in next revision */
					while (*((unsigned *) (ptr + CTRL_OFFSET)))
						;
					tdo = *((unsigned *) (ptr + TDO_OFFSET));
					memcpy(&result[byteIndex], &tdo, bytesLeft);

					if (verbose) {
						printf("LEN : 0x%08x\n", 32);
						printf("TMS : 0x%08x\n", tms);
						printf("TDI : 0x%08x\n", tdi);
						printf("TDO : 0x%08x\n", tdo);
					}

					break;
				}
			}
			munmap(ptr, MAP_SIZE);
		}

		if (write(fd, result, nr_bytes) != nr_bytes) {
			perror("write");
			return 1;
		}

	} while (!(seen_tlr && jtag_state == run_test_idle));
	/* Note: Need to fix JTAG state updates, until then no exit is allowed */
	return 0;
}

int main(int argc, char **argv) {
   int i;
   int s;
   int c;
   struct sockaddr_in address;

   opterr = 0;

   while ((c = getopt(argc, argv, "v")) != -1)
      switch (c) {
      case 'v':
         verbose = 1;
         break;
      case '?':
         fprintf(stderr, "usage: %s [-v]\n", *argv);
         return 1;
      }

   fd_uio = open("/dev/uio0", O_RDWR | O_SYNC);
   if (fd_uio < 1) {
      fprintf(stderr,"Failed to Open UIO Device\n");
      return -1;
   }

   s = socket(AF_INET, SOCK_STREAM, 0);

   if (s < 0) {
      perror("socket");
      return 1;
   }

   i = 1;
   setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &i, sizeof i);

   address.sin_addr.s_addr = INADDR_ANY;
   address.sin_port = htons(2542);
   address.sin_family = AF_INET;

   if (bind(s, (struct sockaddr*) &address, sizeof(address)) < 0) {
      perror("bind");
      return 1;
   }

   if (listen(s, 0) < 0) {
      perror("listen");
      return 1;
   }

   fd_set conn;
   int maxfd = 0;

   FD_ZERO(&conn);
   FD_SET(s, &conn);

   maxfd = s;

   while (1) {
      fd_set read = conn, except = conn;
      int fd;

      if (select(maxfd + 1, &read, 0, &except, 0) < 0) {
         perror("select");
         break;
      }

      for (fd = 0; fd <= maxfd; ++fd) {
         if (FD_ISSET(fd, &read)) {
            if (fd == s) {
               int newfd;
               socklen_t nsize = sizeof(address);

               newfd = accept(s, (struct sockaddr*) &address, &nsize);

//               if (verbose)
                  printf("connection accepted - fd %d\n", newfd);
               if (newfd < 0) {
                  perror("accept");
               } else {
            	   printf("setting TCP_NODELAY to 1\n");
            	  int flag = 1;
            	  int optResult = setsockopt(newfd,
            			  	  	  	  	  	 IPPROTO_TCP,
            			  	  	  	  	  	 TCP_NODELAY,
            			  	  	  	  	  	 (char *)&flag,
            			  	  	  	  	  	 sizeof(int));
            	  if (optResult < 0)
            		  perror("TCP_NODELAY error");
                  if (newfd > maxfd) {
                     maxfd = newfd;
                  }
                  FD_SET(newfd, &conn);
               }
            }
            else if (handle_data(fd)) {

               if (verbose)
                  printf("connection closed - fd %d\n", fd);
               close(fd);
               FD_CLR(fd, &conn);
            }
         }
         else if (FD_ISSET(fd, &except)) {
            if (verbose)
               printf("connection aborted - fd %d\n", fd);
            close(fd);
            FD_CLR(fd, &conn);
            if (fd == s)
               break;
         }
      }
   }
   return 0;
}
