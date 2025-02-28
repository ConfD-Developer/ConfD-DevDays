/*********************************************************************
 * ConfD Subscriber intro example
 * Implements a DHCP server adapter
 *
 * (C) 2005-2007 Tail-f Systems
 * Permission to use this code as a starting point hereby granted
 *
 * See the README file for more information
 ********************************************************************/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdio.h>

#include <confd_lib.h>
#include <confd_cdb.h>

/* include generated file */
#include "dhcpd.h"

/********************************************************************/

static int duration_to_secs(struct confd_duration *d)
{
    return (d->secs) +
        (d->mins    * 60) +
        (d->hours   * 60 * 60) +
        (d->days    * 60 * 60 * 24) +
        (d->months  * 60 * 60 * 24 * 30) +
        (d->years   * 60 * 60 * 24 * 365);
}

static void do_subnet(int rsock, FILE *fp)
{
   struct in_addr ip;
   char buf[BUFSIZ];
   struct confd_duration dur;
   char *ptr;

   cdb_get_ipv4(rsock, &ip, "net");
   fprintf(fp, "subnet %s ", inet_ntoa(ip));
   cdb_get_ipv4(rsock, &ip, "mask");
   fprintf(fp, "netmask %s {\n", inet_ntoa(ip));

   if (cdb_exists(rsock, "range") == 1) {
       int bool;
       fprintf(fp, " range ");
       cdb_get_bool(rsock, &bool, "range/dynamic-bootp");
       if (bool) fprintf(fp, " dynamic-bootp ");
       cdb_get_ipv4(rsock, &ip, "range/low-addr");
       fprintf(fp, " %s ", inet_ntoa(ip));
       cdb_get_ipv4(rsock, &ip, "range/high-addr");
       fprintf(fp, " %s ", inet_ntoa(ip));
       fprintf(fp, "\n");
   }
   if(cdb_get_str(rsock, &buf[0], BUFSIZ, "routers") == CONFD_OK) {

       /* replace space with comma */
       for (ptr = buf; *ptr != '\0'; ptr++) {
           if (*ptr == ' ')
               *ptr = ',';
       }
       fprintf(fp, " option routers %s\n", buf);
   }


   confd_value_t vv;
   cdb_get(rsock, &vv,  "max-lease-time");

   cdb_get_duration(rsock, &dur, "max-lease-time");
   fprintf(fp, " max-lease-time %d\n", duration_to_secs(&dur));
   fprintf(fp, "};\n");
}

static int read_conf(struct sockaddr_in *addr)
{
    FILE *fp;
    struct confd_duration dur;
    int i, n, tmp;
    int rsock;

    if ((rsock = socket(PF_INET, SOCK_STREAM, 0)) < 0 )
        confd_fatal("Failed to open socket\n");

    if (cdb_connect(rsock, CDB_READ_SOCKET, (struct sockaddr*)addr,
                      sizeof (struct sockaddr_in)) < 0)
        return CONFD_ERR;
    if (cdb_start_session(rsock, CDB_RUNNING) != CONFD_OK)
        return CONFD_ERR;
    cdb_set_namespace(rsock, dhcpd__ns);

    if ((fp = fopen("dhcpd.conf.tmp", "w")) == NULL) {
        cdb_close(rsock);
        return CONFD_ERR;
    }
    cdb_get_duration(rsock, &dur, "/dhcp/default-lease-time");
    fprintf(fp, "default-lease-time %d\n", duration_to_secs(&dur));

    cdb_get_duration(rsock, &dur, "/dhcp/max-lease-time");
    fprintf(fp, "max-lease-time %d\n", duration_to_secs(&dur));

    cdb_get_enum_value(rsock, &tmp, "/dhcp/log-facility");
    switch (tmp) {
    case dhcpd_kern:
        fprintf(fp, "log-facility kern\n");
        break;
    case dhcpd_mail:
        fprintf(fp, "log-facility mail\n");
        break;
    case dhcpd_local7:
        fprintf(fp, "log-facility local7\n");
        break;
    }
    n = cdb_num_instances(rsock, "/dhcp/subnets/subnet");
    for (i=0; i<n; i++) {
        cdb_cd(rsock, "/dhcp/subnets/subnet[%d]", i);
        do_subnet(rsock, fp);
    }
    n = cdb_num_instances(rsock, "/dhcp/shared-networks/shared-network");
    for (i=0; i<n; i++) {
        unsigned char *buf;
        int buflen;
        int j, m;

        cdb_get_buf(rsock, &buf, &buflen,
                    "/dhcp/shared-networks/shared-network[%d]/name", i);
        fprintf(fp, "shared-network %.*s {\n", buflen, buf);
        m = cdb_num_instances(rsock,
                "/dhcp/shared-networks/shared-network[%d]/subnets/subnet", i);
        for (j=0; j<m; j++) {
            cdb_pushd(rsock, "/dhcp/shared-networks/shared-network[%d]/"
                      "subnets/subnet[%d]", i, j);
            do_subnet(rsock, fp);
            cdb_popd(rsock);
        }
        fprintf(fp, "}\n");
    }
    fclose(fp);
    return cdb_close(rsock);
}

/********************************************************************/

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    int subsock;
    int status;
    int spoint;

    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFD_PORT);

    confd_init(argv[0], stderr, CONFD_TRACE);

    /*
     * Setup subscriptions
     */
    if ((subsock = socket(PF_INET, SOCK_STREAM, 0)) < 0 )
        confd_fatal("Failed to open socket\n");

    if (cdb_connect(subsock, CDB_SUBSCRIPTION_SOCKET, (struct sockaddr*)&addr,
                      sizeof (struct sockaddr_in)) < 0)
        confd_fatal("Failed to cdb_connect() to confd \n");

    if ((status = cdb_subscribe(subsock, 3, dhcpd__ns, &spoint, "/dhcp"))
        != CONFD_OK) {
        fprintf(stderr, "Terminate: subscribe %d\n", status);
        exit(0);
    }
    if (cdb_subscribe_done(subsock) != CONFD_OK)
        confd_fatal("cdb_subscribe_done() failed");
    printf("Subscription point = %d\n", spoint);

    /*
     * Read initial config
     */
    if ((status = read_conf(&addr)) != CONFD_OK) {
        fprintf(stderr, "Terminate: read_conf %d\n", status);
        exit(0);
    }
    rename("dhcpd.conf.tmp", "dhcpd.conf");
    /* This is the place to HUP the daemon */

    while (1) {
        static int poll_fail_counter=0;
        struct pollfd set[1];

        set[0].fd = subsock;
        set[0].events = POLLIN;
        set[0].revents = 0;

        if (poll(&set[0], 1, -1) < 0) {
            perror("Poll failed:");
            if(++poll_fail_counter < 10)
                continue;
            fprintf(stderr, "Too many poll failures, terminating\n");
            exit(1);
        }

        poll_fail_counter = 0;
        if (set[0].revents & POLLIN) {
            int sub_points[1];
            int reslen;

            if ((status = cdb_read_subscription_socket(subsock,
                                                       &sub_points[0],
                                                       &reslen)) != CONFD_OK) {
                fprintf(stderr, "terminate sub_read: %d\n", status);
                exit(1);
            }
            /* printf("hanging\n"); */
            /* sleep(600); */
            if (reslen > 0) {
                if ((status = read_conf(&addr)) != CONFD_OK) {
                    fprintf(stderr, "Terminate: read_conf %d\n", status);
                    exit(1);
                }
            }
            fprintf(stderr, "Read new config, updating dhcpd config \n");
            rename("dhcpd.conf.tmp", "dhcpd.conf");
            /* this is the place to HUP the daemon */

            if ((status = cdb_sync_subscription_socket(subsock,
                                                       CDB_DONE_PRIORITY))
                != CONFD_OK) {
                fprintf(stderr, "failed to sync subscription: %d\n", status);
                exit(1);
            }
        }
    }
}

/********************************************************************/
