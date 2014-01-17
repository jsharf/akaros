// INFERNO
#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <ip.h>

typedef struct Etherhdr Etherhdr;
struct Etherhdr
{
	uint8_t	d[6];
	uint8_t	s[6];
	uint8_t	t[2];
};

static uint8_t ipbroadcast[IPaddrlen] = {
	0xff,0xff,0xff,0xff,  
	0xff,0xff,0xff,0xff,  
	0xff,0xff,0xff,0xff,  
	0xff,0xff,0xff,0xff,
};

static uint8_t etherbroadcast[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

static void	etherread4(void *a);
static void	etherread6(void *a);
static void	etherbind(struct Ipifc *ifc, int argc, char **argv);
static void	etherunbind(struct Ipifc *ifc);
static void	etherbwrite(struct Ipifc *ifc, struct block *bp, int version,
			       uint8_t *ip);
static void	etheraddmulti(struct Ipifc *ifc, uint8_t *a, uint8_t *ia);
static void	etherremmulti(struct Ipifc *ifc, uint8_t *a, uint8_t *ia);
static struct block*	multicastarp(struct Fs *f, struct arpent *a,
				  struct medium*, uint8_t *mac);
static void	sendarp(struct Ipifc *ifc, struct arpent *a);
static void	sendgarp(struct Ipifc *ifc, uint8_t *unused_uint8_p_t);
static int	multicastea(uint8_t *ea, uint8_t *ip);
static void	recvarpproc(void*);
static void	resolveaddr6(struct Ipifc *ifc, struct arpent *a);
static void	etherpref2addr(uint8_t *pref, uint8_t *ea);

struct medium ethermedium =
{
.name=		"ether",
.hsize=		14,
.mintu=		60,
.maxtu=		1514,
.maclen=	6,
.bind=		etherbind,
.unbind=	etherunbind,
.bwrite=	etherbwrite,
.addmulti=	etheraddmulti,
.remmulti=	etherremmulti,
.ares=		arpenter,
.areg=		sendgarp,
.pref2addr=	etherpref2addr,
};

struct medium gbemedium =
{
.name=		"gbe",
.hsize=		14,
.mintu=		60,
.maxtu=		9014,
.maclen=	6,
.bind=		etherbind,
.unbind=	etherunbind,
.bwrite=	etherbwrite,
.addmulti=	etheraddmulti,
.remmulti=	etherremmulti,
.ares=		arpenter,
.areg=		sendgarp,
.pref2addr=	etherpref2addr,
};

typedef struct	Etherrock Etherrock;
struct Etherrock
{
	struct Fs	*f;		/* file system we belong to */
	struct proc	*arpp;		/* arp process */
	struct proc	*read4p;	/* reading process (v4)*/
	struct proc	*read6p;	/* reading process (v6)*/
	struct chan	*mchan4;	/* Data channel for v4 */
	struct chan	*achan;		/* Arp channel */
	struct chan	*cchan4;	/* Control channel for v4 */
	struct chan	*mchan6;	/* Data channel for v6 */
	struct chan	*cchan6;	/* Control channel for v6 */
};

/*
 *  ethernet arp request
 */
enum
{
	ETARP		= 0x0806,
	ETIP4		= 0x0800,
	ETIP6		= 0x86DD,
	ARPREQUEST	= 1,
	ARPREPLY	= 2,
};

typedef struct Etherarp Etherarp;
struct Etherarp
{
	uint8_t	d[6];
	uint8_t	s[6];
	uint8_t	type[2];
	uint8_t	hrd[2];
	uint8_t	pro[2];
	uint8_t	hln;
	uint8_t	pln;
	uint8_t	op[2];
	uint8_t	sha[6];
	uint8_t	spa[4];
	uint8_t	tha[6];
	uint8_t	tpa[4];
};

static char *nbmsg = "nonblocking";

/*
 *  called to bind an IP ifc to an ethernet device
 *  called with ifc wlock'd
 */
static void
etherbind(struct Ipifc *ifc, int argc, char **argv)
{
	ERRSTACK(2);
	struct chan *mchan4, *cchan4, *achan, *mchan6, *cchan6;
	char addr[Maxpath];	//char addr[2*KNAMELEN];
	char dir[Maxpath];	//char dir[2*KNAMELEN];
	char *buf;
	int fd, cfd, n;
	char *ptr;
	Etherrock *er;

	if(argc < 2)
		error(Ebadarg);

	mchan4 = cchan4 = achan = mchan6 = cchan6 = NULL;
	buf = NULL;
	if(waserror()){
		if(mchan4 != NULL)
			cclose(mchan4);
		if(cchan4 != NULL)
			cclose(cchan4);
		if(achan != NULL)
			cclose(achan);
		if(mchan6 != NULL)
			cclose(mchan6);
		if(cchan6 != NULL)
			cclose(cchan6);
		if(buf != NULL)
			kfree(buf);
		nexterror(); 
	}

	/*
	 *  open ip converstation
	 *
	 *  the dial will fail if the type is already open on
	 *  this device.
	 */
	snprintf(addr, sizeof(addr), "%s!0x800", argv[2]);
	fd = kdial(addr, NULL, dir, &cfd);
	if(fd < 0)
		errorf("dial 0x800 failed: %s", get_cur_errbuf());
	mchan4 = commonfdtochan(fd, ORDWR, 0, 1);
	cchan4 = commonfdtochan(cfd, ORDWR, 0, 1);
	sysclose(fd);
	sysclose(cfd);

	/*
	 *  make it non-blocking
	 */
	devtab[cchan4->type]->write(cchan4, nbmsg, strlen(nbmsg), 0);

	/*
	 *  get mac address and speed
	 */
	snprintf(addr, sizeof(addr), "%s/stats", dir);
	fd = sysopen(addr, OREAD);
	if(fd < 0)
		errorf("can't open ether stats: %s", get_cur_errbuf());

	buf = kzmalloc(512, 0);
	n = sysread(fd, buf, 511);
	sysclose(fd);
	if(n <= 0)
		error(Eio);
	buf[n] = 0;

	ptr = strstr(buf, "addr: ");
	if(!ptr)
		error(Eio);
	ptr += 6;
	parsemac(ifc->mac, ptr, 6);

	ptr = strstr(buf, "mbps: ");
	if(ptr){
		ptr += 6;
		ifc->mbps = atoi(ptr);
	} else
		ifc->mbps = 100;

	/*
 	 *  open arp conversation
	 */
	snprintf(addr, sizeof(addr), "%s!0x806", argv[2]);
	fd = kdial(addr, NULL, NULL, NULL);
	if(fd < 0)
		error("dial 0x806 failed: %s", get_cur_errbuf());
	achan = commonfdtochan(fd, ORDWR, 0, 1);
	sysclose(fd);

	/*
	 *  open ip conversation
	 *
	 *  the dial will fail if the type is already open on
	 *  this device.
	 */
	snprintf(addr, sizeof(addr), "%s!0x86DD", argv[2]);
	fd = kdial(addr, NULL, dir, &cfd);
	if(fd < 0)
		error("dial 0x86DD failed: %s", get_cur_errbuf());
	mchan6 = commonfdtochan(fd, ORDWR, 0, 1);
	cchan6 = commonfdtochan(cfd, ORDWR, 0, 1);
	sysclose(fd);
	sysclose(cfd);

	/*
	 *  make it non-blocking
	 */
	devtab[cchan6->type]->write(cchan6, nbmsg, strlen(nbmsg), 0);

	er = kzmalloc(sizeof(*er), 0);
	er->mchan4 = mchan4;
	er->cchan4 = cchan4;
	er->achan = achan;
	er->mchan6 = mchan6;
	er->cchan6 = cchan6;
	er->f = ifc->conv->p->f;
	ifc->arg = er;

	kfree(buf);
	poperror();

	kproc("etherread4", etherread4, ifc, 0);
	kproc("recvarpproc", recvarpproc, ifc, 0);
	kproc("etherread6", etherread6, ifc, 0);
}

/*
 *  called with ifc wlock'd
 */
static void
etherunbind(struct Ipifc *ifc)
{
	Etherrock *er = ifc->arg;
#warning "postnote"
#if 0
	if(er->read4p)
		postnote(er->read4p, 1, "unbind", 0);
	if(er->read6p)
		postnote(er->read6p, 1, "unbind", 0);
	if(er->arpp)
		postnote(er->arpp, 1, "unbind", 0);
#endif

	/* wait for readers to die */
	while(er->arpp != 0 || er->read4p != 0 || er->read6p != 0)
		;
#warning tsleep
	//	tsleep(&up->sleep, return0, 0, 300);

	if(er->mchan4 != NULL)
		cclose(er->mchan4);
	if(er->achan != NULL)
		cclose(er->achan);
	if(er->cchan4 != NULL)
		cclose(er->cchan4);
	if(er->mchan6 != NULL)
		cclose(er->mchan6);
	if(er->cchan6 != NULL)
		cclose(er->cchan6);

	kfree(er);
}

/*
 *  called by ipoput with a single block to write with ifc rlock'd
 */
static void
etherbwrite(struct Ipifc *ifc, struct block *bp, int version, uint8_t *ip)
{
	Etherhdr *eh;
	struct arpent *a;
	uint8_t mac[6];
	Etherrock *er = ifc->arg;

	/* get mac address of destination */
	a = arpget(er->f->arp, bp, version, ifc, ip, mac);
	if(a){
		/* check for broadcast or multicast */
		bp = multicastarp(er->f, a, ifc->m, mac);
		if(bp==NULL){
			switch(version){
			case V4:
				sendarp(ifc, a);
				break;
			case V6: 
				resolveaddr6(ifc, a);
				break;
			default:
				panic("etherbwrite: version %d", version);
			}
			return;
		}
	}

	/* make it a single block with space for the ether header */
	bp = padblock(bp, ifc->m->hsize);
	if(bp->next)
		bp = concatblock(bp);
	if(BLEN(bp) < ifc->mintu)
		bp = adjustblock(bp, ifc->mintu);
	eh = (Etherhdr*)bp->rp;

	/* copy in mac addresses and ether type */
	memmove(eh->s, ifc->mac, sizeof(eh->s));
	memmove(eh->d, mac, sizeof(eh->d));

 	switch(version){
	case V4:
		eh->t[0] = 0x08;
		eh->t[1] = 0x00;
		devtab[er->mchan4->type]->bwrite(er->mchan4, bp, 0);
		break;
	case V6:
		eh->t[0] = 0x86;
		eh->t[1] = 0xDD;
		devtab[er->mchan6->type]->bwrite(er->mchan6, bp, 0);
		break;
	default:
		panic("etherbwrite2: version %d", version);
	}
	ifc->out++;
}


/*
 *  process to read from the ethernet
 */
static void
etherread4(void *a)
{
	ERRSTACK(2);
	struct Ipifc *ifc;
	struct block *bp;
	Etherrock *er;

	ifc = a;
	er = ifc->arg;
	er->read4p = current;	/* hide identity under a rock for unbind */
	if(waserror()){
		er->read4p = 0;
#warning "pexit"
		//	pexit("hangup", 1);
	}
	for(;;){
		bp = devtab[er->mchan4->type]->bread(er->mchan4, ifc->maxtu, 0);
		if(!canrlock(&ifc->rwlock)){
			freeb(bp);
			continue;
		}
		if(waserror()){
			runlock(&ifc->rwlock);
			nexterror();
		}
		ifc->in++;
		bp->rp += ifc->m->hsize;
		if(ifc->lifc == NULL)
			freeb(bp);
		else
			ipiput4(er->f, ifc, bp);
		runlock(&ifc->rwlock);
		poperror();
	}
}


/*
 *  process to read from the ethernet, IPv6
 */
static void
etherread6(void *a)
{
	ERRSTACK(2);
	struct Ipifc *ifc;
	struct block *bp;
	Etherrock *er;

	ifc = a;
	er = ifc->arg;
	er->read6p = current;	/* hide identity under a rock for unbind */
	if(waserror()){
		er->read6p = 0;
		//	pexit("hangup", 1);
	}
	for(;;){
		bp = devtab[er->mchan6->type]->bread(er->mchan6, ifc->maxtu, 0);
		if(!canrlock(&ifc->rwlock)){
			freeb(bp);
			continue;
		}
		if(waserror()){
			runlock(&ifc->rwlock);
			nexterror();
		}
		ifc->in++;
		bp->rp += ifc->m->hsize;
		if(ifc->lifc == NULL)
			freeb(bp);
		else
			ipiput6(er->f, ifc, bp);
		runlock(&ifc->rwlock);
		poperror();
	}
}

static void
etheraddmulti(struct Ipifc *ifc, uint8_t *a, uint8_t *unused)
{
	uint8_t mac[6];
	char buf[64];
	Etherrock *er = ifc->arg;
	int version;

	version = multicastea(mac, a);
	snprintf(buf, sizeof(buf), "addmulti %E", mac);
	switch(version){
	case V4:
		devtab[er->cchan4->type]->write(er->cchan4, buf, strlen(buf), 0);
		break;
	case V6:
		devtab[er->cchan6->type]->write(er->cchan6, buf, strlen(buf), 0);
		break;
	default:
		panic("etheraddmulti: version %d", version);
	}
}

static void
etherremmulti(struct Ipifc *ifc, uint8_t *a, uint8_t *unused)
{
	uint8_t mac[6];
	char buf[64];
	Etherrock *er = ifc->arg;
	int version;

	version = multicastea(mac, a);
	snprintf(buf, sizeof(buf), "remmulti %E", mac);
	switch(version){
	case V4:
		devtab[er->cchan4->type]->write(er->cchan4, buf, strlen(buf), 0);
		break;
	case V6:
		devtab[er->cchan6->type]->write(er->cchan6, buf, strlen(buf), 0);
		break;
	default:
		panic("etherremmulti: version %d", version);
	}
}

/*
 *  send an ethernet arp
 *  (only v4, v6 uses the neighbor discovery, rfc1970)
 */
static void
sendarp(struct Ipifc *ifc, struct arpent *a)
{
	int n;
	struct block *bp;
	Etherarp *e;
	Etherrock *er = ifc->arg;

	/* don't do anything if it's been less than a second since the last */
	if(NOW - a->ctime < 1000){
		arprelease(er->f->arp, a);
		return;
	}

	/* remove all but the last message */
	while((bp = a->hold) != NULL){
		if(bp == a->last)
			break;
		a->hold = bp->list;
		freeblist(bp);
	}

	/* try to keep it around for a second more */
	a->ctime = NOW;
	arprelease(er->f->arp, a);

	n = sizeof(Etherarp);
	if(n < a->type->mintu)
		n = a->type->mintu;
	bp = allocb(n);
	memset(bp->rp, 0, n);
	e = (Etherarp*)bp->rp;
	memmove(e->tpa, a->ip+IPv4off, sizeof(e->tpa));
	ipv4local(ifc, e->spa);
	memmove(e->sha, ifc->mac, sizeof(e->sha));
	memset(e->d, 0xff, sizeof(e->d));		/* ethernet broadcast */
	memmove(e->s, ifc->mac, sizeof(e->s));

	hnputs(e->type, ETARP);
	hnputs(e->hrd, 1);
	hnputs(e->pro, ETIP4);
	e->hln = sizeof(e->sha);
	e->pln = sizeof(e->spa);
	hnputs(e->op, ARPREQUEST);
	bp->wp += n;

	n = devtab[er->achan->type]->bwrite(er->achan, bp, 0);
	if(n < 0)
		printd("arp: send: %r\n");
}

static void
resolveaddr6(struct Ipifc *ifc, struct arpent *a)
{
	int sflag;
	struct block *bp;
	Etherrock *er = ifc->arg;
	uint8_t ipsrc[IPaddrlen];

	/* don't do anything if it's been less than a second since the last */
	if(NOW - a->ctime < ReTransTimer){
		arprelease(er->f->arp, a);
		return;
	}

	/* remove all but the last message */
	while((bp = a->hold) != NULL){
		if(bp == a->last)
			break;
		a->hold = bp->list;
		freeblist(bp);
	}

	/* try to keep it around for a second more */
	a->ctime = NOW;
	a->rtime = NOW + ReTransTimer;
	if(a->rxtsrem <= 0) {
		arprelease(er->f->arp, a);
		return;
	}

	a->rxtsrem--;
	arprelease(er->f->arp, a);

	if((sflag = ipv6anylocal(ifc, ipsrc)) )
		icmpns(er->f, ipsrc, sflag, a->ip, TARG_MULTI, ifc->mac);
}

/*
 *  send a gratuitous arp to refresh arp caches
 */
static void
sendgarp(struct Ipifc *ifc, uint8_t *ip)
{
	int n;
	struct block *bp;
	Etherarp *e;
	Etherrock *er = ifc->arg;

	/* don't arp for our initial non address */
	if(ipcmp(ip, IPnoaddr) == 0)
		return;

	n = sizeof(Etherarp);
	if(n < ifc->m->mintu)
		n = ifc->m->mintu;
	bp = allocb(n);
	memset(bp->rp, 0, n);
	e = (Etherarp*)bp->rp;
	memmove(e->tpa, ip+IPv4off, sizeof(e->tpa));
	memmove(e->spa, ip+IPv4off, sizeof(e->spa));
	memmove(e->sha, ifc->mac, sizeof(e->sha));
	memset(e->d, 0xff, sizeof(e->d));		/* ethernet broadcast */
	memmove(e->s, ifc->mac, sizeof(e->s));

	hnputs(e->type, ETARP);
	hnputs(e->hrd, 1);
	hnputs(e->pro, ETIP4);
	e->hln = sizeof(e->sha);
	e->pln = sizeof(e->spa);
	hnputs(e->op, ARPREQUEST);
	bp->wp += n;

	n = devtab[er->achan->type]->bwrite(er->achan, bp, 0);
	if(n < 0)
		printd("garp: send: %r\n");
}

static void
recvarp(struct Ipifc *ifc)
{
	int n;
	struct block *ebp, *rbp;
	Etherarp *e, *r;
	uint8_t ip[IPaddrlen];
	static uint8_t eprinted[4];
	Etherrock *er = ifc->arg;

	ebp = devtab[er->achan->type]->bread(er->achan, ifc->maxtu, 0);
	if(ebp == NULL) {
		printd("arp: rcv: %r\n");
		return;
	}

	e = (Etherarp*)ebp->rp;
	switch(nhgets(e->op)) {
	default:
		break;

	case ARPREPLY:
		/* check for machine using my ip address */
		v4tov6(ip, e->spa);
		if(iplocalonifc(ifc, ip) || ipproxyifc(er->f, ifc, ip)){
			if(memcmp(e->sha, ifc->mac, sizeof(e->sha)) != 0){
				printd("arprep: 0x%E/0x%E also has ip addr %V\n",
					e->s, e->sha, e->spa);
				break;
			}
		}

		/* make sure we're not entering broadcast addresses */
		if(ipcmp(ip, ipbroadcast) == 0 ||
			!memcmp(e->sha, etherbroadcast, sizeof(e->sha))){
			printd("arprep: 0x%E/0x%E cannot register broadcast address %I\n",
				e->s, e->sha, e->spa);
			break;
		}

		arpenter(er->f, V4, e->spa, e->sha, sizeof(e->sha), 0);
		break;

	case ARPREQUEST:
		/* don't answer arps till we know who we are */
		if(ifc->lifc == 0)
			break;

		/* check for machine using my ip or ether address */
		v4tov6(ip, e->spa);
		if(iplocalonifc(ifc, ip) || ipproxyifc(er->f, ifc, ip)){
			if(memcmp(e->sha, ifc->mac, sizeof(e->sha)) != 0){
				if (memcmp(eprinted, e->spa, sizeof(e->spa))){
					/* print only once */
					printd("arpreq: 0x%E also has ip addr %V\n", e->sha, e->spa);
					memmove(eprinted, e->spa, sizeof(e->spa));
				}
			}
		} else {
			if(memcmp(e->sha, ifc->mac, sizeof(e->sha)) == 0){
				printd("arpreq: %V also has ether addr %E\n", e->spa, e->sha);
				break;
			}
		}

		/* refresh what we know about sender */
		arpenter(er->f, V4, e->spa, e->sha, sizeof(e->sha), 1);

		/* answer only requests for our address or systems we're proxying for */
		v4tov6(ip, e->tpa);
		if(!iplocalonifc(ifc, ip))
		if(!ipproxyifc(er->f, ifc, ip))
			break;

		n = sizeof(Etherarp);
		if(n < ifc->mintu)
			n = ifc->mintu;
		rbp = allocb(n);
		r = (Etherarp*)rbp->rp;
		memset(r, 0, sizeof(Etherarp));
		hnputs(r->type, ETARP);
		hnputs(r->hrd, 1);
		hnputs(r->pro, ETIP4);
		r->hln = sizeof(r->sha);
		r->pln = sizeof(r->spa);
		hnputs(r->op, ARPREPLY);
		memmove(r->tha, e->sha, sizeof(r->tha));
		memmove(r->tpa, e->spa, sizeof(r->tpa));
		memmove(r->sha, ifc->mac, sizeof(r->sha));
		memmove(r->spa, e->tpa, sizeof(r->spa));
		memmove(r->d, e->sha, sizeof(r->d));
		memmove(r->s, ifc->mac, sizeof(r->s));
		rbp->wp += n;

		n = devtab[er->achan->type]->bwrite(er->achan, rbp, 0);
		if(n < 0)
			printd("arp: write: %r\n");
	}
	freeb(ebp);
}

static void
recvarpproc(void *v)
{
	ERRSTACK(2);
	struct Ipifc *ifc = v;
	Etherrock *er = ifc->arg;

	er->arpp = current;
	if(waserror()){
		er->arpp = 0;
		//	pexit("hangup", 1);
	}
	for(;;)
		recvarp(ifc);
}

static int
multicastea(uint8_t *ea, uint8_t *ip)
{
	int x;

	switch(x = ipismulticast(ip)){
	case V4:
		ea[0] = 0x01;
		ea[1] = 0x00;
		ea[2] = 0x5e;
		ea[3] = ip[13] & 0x7f;
		ea[4] = ip[14];
		ea[5] = ip[15];
		break;
 	case V6:
 		ea[0] = 0x33;
 		ea[1] = 0x33;
 		ea[2] = ip[12];
		ea[3] = ip[13];
 		ea[4] = ip[14];
 		ea[5] = ip[15];
 		break;
	}
	return x;
}

/*
 *  fill in an arp entry for broadcast or multicast
 *  addresses.  Return the first queued packet for the
 *  IP address.
 */
static struct block*
multicastarp(struct Fs *f,
	     struct arpent *a, struct medium *medium, uint8_t *mac)
{
	/* is it broadcast? */
	switch(ipforme(f, a->ip)){
	case Runi:
		return NULL;
	case Rbcast:
		memset(mac, 0xff, 6);
		return arpresolve(f->arp, a, medium, mac);
	default:
		break;
	}

	/* if multicast, fill in mac */
	switch(multicastea(mac, a->ip)){
	case V4:
	case V6:
		return arpresolve(f->arp, a, medium, mac);
	}

	/* let arp take care of it */
	return NULL;
}

void
ethermediumlink(void)
{
	addipmedium(&ethermedium);
	addipmedium(&gbemedium);
}


static void 
etherpref2addr(uint8_t *pref, uint8_t *ea)
{
	pref[8]  = ea[0] | 0x2;
	pref[9]  = ea[1];
	pref[10] = ea[2];
	pref[11] = 0xFF;
	pref[12] = 0xFE;
	pref[13] = ea[3];
	pref[14] = ea[4];
	pref[15] = ea[5];
}