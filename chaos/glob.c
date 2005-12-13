/*
 */

#include <stdio.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/dir.h>
#include <errno.h>
#include <pwd.h>
#ifdef linux
#include <dirent.h>
#include <stdlib.h>
#endif

#include "FILE.h"
#define QUOTE 0200
#define TRIM 0177
#define eq(a,b) (strcmp(a, b)==0)
#define isdir(d)	((d.st_mode & S_IFMT) == S_IFDIR)

static char	**gargv;			/* Pointer to the (stack) arglist */
static int	gargc;				/* Number args in gargv */
static int	gargmax;
static short	gflag;
static int	tglob();
char **glob();
int globerr;
char *home;
struct passwd *getpwnam();
extern int errno;
char *strspl(), **copyblk(), *strend(), **blkcpy();
/*char *malloc(), *strcpy(), *strcat(), *sprintf();*/

static void ginit();
static collect(register char *as);
static void acollect();
static void sort();
static void expand();
static void matchdir();
static int execbrc();
static int match();
static int amatch();
static void Gcat(char *s1, char *s2);
static void addpath();
static void rscan(char **t, int (*f)());
static char *strspl(char *cp, char *dp);


static int	globcnt;

char	*globchars =	"`{[*?";

static char	*gpath, *gpathp, *lastgpathp;
static int	globbed;
static char	*entp;
static char	**sortbas;

#define STARTVEC	100
#define INCVEC		100

char **
glob(v)
	register char *v;
{
	char agpath[BUFSIZ];
	char *vv[2];
	vv[0] = v;
	vv[1] = 0;
	gflag = 0;
	rscan(vv, tglob);
	if (gflag == 0)
		return copyblk(vv);

	globerr = 0;
	gpath = agpath; gpathp = gpath; *gpathp = 0;
	lastgpathp = &gpath[sizeof agpath - 2];
	ginit(); globcnt = 0;
	collect(v);
	if (globcnt == 0 && (gflag&1)) {
		blkfree(gargv), gargv = 0;
		return (0);
	} else
		return (gargv = copyblk(gargv));
}

static void
ginit()
{
	char **agargv;

	if ((agargv = (char **)malloc((STARTVEC + 1) * sizeof(char *))) == (char **)0)
		fatal(NOMEM);
	gargmax = STARTVEC;
	agargv[0] = 0; gargv = agargv; sortbas = agargv; gargc = 0;
}

static
collect(as)
	register char *as;
{
	if (eq(as, "{") || eq(as, "{}")) {
		Gcat(as, "");
		sort();
	} else
		acollect(as);
}

static void
acollect(as)
	register char *as;
{
	register int ogargc = gargc;

	gpathp = gpath; *gpathp = 0; globbed = 0;
	expand(as);
	if (gargc != ogargc)
		sort();
}

static void
sort()
{
	register char **p1, **p2, *c;
	char **Gvp = &gargv[gargc];

	p1 = sortbas;
	while (p1 < Gvp-1) {
		p2 = p1;
		while (++p2 < Gvp)
			if (strcmp(*p1, *p2) > 0)
				c = *p1, *p1 = *p2, *p2 = c;
		p1++;
	}
	sortbas = Gvp;
}

static void
expand(as)
	char *as;
{
	register char *cs;
	register char *sgpathp, *oldcs;
	struct stat stb;

	sgpathp = gpathp;
	cs = as;
	if (*cs == '~' && gpathp == gpath) {
		addpath('~');
		for (cs++; letter(*cs) || digit(*cs) || *cs == '-';)
			addpath(*cs++);
		if (!*cs || *cs == '/') {
			if (gpathp != gpath + 1) {
				*gpathp = 0;
				if (gethdir(gpath + 1)) {
					(void)sprintf(errstring = errbuf,
					"Unknown user name: %s after '~'",
					gpath+1);
					globerr = IPS;
				}
				strcpy(gpath, gpath + 1);
			} else
				strcpy(gpath, home);
			gpathp = strend(gpath);
		}
	}
	while (!any(*cs, globchars) && globerr == 0) {
		if (*cs == 0) {
			if (!globbed)
				Gcat(gpath, "");
			else if (stat(gpath, &stb) >= 0) {
				Gcat(gpath, "");
				globcnt++;
			}
			goto endit;
		}
		addpath(*cs++);
	}
	oldcs = cs;
	while (cs > as && *cs != '/')
		cs--, gpathp--;
	if (*cs == '/')
		cs++, gpathp++;
	*gpathp = 0;
	if (*oldcs == '{') {
		(void)execbrc(cs, ((char *)0));
		return;
	}
	matchdir(cs);
endit:
	gpathp = sgpathp;
	*gpathp = 0;
}

static void
matchdir(pattern)
	char *pattern;
{
	struct stat stb;
	register int dirf;
	register struct direct *dp;
#if defined(BSD42) || defined(linux)
	DIR *dirp;

	dirp = opendir(gpath);
	if (dirp != NULL)
#ifdef linux
		dirf = dirfd(dirp);
#else
		dirf = ((struct DIR *)dirp)->dd_fd;
#endif
	else {
#else

	struct direct dirbuf[BUFSIZ / sizeof (struct direct)];
	int cnt;

	if ((dirf = open(gpath, 0)) < 0) {
#endif
		switch(errno) {
		case ENOENT:
			globerr = DNF;
			break;
		case EACCES:
			globerr = ATD;
			errstring = READDIR;
			break;
		case ENOTDIR:
			globerr = DNF;
			errstring = PATHNOTDIR;
			break;
		default:
			globerr = MSC;
			errstring = (char *)strerror(errno);
		}
		return;
	}
	if (fstat(dirf, &stb) < 0)
		fatal(FSTAT);
	if (!isdir(stb)) {
		globerr = DNF;
		errstring = PATHNOTDIR;
		return;
	}
#if defined(BSD42) || defined(linux)
	while ((dp = readdir(dirp)) != NULL)
#else
	while ((cnt = read(dirf, (char *)dirbuf, sizeof dirbuf)) >= sizeof dirbuf[0])
	for (dp = dirbuf, cnt /= sizeof (struct direct); cnt > 0; cnt--, dp++)
#endif
		if (dp->d_ino == 0 ||
		    dp->d_name[0] == '.' &&
		    (dp->d_name[1] == '\0' ||
		     dp->d_name[1] == '.' && dp->d_name[2] == '\0'))
			continue;
		else {
#if defined(BSD42) || defined(linux)
			if (match(dp->d_name, pattern)) {
				Gcat(gpath, dp->d_name);
#else
			char dname[DIRSIZ+1];
			strncpy(dname, dp->d_name, DIRSIZ);
			if (match(dname, pattern)) {
				Gcat(gpath, dname);
#endif
				globcnt++;
			}
			if (globerr != 0)
				goto out;
		}
out:
#if defined(BSD42) || defined(linux)
	(void)closedir(dirp);
#else
	(void)close(dirf);
#endif
}

static int
execbrc(p, s)
	char *p, *s;
{
	char restbuf[BUFSIZ + 2];
	register char *pe, *pm, *pl;
	int brclev = 0;
	char *lm, savec, *sgpathp;

	for (lm = restbuf; *p != '{'; *lm++ = *p++)
		continue;
	for (pe = ++p; *pe; pe++)
	switch (*pe) {

	case '{':
		brclev++;
		continue;

	case '}':
		if (brclev == 0)
			goto pend;
		brclev--;
		continue;

	case '[':
		for (pe++; *pe && *pe != ']'; pe++)
			continue;
		if (!*pe) {
			globerr = IWC;
			errstring = MISSBRACK;
			return (0);
		}
		continue;
	}
pend:
	if (brclev || !*pe) {
		globerr = IWC;
		errstring = "Missing } in wild card syntax";
		return (0);
	}
	for (pl = pm = p; pm <= pe; pm++)
	switch (*pm & (QUOTE|TRIM)) {

	case '{':
		brclev++;
		continue;

	case '}':
		if (brclev) {
			brclev--;
			continue;
		}
		goto doit;

	case ','|QUOTE:
	case ',':
		if (brclev)
			continue;
doit:
		savec = *pm;
		*pm = 0;
		strcpy(lm, pl);
		strcat(restbuf, pe + 1);
		*pm = savec;
		if (s == 0) {
			sgpathp = gpathp;
			expand(restbuf);
			gpathp = sgpathp;
			*gpathp = 0;
		} else if (amatch(s, restbuf))
			return (1);
		sort();
		pl = pm + 1;
		if (brclev)
			return (0);
		continue;

	case '[':
		for (pm++; *pm && *pm != ']'; pm++)
			continue;
		if (!*pm) {
			globerr = IWC;
			errstring = MISSBRACK;
			return(0);
		}
		continue;
	}
	if (brclev && globerr == 0)
		goto doit;
	return (0);
}

static int
match(s, p)
	char *s, *p;
{
	register int c;
	register char *sentp;
	char sglobbed = globbed;

/* We don't want this "feature"
	if (*s == '.' && *p != '.')
		return (0);
 */
	sentp = entp;
	entp = s;
	c = amatch(s, p);
	entp = sentp;
	globbed = sglobbed;
	return (c);
}

static int
amatch(s, p)
	register char *s, *p;
{
	register int scc;
	int ok, lc;
	char *sgpathp;
	struct stat stb;
	int c, cc;

	globbed = 1;
	for (;;) {
		scc = *s++ & TRIM;
		switch (c = *p++) {

		case '{':
			return (execbrc(p - 1, s - 1));

		case '[':
			ok = 0;
			lc = 077777;
			while (cc = *p++) {
				if (cc == ']') {
					if (ok)
						break;
					return (0);
				}
				if (cc == '-') {
					if (lc <= scc && scc <= *p++)
						ok++;
				} else
					if (scc == (lc = cc))
						ok++;
			}
			if (cc == 0) {
				globerr = IWC;
				errstring = MISSBRACK;
				return (0);
			}
			continue;

		case '*':
			if (!*p)
				return (1);
			if (*p == '/') {
				p++;
				goto slash;
			}
			s--;
			do {
				if (amatch(s, p))
					return (1);
			} while (*s++);
			return (0);

		case 0:
			return (scc == 0);

		default:
			if (c != scc)
				return (0);
			continue;

		case '?':
			if (scc == 0)
				return (0);
			continue;

		case '/':
			if (scc)
				return (0);
slash:
			s = entp;
			sgpathp = gpathp;
			while (*s)
				addpath(*s++);
			addpath('/');
			if (stat(gpath, &stb) == 0 && isdir(stb))
				if (*p == 0) {
					Gcat(gpath, "");
					globcnt++;
				} else
					expand(p);
			gpathp = sgpathp;
			*gpathp = 0;
			return (0);
		}
	}
}

static
Gmatch(s, p)
	register char *s, *p;
{
	register int scc;
	int ok, lc;
	int c, cc;

	for (;;) {
		scc = *s++ & TRIM;
		switch (c = *p++) {

		case '[':
			ok = 0;
			lc = 077777;
			while (cc = *p++) {
				if (cc == ']') {
					if (ok)
						break;
					return (0);
				}
				if (cc == '-') {
					if (lc <= scc && scc <= *p++)
						ok++;
				} else
					if (scc == (lc = cc))
						ok++;
			}
			if (cc == 0) {
				globerr = IWC;
				errstring = MISSBRACK;
				return (0);
			}
			continue;

		case '*':
			if (!*p)
				return (1);
			for (s--; *s; s++)
				if (Gmatch(s, p))
					return (1);
			return (0);

		case 0:
			return (scc == 0);

		default:
			if ((c & TRIM) != scc)
				return (0);
			continue;

		case '?':
			if (scc == 0)
				return (0);
			continue;

		}
	}
}

static void
Gcat(s1, s2)
	register char *s1, *s2;
{
	register int len = strlen(s1) + strlen(s2) + 1;

	if (gargc == gargmax) {
		char **newvec;

		if ((newvec = (char **)malloc((gargc + INCVEC + 1) * sizeof(char *))) == (char **)0)
			fatal(NOMEM);
		sortbas = newvec + (sortbas - gargv);
		(void)blkcpy(newvec, gargv);
		free(gargv);
		gargv = newvec;
		gargmax += INCVEC;
	}
	gargc++;
	gargv[gargc] = 0;
	gargv[gargc - 1] = strspl(s1, s2);
}

static void
addpath(c)
	char c;
{

	if (gpathp >= lastgpathp) {
		globerr = MSC;
		errstring = "Pathname too long";
	} else {
		*gpathp++ = c;
		*gpathp = 0;
	}
}

static void
rscan(t, f)
	register char **t;
	int (*f)();
{
	register char *p, c;

	while (p = *t++) {
		if (f == tglob)
			if (*p == '~')
				gflag |= 2;
			else if (eq(p, "{") || eq(p, "{}"))
				continue;
		while (c = *p++)
			(*f)(c);
	}
}
/*
static
scan(t, f)
	register char **t;
	int (*f)();
{
	register char *p, c;

	while (p = *t++)
		while (c = *p)
			*p++ = (*f)(c);
}
*/
static
tglob(c)
	register char c;
{

	if (any(c, globchars))
		gflag |= c == '{' ? 2 : 1;
	return (c);
}
/*
static
trim(c)
	char c;
{

	return (c & TRIM);
}
*/

letter(c)
	register char c;
{

	return (c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c == '_');
}

digit(c)
	register char c;
{

	return (c >= '0' && c <= '9');
}

any(c, s)
	register int c;
	register char *s;
{

	while (*s)
		if (*s++ == c)
			return(1);
	return(0);
}
blklen(av)
	register char **av;
{
	register int i = 0;

	while (*av++)
		i++;
	return (i);
}

char **
blkcpy(oav, bv)
	char **oav;
	register char **bv;
{
	register char **av = oav;

	while (*av++ = *bv++)
		continue;
	return (oav);
}

blkfree(av0)
	char **av0;
{
	register char **av = av0;

	while (*av)
		free(*av++);
	free((char *)av0);
}

static char *
strspl(char *cp, char *dp)
{
	register char *ep = malloc((unsigned)(strlen(cp) + strlen(dp) + 1));

	if (ep == (char *)0)
		fatal(NOMEM);
	strcpy(ep, cp);
	strcat(ep, dp);
	return (ep);
}

char **
copyblk(v)
	register char **v;
{
	register char **nv = (char **)malloc((unsigned)((blklen(v) + 1) *
						sizeof(char **)));
	if (nv == (char **)0)
		fatal(NOMEM);

	return (blkcpy(nv, v));
}

static
char *
strend(cp)
	register char *cp;
{

	while (*cp)
		cp++;
	return (cp);
}
/*
 * Extract a home directory from the password file
 * The argument points to a buffer where the name of the
 * user whose home directory is sought is currently.
 * We write the home directory of the user back there.
 */
gethdir(home)
	char *home;
{
	register struct passwd *pp = getpwnam(home);

	if (pp == 0)
		return (1);
	strcpy(home, pp->pw_dir);
	return (0);
}
