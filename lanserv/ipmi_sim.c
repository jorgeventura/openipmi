/*
 * ipmi_sim.c
 *
 * MontaVista IPMI code for creating a LAN interface, emulated system
 * interfaces, and a full BMC emulator.
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2003,2004,2005 MontaVista Software Inc.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * Lesser General Public License (GPL) Version 2 or the modified BSD
 * license below.  The following disclamer applies to both licenses:
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * GNU Lesser General Public Licence
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Modified BSD Licence
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *   3. The name of the author may not be used to endorse or promote
 *      products derived from this software without specific prior
 *      written permission.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdarg.h>
#include <popt.h> /* Option parsing made easy */
#include <sys/ioctl.h>
#include <termios.h>

#include <OpenIPMI/ipmi_log.h>
#include <OpenIPMI/ipmi_err.h>
#include <OpenIPMI/ipmi_msgbits.h>
#include <OpenIPMI/ipmi_mc.h>
#include <OpenIPMI/os_handler.h>
#include <OpenIPMI/ipmi_posix.h>
#include <OpenIPMI/serv.h>
#include <OpenIPMI/lanserv.h>
#include <OpenIPMI/serserv.h>

#include "emu.h"

#define MAX_ADDR 4

static char *config_file = "/etc/ipmi/lan.conf";
static char *command_string = NULL;
static char *command_file = NULL;
static int debug = 0;
static int nostdio = 0;


typedef struct misc_data misc_data_t;

typedef struct console_info_s
{
    char buffer[1024];
    unsigned int pos;
    int telnet;
    int echo;
    int shutdown_on_close;
    misc_data_t *data;
    int outfd;
    os_hnd_fd_id_t *conid;
    unsigned int tn_pos;
    unsigned char tn_buf[4];
    emu_out_t out;
    struct console_info_s *prev;
    struct console_info_s *next;
} console_info_t;

struct misc_data
{
    bmc_data_t *bmc;
    emu_data_t *emu;
    os_handler_t *os_hnd;
    os_handler_waiter_factory_t *waiter_factory;
    os_hnd_timer_id_t *timer;
    console_info_t *consoles;
};

static void *
balloc(bmc_data_t *bmc, int size)
{
    return malloc(size);
}

static void
bfree(bmc_data_t *bmc, void *data)
{
    return free(data);
}

typedef struct sim_addr_s
{
    struct sockaddr_storage addr;
    socklen_t       addr_len;
    int             xmit_fd;
} sim_addr_t;

static int
smi_send(channel_t *chan, msg_t *msg)
{
    misc_data_t      *data = chan->oem.user_data;
    unsigned char    msgd[36];
    unsigned int     msgd_len = sizeof(msgd);

    ipmi_emu_handle_msg(data->emu, msg, msgd, &msgd_len);

    ipmi_handle_smi_rsp(chan, msg, msgd, msgd_len);
    return 0;
}

static int
gen_rand(lanserv_data_t *lan, void *data, int len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    int rv;

    if (fd == -1)
	return errno;

    while (len > 0) {
	rv = read(fd, data, len);
	if (rv < 0) {
	    rv = errno;
	    goto out;
	}
	len -= rv;
    }

    rv = 0;

 out:
    close(fd);
    return rv;
}

static void
lan_send(lanserv_data_t *lan,
	 struct iovec *data, int vecs,
	 void *addr, int addr_len)
{
    struct msghdr msg;
    sim_addr_t    *l = addr;
    int           rv;

    /* When we send messages to ourself, we set the address to NULL so
       it won't be used. */
    if (!l)
	return;

    msg.msg_name = &(l->addr);
    msg.msg_namelen = l->addr_len;
    msg.msg_iov = data;
    msg.msg_iovlen = vecs;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    rv = sendmsg(l->xmit_fd, &msg, 0);
    if (rv) {
	/* FIXME - log an error. */
    }
}

static void
lan_data_ready(int lan_fd, void *cb_data, os_hnd_fd_id_t *id)
{
    lanserv_data_t    *lan = cb_data;
    int           len;
    sim_addr_t    l;
    unsigned char msgd[256];

    l.addr_len = sizeof(l.addr);
    len = recvfrom(lan_fd, msgd, sizeof(msgd), 0,
		   (struct sockaddr *) &(l.addr), &(l.addr_len));
    if (len < 0) {
	if (errno != EINTR) {
	    perror("Error receiving message");
	    exit(1);
	}
	goto out;
    }
    l.xmit_fd = lan_fd;

    if (lan->bmcinfo->debug & DEBUG_RAW_MSG) {
	debug_log_raw_msg(lan->bmcinfo, (void *) &l.addr, l.addr_len,
			  "Raw LAN receive from:");
	debug_log_raw_msg(lan->bmcinfo, msgd, len,
			  " Receive message:");
    }

    if (len < 4)
	goto out;

    if (msgd[0] != 6)
	goto out; /* Invalid version */

    /* Check the message class. */
    switch (msgd[3]) {
	case 6:
	    handle_asf(lan, msgd, len, &l, sizeof(l));
	    break;

	case 7:
	    ipmi_handle_lan_msg(lan, msgd, len, &l, sizeof(l));
	    break;
    }
 out:
    return;
}

static int
open_lan_fd(struct sockaddr *addr, socklen_t addr_len)
{
    int fd;
    int rv;

    fd = socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == -1) {
	perror("Unable to create socket");
	exit(1);
    }

    rv = bind(fd, addr, addr_len);
    if (rv == -1) {
	fprintf(stderr, "Unable to bind to LAN port: %s\n",
		strerror(errno));
	exit(1);
    }

    return fd;
}

int
lan_channel_init(misc_data_t *data, channel_t *chan)
{
    lanserv_data_t *lan = chan->chan_info;
    int err;
    unsigned int i;
    int lan_fd;
    os_hnd_fd_id_t *fd_id;

    lan->user_info = data;
    lan->send_out = lan_send;
    lan->gen_rand = gen_rand;

    err = ipmi_lan_init(lan);
    if (err) {
	fprintf(stderr, "Unable to init lan: 0x%x\n", err);
	exit(1);
    }

    if (lan->guid) {
	lmc_data_t *bmc = ipmi_emu_get_bmc_mc(data->emu);
	if (bmc)
	    ipmi_emu_set_mc_guid(bmc, lan->guid, 0);
    }

    if (lan->num_lan_addrs == 0) {
#ifdef AF_INET6
	struct sockaddr_in6 *ipaddr = (void *) &lan->lan_addrs[0].addr;
	memcpy(ipaddr, &in6addr_any, sizeof(*ipaddr));
#else
	struct sockaddr_in *ipaddr = (void *) &lan->lan_addrs[0].addr;
	ipaddr->sin_family = AF_INET;
	ipaddr->sin_port = htons(623);
	ipaddr->sin_addr.s_addr = INADDR_ANY;
#endif
	lan->lan_addrs[0].addr_len = sizeof(*ipaddr);
	lan->num_lan_addrs++;
    }

    for (i=0; i<lan->num_lan_addrs; i++) {
	unsigned char addr_data[6];

	if (lan->lan_addrs[i].addr_len == 0)
	    break;

	lan_fd = open_lan_fd(&lan->lan_addrs[i].addr.s_ipsock.s_addr,
			     lan->lan_addrs[i].addr_len);
	if (lan_fd == -1) {
	    fprintf(stderr, "Unable to open LAN address %d\n", i+1);
	    exit(1);
	}

	memcpy(addr_data,
	       &lan->lan_addrs[i].addr.s_ipsock.s_addr4.sin_addr.s_addr,
	       4);
	memcpy(addr_data+4,
	       &lan->lan_addrs[i].addr.s_ipsock.s_addr4.sin_port, 2);
	ipmi_emu_set_addr(data->emu, i, 0, addr_data, 6);

	err = data->os_hnd->add_fd_to_wait_for(data->os_hnd, lan_fd,
					      lan_data_ready, lan,
					      NULL, &fd_id);
	if (err) {
	    fprintf(stderr, "Unable to add socket wait: 0x%x\n", err);
	    exit(1);
	}
    }

    return err;
}

static void
ser_send(serserv_data_t *ser, unsigned char *data, unsigned int data_len)
{
    int rv;

    if (ser->con_fd == -1)
	/* Not connected */
	return;

    rv = write(ser->con_fd, data, data_len);
    if (rv) {
	/* FIXME - log an error. */
    }
}

static void
ser_data_ready(int fd, void *cb_data, os_hnd_fd_id_t *id)
{
    serserv_data_t *ser = cb_data;
    int           len;
    unsigned char msgd[256];

    len = read(fd, msgd, sizeof(msgd));
    if (len <= 0) {
	if ((len < 0) && (errno == EINTR))
	    return;

	if (ser->codec->disconnected)
	    ser->codec->disconnected(ser);
	ser->os_hnd->remove_fd_to_wait_for(ser->os_hnd, id);
	close(fd);
	ser->con_fd = -1;
	return;
    }

    serserv_handle_data(ser, msgd, len);
}

static void
ser_bind_ready(int fd, void *cb_data, os_hnd_fd_id_t *id)
{
    serserv_data_t *ser = cb_data;
    int rv;
    int err;
    os_hnd_fd_id_t *fd_id;
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int val = 1;

    rv = accept(fd, (struct sockaddr *) &addr, &addr_len);
    if (rv < 0) {
	perror("Error from accept");
	exit(1);
    }

    if (ser->con_fd >= 0) {
	close(rv);
	return;
    }

    setsockopt(rv, IPPROTO_TCP, TCP_NODELAY, (char *)&val, sizeof(val));
    setsockopt(rv, SOL_SOCKET, SO_KEEPALIVE, (char *)&val, sizeof(val));

    ser->con_fd = rv;

    err = ser->os_hnd->add_fd_to_wait_for(ser->os_hnd, ser->con_fd,
					  ser_data_ready, ser,
					  NULL, &fd_id);
    if (err) {
	fprintf(stderr, "Unable to add serial socket wait: 0x%x\n", err);
	ser->con_fd = -1;
	close(rv);
    } else {
	if (ser->codec->connected)
	    ser->codec->connected(ser);
    }
}

int
ser_channel_init(misc_data_t *data, channel_t *chan)
{
    serserv_data_t *ser = chan->chan_info;
    int err;
    int fd;
    struct sockaddr *addr = &ser->addr.addr.s_ipsock.s_addr;
    os_hnd_fd_id_t *fd_id;
    int val;

    ser->os_hnd = data->os_hnd;
    ser->user_info = data;
    ser->send_out = ser_send;

    err = serserv_init(ser);
    if (err) {
	fprintf(stderr, "Unable to init serial: 0x%x\n", err);
	exit(1);
    }

    fd = socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1) {
	perror("Unable to create socket");
	exit(1);
    }

    if (ser->do_connect) {
	err = connect(fd, addr, ser->addr.addr_len);
	if (err == -1) {
	    fprintf(stderr, "Unable to connect to serial TCP port: %s\n",
		    strerror(errno));
	    exit(1);
	}
	ser->con_fd = fd;
	ser->bind_fd = -1;

	err = data->os_hnd->add_fd_to_wait_for(data->os_hnd, ser->con_fd,
					       ser_data_ready, ser,
					       NULL, &fd_id);
	if (err) {
	    fprintf(stderr, "Unable to add serial socket wait: 0x%x\n", err);
	    exit(1);
	}
    } else {
	err = bind(fd, addr, ser->addr.addr_len);
	if (err == -1) {
	    fprintf(stderr, "Unable to bind to serial TCP port: %s\n",
		    strerror(errno));
	    exit(1);
	}
	ser->bind_fd = fd;
	ser->con_fd = -1;

	err = listen(fd, 1);
	if (err == -1) {
	    fprintf(stderr, "Unable to listen to serial TCP port: %s\n",
		    strerror(errno));
	    exit(1);
	}

	val = 1;
	err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&val,
			 sizeof(val));
	if (err == -1) {
	    fprintf(stderr, "Unable to set SO_REUSEADDR on socket: %s\n",
		    strerror(errno));
	    exit(1);
	}
	

	err = data->os_hnd->add_fd_to_wait_for(data->os_hnd, ser->bind_fd,
					       ser_bind_ready, ser,
					       NULL, &fd_id);
	if (err) {
	    fprintf(stderr, "Unable to add serial socket wait: 0x%x\n", err);
	    exit(1);
	}
    }

    return err;
}

static void
isim_log(bmc_data_t *bmc, int logtype, msg_t *msg, char *format, va_list ap,
	 int len)
{
    if (msg) {
	char *str, dummy;
	int pos;
	unsigned int i;

#define mformat " channel=%d netfn=0x%x cmd=0x%x rs_addr=0x%x rs_lun=0x%x" \
	    " rq_addr=0x%x\n rq_lun=0x%x rq_seq=0x%x\n"

	len += snprintf(&dummy, 1, mformat, msg->channel, msg->netfn,
			msg->cmd, msg->rs_addr, msg->rs_lun, msg->rq_addr,
			msg->rq_lun, msg->rq_seq);
	len += 3 * msg->len + 3;
	str = malloc(len);
	if (!str)
	    goto print_no_msg;
	pos = vsprintf(str, format, ap);
	str[pos++] = '\n';
	pos += sprintf(str + pos, mformat, msg->channel, msg->netfn, msg->cmd,
		       msg->rs_addr, msg->rs_lun, msg->rq_addr, msg->rq_lun,
		       msg->rq_seq);
#undef mformat
	if (!nostdio) {
	    printf("Msglen = %d\n", msg->len);
	    for (i = 0; i < msg->len; i++)
		pos += sprintf(str + pos, " %2.2x", msg->data[i]);
	
	    printf("%s\n", str);
	}
#if HAVE_SYSLOG
	if (logtype == DEBUG)
	    syslog(LOG_DEBUG, "%s", str);
	else
	    syslog(LOG_NOTICE, "%s", str);
#endif
	free(str);
	return;
    }

 print_no_msg:
    if (!nostdio) {
	vprintf(format, ap);
	printf("\n");
    }
#if HAVE_SYSLOG
    if (logtype == DEBUG)
	vsyslog(LOG_DEBUG, format, ap);
    else
	vsyslog(LOG_NOTICE, format, ap);
#endif
}

static void
sim_log(bmc_data_t *bmc, int logtype, msg_t *msg, char *format, ...)
{
    va_list ap;
    char dummy;
    int len;

    va_start(ap, format);
    len = vsnprintf(&dummy, 1, format, ap);
    va_end(ap);
    va_start(ap, format);
    isim_log(bmc, logtype, msg, format, ap, len);
    va_end(ap);
}

static void
sim_chan_log(channel_t *chan, int logtype, msg_t *msg, char *format, ...)
{
    va_list ap;
    char dummy;
    int len;

    va_start(ap, format);
    len = vsnprintf(&dummy, 1, format, ap);
    va_end(ap);
    va_start(ap, format);
    isim_log(NULL, logtype, msg, format, ap, len);
    va_end(ap);
}

static struct poptOption poptOpts[]=
{
    {
	"config-file",
	'c',
	POPT_ARG_STRING,
	&config_file,
	'c',
	"configuration file",
	""
    },
    {
	"command-string",
	'x',
	POPT_ARG_STRING,
	&command_string,
	'x',
	"command string",
	""
    },
    {
	"command-file",
	'f',
	POPT_ARG_STRING,
	&command_file,
	'f',
	"command file",
	""
    },
    {
	"debug",
	'd',
	POPT_ARG_NONE,
	NULL,
	'd',
	"debug",
	""
    },
    {
	"nostdio",
	'n',
	POPT_ARG_NONE,
	NULL,
	'n',
	"nostdio",
	""
    },
    POPT_AUTOHELP
    {
	NULL,
	0,
	0,
	NULL,
	0		 
    }	
};

static void
write_config(bmc_data_t *bmc)
{
//    misc_data_t *info = lan->user_info;
}

static void
emu_printf(emu_out_t *out, char *format, ...)
{
    console_info_t *info = out->data;
    va_list ap;
    int rv;
    char buffer[500];
    int start = 0;
    int pos;

    va_start(ap, format);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    for (pos = 0; buffer[pos]; pos++) {
	if (buffer[pos] == '\n') {
	    rv = write(info->outfd, buffer + start, pos - start + 1);
	    rv = write(info->outfd, "\r", 1);
	    start = pos + 1;
	}
    }
    if (pos != start)
	rv = write(info->outfd, buffer + start, pos - start);
}

static void
dummy_printf(emu_out_t *out, char *format, ...)
{
}

#define TN_IAC  255
#define TN_WILL	251
#define TN_WONT	252
#define TN_DO	253
#define TN_DONT	254
#define TN_OPT_SUPPRESS_GO_AHEAD	3
#define TN_OPT_ECHO			1

static unsigned char
handle_telnet(console_info_t *info, unsigned char c)
{
    int err;

    info->tn_buf[info->tn_pos++] = c;
    if ((info->tn_pos == 2) && (info->tn_buf[1] == TN_IAC))
	/* Double IAC, just send it on. */
	return TN_IAC;
    if ((info->tn_pos == 2) && (info->tn_buf[1] < 250))
	/* Ignore 1-byte commands */
	goto cmd_done;
    if ((info->tn_pos == 3) && (info->tn_buf[1] != 250)) {
	/* Two byte commands */
	switch (info->tn_buf[1]) {
	case TN_WILL:
	    goto send_dont;
	case TN_WONT:
	    break;
	case TN_DO:
	    if ((info->tn_buf[2] == TN_OPT_ECHO)
		|| (info->tn_buf[2] == TN_OPT_SUPPRESS_GO_AHEAD))
		break;
	    goto send_wont;
	}
	goto cmd_done;
    }

    if (info->tn_pos < 4)
	return 0;

    /*
     * We are in a suboption, which we ignore.  Just look for
     * IAC 240 for the end.  Use tn_buf[2] to track the last
     * character we got.
     */
    if ((info->tn_buf[2] == TN_IAC) && (info->tn_buf[3] == 240))
	goto cmd_done;
    info->tn_buf[2] = info->tn_buf[3];
    info->tn_pos--;

 send_wont:
    info->tn_buf[1] = TN_WONT;
    err = write(info->outfd, info->tn_buf, 3);
    goto cmd_done;

 send_dont:
    info->tn_buf[1] = TN_DONT;
    err = write(info->outfd, info->tn_buf, 3);
    goto cmd_done;

 cmd_done:
    info->tn_pos = 0;
    return 0;
}

static int
handle_user_char(console_info_t *info, unsigned char c)
{
    int rv;

    if (info->tn_pos)
	c = handle_telnet(info, c);

    if (!c)
	return 0;

    switch(c) {
    case TN_IAC:
	if (info->telnet) {
	    info->tn_buf[0] = c;
	    info->tn_pos = 1;
	} else
	    goto handle_char;
	break;

    case 8:
    case 0x7f:
	if (info->pos > 0) {
	    info->pos--;
	    if (info->echo)
		rv = write(info->outfd, "\b \b", 3);
	}
	break;

    case 4:
	if (info->pos == 0) {
	    if (info->echo)
		rv = write(info->outfd, "\n", 1);
	    return 1;
	}
	break;

    case 10:
    case 13:
	if (info->echo) {
	    rv = write(info->outfd, "\n", 1);
	    if (info->telnet)
		rv = write(info->outfd, "\r", 1);
	}
	info->buffer[info->pos] = '\0';
	if (strcmp(info->buffer, "noecho") == 0) {
	    info->echo = 0;
	} else {
	    ipmi_emu_cmd(&info->out, info->data->emu, info->buffer);
	}
	if (info->echo)
	    rv = write(info->outfd, "> ", 2);
	info->pos = 0;
	break;

    handle_char:
    default:
	if (info->pos >= sizeof(info->buffer)-1) {
	    char *msg = "\nCommand is too long, max of %d characters\n";
	    rv = write(info->outfd, msg, strlen(msg));
	} else {
	    info->buffer[info->pos] = c;
	    info->pos++;
	    if (info->echo)
		rv = write(info->outfd, &c, 1);
	}
    }

    return 0;
}

static void
user_data_ready(int fd, void *cb_data, os_hnd_fd_id_t *id)
{
    console_info_t *info = cb_data;
    unsigned char  rc[50];
    unsigned char  *c = rc;
    int         count;

    count = read(fd, rc, sizeof(rc));
    if (count == 0)
	goto closeit;
    while (count > 0) {
	if (handle_user_char(info, *c))
	    goto closeit;
	c++;
	count--;
    }
    return;

 closeit:
    if (info->shutdown_on_close) {
	ipmi_emu_shutdown(info->data->emu);
	return;
    }

    info->data->os_hnd->remove_fd_to_wait_for(info->data->os_hnd, info->conid);
    close(fd);
    if (info->prev)
	info->prev->next = info->next;
    else
	info->data->consoles = info->next;
    if (info->next)
	info->next->prev = info->prev;
    free(info);
}

static void
console_bind_ready(int fd, void *cb_data, os_hnd_fd_id_t *id)
{
    misc_data_t *misc = cb_data;
    console_info_t *newcon;
    int rv;
    int err;
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int val = 1;
    static unsigned char telnet_init_seq[] = {
	TN_IAC, TN_WILL, TN_OPT_SUPPRESS_GO_AHEAD,
	TN_IAC, TN_WILL, TN_OPT_ECHO,
	TN_IAC, TN_DONT, TN_OPT_ECHO,
    };

    rv = accept(fd, (struct sockaddr *) &addr, &addr_len);
    if (rv < 0) {
	perror("Error from accept");
	exit(1);
    }

    newcon = malloc(sizeof(*newcon));
    if (!newcon) {
	char *msg = "Out of memory\n";
	err = write(rv, msg, strlen(msg));
	close(rv);
	return;
    }

    newcon->data = misc;
    newcon->outfd = rv;
    newcon->pos = 0;
    newcon->echo = 1;
    newcon->shutdown_on_close = 0;
    newcon->telnet = 1;
    newcon->tn_pos = 0;
    newcon->out.printf = emu_printf;
    newcon->out.data = newcon;

    setsockopt(rv, IPPROTO_TCP, TCP_NODELAY, (char *)&val, sizeof(val));
    setsockopt(rv, SOL_SOCKET, SO_KEEPALIVE, (char *)&val, sizeof(val));

    err = misc->os_hnd->add_fd_to_wait_for(misc->os_hnd, rv,
					   user_data_ready, newcon,
					   NULL, &newcon->conid);
    if (err) {
	char *msg = "Unable to add socket wait\n";
	err = write(rv, msg, strlen(msg));
	close(rv);
	free(newcon);
    }

    newcon->next = misc->consoles;
    if (newcon->next)
	newcon->next->prev = newcon;
    newcon->prev = NULL;
    misc->consoles = newcon;

    err = write(rv, telnet_init_seq, sizeof(telnet_init_seq));
    err = write(rv, "> ", 2);
}

struct termios old_termios;
int old_flags;

static void
init_term(void)
{
    struct termios new_termios;

    tcgetattr(0, &old_termios);
    new_termios = old_termios;
    new_termios.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
			     |INLCR|IGNCR|ICRNL|IXON);
    new_termios.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
    tcsetattr(0, TCSADRAIN, &new_termios);
}

void
ipmi_emu_shutdown(emu_data_t *emu)
{
    misc_data_t *data = ipmi_emu_get_user_data(emu);
    console_info_t *con;
    
    if (data->bmc->console_fd != -1)
	close(data->bmc->console_fd);
    con = data->consoles;
    while (con) {
	data->os_hnd->remove_fd_to_wait_for(data->os_hnd, con->conid);
	close(con->outfd);
	con = con->next;
    }
	
    if (!nostdio)
	tcsetattr(0, TCSADRAIN, &old_termios);
    fcntl(0, F_SETFL, old_flags);
    tcdrain(0);
    exit(0);
}

/* Sleep and don't take any user input. */
static void
sleeper(emu_data_t *emu, struct timeval *time)
{
    misc_data_t    *data = ipmi_emu_get_user_data(emu);
    os_handler_waiter_t *waiter;

    waiter = os_handler_alloc_waiter(data->waiter_factory);
    if (!waiter) {
	fprintf(stderr, "Unable to allocate waiter\n");
	exit(1);
    }

    os_handler_waiter_wait(waiter, time);
    os_handler_waiter_release(waiter);
}

struct ipmi_timer_s
{
    os_hnd_timer_id_t *id;
    misc_data_t *data;
    void (*cb)(void *cb_data);
    void *cb_data;
};

static int
ipmi_alloc_timer(bmc_data_t *bmc, void (*cb)(void *cb_data),
		 void *cb_data, ipmi_timer_t **rtimer)
{
    misc_data_t *data = bmc->info;
    ipmi_timer_t *timer;
    int err;

    timer = malloc(sizeof(ipmi_timer_t));
    if (!timer)
	return ENOMEM;

    timer->cb = cb;
    timer->cb_data = cb_data;
    timer->data = data;
    err = data->os_hnd->alloc_timer(data->os_hnd, &timer->id);
    if (err) {
	free(timer);
	return err;
    }

    *rtimer = timer;
    return 0;
}

static void
timer_cb(void *cb_data, os_hnd_timer_id_t *id)
{
    ipmi_timer_t *timer = cb_data;

    timer->cb(timer->cb_data);
}

static int
ipmi_start_timer(ipmi_timer_t *timer, struct timeval *timeout)
{
    return timer->data->os_hnd->start_timer(timer->data->os_hnd, timer->id,
					    timeout, timer_cb, timer);
}

static int
ipmi_stop_timer(ipmi_timer_t *timer)
{
    return timer->data->os_hnd->stop_timer(timer->data->os_hnd, timer->id);
}

static void
ipmi_free_timer(ipmi_timer_t *timer)
{
    timer->data->os_hnd->free_timer(timer->data->os_hnd, timer->id);
}

static void
tick(void *cb_data, os_hnd_timer_id_t *id)
{
    misc_data_t *data = cb_data;
    struct timeval tv;
    int err;
    unsigned int i;

    for (i = 0; i < IPMI_MAX_CHANNELS; i++) {
	channel_t *chan = data->bmc->channels[i];

	if (chan && (chan->medium_type == IPMI_CHANNEL_MEDIUM_8023_LAN)) {
	    ipmi_lan_tick(chan->chan_info, 1);
	}
    }
    ipmi_emu_tick(data->emu, 1);

    tv.tv_sec = 1;
    tv.tv_usec = 0;
    err = data->os_hnd->start_timer(data->os_hnd, data->timer, &tv, tick, data);
    if (err) {
	fprintf(stderr, "Unable to start timer: 0x%x\n", err);
	exit(1);
    }
}

static void *
ialloc(channel_t *chan, int size)
{
    return malloc(size);
}

static void
ifree(channel_t *chan, void *data)
{
    return free(data);
}

int
main(int argc, const char *argv[])
{
    bmc_data_t  bmcinfo;
    misc_data_t data;
    int err;
    int i;
    poptContext poptCtx;
    struct timeval tv;
    console_info_t stdio_console;

    poptCtx = poptGetContext(argv[0], argc, argv, poptOpts, 0);
    while ((i = poptGetNextOpt(poptCtx)) >= 0) {
	switch (i) {
	    case 'd':
		debug++;
		break;
	    case 'n':
		nostdio = 1;
		break;
	}
    }
    poptFreeContext(poptCtx);

    data.os_hnd = ipmi_posix_setup_os_handler();
    if (!data.os_hnd) {
	fprintf(stderr, "Unable to allocate OS handler\n");
	exit(1);
    }

    err = os_handler_alloc_waiter_factory(data.os_hnd, 0, 0,
					  &data.waiter_factory);
    if (err) {
	fprintf(stderr, "Unable to allocate waiter factory: 0x%x\n", err);
	exit(1);
    }

    err = data.os_hnd->alloc_timer(data.os_hnd, &data.timer);
    if (err) {
	fprintf(stderr, "Unable to allocate timer: 0x%x\n", err);
	exit(1);
    }

    bmcinfo_init(&bmcinfo);
    bmcinfo.info = &data;
    bmcinfo.alloc = balloc;
    bmcinfo.free = bfree;
    bmcinfo.alloc_timer = ipmi_alloc_timer;
    bmcinfo.start_timer = ipmi_start_timer;
    bmcinfo.stop_timer = ipmi_stop_timer;
    bmcinfo.free_timer = ipmi_free_timer;
    bmcinfo.write_config = write_config;
    bmcinfo.debug = debug;
    bmcinfo.log = sim_log;
    data.bmc = &bmcinfo;

    data.emu = ipmi_emu_alloc(&data, sleeper, &bmcinfo);

    /* Set this up for console I/O, even if we don't use it. */
    stdio_console.data = &data;
    stdio_console.outfd = 1;
    stdio_console.pos = 0;
    stdio_console.echo = 1;
    stdio_console.shutdown_on_close = 1;
    stdio_console.telnet = 0;
    stdio_console.tn_pos = 0;
    if (nostdio) {
	stdio_console.out.printf = dummy_printf;
	stdio_console.out.data = &stdio_console;
    } else {
	stdio_console.out.printf = emu_printf;
	stdio_console.out.data = &stdio_console;
    }

    if (read_config(&bmcinfo, config_file))
	exit(1);

    if (command_string)
	ipmi_emu_cmd(&stdio_console.out, data.emu, command_string);

    if (command_file)
	read_command_file(&stdio_console.out, data.emu, command_file);

    for (i = 0; i < IPMI_MAX_CHANNELS; i++) {
	channel_t *chan = bmcinfo.channels[i];

	if (!chan)
	    continue;

	chan->smi_send = smi_send;
	chan->oem.user_data = &data;
	chan->alloc = ialloc;
	chan->free = ifree;
	chan->log = sim_chan_log;

	if (chan->medium_type == IPMI_CHANNEL_MEDIUM_8023_LAN)
	    err = lan_channel_init(&data, bmcinfo.channels[i]);
	else if (chan->medium_type == IPMI_CHANNEL_MEDIUM_RS232)
	    err = ser_channel_init(&data, bmcinfo.channels[i]);
	else 
	    chan_init(chan);
    }

    bmcinfo.console_fd = -1;
    if (bmcinfo.console_addr_len) {
	int nfd;
	int val;
	os_hnd_fd_id_t *conid;

	nfd = socket(bmcinfo.console_addr.s_ipsock.s_addr.sa_family,
		     SOCK_STREAM, IPPROTO_TCP);
	if (nfd == -1) {
	    perror("Console socket open");
	    exit(1);
	}
	err = bind(nfd, (struct sockaddr *) &bmcinfo.console_addr,
		   bmcinfo.console_addr_len);
	if (err) {
	    perror("bind to console socket");
	    exit(1);
	}
	err = listen(nfd, 1);
	if (err == -1) {
	    perror("listen to console socket");
	    exit(1);
	}
	val = 1;
	err = setsockopt(nfd, SOL_SOCKET, SO_REUSEADDR,
			 (char *)&val, sizeof(val));
	if (err) {
	    perror("console setsockopt reuseaddr");
	    exit(1);
	}
	bmcinfo.console_fd = nfd;

	err = data.os_hnd->add_fd_to_wait_for(data.os_hnd, nfd,
					      console_bind_ready, &data,
					      NULL, &conid);
	if (err) {
	    fprintf(stderr, "Unable to add console wait: 0x%x\n", err);
	    exit(1);
	}
    }

    if (!nostdio) {
	init_term();

	err = write(1, "> ", 2);
	err = data.os_hnd->add_fd_to_wait_for(data.os_hnd, 0,
					      user_data_ready, &stdio_console,
					      NULL, &stdio_console.conid);
	if (err) {
	    fprintf(stderr, "Unable to add input wait: 0x%x\n", err);
	    exit(1);
	}
    }

    tv.tv_sec = 1;
    tv.tv_usec = 0;
    err = data.os_hnd->start_timer(data.os_hnd, data.timer, &tv, tick, &data);
    if (err) {
	fprintf(stderr, "Unable to start timer: 0x%x\n", err);
	exit(1);
    }

    data.os_hnd->operation_loop(data.os_hnd);
    return 0;
}
