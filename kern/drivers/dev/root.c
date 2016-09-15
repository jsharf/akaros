/* Copyright © 1994-1999 Lucent Technologies Inc.  All rights reserved.
 * Portions Copyright © 1997-1999 Vita Nuova Limited
 * Portions Copyright © 2000-2007 Vita Nuova Holdings Limited
 *                                (www.vitanuova.com)
 * Revisions Copyright © 2000-2007 Lucent Technologies Inc. and others
 *
 * Modified for the Akaros operating system:
 * Copyright (c) 2013-2014 The Regents of the University of California
 * Copyright (c) 2013-2015 Google Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

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
#include <umem.h>

struct dev rootdevtab;

static char *devname(void)
{
	return rootdevtab.name;
}

/* make it a power of 2 and nobody gets hurt */
#define MAXFILE 1024
int rootmaxq = MAXFILE;
int inumber = 13;

/* TODO:
 *  - synchronization!  what needs protection from concurrent use, etc.
 * 	- clean up documentation and whatnot
 * 	- does remove, mkdir, rmdir work?
 * 	- fill this with cpio stuff
 * 	- figure out how to use the page cache
 */

/* this gives you some idea of how much I like linked lists. Just make
 * a big old table. Later on we can put next and prev indices into the
 * data if we want but, our current kfs is 1-3 levels deep and very small
 * (< 200 entries) so I doubt we'll need to do that. It just makes debugging
 * memory a tad easier.
 */
/* Da Rules.
 * The roottab contains [name, qid, length, perm]. Length means length for files.
 * Qid is [path, vers, type]. Path is me. vers is next. Type is QTDIR for dir
 * and QTFILE for file and 0 for empty.
 * Data is [dotdot, ptr, size, *sizep, next]
 * dotdot is .., ptr is data (for files)
 * size is # elements (for dirs)
 * *sizep is a pointer for reasons not understood.
 * child is the qid.path of the first child of a directory.
 * Possibly 0 == no child.
 * To find the next sibling (in a directory), look at roottab[i].qid.vers.
 *
 *	int	dotdot;
 *      int     child;
 *	void	*ptr;
 *	int	size;
 *	int	*sizep;
 *
 * entry is empty if type is 0. We look in roottab to determine that.
*/
/* we pack the qid as follows: path is the index, vers is ., and type is type */

/* Inferno seems to want to: perm |= DMDIR.  It gets checked in other places.
 * NxM didn't want this, IIRC.
 *
 * Also note that "" (/, #root, whatever) has no vers/next/sibling.
 *
 * If you want to add new entries, add it to the roottab such that the linked
 * list of indexes is a cycle (change the last current one), then add an entry
 * to rootdata, and then change the first rootdata entry to have another entry.
 * Yeah, it's a pain in the ass.
 *
 * To add subdirectories, or any child of a directory, the files (e.g. env_dir1)
 * go in roottab.  Children of a parent are linked with their vers (note
 * env_dir1 points to env_dir2), and the last item's vers = 0.  These files need
 * their dotdot set in rootdata to the qid of their parent.  The directory that
 * has children needs its child pointer set to the first qid in the list, and
 * its data pointer must point to the roottab entry for the child.  This also
 * means that all child entries in roottab for a parent must be contiguous.
 *
 * Yeah, it's a pain in the ass.  And, given this structure, it probably can't
 * grow dynamically (I think we assume roottab[i] = entry for qid.path all over
 * the place - imagine what happens if we wanted to squeeze in a new entry). */
struct dirtab roottab[MAXFILE] = {
	{"", {0, 0, QTDIR}, 0, DMDIR | 0777},
	{"chan", {1, 2, QTDIR}, 0, DMDIR | 0777},
	{"dev", {2, 3, QTDIR}, 0, DMDIR | 0777},
	{"fd", {3, 4, QTDIR}, 0, DMDIR | 0777},
	{"prog", {4, 5, QTDIR}, 0, DMDIR | 0777},
	{"prof", {5, 6, QTDIR}, 0, DMDIR | 0777},
	{"net", {6, 7, QTDIR}, 0, DMDIR | 0777},
	{"net.alt", {7, 8, QTDIR}, 0, DMDIR | 0777},
	{"nvfs", {8, 9, QTDIR}, 0, DMDIR | 0777},
	{"env", {9, 10, QTDIR}, 0, DMDIR | 0777},
	{"root", {10, 11, QTDIR}, 0, DMDIR | 0777},
	{"srv", {11, 12, QTDIR}, 0, DMDIR | 0777},
	{"mnt", {12, 13, QTDIR}, 0, DMDIR | 0777},
	{"proc", {13, 0, QTDIR}, 0, DMDIR | 0777},
	{"env_dir1", {14, 15, QTDIR}, 0, DMDIR | 0777},
	{"env_dir2", {15, 0, QTDIR}, 0, DMDIR | 0777},
};

struct rootdata {
	int dotdot;
	int child;
	void *ptr;
	int size;
	int *sizep;
};

struct rootdata rootdata[MAXFILE] = {
	{0,	1,	 &roottab[1],	 13,	NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	14,	 &roottab[14],	 2,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{0,	0,	 NULL,	 0,	 NULL},
	{9,	0,	 NULL,	 0,	 NULL},
	{9,	0,	 NULL,	 0,	 NULL},
};

/* this is super useful */
void dumprootdev(void)
{
	struct dirtab *r = roottab;
	struct rootdata *rd = rootdata;
	int i;

	printk("[       dirtab     ]      name: [pth, ver, typ],   len,        "
	       "perm,  .., chld,       data pointer,  size,       size pointer\n");
	for (i = 0; i < rootmaxq; i++, r++, rd++) {
		if (i && (!r->name[0]))
			continue;
		printk("[%p]%10s: [%3d, %3d, %3d], %5d, %11o,",
			   r,
			   r->name, r->qid.path, r->qid.vers, r->qid.type,
			   r->length, r->perm);
		printk(" %3d, %4d, %p, %5d, %p\n",
			   rd->dotdot, rd->child, rd->ptr, rd->size, rd->sizep);
	}
}

static int findempty(void)
{
	int i;
	for (i = 0; i < rootmaxq; i++) {
		if (!roottab[i].qid.type) {
			memset(&roottab[i], 0, sizeof(roottab[i]));
			return i;
		}
	}
	return -1;
}

static void freeempty(int i)
{
	roottab[i].qid.type = 0;
}

static int newentry(int parent)
{
	int n = findempty();
	int sib;
	if (n < 0)
		error(EFAIL, "#root. No more");
	printd("new entry is %d\n", n);
	/* add the new one to the head of the linked list.  vers is 'next' */
	roottab[n].qid.vers = rootdata[parent].child;
	rootdata[parent].child = n;
	return n;
}

static int createentry(int dir, char *name, int omode, int perm)
{
	int n = newentry(dir);
	strncpy(roottab[n].name, name, sizeof(roottab[n].name));
	roottab[n].length = 0;
	roottab[n].perm = perm;
	/* vers is already properly set. */
	mkqid(&roottab[n].qid, n, roottab[n].qid.vers,
	      perm & DMDIR ? QTDIR : QTFILE);
	rootdata[n].dotdot = roottab[dir].qid.path;
	rootdata[dir].ptr = &roottab[n];
	rootdata[n].size = 0;
	rootdata[n].sizep = &rootdata[n].size;
	return n;
}

static void rootinit(void)
{
	/* brho: pretty sure this should only be run once.  putting it in attach
	 * will run it multiple times. */
	int i;
	uint32_t len;
	struct rootdata *r;
	/* this begins with the root. */
	for (i = 0;; i++) {
		r = &rootdata[i];
		if (r->sizep) {
			len = *r->sizep;
			r->size = len;
			roottab[i].length = len;
		}
		i = roottab[i].qid.vers;
		if (!i)
			break;
	}
}

static struct chan *rootattach(char *spec)
{
	struct chan *c;
	if (*spec)
		error(EINVAL, ERROR_FIXME);
	c = devattach(devname(), spec);
	mkqid(&c->qid, roottab[0].qid.path, roottab[0].qid.vers, QTDIR);
	return c;
}

static int
rootgen(struct chan *c, char *name,
		struct dirtab *tab, int nd, int s, struct dir *dp)
{
	int p, i;
	struct rootdata *r;
	int iter;
	printd("rootgen, path is %d, tap %p, nd %d s %d name %s\n", c->qid.path,
	       tab, nd, s, name);

	if (s == DEVDOTDOT) {
		p = rootdata[c->qid.path].dotdot;
		c->qid = roottab[p].qid;
		name = roottab[p].name;
		devdir(c, c->qid, name, 0, eve, 0777, dp);
		return 1;
	}

	if (c->qid.type != QTDIR) {
		/* return ourselved the first time; after that, -1 */
		if (s)
			return -1;
		tab = &roottab[c->qid.path];
		devdir(c, tab->qid, tab->name, tab->length, eve, tab->perm, dp);
		return 1;
	}

	if (name != NULL) {
		int path = c->qid.path;
		isdir(c);
		tab = &roottab[rootdata[path].child];
		/* we're starting at a directory. It might be '.' */
		for (iter = 0, i = rootdata[path].child; /* break */; iter++) {
			if (strncmp(tab->name, name, KNAMELEN) == 0) {
				printd("Rootgen returns 1 for %s\n", name);
				devdir(c, tab->qid, tab->name, tab->length, eve, tab->perm, dp);
				printd("return 1 with [%d, %d, %d]\n", dp->qid.path,
				       dp->qid.vers, dp->qid.type);
				return 1;
			}
			if (iter > rootmaxq) {
				printk("BUG:");
				dumprootdev();
				printk("name %s\n", name);
				return -1;
			}
			i = roottab[i].qid.vers;
			if (!i)
				break;
			tab = &roottab[i];
		}
		printd("rootgen: :%s: failed at path %d\n", name, path);
		return -1;
	}
	/* need to gen the file or the contents of the directory we are currently
	 * at.  but i think the tab entries are all over the place.  nd is how
	 * many entries the directory has. */
	if (s >= nd) {
		printd("S OVERFLOW\n");
		return -1;
	}
	//tab += s;	/* this would only work if our entries were contig in the tab */
	for (i = rootdata[c->qid.path].child; i; i = roottab[i].qid.vers) {
		tab = &roottab[i];
		if (s-- == 0)
			break;
	}
	if (!i) {
		printd("I OVERFLOW\n");
		return -1;
	}
	printd("root scan find returns path %p name %s\n", tab->qid.path, tab->name);
	devdir(c, tab->qid, tab->name, tab->length, eve, tab->perm, dp);
	return 1;
}

static struct walkqid *rootwalk(struct chan *c, struct chan *nc, char **name,
								int nname)
{
	uint32_t p;
	if (0){
		printk("rootwalk: c %p. :", c);
		if (nname){
			int i;
			for (i = 0; i < nname - 1; i++)
				printk("%s/", name[i]);
			printk("%s:\n", name[i]);
		}
	}
	p = c->qid.path;
	printd("Start from #%d at %p\n", p, &roottab[p]);
	return devwalk(c, nc, name, nname, &roottab[p], rootdata[p].size, rootgen);
}

static int rootstat(struct chan *c, uint8_t * dp, int n)
{
	int p = c->qid.path;
	return devstat(c, dp, n, rootdata[p].ptr, rootdata[p].size, rootgen);
}

static struct chan *rootopen(struct chan *c, int omode)
{
	int p;
	printd("rootopen: omode %o\n", omode);
	p = c->qid.path;
	return devopen(c, omode, rootdata[p].ptr, rootdata[p].size, rootgen);
}

static void rootcreate(struct chan *c, char *name, int omode, uint32_t perm)
{
	struct dirtab *r = &roottab[c->qid.path], *newr;
	struct rootdata *rd = &rootdata[c->qid.path];
	/* need to filter openmode so that it gets only the access-type bits */
	omode = openmode(omode);
	c->mode = openmode(omode);
	printd("rootcreate: c %p, name %s, omode %o, perm %x\n",
	       c, name, omode, perm);
	/* find an empty slot */
	int path = c->qid.path;
	int newfile;
	newfile = createentry(path, name, omode, perm);
	c->qid = roottab[newfile].qid;	/* need to update c */
	rd->size++;
	if (newfile > rootmaxq)
		rootmaxq = newfile;
	printd("create: %s, newfile %d, dotdot %d, rootmaxq %d\n", name, newfile,
	       rootdata[newfile].dotdot, rootmaxq);
}

/*
 * sysremove() knows this is a nop
 * 		fyi, this isn't true anymore!  they need to set c->type = -1;
 */
static void rootclose(struct chan *c)
{
}

static long rootread(struct chan *c, void *buf, long n, int64_t offset)
{
	uint32_t p, len;
	uint8_t *data;

	p = c->qid.path;
	if (c->qid.type & QTDIR) {
		return devdirread(c, buf, n, rootdata[p].ptr, rootdata[p].size,
						  rootgen);
	}
	len = rootdata[p].size;
	if (offset < 0 || offset >= len) {
		return 0;
	}
	if (offset + n > len)
		n = len - offset;
	data = rootdata[p].ptr;
	/* we can't really claim it has to be a user address. Lots of
	 * kernel things read directly, e.g. /dev/reboot, #nix, etc.
	 * Address validation should be done in the syscall layer.
	 */
	memcpy(buf, data + offset, n);
	return n;
}

/* For now, just kzmalloc the right amount. Later, we should use
 * pages so mmap will go smoothly. Would be really nice to have a
 * kpagemalloc ... barret?
 * 		we have kpage_alloc (gives a page) and kpage_alloc_addr (void*)
 */
static long rootwrite(struct chan *c, void *a, long n, int64_t off)
{
	struct rootdata *rd = &rootdata[c->qid.path];
	struct dirtab *r = &roottab[c->qid.path];

	if (off < 0)
		error(EFAIL, "rootwrite: offset < 0!");

	if (off + n > rd->size){
		void *p;
		p = krealloc(rd->ptr, off + n, MEM_WAIT);
		if (! p)
			error(EFAIL, "rootwrite: could not grow the file to %d bytes",
				  off + n);
		rd->ptr = p;
		rd->size = off + n;
	}
	assert(current);
	if (memcpy_from_user_errno(current, rd->ptr + off, a, n) < 0)
		error(EFAIL, "%s: bad user addr %p", __FUNCTION__, a);

	return n;
}

static int rootwstat(struct chan *c, uint8_t *m_buf, int m_buf_sz)
{
	struct dirtab *file = &roottab[c->qid.path];
	struct dir *dir;
	int m_sz;

	/* TODO: some security check, Eperm on error */

	/* common trick in wstats.  we want the dir and any strings in the M.  the
	 * strings are smaller than entire M (strings plus other M).  the strings
	 * will be placed right after the dir (dir[1]) */
	dir = kzmalloc(sizeof(struct dir) + m_buf_sz, MEM_WAIT);
	m_sz = convM2D(m_buf, m_buf_sz, &dir[0], (char*)&dir[1]);
	if (!m_sz) {
		kfree(dir);
		error(ENODATA, ERROR_FIXME);
	}
	/* TODO: handle more things than just the mode */
	if (!emptystr(dir->name))
		printk("[%s] attempted rename of %s to %s\n", __FUNCTION__,
		       file->name, dir->name);	/* strncpy for this btw */
	if (dir->mode != ~0UL)
		file->perm = dir->mode | (file->qid.type == QTDIR ? DMDIR : 0);
	kfree(dir);
	return m_sz;
}

struct dev rootdevtab __devtab = {
	.name = "root",
	.reset = devreset,
	.init = rootinit,
	.shutdown = devshutdown,
	.attach = rootattach,
	.walk = rootwalk,
	.stat = rootstat,
	.open = rootopen,
	.create = rootcreate,
	.close = rootclose,
	.read = rootread,
	.bread = devbread,
	.write = rootwrite,
	.bwrite = devbwrite,
	.remove = devremove,
	.wstat = rootwstat,
	.power = devpower,
	.chaninfo = devchaninfo,
};
