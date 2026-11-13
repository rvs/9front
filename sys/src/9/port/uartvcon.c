/* modern virtio-serial, fixed generic port 1; raw queued I/O only */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/pci.h"
#include "../port/virtio10.h"

enum {
	Chunksize = 8192,
	Nrx = 8,
	Nring = 16,
	Vwrite = 2,
	Fmultiport = 1<<1,
	Portid = 1,
	Ncontrol = 8,
	Controlsize = 256,
	Cdevready = 0,
	Cportadd = 1,
	Cportremove = 2,
	Cportready = 3,
	Cportopen = 6,
};

typedef struct Vqueue Vqueue;
typedef struct Ctlr Ctlr;

struct Vqueue {
	Vio notify;
	int num;
	int size;
	int memsize;
	Vdesc *desc;
	Vring *avail;
	u16int *availent;
	Vring *used;
	Vused *usedent;
	u16int lastused;
	void *mem;
};

struct Ctlr {
	Lock;
	Pcidev *pci;
	Vio cfg, isr, notify;
	u32int mult;
	int nnotify;
	Vqueue rx, tx, crx, ctx;
	uchar *rxbuf, *txbuf;
	uchar *crxbuf, *ctxbuf;
	int ctxbusy[Ncontrol], ctxevent[Ncontrol];
	int portadded, hostopen, guestopen;
	ulong crxmsgs, ctxmsgs, ctxdone, ctlerr;
	Uart uart;
	Timer retry;
	int started, configured, active, failed, txbusy, rxwait;
	uvlong rxbytes, txbytes, txchunks, rxchunks;
};

extern PhysUart vconphysuart;

static int
matchcap(Pcidev *p, int cap, int off, int typ)
{
	int bar;

	if(cap != 9 || pcicfgr8(p, off+3) != typ
	|| pcicfgr8(p, off+2) < (typ == 2 ? 20 : 16))
		return 1;
	bar = pcicfgr8(p, off+4);
	return bar >= nelem(p->mem) || p->mem[bar].size == 0
		|| (p->mem[bar].bar & 1) != 0;
}

static int
vcap(Pcidev *p, int typ)
{
	return pcienumcaps(p, matchcap, typ);
}

static int
mkqueue(Ctlr *c, Vqueue *q, int num)
{
	uchar *p;
	int n;
	uvlong off;

	vout16(&c->cfg, Vconf_queuesel, num);
	n = vin16(&c->cfg, Vconf_queuesize);
	if(n > Nring)
		n = Nring;
	if(n < Nrx || (n & (n-1)) != 0)
		return -1;
	off = (uvlong)c->mult * vin16(&c->cfg, Vconf_queuenotifyoff);
	if(off+2 > c->nnotify)
		return -1;
	if(q->mem == nil){
		q->memsize = PGROUND(sizeof(Vdesc)*Nring + 4 + sizeof(u16int)*(Nring+1))
			+ PGROUND(4 + sizeof(Vused)*Nring + sizeof(u16int));
		q->mem = mallocalign(q->memsize, BY2PG, 0, 0);
		if(q->mem == nil)
			return -1;
	}
	memset(q->mem, 0, q->memsize);
	q->lastused = 0;
	q->size = n;
	q->num = num;
	p = q->mem;
	q->desc = (void*)p;
	p += sizeof(Vdesc)*n;
	q->avail = (void*)p;
	p += 4;
	q->availent = (void*)p;
	p += sizeof(u16int)*(n+1);
	p = (uchar*)PGROUND((uintptr)p);
	q->used = (void*)p;
	p += 4;
	q->usedent = (void*)p;
	q->notify = c->notify;
	q->notify.mem += off;
	vout16(&c->cfg, Vconf_queuesize, q->size);
	coherence();
	vout64(&c->cfg, Vconf_queuedesc, PADDR(q->desc));
	vout64(&c->cfg, Vconf_queueavail, PADDR(q->avail));
	vout64(&c->cfg, Vconf_queueused, PADDR(q->used));
	return 0;
}

/* caller holds the controller lock, or the device has not started */
static void
post(Vqueue *q, int id)
{
	q->availent[q->avail->idx & (q->size-1)] = id;
	coherence();
	q->avail->idx++;
}

static void
vqnotify(Vqueue *q)
{
	coherence();
	/* EVENT_IDX is not negotiated */
	vout16(&q->notify, 0, q->num);
}

/* each control descriptor owns its buffer until completion */
static int
control(Ctlr *c, u32int port, int event)
{
	Vqueue *q;
	uchar *b;
	int id;

	for(id = 0; id < Ncontrol; id++)
		if(!c->ctxbusy[id])
			break;
	if(id == Ncontrol){
		c->ctlerr++;
		return -1;
	}
	q = &c->ctx;
	b = c->ctxbuf+id*8;
	PBIT32(b, port);
	PBIT16(b+4, event);
	PBIT16(b+6, 1);
	c->ctxbusy[id] = 1;
	c->ctxevent[id] = event;
	q->desc[id].addr = PADDR(b);
	q->desc[id].len = 8;
	q->desc[id].flags = 0;
	c->ctxmsgs++;
	post(q, id);
	vqnotify(q);
	return 0;
}

/* stop without reusing or freeing any memory still owned by the device */
static void
vconfail(Ctlr *c)
{
	c->uart.serr++;
	c->failed = 1;
	c->configured = c->active = c->rxwait = 0;
	vout8(&c->cfg, Vconf_status, 0);
	qhangup(c->uart.iq, "virtserialport failure");
	qhangup(c->uart.oq, "virtserialport failure");
}

static int
pending(Vqueue *q, int max)
{
	int n;

	coherence();
	n = (u16int)(q->used->idx - q->lastused);
	coherence();
	return n > max ? -1 : n;
}

static int
controlharvest(Ctlr *c)
{
	Vqueue *q;
	Vused *u;
	uchar *b;
	int id, n, event, i, mask;

	q = &c->ctx;
	if((n = pending(q, Ncontrol)) < 0)
		return -1;
	while(n-- > 0){
		u = &q->usedent[q->lastused & (q->size-1)];
		id = u->id;
		if(id < 0 || id >= Ncontrol || !c->ctxbusy[id] || u->len != 0)
			return -1;
		if(c->ctxevent[id] == Cportopen)
			c->guestopen = 1;
		c->ctxbusy[id] = 0;
		c->ctxdone++;
		q->lastused++;
	}
	q = &c->crx;
	if((n = pending(q, Ncontrol)) < 0)
		return -1;
	mask = 0;
	while(n-- > 0){
		u = &q->usedent[q->lastused & (q->size-1)];
		id = u->id;
		if(id < 0 || id >= Ncontrol || u->len < 8 || u->len > Controlsize)
			return -1;
		if(mask & (1<<id))
			return -1;
		mask |= 1<<id;
		b = c->crxbuf+id*Controlsize;
		c->crxmsgs++;
		if(GBIT32(b) == Portid){
			event = GBIT16(b+4);
			if(event == Cportadd && !c->portadded){
				c->portadded = 1;
				if(control(c, Portid, Cportready) < 0
				|| control(c, Portid, Cportopen) < 0)
					return -1;
			}else if(event == Cportopen)
				c->hostopen = GBIT16(b+6) != 0;
			else if(event == Cportremove)
				return -1;
			/* names and other metadata are not used */
		}
		q->lastused++;
	}
	for(i = 0; i < Ncontrol; i++)
		if(mask & (1<<i))
			post(q, i);
	if(mask != 0)
		vqnotify(q);
	return 0;
}

/* leave completed RX buffers with the device ring until iq has room */
static int
harvest(Ctlr *c)
{
	Vqueue *q;
	Vused *u;
	Uart *p;
	int n, id, i, mask, kick, ready;

	if(!c->active)
		return 0;
	p = &c->uart;
	ready = c->hostopen && c->guestopen;
	if(controlharvest(c) < 0){
		c->ctlerr++;
		goto Bad;
	}
	kick = !ready && c->hostopen && c->guestopen;
	q = &c->tx;
	if((n = pending(q, 1)) < 0)
		goto Bad;
	if(n != 0){
		u = &q->usedent[q->lastused & (q->size-1)];
		if(!c->txbusy || u->id != 0 || u->len != 0)
			goto Bad;
		q->lastused++;
		c->txbusy = 0;
		kick = 1;
	}
	q = &c->rx;
	if((n = pending(q, Nrx)) < 0)
		goto Bad;
	c->rxwait = 0;
	mask = 0;
	while(n-- > 0){
		u = &q->usedent[q->lastused & (q->size-1)];
		id = u->id;
		if(id < 0 || id >= Nrx || u->len > Chunksize)
			goto Bad;
		if(mask & (1<<id))
			goto Bad;
		if(u->len != 0){
			if(qfull(p->iq)
			|| qproduce(p->iq, c->rxbuf+id*Chunksize, u->len) <= 0){
				c->rxwait = 1;
				break;
			}
			c->rxbytes += u->len;
			c->rxchunks++;
		}
		q->lastused++;
		mask |= 1<<id;
	}
	for(i = 0; i < Nrx; i++)
		if(mask & (1<<i))
			post(q, i);
	if(mask != 0)
		vqnotify(q);
	return kick;
Bad:
	vconfail(c);
	return 1;
}

static void
vconinterrupt(Ureg*, void *arg)
{
	Ctlr *c;
	int kick;

	c = arg;
	if((vin8(&c->isr, 0) & 1) == 0)
		return;
	ilock(c);
	kick = harvest(c);
	iunlock(c);
	/* kick takes tlock before the controller lock */
	if(kick)
		uartkick(&c->uart);
}

static void
vconretry(Ureg*, Timer *t)
{
	Ctlr *c;
	int kick;

	c = t->ta;
	ilock(c);
	kick = 0;
	if(c->rxwait)
		kick = harvest(c);
	iunlock(c);
	if(kick)
		uartkick(&c->uart);
}

/* called with tlock held; no allocation or sleep */
static void
vconkick(Uart *p)
{
	Ctlr *c;
	Vqueue *q;
	int n, m;

	c = p->regs;
	ilock(c);
	if(c->failed){
		p->op = p->oe;
		qflush(p->oq);
	}
	if(!c->active || c->txbusy || p->blocked
	|| !c->hostopen || !c->guestopen){
		iunlock(c);
		return;
	}
	n = 0;
	while(n < Chunksize){
		/* keep the unconsumed tail of the previous stage */
		if(p->op == p->oe && uartstageoutput(p) == 0)
			break;
		m = p->oe - p->op;
		if(m > Chunksize-n)
			m = Chunksize-n;
		memmove(c->txbuf+n, p->op, m);
		p->op += m;
		n += m;
	}
	if(n != 0){
		q = &c->tx;
		q->desc[0].addr = PADDR(c->txbuf);
		q->desc[0].len = n;
		q->desc[0].flags = 0;
		c->txbusy = 1;
		c->txbytes += n;
		c->txchunks++;
		post(q, 0);
		vqnotify(q);
	}
	iunlock(c);
}

static int
vconreset(Ctlr *c)
{
	int i;

	vout8(&c->cfg, Vconf_status, 0);
	for(i = 0; i < 5; i++){
		if(vin8(&c->cfg, Vconf_status) == 0)
			return 0;
		delay(1);
	}
	return -1;
}

/* only called while inactive; reset must finish before any DMA memory is reused */
static int
vconinit(Ctlr *c)
{
	if(vconreset(c) < 0)
		return -1;
	vout8(&c->cfg, Vconf_status, Sacknowledge|Sdriver);
	vout32(&c->cfg, Vconf_devfeatsel, 1);
	if((vin32(&c->cfg, Vconf_devfeat) & Fversion1) == 0)
		return -1;
	vout32(&c->cfg, Vconf_drvfeatsel, 1);
	vout32(&c->cfg, Vconf_drvfeat, Fversion1);
	vout32(&c->cfg, Vconf_devfeatsel, 0);
	if((vin32(&c->cfg, Vconf_devfeat) & Fmultiport) == 0)
		return -1;
	vout32(&c->cfg, Vconf_drvfeatsel, 0);
	vout32(&c->cfg, Vconf_drvfeat, Fmultiport);
	vout8(&c->cfg, Vconf_status, Sacknowledge|Sdriver|Sfeaturesok);
	if((vin8(&c->cfg, Vconf_status) & Sfeaturesok) == 0
	|| vin16(&c->cfg, Vconf_nqueues) < 6)
		return -1;
	if(mkqueue(c, &c->crx, 2) < 0 || mkqueue(c, &c->ctx, 3) < 0
	|| mkqueue(c, &c->rx, 4) < 0 || mkqueue(c, &c->tx, 5) < 0)
		return -1;
	ilock(c);
	memset(c->ctxbusy, 0, sizeof(c->ctxbusy));
	c->portadded = c->hostopen = c->guestopen = 0;
	c->txbusy = c->rxwait = c->failed = 0;
	c->configured = 1;
	iunlock(c);
	return 0;
}

static void
vconenable(Uart *p, int)
{
	Ctlr *c;
	int i;

	c = p->regs;
	if(!c->configured && vconinit(c) < 0){
		ilock(c);
		vconfail(c);
		iunlock(c);
		return;
	}
	if(!c->started){
		intrenable(c->pci->intl, vconinterrupt, c,
			c->pci->tbdf, "uartvcon");
		c->retry.tmode = Tperiodic;
		c->retry.tns = 1000000;
		c->retry.tf = vconretry;
		c->retry.ta = c;
		timeradd(&c->retry);
		c->started = 1;
	}
	ilock(c);
	pcisetbme(c->pci);
	for(i = 0; i < Nrx; i++){
		c->rx.desc[i].addr = PADDR(c->rxbuf+i*Chunksize);
		c->rx.desc[i].len = Chunksize;
		c->rx.desc[i].flags = Vwrite;
		post(&c->rx, i);
	}
	for(i = 0; i < Ncontrol; i++){
		c->crx.desc[i].addr = PADDR(c->crxbuf+i*Controlsize);
		c->crx.desc[i].len = Controlsize;
		c->crx.desc[i].flags = Vwrite;
		post(&c->crx, i);
	}
	for(i = 2; i < 6; i++){
		vout16(&c->cfg, Vconf_queuesel, i);
		vout16(&c->cfg, Vconf_queueenable, 1);
	}
	c->active = 1;
	coherence();
	vout8(&c->cfg, Vconf_status,
		Sacknowledge|Sdriver|Sfeaturesok|Sdriverok);
	vqnotify(&c->crx);
	vqnotify(&c->rx);
	if(control(c, ~0U, Cdevready) < 0)
		vconfail(c);
	iunlock(c);
}

static void
vcondisable(Uart *p)
{
	Ctlr *c;
	int failed;

	c = p->regs;
	ilock(c);
	c->active = c->configured = c->rxwait = 0;
	iunlock(c);
	/* reset ends the old stream, including in-flight RX and TX */
	failed = vconreset(c) < 0;
	ilock(c);
	if(failed)
		c->failed = 1;
	iunlock(c);
	/* rings and buffers remain allocated even if reset times out */
}

static Uart*
vconpnp(void)
{
	Pcidev *p;
	Ctlr *c;
	Uart *head, **tail;
	int cap, mapped;
	u32int len;

	head = nil;
	tail = &head;
	for(p = nil; (p = pcimatch(p, 0x1af4, 0x1043)) != nil;){
		if((c = mallocz(sizeof(*c), 1)) == nil)
			break;
		c->pci = p;
		mapped = 0;
		if(virtiomapregs(p, vcap(p, 1), Vconf_sz, &c->cfg) == nil){
			free(c);
			continue;
		}
		pcienable(p);
		if(virtiomapregs(p, vcap(p, 3), 1, &c->isr) == nil)
			goto Bad;
		mapped = 1;
		cap = vcap(p, 2);
		if(cap < 0)
			goto Bad;
		len = pcicfgr32(p, cap+12);
		if(len < 2 || len > 0x7fffffff)
			goto Bad;
		c->nnotify = len;
		if(virtiomapregs(p, cap, c->nnotify, &c->notify) == nil)
			goto Bad;
		mapped = 2;
		c->mult = pcicfgr32(p, cap+16);
		if(vconinit(c) < 0)
			goto Bad;
		c->rxbuf = mallocalign(Nrx*Chunksize, BY2PG, 0, 0);
		c->txbuf = mallocalign(Chunksize, BY2PG, 0, 0);
		c->crxbuf = mallocalign(Ncontrol*Controlsize, BY2PG, 0, 0);
		c->ctxbuf = mallocalign(Ncontrol*8, BY2PG, 0, 0);
		if(c->rxbuf == nil || c->txbuf == nil
		|| c->crxbuf == nil || c->ctxbuf == nil)
			goto Bad;
		c->uart.regs = c;
		c->uart.name = "virtserialport1";
		c->uart.phys = &vconphysuart;
		c->uart.baud = 115200;
		c->uart.bits = 8;
		c->uart.stop = 1;
		c->uart.parity = 'n';
		*tail = &c->uart;
		tail = &c->uart.next;
		print("uartvcon: virtserialport1 pci %.8ux irq %d\n",
			p->tbdf, p->intl);
		continue;
Bad:
		print("uartvcon: probe failed pci %.8ux\n", p->tbdf);
		vout8(&c->cfg, Vconf_status, Sfailed);
		cap = vconreset(c);
		pcidisable(p);
		if(cap < 0)
			continue;	/* retain memory if DMA ownership is uncertain */
		free(c->rx.mem);
		free(c->tx.mem);
		free(c->crx.mem);
		free(c->ctx.mem);
		free(c->rxbuf);
		free(c->txbuf);
		free(c->crxbuf);
		free(c->ctxbuf);
		if(mapped >= 2)
			virtiounmap(&c->notify, c->nnotify);
		if(mapped >= 1)
			virtiounmap(&c->isr, 1);
		virtiounmap(&c->cfg, Vconf_sz);
		free(c);
	}
	return head;
}

static int
vconbaud(Uart *p, int n)
{
	if(n <= 0)
		return -1;
	p->baud = n;
	return 0;
}

static int
vconbits(Uart *p, int n)
{
	if(n != 8)
		return -1;
	p->bits = n;
	return 0;
}

static int
vconstop(Uart *p, int n)
{
	if(n != 1)
		return -1;
	p->stop = n;
	return 0;
}

static int
vconparity(Uart *p, int n)
{
	if(n != 'n')
		return -1;
	p->parity = n;
	return 0;
}

static void
vconmodem(Uart *p, int)
{
	p->modem = 0;
	p->cts = 1;
}

static void
vconnop(Uart*, int)
{
}

static char*
vconstatus(Uart *p, char *s, char *e)
{
	Ctlr *c;

	c = p->regs;
	ilock(c);
	s = seprint(s, e,
		"b%d c%d d%d e%d l%d m%d p%c r%d s%d i0\n"
		"dev(%d) virtserialport1 rxbytes %llud txbytes %llud "
		"rxchunks %llud txchunks %llud serr %d\n"
		"port 1 added %d hostopen %d guestopen %d active %d failed %d "
		"crx %lud ctx %lud ctxdone %lud ctlerr %lud\n",
		p->baud, p->hup_dcd, 0, p->hup_dsr, p->bits,
		p->modem, p->parity, 0, p->stop, p->dev,
		c->rxbytes, c->txbytes, c->rxchunks, c->txchunks, p->serr,
		c->portadded, c->hostopen, c->guestopen, c->active, c->failed,
		c->crxmsgs, c->ctxmsgs, c->ctxdone, c->ctlerr);
	iunlock(c);
	return s;
}

PhysUart vconphysuart = {
	.name = "virtserialport1",
	.pnp = vconpnp,
	.enable = vconenable,
	.disable = vcondisable,
	.kick = vconkick,
	.dobreak = vconnop,
	.baud = vconbaud,
	.bits = vconbits,
	.stop = vconstop,
	.parity = vconparity,
	.modemctl = vconmodem,
	.rts = vconnop,
	.dtr = vconnop,
	.status = vconstatus,
	.fifo = vconnop,
	.power = vconnop,
};
