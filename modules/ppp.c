/*
 * ppp.c - STREAMS multiplexing pseudo-device driver for PPP.
 *
 * Copyright (c) 1994 The Australian National University.
 * All rights reserved.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation is hereby granted, provided that the above copyright
 * notice appears in all copies.  This software is provided without any
 * warranty, express or implied. The Australian National University
 * makes no representations about the suitability of this software for
 * any purpose.
 *
 * IN NO EVENT SHALL THE AUSTRALIAN NATIONAL UNIVERSITY BE LIABLE TO ANY
 * PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF
 * THE AUSTRALIAN NATIONAL UNIVERSITY HAS BEEN ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * THE AUSTRALIAN NATIONAL UNIVERSITY SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE AUSTRALIAN NATIONAL UNIVERSITY HAS NO
 * OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS,
 * OR MODIFICATIONS.
 *
 * $Id: ppp.c,v 1.6 1996/06/26 00:53:38 paulus Exp $
 */

/*
 * This file is used under Solaris 2, SVR4, SunOS 4, and Digital UNIX.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/errno.h>
#ifdef __osf__
#include <sys/ioctl.h>
#include <sys/cmn_err.h>
#define queclass(mp)	((mp)->b_band & QPCTL)
#else
#include <sys/ioccom.h>
#endif
#include <sys/time.h>
#ifdef SVR4
#include <sys/cmn_err.h>
#include <sys/conf.h>
#include <sys/dlpi.h>
#include <sys/ddi.h>
#ifdef SOL2
#include <sys/kstat.h>
#include <sys/sunddi.h>
#else
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <netinet/in.h>
#endif /* SOL2 */
#else /* not SVR4 */
#include <sys/user.h>
#endif /* SVR4 */
#include <net/ppp_defs.h>
#include <net/pppio.h>
#include "ppp_mod.h"

#ifdef __STDC__
#define __P(x)	x
#else
#define __P(x)	()
#endif

/*
 * The IP module may use this SAP value for IP packets.
 */
#ifndef ETHERTYPE_IP
#define ETHERTYPE_IP	0x800
#endif

#ifndef PPP_MAXMTU
#define PPP_MAXMTU	65535
#endif

extern time_t time;

/*
 * Private information; one per upper stream.
 */
typedef struct upperstr {
    minor_t mn;			/* minor device number */
    struct upperstr *nextmn;	/* next minor device */
    queue_t *q;			/* read q associated with this upper stream */
    int flags;			/* flag bits, see below */
    int state;			/* current DLPI state */
    int sap;			/* service access point */
    int req_sap;		/* which SAP the DLPI client requested */
    struct upperstr *ppa;	/* control stream for our ppa */
    struct upperstr *next;	/* next stream for this ppa */
    uint ioc_id;		/* last ioctl ID for this stream */
    enum NPmode npmode;		/* what to do with packets on this SAP */
    /*
     * There is exactly one control stream for each PPA.
     * The following fields are only used for control streams.
     */
    int ppa_id;
    queue_t *lowerq;		/* write queue attached below this PPA */
    struct upperstr *nextppa;	/* next control stream */
    int mru;
    int mtu;
    struct pppstat stats;	/* statistics */
    time_t last_sent;		/* time last NP packet sent */
    time_t last_recv;		/* time last NP packet rcvd */
#ifdef SOL2
    kstat_t *kstats;		/* stats for netstat */
#endif /* SOL2 */
#ifdef LACHTCP
    int ifflags;
    char ifname[IFNAMSIZ];
    struct ifstats ifstats;
#endif /* LACHTCP */
} upperstr_t;

/* Values for flags */
#define US_PRIV		1	/* stream was opened by superuser */
#define US_CONTROL	2	/* stream is a control stream */
#define US_BLOCKED	4	/* flow ctrl has blocked lower write stream */
#define US_LASTMOD	8	/* no PPP modules below us */
#define US_DBGLOG	0x10	/* log various occurrences */

static upperstr_t *minor_devs = NULL;
static upperstr_t *ppas = NULL;

#ifdef SVR4
static int pppopen __P((queue_t *, dev_t *, int, int, cred_t *));
static int pppclose __P((queue_t *, int, cred_t *));
#else
static int pppopen __P((queue_t *, int, int, int));
static int pppclose __P((queue_t *, int));
#endif /* SVR4 */
static int pppuwput __P((queue_t *, mblk_t *));
static int pppursrv __P((queue_t *));
static int pppuwsrv __P((queue_t *));
static int ppplrput __P((queue_t *, mblk_t *));
static int ppplwput __P((queue_t *, mblk_t *));
static int ppplrsrv __P((queue_t *));
static int ppplwsrv __P((queue_t *));
#ifndef NO_DLPI
static void dlpi_request __P((queue_t *, mblk_t *, upperstr_t *));
static void dlpi_error __P((queue_t *, int, int, int));
static void dlpi_ok __P((queue_t *, int));
#endif
static int send_data __P((mblk_t *, upperstr_t *));
static void new_ppa __P((queue_t *, mblk_t *));
static void attach_ppa __P((queue_t *, mblk_t *));
static void detach_ppa __P((queue_t *, mblk_t *));
static void debug_dump __P((queue_t *, mblk_t *));
static upperstr_t *find_dest __P((upperstr_t *, int));
static int putctl2 __P((queue_t *, int, int, int));
static int putctl4 __P((queue_t *, int, int, int));

#define PPP_ID 0xb1a6
static struct module_info ppp_info = {
    PPP_ID, "ppp", 0, 512, 512, 128
};

static struct qinit pppurint = {
    NULL, pppursrv, pppopen, pppclose, NULL, &ppp_info, NULL
};

static struct qinit pppuwint = {
    pppuwput, pppuwsrv, NULL, NULL, NULL, &ppp_info, NULL
};

static struct qinit ppplrint = {
    ppplrput, ppplrsrv, NULL, NULL, NULL, &ppp_info, NULL
};

static struct qinit ppplwint = {
    ppplwput, ppplwsrv, NULL, NULL, NULL, &ppp_info, NULL
};

#ifdef LACHTCP
extern struct ifstats *ifstats;
int pppdevflag = 0;
#endif

struct streamtab pppinfo = {
    &pppurint, &pppuwint,
    &ppplrint, &ppplwint
};

int ppp_count;

/*
 * How we maintain statistics.
 */
#ifdef SOL2
#define INCR_IPACKETS(ppa)				\
	if (ppa->kstats != 0) {				\
	    KSTAT_NAMED_PTR(ppa->kstats)[0].value.ul++;	\
	}
#define INCR_IERRORS(ppa)				\
	if (ppa->kstats != 0) {				\
	    KSTAT_NAMED_PTR(ppa->kstats)[1].value.ul++;	\
	}
#define INCR_OPACKETS(ppa)				\
	if (ppa->kstats != 0) {				\
	    KSTAT_NAMED_PTR(ppa->kstats)[2].value.ul++;	\
	}
#define INCR_OERRORS(ppa)				\
	if (ppa->kstats != 0) {				\
	    KSTAT_NAMED_PTR(ppa->kstats)[3].value.ul++;	\
	}
#endif

#ifdef LACHTCP
#define INCR_IPACKETS(ppa)	ppa->ifstats.ifs_ipackets++;
#define INCR_IERRORS(ppa)	ppa->ifstats.ifs_ierrors++;
#define INCR_OPACKETS(ppa)	ppa->ifstats.ifs_opackets++;
#define INCR_OERRORS(ppa)	ppa->ifstats.ifs_oerrors++;
#endif

/*
 * STREAMS driver entry points.
 */
static int
#ifdef SVR4
pppopen(q, devp, oflag, sflag, credp)
    queue_t *q;
    dev_t *devp;
    int oflag, sflag;
    cred_t *credp;
#else
pppopen(q, dev, oflag, sflag)
    queue_t *q;
    int dev;			/* really dev_t */
    int oflag, sflag;
#endif
{
    upperstr_t *up;
    upperstr_t **prevp;
    minor_t mn;

    if (q->q_ptr)
	DRV_OPEN_OK(dev);	/* device is already open */

    if (sflag == CLONEOPEN) {
	mn = 0;
	for (prevp = &minor_devs; (up = *prevp) != 0; prevp = &up->nextmn) {
	    if (up->mn != mn)
		break;
	    ++mn;
	}
    } else {
#ifdef SVR4
	mn = getminor(*devp);
#else
	mn = minor(dev);
#endif
	for (prevp = &minor_devs; (up = *prevp) != 0; prevp = &up->nextmn) {
	    if (up->mn >= mn)
		break;
	}
	if (up->mn == mn) {
	    /* this can't happen */
	    q->q_ptr = WR(q)->q_ptr = (caddr_t) up;
	    DRV_OPEN_OK(dev);
	}
    }

    /*
     * Construct a new minor node.
     */
    up = (upperstr_t *) ALLOC_SLEEP(sizeof(upperstr_t));
    bzero((caddr_t) up, sizeof(upperstr_t));
    if (up == 0) {
	DPRINT("pppopen: out of kernel memory\n");
	OPEN_ERROR(ENXIO);
    }
    up->nextmn = *prevp;
    *prevp = up;
    up->mn = mn;
#ifdef SVR4
    *devp = makedevice(getmajor(*devp), mn);
#endif
    up->q = q;
    if (NOTSUSER() == 0)
	up->flags |= US_PRIV;
#ifndef NO_DLPI
    up->state = DL_UNATTACHED;
#endif
#ifdef LACHTCP
    up->ifflags = IFF_UP | IFF_POINTOPOINT;
#endif
    up->sap = -1;
    up->last_sent = up->last_recv = time;
    up->npmode = NPMODE_DROP;
    q->q_ptr = (caddr_t) up;
    WR(q)->q_ptr = (caddr_t) up;
    noenable(WR(q));
    ++ppp_count;

    qprocson(q);
    DRV_OPEN_OK(makedev(major(dev), mn));
}

static int
#ifdef SVR4
pppclose(q, flag, credp)
    queue_t *q;
    int flag;
    cred_t *credp;
#else
pppclose(q, flag)
    queue_t *q;
    int flag;
#endif
{
    upperstr_t *up, **upp;
    upperstr_t *as, *asnext;
    upperstr_t **prevp;

    qprocsoff(q);

    up = (upperstr_t *) q->q_ptr;
    if (up->flags & US_DBGLOG)
	DPRINT2("ppp/%d: close, flags=%x\n", up->mn, up->flags);
    if (up == 0)
	return 0;
    if (up->flags & US_CONTROL) {
#ifdef LACHTCP
	struct ifstats *ifp, *pifp;
#endif
	/*
	 * This stream represents a PPA:
	 * For all streams attached to the PPA, clear their
	 * references to this PPA.
	 * Then remove this PPA from the list of PPAs.
	 */
	for (as = up->next; as != 0; as = asnext) {
	    asnext = as->next;
	    as->next = 0;
	    as->ppa = 0;
	    if (as->flags & US_BLOCKED) {
		as->flags &= ~US_BLOCKED;
		flushq(WR(as->q), FLUSHDATA);
	    }
	}
	for (upp = &ppas; *upp != 0; upp = &(*upp)->nextppa)
	    if (*upp == up) {
		*upp = up->nextppa;
		break;
	    }
#ifdef LACHTCP
	/* Remove the statistics from the active list.  */
	for (ifp = ifstats, pifp = 0; ifp; ifp = ifp->ifs_next) {
	    if (ifp == &up->ifstats) {
		if (pifp)
		    pifp->ifs_next = ifp->ifs_next;
		else
		    ifstats = ifp->ifs_next;
		break;
	    }
	    pifp = ifp;
	}
#endif
    } else {
	/*
	 * If this stream is attached to a PPA,
	 * remove it from the PPA's list.
	 */
	if ((as = up->ppa) != 0) {
	    for (; as->next != 0; as = as->next)
		if (as->next == up) {
		    as->next = up->next;
		    break;
		}
	}
    }

#ifdef SOL2
    if (up->kstats)
	kstat_delete(up->kstats);
#endif

    q->q_ptr = NULL;
    WR(q)->q_ptr = NULL;

    for (prevp = &minor_devs; *prevp != 0; prevp = &(*prevp)->nextmn) {
	if (*prevp == up) {
	    *prevp = up->nextmn;
	    break;
	}
    }
    FREE(up, sizeof(upperstr_t));
    --ppp_count;

    return 0;
}

/*
 * A message from on high.  We do one of three things:
 *	- qreply()
 *	- put the message on the lower write stream
 *	- queue it for our service routine
 */
static int
pppuwput(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    upperstr_t *us, *usnext, *ppa, *os, *nps;
    struct iocblk *iop;
    struct linkblk *lb;
#ifdef LACHTCP
    struct ifreq *ifr;
    int i;
#endif
    queue_t *lq;
    int error, n, sap;
    mblk_t *mq;
    struct ppp_idle *pip;
    int len;

    us = (upperstr_t *) q->q_ptr;
    switch (mp->b_datap->db_type) {
#ifndef NO_DLPI
    case M_PCPROTO:
    case M_PROTO:
	dlpi_request(q, mp, us);
	break;
#endif /* NO_DLPI */

    case M_DATA:
	if (us->flags & US_DBGLOG)
	    DPRINT3("ppp/%d: uwput M_DATA len=%d flags=%x\n",
		    us->mn, msgdsize(mp), us->flags);
	if (us->ppa == 0 || msgdsize(mp) > us->ppa->mtu + PPP_HDRLEN
#ifndef NO_DLPI
	    || (us->flags & US_CONTROL) == 0
#endif /* NO_DLPI */
	    ) {
	    DPRINT1("pppuwput: junk data len=%d\n", msgdsize(mp));
	    freemsg(mp);
	    break;
	}
#ifdef NO_DLPI
	if (!pass_packet(us->ppa, mp, 1)) {
	    freemsg(mp);
	    break;
	}
#endif
	if (!send_data(mp, us))
	    putq(q, mp);
	break;

    case M_IOCTL:
	iop = (struct iocblk *) mp->b_rptr;
	error = EINVAL;
	if (us->flags & US_DBGLOG)
	    DPRINT3("ppp/%d: ioctl %x count=%d\n",
		    us->mn, iop->ioc_cmd, iop->ioc_count);
	switch (iop->ioc_cmd) {
	case I_LINK:
	    if ((us->flags & US_CONTROL) == 0 || us->lowerq != 0)
		break;
	    lb = (struct linkblk *) mp->b_cont->b_rptr;
	    us->lowerq = lq = lb->l_qbot;
	    lq->q_ptr = (caddr_t) us;
	    RD(lq)->q_ptr = (caddr_t) us;
	    noenable(RD(lq));
	    flushq(RD(lq), FLUSHALL);
	    iop->ioc_count = 0;
	    error = 0;
	    us->flags &= ~US_LASTMOD;
	    /* Unblock upper streams which now feed this lower stream. */
	    qenable(lq);
	    /* Send useful information down to the modules which
	       are now linked below us. */
	    putctl2(lq, M_CTL, PPPCTL_UNIT, us->ppa_id);
	    putctl4(lq, M_CTL, PPPCTL_MRU, us->mru);
	    putctl4(lq, M_CTL, PPPCTL_MTU, us->mtu);
	    break;

	case I_UNLINK:
	    lb = (struct linkblk *) mp->b_cont->b_rptr;
#if DEBUG
	    if (us->lowerq != lb->l_qbot)
		DPRINT2("ppp unlink: lowerq=%x qbot=%x\n",
			us->lowerq, lb->l_qbot);
#endif
	    us->lowerq = 0;
	    iop->ioc_count = 0;
	    error = 0;
	    /* Unblock streams which now feed back up the control stream. */
	    qenable(us->q);
	    break;

	case PPPIO_NEWPPA:
	    if (us->flags & US_CONTROL)
		break;
	    if ((us->flags & US_PRIV) == 0) {
		error = EPERM;
		break;
	    }
	    /* Arrange to return an int */
	    if ((mq = mp->b_cont) == 0
		|| mq->b_datap->db_lim - mq->b_rptr < sizeof(int)) {
		mq = allocb(sizeof(int), BPRI_HI);
		if (mq == 0) {
		    error = ENOSR;
		    break;
		}
		if (mp->b_cont != 0)
		    freemsg(mp->b_cont);
		mp->b_cont = mq;
		mq->b_cont = 0;
	    }
	    iop->ioc_count = sizeof(int);
	    mq->b_wptr = mq->b_rptr + sizeof(int);
	    qwriter(q, mp, new_ppa, PERIM_OUTER);
	    error = -1;
	    break;

	case PPPIO_ATTACH:
	    /* like dlpi_attach, for programs which can't write to
	       the stream (like pppstats) */
	    if (iop->ioc_count != sizeof(int) || us->ppa != 0)
		break;
	    n = *(int *)mp->b_cont->b_rptr;
	    for (ppa = ppas; ppa != 0; ppa = ppa->nextppa)
		if (ppa->ppa_id == n)
		    break;
	    if (ppa == 0)
		break;
	    us->ppa = ppa;
	    iop->ioc_count = 0;
	    qwriter(q, mp, attach_ppa, PERIM_OUTER);
	    error = -1;
	    break;

#ifdef NO_DLPI
	case PPPIO_BIND:
	    /* Attach to a given SAP. */
	    if (iop->ioc_count != sizeof(int) || us->ppa == 0)
		break;
	    n = *(int *)mp->b_cont->b_rptr;
	    /* n must be a valid PPP network protocol number. */
	    if (n < 0x21 || n > 0x3fff || (n & 0x101) != 1)
		break;
	    /* check that no other stream is bound to this sap already. */
	    for (os = us->ppa; os != 0; os = os->next)
		if (os->sap == n)
		    break;
	    if (os != 0)
		break;
	    us->sap = n;
	    iop->ioc_count = 0;
	    error = 0;
	    break;
#endif /* NO_DLPI */

	case PPPIO_MRU:
	    if (iop->ioc_count != sizeof(int) || (us->flags & US_CONTROL) == 0)
		break;
	    n = *(int *)mp->b_cont->b_rptr;
	    if (n <= 0 || n > PPP_MAXMTU)
		break;
	    if (n < PPP_MRU)
		n = PPP_MRU;
	    us->mru = n;
	    if (us->lowerq)
		putctl4(us->lowerq, M_CTL, PPPCTL_MRU, n);
	    error = 0;
	    iop->ioc_count = 0;
	    break;

	case PPPIO_MTU:
	    if (iop->ioc_count != sizeof(int) || (us->flags & US_CONTROL) == 0)
		break;
	    n = *(int *)mp->b_cont->b_rptr;
	    if (n <= 0 || n > PPP_MAXMTU)
		break;
	    if (n < PPP_MRU)
		n = PPP_MRU;
	    us->mtu = n;
#ifdef LACHTCP
	    us->ifstats.ifs_mtu = n;
#endif
	    if (us->lowerq)
		putctl4(us->lowerq, M_CTL, PPPCTL_MTU, n);
	    error = 0;
	    iop->ioc_count = 0;
	    break;

	case PPPIO_LASTMOD:
	    us->flags |= US_LASTMOD;
	    error = 0;
	    break;

	case PPPIO_DEBUG:
	    if (iop->ioc_count != sizeof(int))
		break;
	    n = *(int *)mp->b_cont->b_rptr;
	    if (n == PPPDBG_DUMP + PPPDBG_DRIVER) {
		qwriter(q, NULL, debug_dump, PERIM_OUTER);
		iop->ioc_count = 0;
		error = 0;
	    } else if (n == PPPDBG_LOG + PPPDBG_DRIVER) {
		DPRINT1("ppp/%d: debug log enabled\n", us->mn);
		us->flags |= US_DBGLOG;
		iop->ioc_count = 0;
		error = 0;
	    } else {
		if (us->ppa == 0 || us->ppa->lowerq == 0)
		    break;
		putnext(us->ppa->lowerq, mp);
		error = -1;
	    }
	    break;

	case PPPIO_NPMODE:
	    if (iop->ioc_count != 2 * sizeof(int))
		break;
	    if ((us->flags & US_CONTROL) == 0)
		break;
	    sap = ((int *)mp->b_cont->b_rptr)[0];
	    for (nps = us->next; nps != 0; nps = nps->next)
		if (nps->sap == sap)
		    break;
	    if (nps == 0) {
		if (us->flags & US_DBGLOG)
		    DPRINT2("ppp/%d: no stream for sap %x\n", us->mn, sap);
		break;
	    }
	    nps->npmode = (enum NPmode) ((int *)mp->b_cont->b_rptr)[1];
	    if (nps->npmode == NPMODE_DROP || nps->npmode == NPMODE_ERROR)
		flushq(WR(nps->q), FLUSHDATA);
	    else if (nps->npmode == NPMODE_PASS && qsize(WR(nps->q)) > 0
		     && (nps->flags & US_BLOCKED) == 0)
		qenable(WR(nps->q));
	    iop->ioc_count = 0;
	    error = 0;
	    break;

	case PPPIO_GIDLE:
	    if ((ppa = us->ppa) == 0)
		break;
	    mq = allocb(sizeof(struct ppp_idle), BPRI_HI);
	    if (mq == 0) {
		error = ENOSR;
		break;
	    }
	    if (mp->b_cont != 0)
		freemsg(mp->b_cont);
	    mp->b_cont = mq;
	    mq->b_cont = 0;
	    pip = (struct ppp_idle *) mq->b_wptr;
	    pip->xmit_idle = time - ppa->last_sent;
	    pip->recv_idle = time - ppa->last_recv;
	    mq->b_wptr += sizeof(struct ppp_idle);
	    iop->ioc_count = sizeof(struct ppp_idle);
	    error = 0;
	    break;

#ifdef LACHTCP
	case SIOCSIFNAME:
	    printf("SIOCSIFNAME\n");
	    /* Sent from IP down to us.  Attach the ifstats structure.  */
	    if (iop->ioc_count != sizeof(struct ifreq) || us->ppa == 0)
	        break;
	    ifr = (struct ifreq *)mp->b_cont->b_rptr;
	    /* Find the unit number in the interface name.  */
	    for (i = 0; i < IFNAMSIZ; i++) {
		if (ifr->ifr_name[i] == 0 ||
		    (ifr->ifr_name[i] >= '0' &&
		     ifr->ifr_name[i] <= '9'))
		    break;
		else
		    us->ifname[i] = ifr->ifr_name[i];
	    }
	    us->ifname[i] = 0;

	    /* Convert the unit number to binary.  */
	    for (n = 0; i < IFNAMSIZ; i++) {
		if (ifr->ifr_name[i] == 0) {
		    break;
		}
	        else {
		    n = n * 10 + ifr->ifr_name[i] - '0';
		}
	    }

	    /* Verify the ppa.  */
	    if (us->ppa->ppa_id != n)
		break;
	    ppa = us->ppa;

	    /* Set up the netstat block.  */
	    strncpy (ppa->ifname, us->ifname, IFNAMSIZ);

	    ppa->ifstats.ifs_name = ppa->ifname;
	    ppa->ifstats.ifs_unit = n;
	    ppa->ifstats.ifs_active = us->state != DL_UNBOUND;
	    ppa->ifstats.ifs_mtu = ppa->mtu;

	    /* Link in statistics used by netstat.  */
	    ppa->ifstats.ifs_next = ifstats;
	    ifstats = &ppa->ifstats;

	    iop->ioc_count = 0;
	    error = 0;
	    break;

	case SIOCGIFFLAGS:
	    printf("SIOCGIFFLAGS\n");
	    if (!(us->flags & US_CONTROL)) {
		if (us->ppa)
		    us = us->ppa;
	        else
		    break;
	    }
	    ((struct iocblk_in *)iop)->ioc_ifflags = us->ifflags;
	    error = 0;
	    break;

	case SIOCSIFFLAGS:
	    printf("SIOCSIFFLAGS\n");
	    if (!(us->flags & US_CONTROL)) {
		if (us->ppa)
		    us = us->ppa;
		else
		    break;
	    }
	    us->ifflags = ((struct iocblk_in *)iop)->ioc_ifflags;
	    error = 0;
	    break;

	case SIOCSIFADDR:
	    printf("SIOCSIFADDR\n");
	    if (!(us->flags & US_CONTROL)) {
		if (us->ppa)
		    us = us->ppa;
		else
		    break;
	    }
	    us->ifflags |= IFF_RUNNING;
	    ((struct iocblk_in *)iop)->ioc_ifflags |= IFF_RUNNING;
	    error = 0;
	    break;

	case SIOCGIFNETMASK:
	case SIOCSIFNETMASK:
	case SIOCGIFADDR:
	case SIOCGIFDSTADDR:
	case SIOCSIFDSTADDR:
	case SIOCGIFMETRIC:
	    error = 0;
	    break;
#endif /* LACHTCP */

	default:
	    if (us->ppa == 0 || us->ppa->lowerq == 0)
		break;
	    us->ioc_id = iop->ioc_id;
	    error = -1;
	    switch (iop->ioc_cmd) {
	    case PPPIO_GETSTAT:
	    case PPPIO_GETCSTAT:
		if (us->flags & US_LASTMOD) {
		    error = EINVAL;
		    break;
		}
		putnext(us->ppa->lowerq, mp);
		break;
	    default:
		if (us->flags & US_PRIV)
		    putnext(us->ppa->lowerq, mp);
		else {
		    DPRINT1("ppp ioctl %x rejected\n", iop->ioc_cmd);
		    error = EPERM;
		}
		break;
	    }
	    break;
	}

	if (error > 0) {
	    iop->ioc_error = error;
	    mp->b_datap->db_type = M_IOCNAK;
	    qreply(q, mp);
	} else if (error == 0) {
	    mp->b_datap->db_type = M_IOCACK;
	    qreply(q, mp);
	}
	break;

    case M_FLUSH:
	if (us->flags & US_DBGLOG)
	    DPRINT2("ppp/%d: flush %x\n", us->mn, *mp->b_rptr);
	if (*mp->b_rptr & FLUSHW)
	    flushq(q, FLUSHDATA);
	if (*mp->b_rptr & FLUSHR) {
	    *mp->b_rptr &= ~FLUSHW;
	    qreply(q, mp);
	} else
	    freemsg(mp);
	break;

    default:
	freemsg(mp);
	break;
    }
    return 0;
}

#ifndef NO_DLPI
static void
dlpi_request(q, mp, us)
    queue_t *q;
    mblk_t *mp;
    upperstr_t *us;
{
    union DL_primitives *d = (union DL_primitives *) mp->b_rptr;
    int size = mp->b_wptr - mp->b_rptr;
    mblk_t *reply, *np;
    upperstr_t *ppa, *os;
    int sap, *ip, len;
    dl_info_ack_t *info;
    dl_bind_ack_t *ackp;

    if (us->flags & US_DBGLOG)
	DPRINT3("ppp/%d: dlpi prim %x len=%d\n", us->mn,
		d->dl_primitive, size);
    switch (d->dl_primitive) {
    case DL_INFO_REQ:
	if (size < sizeof(dl_info_req_t))
	    goto badprim;
	if ((reply = allocb(sizeof(dl_info_ack_t), BPRI_HI)) == 0)
	    break;		/* should do bufcall */
	reply->b_datap->db_type = M_PCPROTO;
	info = (dl_info_ack_t *) reply->b_wptr;
	reply->b_wptr += sizeof(dl_info_ack_t);
	bzero((caddr_t) info, sizeof(dl_info_ack_t));
	info->dl_primitive = DL_INFO_ACK;
	info->dl_max_sdu = PPP_MAXMTU;
	info->dl_min_sdu = 1;
	info->dl_addr_length = sizeof(ulong);
#ifdef DL_OTHER
	info->dl_mac_type = DL_OTHER;
#else
	info->dl_mac_type = DL_HDLC;	/* a lie */
#endif
	info->dl_current_state = us->state;
	info->dl_service_mode = DL_CLDLS;
	info->dl_provider_style = DL_STYLE2;
#if DL_CURRENT_VERSION >= 2
	info->dl_sap_length = sizeof(ulong);
	info->dl_version = DL_CURRENT_VERSION;
#endif
	qreply(q, reply);
	break;

    case DL_ATTACH_REQ:
	if (size < sizeof(dl_attach_req_t))
	    goto badprim;
	if (us->state != DL_UNATTACHED || us->ppa != 0) {
	    dlpi_error(q, DL_ATTACH_REQ, DL_OUTSTATE, 0);
	    break;
	}
	for (ppa = ppas; ppa != 0; ppa = ppa->nextppa)
	    if (ppa->ppa_id == d->attach_req.dl_ppa)
		break;
	if (ppa == 0) {
	    dlpi_error(q, DL_ATTACH_REQ, DL_BADPPA, 0);
	    break;
	}
	us->ppa = ppa;
	qwriter(q, mp, attach_ppa, PERIM_OUTER);
	break;

    case DL_DETACH_REQ:
	if (size < sizeof(dl_detach_req_t))
	    goto badprim;
	if (us->state != DL_UNBOUND || us->ppa == 0) {
	    dlpi_error(q, DL_DETACH_REQ, DL_OUTSTATE, 0);
	    break;
	}
	qwriter(q, mp, detach_ppa, PERIM_OUTER);
	break;

    case DL_BIND_REQ:
	if (size < sizeof(dl_bind_req_t))
	    goto badprim;
	if (us->state != DL_UNBOUND || us->ppa == 0) {
	    dlpi_error(q, DL_BIND_REQ, DL_OUTSTATE, 0);
	    break;
	}
	if (d->bind_req.dl_service_mode != DL_CLDLS) {
	    dlpi_error(q, DL_BIND_REQ, DL_UNSUPPORTED, 0);
	    break;
	}

	/* saps must be valid PPP network protocol numbers,
	   except that we accept ETHERTYPE_IP in place of PPP_IP. */
	sap = d->bind_req.dl_sap;
	us->req_sap = sap;
	if (sap == ETHERTYPE_IP)
	    sap = PPP_IP;
	if (sap < 0x21 || sap > 0x3fff || (sap & 0x101) != 1) {
	    dlpi_error(q, DL_BIND_REQ, DL_BADADDR, 0);
	    break;
	}

	/* check that no other stream is bound to this sap already. */
	for (os = us->ppa; os != 0; os = os->next)
	    if (os->sap == sap)
		break;
	if (os != 0) {
	    dlpi_error(q, DL_BIND_REQ, DL_NOADDR, 0);
	    break;
	}

	us->sap = sap;
	us->state = DL_IDLE;

	if ((reply = allocb(sizeof(dl_bind_ack_t) + sizeof(ulong),
			    BPRI_HI)) == 0)
	    break;		/* should do bufcall */
	ackp = (dl_bind_ack_t *) reply->b_wptr;
	reply->b_wptr += sizeof(dl_bind_ack_t) + sizeof(ulong);
	reply->b_datap->db_type = M_PCPROTO;
	bzero((caddr_t) ackp, sizeof(dl_bind_ack_t));
	ackp->dl_primitive = DL_BIND_ACK;
	ackp->dl_sap = sap;
	ackp->dl_addr_length = sizeof(ulong);
	ackp->dl_addr_offset = sizeof(dl_bind_ack_t);
	*(ulong *)(ackp+1) = sap;
	qreply(q, reply);
	break;

    case DL_UNBIND_REQ:
	if (size < sizeof(dl_unbind_req_t))
	    goto badprim;
	if (us->state != DL_IDLE) {
	    dlpi_error(q, DL_UNBIND_REQ, DL_OUTSTATE, 0);
	    break;
	}
	us->sap = -1;
	us->state = DL_UNBOUND;
#ifdef LACHTCP
	us->ppa->ifstats.ifs_active = 0;
#endif
	dlpi_ok(q, DL_UNBIND_REQ);
	break;

    case DL_UNITDATA_REQ:
	if (size < sizeof(dl_unitdata_req_t))
	    goto badprim;
	if (us->state != DL_IDLE) {
	    dlpi_error(q, DL_UNITDATA_REQ, DL_OUTSTATE, 0);
	    break;
	}
	if ((ppa = us->ppa) == 0) {
	    cmn_err(CE_CONT, "ppp: in state dl_idle but ppa == 0?\n");
	    break;
	}
	len = mp->b_cont == 0? 0: msgdsize(mp->b_cont);
	if (len > ppa->mtu) {
	    DPRINT2("dlpi data too large (%d > %d)\n", len, ppa->mtu);
	    break;
	}
	/* this assumes PPP_HDRLEN <= sizeof(dl_unitdata_req_t) */
	if (mp->b_datap->db_ref > 1) {
	    np = allocb(PPP_HDRLEN, BPRI_HI);
	    if (np == 0)
		break;		/* gak! */
	    np->b_cont = mp->b_cont;
	    mp->b_cont = 0;
	    freeb(mp);
	    mp = np;
	} else
	    mp->b_datap->db_type = M_DATA;
	/* XXX should use dl_dest_addr_offset/length here,
	   but we would have to translate ETHERTYPE_IP -> PPP_IP */
	mp->b_wptr = mp->b_rptr + PPP_HDRLEN;
	mp->b_rptr[0] = PPP_ALLSTATIONS;
	mp->b_rptr[1] = PPP_UI;
	mp->b_rptr[2] = us->sap >> 8;
	mp->b_rptr[3] = us->sap;
	if (!pass_packet(ppa, mp, 1)) {
	    freemsg(mp);
	} else {
	    if (!send_data(mp, us))
		putq(q, mp);
	}
	return;

#if DL_CURRENT_VERSION >= 2
    case DL_SUBS_BIND_REQ:
    case DL_SUBS_UNBIND_REQ:
    case DL_ENABMULTI_REQ:
    case DL_DISABMULTI_REQ:
    case DL_PROMISCON_REQ:
    case DL_PROMISCOFF_REQ:
    case DL_PHYS_ADDR_REQ:
    case DL_SET_PHYS_ADDR_REQ:
    case DL_XID_REQ:
    case DL_TEST_REQ:
    case DL_REPLY_UPDATE_REQ:
    case DL_REPLY_REQ:
    case DL_DATA_ACK_REQ:
#endif
    case DL_CONNECT_REQ:
    case DL_TOKEN_REQ:
	dlpi_error(q, d->dl_primitive, DL_NOTSUPPORTED, 0);
	break;

    case DL_CONNECT_RES:
    case DL_DISCONNECT_REQ:
    case DL_RESET_REQ:
    case DL_RESET_RES:
	dlpi_error(q, d->dl_primitive, DL_OUTSTATE, 0);
	break;

    case DL_UDQOS_REQ:
	dlpi_error(q, d->dl_primitive, DL_BADQOSTYPE, 0);
	break;

#if DL_CURRENT_VERSION >= 2
    case DL_TEST_RES:
    case DL_XID_RES:
	break;
#endif

    default:
	cmn_err(CE_CONT, "ppp: unknown dlpi prim 0x%x\n", d->dl_primitive);
	/* fall through */
    badprim:
	dlpi_error(q, d->dl_primitive, DL_BADPRIM, 0);
	break;
    }
    freemsg(mp);
}

static void
dlpi_error(q, prim, err, uerr)
    queue_t *q;
    int prim, err, uerr;
{
    mblk_t *reply;
    dl_error_ack_t *errp;

    reply = allocb(sizeof(dl_error_ack_t), BPRI_HI);
    if (reply == 0)
	return;			/* XXX should do bufcall */
    reply->b_datap->db_type = M_PCPROTO;
    errp = (dl_error_ack_t *) reply->b_wptr;
    reply->b_wptr += sizeof(dl_error_ack_t);
    errp->dl_primitive = DL_ERROR_ACK;
    errp->dl_error_primitive = prim;
    errp->dl_errno = err;
    errp->dl_unix_errno = uerr;
    qreply(q, reply);
}

static void
dlpi_ok(q, prim)
    queue_t *q;
    int prim;
{
    mblk_t *reply;
    dl_ok_ack_t *okp;

    reply = allocb(sizeof(dl_ok_ack_t), BPRI_HI);
    if (reply == 0)
	return;			/* XXX should do bufcall */
    reply->b_datap->db_type = M_PCPROTO;
    okp = (dl_ok_ack_t *) reply->b_wptr;
    reply->b_wptr += sizeof(dl_ok_ack_t);
    okp->dl_primitive = DL_OK_ACK;
    okp->dl_correct_primitive = prim;
    qreply(q, reply);
}
#endif /* NO_DLPI */

static int
pass_packet(ppa, mp, outbound)
    upperstr_t *ppa;
    mblk_t *mp;
    int outbound;
{
    /*
     * Here is where we might, in future, decide whether to pass
     * or drop the packet, and whether it counts as link activity.
     */
    if (outbound)
	ppa->last_sent = time;
    else
	ppa->last_recv = time;
    return 1;
}

static int
send_data(mp, us)
    mblk_t *mp;
    upperstr_t *us;
{
    queue_t *q;
    upperstr_t *ppa;

    if ((us->flags & US_BLOCKED) || us->npmode == NPMODE_QUEUE)
	return 0;
    ppa = us->ppa;
    if (ppa == 0 || us->npmode == NPMODE_DROP || us->npmode == NPMODE_ERROR) {
	if (us->flags & US_DBGLOG)
	    DPRINT2("ppp/%d: dropping pkt (npmode=%d)\n", us->mn, us->npmode);
	freemsg(mp);
	return 1;
    }
    if ((q = ppa->lowerq) == 0) {
	/* try to send it up the control stream */
	if (canputnext(ppa->q)) {
	    putnext(ppa->q, mp);
	    return 1;
	}
    } else {
	if (canputnext(ppa->lowerq)) {
	    /*
	     * The lower write queue's put procedure just updates counters
	     * and does a putnext.  We call it so that on SMP systems, we
	     * enter the lower queues' perimeter so that the counter
	     * updates are serialized.
	     */
	    put(ppa->lowerq, mp);
	    return 1;
	}
    }
    us->flags |= US_BLOCKED;
    return 0;
}

/*
 * Allocate a new PPA id and link this stream into the list of PPAs.
 * This procedure is called with an exclusive lock on all queues in
 * this driver.
 */
static void
new_ppa(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    upperstr_t *us, **usp;
    int ppa_id;

    usp = &ppas;
    ppa_id = 0;
    while ((us = *usp) != 0 && ppa_id == us->ppa_id) {
	++ppa_id;
	usp = &us->nextppa;
    }
    us = (upperstr_t *) q->q_ptr;
    us->ppa_id = ppa_id;
    us->ppa = us;
    us->next = 0;
    us->nextppa = *usp;
    *usp = us;
    us->flags |= US_CONTROL;
    us->npmode = NPMODE_PASS;

    us->mtu = PPP_MRU;
    us->mru = PPP_MRU;

#ifdef SOL2
    /*
     * Create a kstats record for our statistics, so netstat -i works.
     */
    if (us->kstats == 0) {
	char unit[32];

	sprintf(unit, "ppp%d", us->ppa->ppa_id);
	us->kstats = kstat_create("ppp", us->ppa->ppa_id, unit,
				  "net", KSTAT_TYPE_NAMED, 4, 0);
	if (us->kstats != 0) {
	    kstat_named_t *kn = KSTAT_NAMED_PTR(us->kstats);

	    strcpy(kn[0].name, "ipackets");
	    kn[0].data_type = KSTAT_DATA_ULONG;
	    strcpy(kn[1].name, "ierrors");
	    kn[1].data_type = KSTAT_DATA_ULONG;
	    strcpy(kn[2].name, "opackets");
	    kn[2].data_type = KSTAT_DATA_ULONG;
	    strcpy(kn[3].name, "oerrors");
	    kn[3].data_type = KSTAT_DATA_ULONG;
	    kstat_install(us->kstats);
	}
    }
#endif /* SOL2 */

    *(int *)mp->b_cont->b_rptr = ppa_id;
    mp->b_datap->db_type = M_IOCACK;
    qreply(q, mp);
}

static void
attach_ppa(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    upperstr_t *us, *t;

    us = (upperstr_t *) q->q_ptr;
#ifndef NO_DLPI
    us->state = DL_UNBOUND;
#endif
    for (t = us->ppa; t->next != 0; t = t->next)
	;
    t->next = us;
    us->next = 0;
    if (mp->b_datap->db_type == M_IOCTL) {
	mp->b_datap->db_type = M_IOCACK;
	qreply(q, mp);
    } else {
#ifndef NO_DLPI
	dlpi_ok(q, DL_ATTACH_REQ);
#endif
    }
}

static void
detach_ppa(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    upperstr_t *us, *t;

    us = (upperstr_t *) q->q_ptr;
    for (t = us->ppa; t->next != 0; t = t->next)
	if (t->next == us) {
	    t->next = us->next;
	    break;
	}
    us->next = 0;
    us->ppa = 0;
#ifndef NO_DLPI
    us->state = DL_UNATTACHED;
    dlpi_ok(q, DL_DETACH_REQ);
#endif
}

static int
pppuwsrv(q)
    queue_t *q;
{
    upperstr_t *us;
    struct lowerstr *ls;
    queue_t *lwq;
    mblk_t *mp;

    us = (upperstr_t *) q->q_ptr;
    us->flags &= ~US_BLOCKED;
    while ((mp = getq(q)) != 0) {
	if (!send_data(mp, us)) {
	    putbq(q, mp);
	    break;
	}
    }
    return 0;
}

static int
ppplwput(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    upperstr_t *ppa;

    ppa = (upperstr_t *) q->q_ptr;
    if (ppa != 0) {		/* why wouldn't it? */
	ppa->stats.ppp_opackets++;
	ppa->stats.ppp_obytes += msgdsize(mp);
#ifdef INCR_OPACKETS
	INCR_OPACKETS(ppa);
#endif
    }
    putnext(q, mp);
    return 0;
}

static int
ppplwsrv(q)
    queue_t *q;
{
    upperstr_t *us;

    /*
     * Flow control has back-enabled this stream:
     * enable the write service procedures of all upper
     * streams feeding this lower stream.
     */
    for (us = (upperstr_t *) q->q_ptr; us != NULL; us = us->next)
	if (us->flags & US_BLOCKED)
	    qenable(WR(us->q));
    return 0;
}

static int
pppursrv(q)
    queue_t *q;
{
    upperstr_t *us, *as;
    mblk_t *mp, *hdr;
#ifndef NO_DLPI
    dl_unitdata_ind_t *ud;
#endif
    int proto;

    us = (upperstr_t *) q->q_ptr;
    if (us->flags & US_CONTROL) {
	/*
	 * A control stream.
	 * If there is no lower queue attached, run the write service
	 * routines of other upper streams attached to this PPA.
	 */
	if (us->lowerq == 0) {
	    as = us;
	    do {
		if (as->flags & US_BLOCKED)
		    qenable(WR(as->q));
		as = as->next;
	    } while (as != 0);
	}
    } else {
	/*
	 * A network protocol stream.  Put a DLPI header on each
	 * packet and send it on.
	 * (Actually, it seems that the IP module will happily
	 * accept M_DATA messages without the DL_UNITDATA_IND header.)
	 */
	while ((mp = getq(q)) != 0) {
	    if (!canputnext(q)) {
		putbq(q, mp);
		break;
	    }
#ifndef NO_DLPI
	    proto = PPP_PROTOCOL(mp->b_rptr);
	    mp->b_rptr += PPP_HDRLEN;
	    hdr = allocb(sizeof(dl_unitdata_ind_t) + 2 * sizeof(ulong),
			 BPRI_MED);
	    if (hdr == 0) {
		/* XXX should put it back and use bufcall */
		freemsg(mp);
		continue;
	    }
	    hdr->b_datap->db_type = M_PROTO;
	    ud = (dl_unitdata_ind_t *) hdr->b_wptr;
	    hdr->b_wptr += sizeof(dl_unitdata_ind_t) + 2 * sizeof(ulong);
	    hdr->b_cont = mp;
	    ud->dl_primitive = DL_UNITDATA_IND;
	    ud->dl_dest_addr_length = sizeof(ulong);
	    ud->dl_dest_addr_offset = sizeof(dl_unitdata_ind_t);
	    ud->dl_src_addr_length = sizeof(ulong);
	    ud->dl_src_addr_offset = ud->dl_dest_addr_offset + sizeof(ulong);
#if DL_CURRENT_VERSION >= 2
	    ud->dl_group_address = 0;
#endif
	    /* Send the DLPI client the data with the SAP they requested,
	       (e.g. ETHERTYPE_IP) rather than the PPP protocol number
	       (e.g. PPP_IP) */
	    ((ulong *)(ud + 1))[0] = us->req_sap;	/* dest SAP */
	    ((ulong *)(ud + 1))[1] = us->req_sap;	/* src SAP */
	    putnext(q, hdr);
#else /* NO_DLPI */
	    putnext(q, mp);
#endif /* NO_DLPI */
	}
    }

    /*
     * If this stream is attached to a PPA with a lower queue pair,
     * enable the read queue's service routine if it has data queued.
     * XXX there is a possibility that packets could get out of order
     * if ppplrput now runs before ppplrsrv.
     */
    if (us->ppa != 0 && us->ppa->lowerq != 0)
	qenable(RD(us->ppa->lowerq));

    return 0;
}

static upperstr_t *
find_dest(ppa, proto)
    upperstr_t *ppa;
    int proto;
{
    upperstr_t *us;

    for (us = ppa->next; us != 0; us = us->next)
	if (proto == us->sap)
	    break;
    return us;
}

static int
ppplrput(q, mp)
    queue_t *q;
    mblk_t *mp;
{
    upperstr_t *ppa, *us;
    queue_t *uq;
    int proto, len;
    mblk_t *np;
    struct iocblk *iop;

    ppa = (upperstr_t *) q->q_ptr;
    if (ppa == 0) {
	DPRINT1("ppplrput: q = %x, ppa = 0??\n", q);
	freemsg(mp);
	return 0;
    }
    switch (mp->b_datap->db_type) {
    case M_FLUSH:
	if (*mp->b_rptr & FLUSHW) {
	    *mp->b_rptr &= ~FLUSHR;
	    qreply(q, mp);
	} else
	    freemsg(mp);
	break;

    case M_CTL:
	switch (*mp->b_rptr) {
	case PPPCTL_IERROR:
#ifdef INCR_IERRORS
	    INCR_IERRORS(ppa);
#endif
	    ppa->stats.ppp_ierrors++;
	    break;
	case PPPCTL_OERROR:
#ifdef INCR_OERRORS
	    INCR_OERRORS(ppa);
#endif
	    ppa->stats.ppp_oerrors++;
	    break;
	}
	freemsg(mp);
	break;

    case M_IOCACK:
    case M_IOCNAK:
	/*
	 * Attempt to match up the response with the stream
	 * that the request came from.
	 */
	iop = (struct iocblk *) mp->b_rptr;
	for (us = ppa; us != 0; us = us->next)
	    if (us->ioc_id == iop->ioc_id)
		break;
	if (us == 0)
	    freemsg(mp);
	else
	    putnext(us->q, mp);
	break;

    case M_HANGUP:
	/*
	 * The serial device has hung up.  We don't want to send
	 * the M_HANGUP message up to pppd because that will stop
	 * us from using the control stream any more.  Instead we
	 * send a zero-length message as an end-of-file indication.
	 */
	freemsg(mp);
	mp = allocb(1, BPRI_HI);
	if (mp == 0) {
	    DPRINT1("ppp/%d: couldn't allocate eof message!\n", ppa->mn);
	    break;
	}
	putnext(ppa->q, mp);
	break;

    default:
	if (mp->b_datap->db_type == M_DATA) {
	    len = msgdsize(mp);
	    if (mp->b_wptr - mp->b_rptr < PPP_HDRLEN) {
		PULLUP(mp, PPP_HDRLEN);
		if (mp == 0) {
		    DPRINT1("ppp_lrput: msgpullup failed (len=%d)\n", len);
		    break;
		}
	    }
	    ppa->stats.ppp_ipackets++;
	    ppa->stats.ppp_ibytes += len;
#ifdef INCR_IPACKETS
	    INCR_IPACKETS(ppa);
#endif
	    if (!pass_packet(ppa, mp, 0)) {
		freemsg(mp);
		break;
	    }
	    proto = PPP_PROTOCOL(mp->b_rptr);
	    if (proto < 0x8000 && (us = find_dest(ppa, proto)) != 0) {
		/*
		 * A data packet for some network protocol.
		 * Queue it on the upper stream for that protocol.
		 */
		if (canput(us->q))
		    putq(us->q, mp);
		else
		    putq(q, mp);
		break;
	    }
	}
	/*
	 * A control frame, a frame for an unknown protocol,
	 * or some other message type.
	 * Send it up to pppd via the control stream.
	 */
	if (queclass(mp) == QPCTL || canputnext(ppa->q))
	    putnext(ppa->q, mp);
	else
	    putq(q, mp);
	break;
    }

    return 0;
}

static int
ppplrsrv(q)
    queue_t *q;
{
    mblk_t *mp;
    upperstr_t *ppa, *us;
    int proto;

    /*
     * Packets only get queued here for flow control reasons.
     */
    ppa = (upperstr_t *) q->q_ptr;
    while ((mp = getq(q)) != 0) {
	if (mp->b_datap->db_type == M_DATA
	    && (proto = PPP_PROTOCOL(mp->b_rptr)) < 0x8000
	    && (us = find_dest(ppa, proto)) != 0) {
	    if (canput(us->q))
		putq(us->q, mp);
	    else {
		putbq(q, mp);
		break;
	    }
	} else {
	    if (canputnext(ppa->q))
		putnext(ppa->q, mp);
	    else {
		putbq(q, mp);
		break;
	    }
	}
    }
    return 0;
}

static int
putctl2(q, type, code, val)
    queue_t *q;
    int type, code, val;
{
    mblk_t *mp;

    mp = allocb(2, BPRI_HI);
    if (mp == 0)
	return 0;
    mp->b_datap->db_type = type;
    mp->b_wptr[0] = code;
    mp->b_wptr[1] = val;
    mp->b_wptr += 2;
    putnext(q, mp);
    return 1;
}

static int
putctl4(q, type, code, val)
    queue_t *q;
    int type, code, val;
{
    mblk_t *mp;

    mp = allocb(4, BPRI_HI);
    if (mp == 0)
	return 0;
    mp->b_datap->db_type = type;
    mp->b_wptr[0] = code;
    ((short *)mp->b_wptr)[1] = val;
    mp->b_wptr += 4;
    putnext(q, mp);
    return 1;
}

static void
debug_dump(q, mp)
    queue_t *q;			/* not used */
    mblk_t *mp;			/* not used either */
{
    upperstr_t *us;
    queue_t *uq, *lq;

    DPRINT("ppp upper streams:\n");
    for (us = minor_devs; us != 0; us = us->nextmn) {
	uq = us->q;
	DPRINT3(" %d: q=%x rlev=%d",
		us->mn, uq, (uq? qsize(uq): 0));
	DPRINT3(" wlev=%d flags=0x%b", (uq? qsize(WR(uq)): 0),
		us->flags, "\020\1priv\2control\3blocked\4last");
	DPRINT3(" state=%x sap=%x req_sap=%x", us->state, us->sap,
		us->req_sap);
	if (us->ppa == 0)
	    DPRINT(" ppa=?\n");
	else
	    DPRINT1(" ppa=%d\n", us->ppa->ppa_id);
	if (us->flags & US_CONTROL) {
	    lq = us->lowerq;
	    DPRINT3("    control for %d lq=%x rlev=%d",
		    us->ppa_id, lq, (lq? qsize(RD(lq)): 0));
	    DPRINT3(" wlev=%d mru=%d mtu=%d\n",
		    (lq? qsize(lq): 0), us->mru, us->mtu);
	}
    }
}
