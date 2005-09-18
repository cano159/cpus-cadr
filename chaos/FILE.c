/*
 * Adapted from MIT UNIX chaosnet code
 * 10/2004 brad@heeltoe.com
 */

/* NOTES
 * 7/5/84 -Cory Myers
 * changed creat mode to 0644 - no group or other write from 0666
 * fixed filepos to adjust for bytesize
 *
 * 9/11/84 dove
 *	finger users to console
 *
 * 9/20/84 Cory Myers
 *	Login users must have passwords
 *
 * 1/30/85 dove
 *	use initgroups() not setgid() in BSD42
 *
 * 4/29/85 Cory Myers
 *      allow logins without passwords from privileged hosts
 *
 * 8/23/85 dove (was FILE.c4)
 *  finger users to logger (a.k.a. syslog) instead of to console
 *
 * 3/08/88 Greg Troxel
 *      ifdef out cory's code of 4/29/85 for privileged hosts
 *	Conditional on PRIVHOSTS
 */
#include <stdio.h>
#include <ctype.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/timeb.h>

#ifdef BSD42
#include <sys/time.h>
#include <sys/timeb.h>
/* #include <sys/fs.h> */
/*#include <ufs/fs.h>*/
#include <ufs/ffs/fs.h>
#include <sys/dir.h>
#define FSBSIZE MAXBSIZE
#else
#include <time.h>
#include <sys/dir.h>
#ifdef linux
#include <sys/vfs.h>
#include <string.h>
#define BSIZE 512
#define SBLOCK 8
#define FSBSIZE BSIZE
#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

#define _XOPEN_SOURCE
#include <unistd.h>
#include <stdlib.h>

#else
#include <sys/filsys.h>
#define FSBSIZE BSIZE
#endif
#endif

#include <errno.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
//#include <sys/chaos.h>
#include "chaos.h"
#define DEFERROR
#include "FILE.h"
#ifdef SYSLOG
#include <syslog.h>
#else
#define LOG_INFO	0
#define LOG_ERR		1
#define LOG_NOTICE	/* 7 */ 0
/* VARARGS2 */
syslog(n,a,b,c,d,e) char *a;{if (n) fprintf(stderr,a,b,c,d,e);}
#endif
#ifdef pdp11
#ifndef lint
#ifndef void
#define void int
#endif
#endif
#endif
#define tell(fd)	lseek(fd, (off_t)0, SEEK_CUR)

#define FILEUTMP "/tmp/chfile.utmp"

/*
 * Chaosnet file protocol server.  Written by JEK, Symbolics
 * TODO:
	Performance hacking.
	More strict syntax checking in string()
	System notifications?
	Quoting, syntax checking
	Subroutines for error processing?
	Check for default match in prefix() to clean up complete().
	Cacheing user-id's for faster directory list etc.
 */

#define	BIT(n)	(1<<(n))
#define NOLOGIN	"/etc/nologin"
/*
 * Structure describing each transfer in progress.
 * Essentially each OPEN or DIRECTORY command creates a transfer
 * task, which runs (virtually) asynchronously with respect to the
 * control/command main task.  Certain transactions/commands
 * are performed synchronously with the control/main process, like
 * OPEN-PROBES, completion, etc., while others relate to the action
 * of a transfer process (e.g. SET-BYTE-POINTER).
 * Until the Berkeley select mechanism is done, we must implement transfer
 * tasks as sub-processes.
 * The #ifdef SELECT code is for when that time comes (untested, incomplete).
 */
struct xfer		{
	struct xfer		*x_next;	/* Next in global list */
	struct xfer		*x_runq;	/* Next in runnable list */
	char			x_state;	/* TASK state */
	struct file_handle	*x_fh;		/* File handle of process */
	struct transaction	*x_close;	/* Saved close transaction */
	int			x_bytesize;	/* Bytesize for binary xfers */
	long			x_options;	/* OPEN options flags */
	int			x_flags;	/* Randome state bits */
	time_t			x_atime;	/* Access time to restore */
	time_t			x_mtime;	/* Mod time to restore */
	int			x_fd;		/* File descriptor of file
						   being read or written */
	char			*x_dirname;	/* dirname of filename */
	char			*x_realname;	/* filename */
	char			*x_tempname;	/* While writing */
	int			x_left;		/* Bytes in input buffer */
	int			x_room;		/* Room in output buffer */
	char			*x_bptr;	/* Disk buffer pointer */
	char			*x_pptr;	/* Packet buffer pointer */
	char			x_bbuf[FSBSIZE];	/* Disk buffer */
	struct chpacket		x_pkt;		/* Packet buffer */
#define	x_op			x_pkt.cp_op	/* Packet buffer opcode */
#define x_pbuf			x_pkt.cp_data	/* Packet data buffer */
	char			**x_glob;	/* Files for DIRECTORY */
	char			**x_gptr;	/* Ptr into x_glob vector */
#ifdef SELECT
	struct transaction	*x_work;	/* Queued transactions */
#else
	int			x_pfd;		/* Subprocess pipe file */
	int			x_pid;		/* Subprocess pid */
#endif
} *xfers;

#define XNULL	((struct xfer *)0)
/*
 * Values for x_state.
 */
#define	X_PROCESS	0	/* Work is in process - everything is ok */
#define X_DONE		1	/* Successful completion has occurred */
				/* Waiting to close normally */
/*
 * States for the input side only.
 */
#define X_BROKEN	2	/* Data connection disappeared on READ */
#define X_ABORT		3	/* Fatal error in reading. Awaiting CLOSE */
#define X_REOF		4	/* Read an EOF and sent all the data */
#define X_SEOF		5	/* Sent an EOF, awaiting FILEPOS or CLOSE */
#define X_IDLE		6	/* Waiting to start */
#define X_DERROR	7	/* Directory list error */
/*
 * States for the output side only.
 */
#define	X_RSYNC		8	/* Received SYNCMARK, waiting for CLOSE */
#define	X_WSYNC		9	/* Transfer punted or EOF, waiting for
				   SYNCMARK */
#define X_ERROR		10	/* Hung after recoverable error */
				/* Waiting to continue or close abnormally */
#define X_RETRY		11	/* After a continue, waiting to retry the
				   recoverable operation. */
/*
 * Values for x_flags
 */
#define X_EOF		BIT(1)	/* EOF has been read from network */
#define X_QUOTED	BIT(2)	/* Used in character set translation */
#define	X_CLOSE		BIT(3)	/* CLOSE command received */
#define X_DELETE	BIT(4)	/* File deleted (close is an abort) */
#define X_ATIME		BIT(5)	/* Change access time at close */
#define X_MTIME		BIT(6)	/* Change mod time at close */
/*
 * Structure describing each issued command.
 * Created by the command parser (getcmd) and extant while the command is
 * in progress until it is done and responded to.
 */
struct transaction	{
	struct transaction	*t_next;	/* For queuing work on xfers*/
	char			*t_tid;		/* Id. for this transaction */
	struct file_handle	*t_fh;		/* File handle to use */
	struct command		*t_command;	/* Command to perform */
	struct cmdargs		*t_args;	/* Args for this command */
};
#define	TNULL	((struct transaction *) 0)

/*
 * Structure for each possible command.
 * Used by the parser for argument syntax and holds the actual function
 * which performs the work.
 */
struct command		{
	char			*c_name;	/* Command name */
	int			(*c_func)();	/* Function to call */
	int			c_flags;	/* Various bits. See below */
	char			*c_syntax;	/* Syntax description */
};
#define	CNULL	((struct command *)0)
/*
 * Bit values for c_flags
 */
#define	C_FHMUST	BIT(0)	/* File handle must be present */
#define	C_FHCANT	BIT(1)	/* File handle can't be present */
#define C_FHINPUT	BIT(2)	/* File handle must be for input */
#define C_FHOUTPUT	BIT(3)	/* File handle must be for output */
#define C_XFER		BIT(4)	/* Command should be queued on a transfer */
#define C_NOLOG		BIT(5)	/* Command permitted when not logged in */
struct plist	{
	struct plist	*p_next;
	char		*p_name;
	char		*p_value;
};
#define PNULL	((struct plist *)0)

/*
 * Structure for each "file handle", essentially one side of a
 * bidirectional chaos "data connection".
 */
struct file_handle	{
	struct file_handle	*f_next;	/* Next on global list */
	char			*f_name;	/* Identifier from user end */
	int			f_type;		/* See below */
	int			f_fd;		/* UNIX file descriptor */
	struct xfer		*f_xfer;	/* Active xfer on this fh */
	struct file_handle	*f_other;	/* Other fh of in/out pair */
} *file_handles;
#define	FNULL	((struct file_handle *)0)
/*
 * Values for f_type
 */
#define	F_INPUT		0	/* Input side of connection */
#define F_OUTPUT	1	/* Output size of connection */

/*
 * Command options.
 */
struct option	{
	char	*o_name;	/* Name of option */
	char	o_type;		/* Type of value */
	long	o_value;	/* Value of option */
	long	o_cant;		/* Bit values of mutually exclusive options*/
};
/*
 * Values for o_type
 */
#define	O_BIT		0	/* Value is bit to turn on in a_options */
#define O_NUMBER	1	/* Value is (SP, NUM) following name */
#define O_EXISTS	2	/* Existence checking keywords */

/*
 * Values for o_value for O_BIT
 */
#define	O_READ		BIT(0)	/* An open is for READing a file */
#define	O_WRITE		BIT(1)	/* An open is for WRITEing a file */
#define	O_PROBE		BIT(2)	/* An open is for PROBEing for existence */
#define	O_CHARACTER	BIT(3)	/* Data will be character (lispm) */
#define	O_BINARY	BIT(4)	/* Binary data */
#define	O_RAW		BIT(5)	/* RAW character transfer - no translation */
#define	O_SUPER		BIT(6)	/* Super-image mode */
#define	O_TEMPORARY	BIT(7)	/* An open file should be temporary */
#define	O_DELETED	BIT(8)	/* Include DELETED files in DIRECTORY list */
#define	O_OLD		BIT(9)	/* Complete for an existing file in COMPLETE */
#define	O_NEWOK		BIT(10)	/* Complete for a possible new file */
#define O_DEFAULT	BIT(11)	/* Choose character unless QFASL */
#define O_DIRECTORY	BIT(12)	/* Used internally - not a keyword option */
#define O_FAST		BIT(13)	/* Fast directory list - no properties */
#define O_PRESERVE	BIT(14)	/* Preserve reference dates - not implemented */
#define O_SORTED	BIT(15) /* Return directory list sorted - we do this */
#define O_PROBEDIR	BIT(16) /* Probe is for the directory, not the file */
#define O_PROPERTIES	BIT(17)	/* To distinguish PROPERTIES from DIRECTORY internally */
/*
 * Values for o_value for O_NUMBER
 */
#define O_BYTESIZE	0
#define O_ESTSIZE	1
#define O_MAXNUMS	2
/*
 * Values for o_value for O_EXISTS
 */
#define O_IFEXISTS	0
#define O_IFNEXISTS	1
#define O_MAXEXS	2
/*
 * Values for O_EXISTS keywords
 */
#define O_XNONE		0	/* Unspecified */
#define O_XNEWVERSION	1	/* Increment version before writing new file */
#define O_XRENAME	2	/* Rename before writing new file */
#define O_XRENDEL	3	/* Rename, then delete before writing new file */
#define O_XOVERWRITE	4	/* Smash the existing file */
#define O_XAPPEND	5	/* Append to existing file */
#define O_XSUPERSEDE	6	/* Don't clobber existing file until non-abort close */
#define O_XCREATE	7	/* Create file is it doesn't exists */
#define O_XERROR	8	/* Signal error */
#define O_XTRUNCATE	9	/* Truncate as well as overwrite on open */
/*
 * Structure of arguments to commands.
 */
#define MAXSTRINGS	3	/* Maximum # of string arguments in any cmd */
struct cmdargs	{
	long			a_options;		/* Option bits */
	char			*a_strings[MAXSTRINGS];/* Random strings */
	char			a_exists[O_MAXEXS];
	long			a_numbers[O_MAXNUMS];	/* Random numbers */
	struct plist		*a_plist;		/* Property list */
};
#define ANULL	((struct cmdargs *)0)
/*
 * String names for above options
 */
struct xoption {
	char	*x_name;
	long	x_value;
	long	x_cant;		/* Which O_EXISTS value can't occur. */
} xoptions[] = {
{	"ERROR",		O_XERROR,	O_MAXEXS,	},
{	"NEW-VERSION",		O_XNEWVERSION,	O_IFNEXISTS,	},
{	"RENAME",		O_XRENAME,	O_IFNEXISTS,	},
{	"RENAME-AND-DELETE",	O_XRENDEL,	O_IFNEXISTS,	},
{	"OVERWRITE",		O_XOVERWRITE,	O_IFNEXISTS,	},
{	"APPEND",		O_XAPPEND,	O_IFNEXISTS,	},
{	"SUPERSEDE",		O_XSUPERSEDE,	O_IFNEXISTS,	},
{	"CREATE",		O_XCREATE,	O_IFEXISTS,	},
{	"TRUNCATE",		O_XTRUNCATE,	O_IFNEXISTS,	},
{	0,			0,		0,		},
};
/*
 * Structure allocation macros.
 */
#define	salloc(s)	(struct s *)malloc(sizeof(struct s))
#define sfree(sp)	free((char *)sp)
#define NOSTR		((char *)0)
#define NOBLK		((char **)0)
/*
 * Return values from dowork indicating the disposition of the transfer task.
 */
#define X_FLUSH		0	/* Flush the transfer, its all over */
#define X_CONTINUE	1	/* Everything's ok, just keep going */
#define X_HANG		2	/* Wait for more commands before continuing */
#define X_SYNCMARK	3	/* Flush after sending a syncmark */
/*
 * Constants of the protocol
 */
#define	TIDLEN		5		/* Significant length of tid's */
#define FHLEN		5		/* Significant length of fh's */
#define SYNOP		0201		/* Opcode for synchronous marks */
#define ASYNOP		0202		/* Opcode for asynchronous marks */
#define FALSE		"NIL"		/* False value in binary options */
#define TRUE		"T"		/* True value in binary options */
#define QBIN1		((unsigned)0143150)		/* First word of "QFASL" in SIXBIT */
#define QBIN2		071660		/* Second word of "QFASL" */
#define LBIN1		((unsigned)0170023)		/* First word of BIN file */
#define LBIN2LIMIT	100		/* Maximum value of second word of BIN file */

#ifndef SELECT
/*
 * Definitions for communication between the control process and transfer
 * processes.  YICK.
 */
/*
 * The structure in which a command is sent to a transfer process.
 */
struct pmesg {
	char		pm_tid[TIDLEN + 1];	/* TIDLEN chars of t->t_tid */
	struct command	*pm_cmd;		/* Actual t_command */
	char		pm_args;		/* 0 = no args, !0 = args */
	long		pm_n;			/* copy of t_args->a_numbers[0] */
	unsigned	pm_strlen;		/* length of string arg. */
};
int ctlpipe[2];					/* Pipe - xfer proc to ctl */
						/* Just PID's are written */
jmp_buf closejmp;
void interrupt(int arg);
struct xfer *myxfer;
int nprocdone;					/* Number of processes done */
#endif

/*
 * Definition of options
 */
struct option options[] = {
/*	o_name		o_type		o_value		o_cant	 */
{	"READ",		O_BIT,		O_READ,		O_WRITE|O_PROBE	},
{	"WRITE",	O_BIT,		O_WRITE,	O_READ|O_PROBE	},
{	"PROBE",	O_BIT,		O_PROBE,	O_READ|O_WRITE	},
{	"CHARACTER",	O_BIT,		O_CHARACTER,	O_BINARY|O_DEFAULT},
{	"BINARY",	O_BIT,		O_BINARY,	O_CHARACTER|O_DEFAULT},
{	"BYTE-SIZE",	O_NUMBER,	O_BYTESIZE,	O_CHARACTER	},
{	"RAW",		O_BIT,		O_RAW,		O_BINARY|O_SUPER},
{	"SUPER-IMAGE",	O_BIT,		O_SUPER,	O_BINARY|O_RAW	},
{	"TEMPORARY",	O_BIT,		O_TEMPORARY,	0},
{	"DELETED",	O_BIT,		O_DELETED,	0},
{	"OLD",		O_BIT,		O_OLD,		O_NEWOK	},
{	"NEW-OK",	O_BIT,		O_NEWOK,	O_OLD	},
{	"DEFAULT",	O_BIT,		O_DEFAULT,	O_BINARY|O_CHARACTER|O_PROBE|O_WRITE},
{	"FAST",		O_BIT,		O_FAST,		0},
{	"SORTED",	O_BIT,		O_SORTED,	0},
{	"PRESERVE-DATES",O_BIT,		O_PRESERVE,	0},
{	"INHIBIT-LINKS",O_BIT,		0,		0},
{	"PROBE-DIRECTORY",O_BIT,	O_PROBEDIR,	O_READ|O_WRITE },
{	"ESTIMATED-LENGTH",O_NUMBER,	O_ESTSIZE,	O_READ|O_PROBE },
{	"IF-EXISTS",	O_EXISTS,	O_IFEXISTS,	O_READ|O_PROBE },
{	"IF-DOES-NOT-EXIST",O_EXISTS,	O_IFNEXISTS,	O_READ|O_PROBE },
{	"NO-EXTRA-INFO",O_BIT,		0,		0},
{	NOSTR,	},
};

/*
 * Syntax definition - values for syntax strings
 */
#define	SP	1	/* Must be CHSP character */
#define	NL	2	/* Must be CHNL character */
#define	STRING	3	/* All characters until following character */
#define OPTIONS	4	/* Zero or more (SP, WORD), WORD in options */
#define NUMBER	5	/* [-]digits, base ten */
#define OPEND	6	/* Optional end of command */
#define PNAME	7	/* Property name */
#define PVALUE	8	/* Property value */
#define REPEAT	9	/* Repeat from last OPEND */
#define END	10	/* End of command, must be at end of args */
#define FHSKIP	11	/* Skip next if FH present - kludge for change properties */

char	dcsyn[] =	{ STRING, SP, STRING, END };
char	nosyn[] =	{ END };
char	opensyn[] =	{ OPTIONS, NL, STRING, NL, END };
char	possyn[] =	{ NUMBER, END };
char	delsyn[] =	{ OPEND, NL, STRING, NL, END };
char	rensyn[] =	{ NL, STRING, NL, OPEND, STRING, NL, END };
/*char	setsyn[] =	{ NUMBER, SP, OPEND, NUMBER, END };*/
char	logsyn[] =	{ OPEND, STRING, OPEND, SP, STRING, OPEND,
			  SP, STRING, END };
char	dirsyn[] =	{ OPTIONS, NL, STRING, NL, END };
char	compsyn[] =	{ OPTIONS, NL, STRING, NL, STRING, NL, END };
char	changesyn[] =	{ NL, FHSKIP, 2, STRING, NL, OPEND,
			  PNAME, SP, PVALUE, NL, REPEAT };
char	crdirsyn[] =	{ NL, STRING, NL, END };
char	expsyn[] =	{ NL, STRING, NL, END };
char	propsyn[] =	{ STRING, OPEND, NL, STRING, NL, END };
/*char	crlsyn[] =	{ NL, STRING, NL, STRING, END };	*/
/*
 * Command definitions
 */
int	dataconn(), undataconn(), fileopen(), fileclose(), filepos(),
	delete(), xrename(), filecontinue(), setbytesize(), login(),
	directory(), complete(), chngprop(), crdir(), expunge(),
	getprops()
	;
struct command commands[] = {
/*	c_name			c_funct		c_flags		c_syntax */
{	"DATA-CONNECTION",	dataconn,	C_FHCANT|C_NOLOG,dcsyn	},
{	"UNDATA-CONNECTION",	undataconn,	C_FHMUST|C_NOLOG,nosyn	},
{	"OPEN",			fileopen,	0,		opensyn	},
{	"CLOSE",		fileclose,	C_XFER|C_FHMUST,nosyn	},
{	"FILEPOS",		filepos,	C_XFER|C_FHINPUT,possyn	},
{	"DELETE",		delete,		0,		delsyn	},
{	"RENAME",		xrename,	0,		rensyn	},
{	"CONTINUE",		filecontinue,	C_XFER|C_FHMUST,nosyn	},
{	"EXPUNGE",		expunge,	C_FHCANT,	expsyn	},
/*{	"SET-BYTE-SIZE",	setbytesize,	C_XFER|C_FHINPUT,setsyn	},*/
/*{	"ENABLE-CAPABILITIES", etc */
/*{	"DISABLE-CAPABILITIES", etc */
{	"LOGIN",		login,		C_FHCANT|C_NOLOG,logsyn	},
{	"DIRECTORY",		directory,	C_FHINPUT,	dirsyn	},
{	"COMPLETE",		complete,	C_FHCANT,	compsyn	},
{	"CHANGE-PROPERTIES",	chngprop,	0,		changesyn },
{	"CREATE-DIRECTORY",	crdir,		C_FHCANT,	crdirsyn },
{	"PROPERTIES",		getprops,	C_FHINPUT,	propsyn },
{	NOSTR	},
};
/*
 * Kludge for actually responding to the DIRECTORY command in the subtask.
 */
int diropen();
struct command dircom = {
	"DIRECTORY",	diropen,	0,		0
};
int propopen();
struct command propcom = {
	"PROPERTIES",	propopen,	0,		0
};
/*
 * The following three kludges are for communicating information to the
 * transfer process about what has happened in the top level process.
 * We don't pass the whole command down since it then would happen right.
 * Boy, what I wouldn't do for asynchronous i/o.
 */
int delfh(), renfh(), mdatfh(), adatfh();
struct command
	delcom = {
	"foo", delfh, 0, 0
	}, rencom = {
	"bar", renfh, 0, 0
	}, mdatcom = {
	"baz", mdatfh, 0, 0
	}, adatcom = {
	"quux", adatfh, 0, 0
};
/*
 * Property definitions for change-properties.
 */
char	*getname(), *getdev(), *getprot(), *getblock(), *getspace(),
	*getbyte(), *getbsize(), *getsize(), *getcdate(), *getmdate(),
	*getrdate(), *getsprops(), *getdir();
int 	putname(), putprot(), putmdate(), putrdate();

#define P_GET		1
#define P_DELAY		2
#define P_GLOBAL	4

struct property {
	char	*p_indicator;	/* Property name */
	int	p_flags;	/* Attributes of this property */
	char	*(*p_get)();	/* Function to get the property */
	int	(*p_put)();	/* Function to set the property */
} properties[] = {
/*	name				flags						 */
{	"AUTHOR",			P_GET,		getname,	putname,	},
{	"PHYSICAL-VOLUME",		P_GET,		getdev,		0,		},
{	"PROTECTION",			P_GET,		getprot,	putprot,	},
{	"DISK-SPACE-DESCRIPTION",	P_GLOBAL,	getspace,	0,		},
{	"BLOCK-SIZE",			P_GLOBAL,	getblock,	0,		},
{	"BYTE-SIZE",			P_GET,		getbyte,	0,		},
{	"LENGTH-IN-BLOCKS",		P_GET,		getbsize,	0,		},
{	"LENGTH-IN-BYTES",		P_GET,		getsize,	0,		},
{	"CREATION-DATE",		P_GET,		getmdate,	putmdate,	},
{	"MODIFICATION-DATE",		P_GET|P_DELAY,	getmdate,	putmdate,	},
{	"REFERENCE-DATE",		P_GET|P_DELAY,	getrdate,	putrdate,	},
{	"SETTABLE-PROPERTIES",		P_GLOBAL,	getsprops,	0,		},
/*{	"PHYSICAL-VOLUME-FREE-BLOCKS",	P_GLOBAL,	getfree,	0,		},*/
{	"DIRECTORY",			P_GET,		getdir,		0,		},
{	0,				0,		0,		0,		},
};
/*
 * Globals
 */
struct chstatus chst;		/* Status buffer for connections */
struct stat sbuf;		/* Used so often, made global */
char errtype;			/* Error type if not E_COMMAND */
char *errstring;		/* Error message if non-standard */
char errbuf[ERRSIZE + 1];	/* Buffer for building error messages */
char *home;			/* Home directory, after logged in */
char *cwd;			/* Current working directory, if one */
int protocol;			/* Which number argument after FILE in RFC */
int mypid;			/* Pid of controlling process */
struct timeb timeinfo;
struct chlogin mylogin;		/* Record for login */
extern int errno;		/* System call error code */

/*
 * Non-integer return types from functions
 */
/*char *savestr(), *malloc(), *strcpy(), *sprintf(), *rindex(), *strcat();*/
char *savestr();
char *fullname(), *crypt(), *downcase();
/*long lseek(), fseek(), tell();*/
struct passwd *getpwnam(), *getpwuid();
char *tempfile();
struct tm *localtime();
struct transaction *getwork(), *makecmd();
char **glob();
struct xfer *makexfer();
void finish(int arg);

/*
 * This server gets called from the general RFC server, who gives us control
 * with standard input and standard output set to the control connection
 * and with the connection open (already accepted)
 */
main(argc, argv)
int argc;
char **argv;
{
	register struct transaction *t;
	struct transaction *getwork();
	register int ufd;
	static char ebuf[BUFSIZ];

	ftime(&timeinfo);
#ifndef SYSLOG
	(void)close(2);
	(void)open("FILE.log", 1);
	(void)lseek(2, 0L, 2);
	setbuf(stderr, ebuf);
#endif
	if (argc > 1)
		protocol = atoi(argv[1]);
	if (protocol > 1) {
		syslog(LOG_ERR, "FILE: protocol I can't handle: %d\n",
			protocol);
		chreject(0, "Unknown FILE protocol version");
		exit(1);
	}

//	ioctl(0, CHIOCACCEPT, NOSTR);
	mypid = getpid();
//	ioctl(0, CHIOCSMODE, (char *)CHRECORD);
//	ioctl(0, CHIOCGSTAT, (char *)&chst);
	mylogin.cl_atime = timeinfo.time;
	mylogin.cl_ltime = timeinfo.time;
	mylogin.cl_cnum = chst.st_cnum;
	mylogin.cl_haddr = chst.st_fhost;
	mylogin.cl_pid = mypid;
	strncpy(mylogin.cl_user, "no-login", sizeof(mylogin.cl_user));
	if ((ufd = open(FILEUTMP, 1)) >= 0) {
		(void)lseek(ufd,
			    (long)(mylogin.cl_cnum * sizeof(struct chlogin)),
			    0);
		(void)write(ufd, (char *)&mylogin, sizeof(mylogin));
		(void)close(ufd);
	}
	(void)signal(SIGTERM, finish);
	(void)signal(SIGHUP, SIG_IGN);
	while (t = getwork())
		if (t->t_command->c_flags & C_XFER)
			xcommand(t);
		else
			(*t->t_command->c_func)(t);
	syslog(LOG_INFO, "FILE: exiting normally\n");
	finish(0);
}
/*
 * Getwork - read a command from the control connection (standard input),
 *	parse it, including all arguments (as much as possible),
 *	and do any possible error checking.  Any errors cause rejection
 *	here, and such commands are not returned.
 */
 struct transaction *
getwork()
{
	register struct transaction *t;
	register struct command *c;
	register unsigned char *cp;
	register int length;
	char *fhname, *cname, save;
	int errcode;
	struct chpacket p;
	
	for (;;) {
		if ((t = salloc(transaction)) == TNULL)
			fatal(NOMEM);

		tinit(t);
		fhname = "";
		errcode = BUG;		/* Default error is bad syntax */

#ifndef SELECT
		xcheck();
#endif
		length = read(0, (char *)&p, sizeof(p));
#ifndef SELECT
		xcheck();
#endif
		if (length <= 0) {
			syslog(LOG_INFO, "FILE: Ctl connection broken(%d,%d)\n",
				length, errno);
			tfree(t);
			return (TNULL);
		}

		((char *)&p)[length] = '\0';
#if 0
		syslog(LOG_INFO, "FILE: pkt(%d):%.*s\n", p.cp_op&0377, length-1,
			p.cp_data);
#endif
		switch(p.cp_op) {
		case EOFOP:
			tfree(t);
			return TNULL;
		case CLSOP:
		case LOSOP:
			syslog(LOG_INFO, "FILE: Close pkt: '%s'\n", p.cp_data);
			tfree(t);
			return TNULL;
		case RFCOP:
			continue;
		case DATOP:
			break;
		default:
			syslog(LOG_ERR, "FILE: Bad op: 0%o\n", p.cp_op);
			tfree(t);
			return TNULL;
		}
		for (cp = (unsigned char *)p.cp_data; *cp != CHSP; )
			if (*cp++ == '\0') {
				errstring = "Missing transaction id";
				goto cmderr;
			}
		*cp = '\0';
		t->t_tid = savestr(p.cp_data);
		if (*++cp != CHSP) {
			struct file_handle *f;

			for (fhname = (char *)cp; *++cp != CHSP; )
				if (*cp == '\0') {
					errstring =
						"No command after file handle";
					goto cmderr;
				}
			*cp = '\0';
			for (f = file_handles; f; f = f->f_next)
				if (strncmp(fhname, f->f_name, FHLEN) == 0)
					break;
			if (f == FNULL) {
				(void)sprintf(errstring = errbuf,
					"Unknown file handle: %s", fhname);
				goto cmderr;
			}
			t->t_fh = f;
		}
		for (cname = (char *)++cp;
		     *cp != '\0' && *cp != CHSP && *cp != CHNL;)
			cp++;
		save = *cp;
		*cp = '\0';
		if (*cname == '\0') {
			errstring = "Null command name";
			goto cmderr;
		}
syslog(LOG_INFO, "FILE: command %s\n", cname);
		for (c = commands; c->c_name; c++)
			if (strcmp(cname, c->c_name) == 0)
				break;
		if (c == CNULL) {
			(void)sprintf(errstring = errbuf,
				"Unknown command: %s", cname);
			errcode = UKC;
			goto cmderr;
		}
		if (home == NOSTR && (c->c_flags & C_NOLOG) == 0) {
			errcode = NLI;
			goto cmderr;
		}
		t->t_command = c;
		*cp = save;
		if (t->t_fh == FNULL) {
			if (c->c_flags & (C_FHMUST|C_FHINPUT|C_FHOUTPUT)) {
				(void)sprintf(errstring = errbuf,
					"Missing file handle on command: %s",
					cname);
				goto cmderr;
			}
		} else if (c->c_flags & C_FHCANT) {
			(void)sprintf(errstring = errbuf,
					"File handle present on command: %s",
					cname);
			goto cmderr;
		} else if ((c->c_flags & C_FHINPUT &&
			   t->t_fh->f_type != F_INPUT) ||
			  (c->c_flags & C_FHOUTPUT &&
			   t->t_fh->f_type != F_OUTPUT)) {
			(void)sprintf(errstring = errbuf,
			"Wrong direction file handle (%s) for command: %s",
				fhname, cname);
			goto cmderr;
		}
		if (*cp == ' ')
			cp++;
		if ((errcode = parseargs(cp, c, t)) == 0)
			break;
	cmderr:
syslog(LOG_INFO, "FILE: %s\n", errstring);
		error(t, fhname, errcode);
	}
	return t;
}
/*
 * Parse the argument part of the command, returning an error code if
 * an error was found.
 */
parseargs(args, c, t)
unsigned char *args;
struct command *c;
register struct transaction *t;
{
	register char *syntax;
	register struct cmdargs *a;
	struct plist *p;
	struct option *o;
	int nnums, nstrings, errcode;
	long n;

	t->t_args = ANULL;
	if (c->c_syntax[0] == END)
		if (args[0] == '\0')
			return 0;
		else {
			errstring = "Arguments present where none expected";
			return BUG;
		}
	if ((a = salloc(cmdargs)) == ANULL)
		fatal(NOMEM);
	/* From here on, goto synerr for errors so A gets freed */
	errcode = BUG;
	ainit(a);
	nstrings = nnums = 0;
	for (syntax = c->c_syntax; *syntax != END; ) {
		switch (*syntax++) {
		case FHSKIP:
			if (t->t_fh != FNULL)
				syntax += *syntax + 1;
			else
				syntax++;
			continue;
		case SP:
			if (*args++ != CHSP) {
				(void)sprintf(errstring = errbuf,
				"Space expected where 0%o (octal) occurred",
					args[-1]);
				goto synerr;
			}
			continue;
		case NL:
			if (*args++ != CHNL) {
				(void)sprintf(errstring = errbuf,
				"Newline expected where 0%o (octal) occurred",
					args[-1]);
				goto synerr;
			}
			continue;
		case OPEND:
			if (*args == '\0')
				break;
			continue;
		case REPEAT:
			while (*--syntax != OPEND)
				;
			continue;
		case NUMBER:
			if (!isdigit(*args)) {
				(void)sprintf(errstring = errbuf,
				"Digit expected where 0%o (octal) occurred",
					args[0]);
				goto synerr;
			}
			n = 0;
			while (isdigit(*args)) {
				n *= 10;
				n += *args++ - '0';
			}
			a->a_numbers[nnums++] = n;
			continue;
		case STRING:
			if (errcode = string(&args, syntax,
					     &a->a_strings[nstrings++]))
				goto synerr;
			continue;
		case PNAME:
			if ((p = a->a_plist) != PNULL && p->p_value == NOSTR) {
				errstring =
				"Property name expected when already given";
				goto synerr;
			}
			if ((p = salloc(plist)) == PNULL)
				fatal(NOMEM);
			if (errcode = string(&args, syntax, &p->p_name)) {
				sfree(p);
				goto synerr;
			}
			p->p_next = a->a_plist;
			a->a_plist = p;
			p->p_value = NOSTR;
			continue;
		case PVALUE:
			if ((p = a->a_plist) == PNULL || p->p_value != NOSTR) {
				errstring =
				"Property value expected when no name given";
				goto synerr;
			}
			if (errcode = string(&args, syntax, &p->p_value))
				goto synerr;
			continue;
		case OPTIONS:
			while (*args != '\0' && *args != CHNL) {
				char *oname;

				while (*args == ' ')
					args++;
				if (errcode = string(&args, "", &oname))
					goto synerr;
				if (*oname == '\0')
					continue;
				for (o = options; o->o_name; o++)
					if (strcmp(oname, o->o_name) == 0)
						break;
				free(oname);
				if (o->o_name == NOSTR) {
					syslog(LOG_ERR, "FILE: UOO:'%s'\n",
						oname);
					(void)sprintf(errstring = errbuf,
						"Unknown open option: %s",
						oname);
					return UUO;
				}
				switch (o->o_type) {
				case O_BIT:
					a->a_options |= o->o_value;
					break;
				case O_NUMBER:
					if (*args++ != CHSP ||
					    !isdigit(*args)) {
						(void)sprintf(errstring = errbuf,
						"Digit expected where 0%o (octal) occurred",
							args[-1]);
						goto synerr;
					}
					n = 0;
					while (isdigit(*args)) {
						n *= 10;
						n += *args++ - '0';
					}
					a->a_numbers[o->o_value] = n;
					break;
				case O_EXISTS:
					if (errcode = getxoption(a, o->o_value, &args))
						goto synerr;
					break;
				}
			}
			for (o = options; o->o_name; o++)
				if (o->o_type == O_BIT &&
				    (o->o_value & a->a_options) != 0 &&
				    (o->o_cant & a->a_options)) {
					errcode = ICO;
					goto synerr;
				}
			continue;
		default:
			fatal("Bad token type in syntax");
		}
		break;
	}
	t->t_args = a;
	return 0;
synerr:
	afree(a);
	return errcode;
}
getxoption(a, xvalue, args)
register struct cmdargs *a;
char **args;
{
	register struct xoption *x;
	int errcode;
	char *xname;

	if (*(*args)++ != CHSP) {
		errstring = "Missing value for open option";
		return MSC;
	}
	if (errcode = string(args, "", &xname))
		return errcode;
	if (*xname == '\0') {
		errstring = "Empty value for open option";
		return MSC;
	}
	for (x = xoptions; x->x_name; x++)
		if (strcmp(xname, x->x_name) == 0) {
			if (x->x_cant == xvalue)
				return ICO;
			a->a_exists[xvalue] = x->x_value;
			return 0;
		}
	return UUO;
}		
/*
 * Return the saved string starting at args and ending at the
 * point indicated by *term.  Return the saved string, and update the given
 * args pointer to point at the terminating character.
 * If term is null, terminate the string on CHSP, CHNL, or null.
 */
string(args, term, dest)
register unsigned char **args;
unsigned char *term, **dest;
{
	register unsigned char *cp;
	register unsigned char *s;
	unsigned char delim, save;

	if (*term == OPEND)
		term++;

	switch (*term) {
	case SP:
		delim = CHSP;
		break;
	case NL:
		delim = CHNL;
		break;
	case END:
		delim = '\0';
	case 0:
		delim = '\0';
		break;
	default:
		fatal("Bad delimiter for string: %o", *term);
	}
	for (cp = *args; *cp != delim; cp++)
		if (*cp == '\0' || *term == 0 && (*cp == CHSP || *cp == CHNL))
			break;
	save = *cp;
	*cp = '\0';
	s = (unsigned char *)savestr((char *)*args);
	*cp = save;
	*args = cp;
	*dest = s;
	return 0;
}

/*
 * Initialize the transaction structure.
 */
tinit(t)
register struct transaction *t;
{
	t->t_tid = NOSTR;
	t->t_fh = FNULL;
	t->t_command = CNULL;
	t->t_args = ANULL;
}
/*
 * Free storage specifically associated with a transaction (not file handles)
 */
tfree(t)
register struct transaction *t;
{
	if (t->t_tid)
		free(t->t_tid);
	if (t->t_args)
		afree(t->t_args);
	sfree(t);
}
/*
 * Free storage in an argumet structure
 */
afree(a)
register struct cmdargs *a;
{
	register char **ap, i;

	for (ap = a->a_strings, i = 0; i < MAXSTRINGS; ap++, i++)
		if (*ap)
			free(*ap);
	pfree(a->a_plist);
}
/*
 * Free storage in a plist.
 */
pfree(p)
register struct plist *p;
{
	register struct plist *np;

	while (p) {
		if (p->p_name)
			free(p->p_name);
		if (p->p_value)
			free(p->p_value);
		np = p->p_next;
		sfree(p);
		p = np;
	}
}

/*
 * Make a static copy of the given string.
 */
 char *
savestr(s)
char *s;
{
	register char *p;

	if ((p = malloc((unsigned)(strlen(s) + 1))) == NOSTR)
		fatal(NOMEM);
	(void)strcpy(p, s);
	return p;
}

/*
 * Initialize an argument struct
 */
ainit(a)
register struct cmdargs *a;
{
	register int i;

	a->a_options = 0;
	for (i = 0; i < MAXSTRINGS; i++)
		a->a_strings[i] = NOSTR;
	for (i = 0; i < O_MAXNUMS; i++)
		a->a_numbers[i] = 0;
	for (i = 0; i < O_MAXEXS; i++)
		a->a_exists[i] = O_XNONE;
	a->a_plist = PNULL;
}
/*
 * Blow me away completely. I am losing.
 */
/* VARARGS1*/
fatal(s, a1, a2, a3)
char *s;
{
	syslog(LOG_ERR, "Fatal error in chaos file server: ");
	syslog(LOG_ERR, s, a1, a2, a3);
	syslog(LOG_ERR, "\n");
	if (getpid() != mypid)
		(void)kill(mypid, SIGTERM);

	finish(0);
}
/*
 * Respond to the given transaction, including the given results string.
 * If the result string is non-null a space is prepended
 */
respond(t, results)
register struct transaction *t;
char *results;
{
	register int len;
	struct chpacket p;

	p.cp_op = DATOP;
	(void)sprintf(p.cp_data, "%s %s %s%s%s", t->t_tid,
		t->t_fh != FNULL ? t->t_fh->f_name : "",
		t->t_command->c_name, results != NOSTR ? " " : "",
		results != NOSTR ? results : "");
	len = 1 + strlen(p.cp_data);
	if (write(1, (char *)&p, len) != len) {
		fatal(CTLWRITE);
	}
	tfree(t);
}
/*
 * The transaction found an error, inform the other end.
 * If errstring has been set, use it as the message, otherwise use the
 * standard one.
 * If errtype has been set, use it as the error type instead of E_COMMAND.
 */
error(t, fh, code)
struct transaction *t;
char *fh;
{
	struct chpacket p;
	register struct file_error *e = &errors[code];
	register int len;

	p.cp_op = DATOP;
	(void)sprintf(p.cp_data, "%s %s ERROR %s %c %s", t->t_tid, fh,
		e->e_code, errtype ? errtype : E_COMMAND,
		errstring ? errstring : e->e_string);
#if 1
	syslog(LOG_INFO, "FILE: %s\n", p.cp_data);
#endif
	errstring = NOSTR;
	errtype = 0;
	len = 1 + strlen(p.cp_data);
	if (write(1, (char *)&p, len) != len)
		fatal(CTLWRITE);
	tfree(t);
}
/*
 * Send a synchronous mark on the given file handle.
 * It better be for output!
 */
syncmark(f)
register struct file_handle *f;
{
	char op;

	if (f->f_type != F_INPUT)
		fatal("Syncmark");
	op = SYNOP;
	if (write(f->f_fd, &op, 1) != 1)
		return -1;
#ifdef LOG_VERBOSE
	syslog(LOG_INFO, "FILE: wrote syncmark to net\n");
#endif
	return 0;
}


/*
 * Here are all the "top level" commands that don't relate to file system
 * operation at all, but only exists for data connection management and
 * user validation.
 */

/*
 * Create a new data connection.  Output file handle (second string argument
 * is the contact name the other end is listening for.  We must send an RFC.
 * The waiting for the new connection is done synchronously, and thus prevents
 * tha handling of any other commands for a bit.  It should, if others already
 * exist, create a transfer task, just for the purpose of waiting. Yick.
 */
dataconn(t)
register struct transaction *t;
{
	register struct file_handle *ifh, *ofh;
	char *ifhname, *ofhname;
	int fd;

	ifhname = t->t_args->a_strings[0];
	ofhname = t->t_args->a_strings[1];

	/*
	 * Make sure that our "new" file handle names are really new.
	 */
	for (ifh = file_handles; ifh; ifh = ifh->f_next)
		if (strcmp(ifhname, ifh->f_name) == 0 ||
		    strcmp(ofhname, ifh->f_name) == 0) {
			errstring = "File handle already exists";
			error(t, "", BUG);
			return;
		}
	/*
	 * The output file handle name is the contact name the user end
	 * is listening for, so send it.
	 */
	if ((fd = chopen(mylogin.cl_haddr, ofhname, 2, 1, 0, 0, 0)) < 0 ||
//	    ioctl(fd, CHIOCSWAIT, (char *)CSRFCSENT) < 0 ||/* Hangs here */
//	    ioctl(fd, CHIOCGSTAT, (char *)&chst) < 0 ||
//	    chst.st_state != CSOPEN ||
//	    ioctl(fd, CHIOCSMODE, (char *)CHRECORD
	    1)
	{
		if (fd >= 0)
			(void)close(fd);
		errstring = "Data connection could not be established";
		error(t, "", NET);
		return;
	}
	if ((ifh = salloc(file_handle)) == FNULL ||
	    (ofh = salloc(file_handle)) == FNULL)
		fatal(NOMEM);
	ifh->f_fd = fd;
	ifh->f_type = F_INPUT;
	ifh->f_name = savestr(ifhname);
	ifh->f_xfer = XNULL;
	ifh->f_other = ofh;
	ifh->f_next = file_handles;
	file_handles = ifh;
	ofh->f_fd = fd;
	ofh->f_type = F_OUTPUT;
	ofh->f_name = savestr(ofhname);
	ofh->f_xfer = XNULL;
	ofh->f_other = ifh;
	ofh->f_next = file_handles;
	file_handles = ofh;
	respond(t, NOSTR);
}

/*
 * Close the data connection indicated by the file handle.
 */
undataconn(t)
register struct transaction *t;
{
	register struct file_handle *f, *of;

	f = t->t_fh;
	if (f->f_xfer != XNULL || f->f_other->f_xfer != XNULL) {
		errstring =
			"The data connection to be removed is still in use";
		error(t, f->f_name, BUG);
	} else {
		(void)close(f->f_fd);
		if (file_handles == f)
			file_handles = f->f_next;
		else {
			for (of = file_handles; of->f_next != f;
							of = of->f_next)
				if (of->f_next == FNULL)
					fatal(BADFHLIST);
			of->f_next = f->f_next;
		}
		if (file_handles == f->f_other)
			file_handles = f->f_other->f_next;
		else {
			for (of = file_handles; of->f_next != f->f_other;
							of = of->f_next)
				if (of->f_next == FNULL)
					fatal(BADFHLIST);
			of->f_next = f->f_other->f_next;
		}
		free(f->f_name);
		of = f->f_other;
		free((char *)f);
		free(of->f_name);
		free((char *)of);
		respond(t, NOSTR);
	}
}

#ifdef PRIVHOSTS
/* 4/29/85 Cory Myers
 *      allow logins without passwords from privileged hosts
 */

char *privileged_hosts[] = {"mit-tweety-pie", "mit-daffy-duck"};
#define NUM_PRIVILEGED_HOSTS (sizeof(privileged_hosts)/sizeof(char *))
#endif

/*
 * Process a login command... verify the users name and
 * passwd.
 */
login(t)
register struct transaction *t;
{
	register struct passwd *p;
	register struct cmdargs *a;
	register int ufd;
	char *name;
	char response[CHMAXDATA];

syslog(LOG_INFO, "FILE: login()\n");

#ifdef PRIVHOSTS
/* 4/29/85 Cory Myers
 *      allow logins without passwords from privileged hosts
 */
	int host_name,privileged_host;

	privileged_host = 0;
	for (host_name = 0; host_name < NUM_PRIVILEGED_HOSTS; host_name++) {
	  if (strcmp(privileged_hosts[host_name],chaos_name(mylogin.cl_haddr))
	      == 0) {
		privileged_host = 1;
		break;
	      }
	}
#endif

	a = t->t_args;
	if ((name = a->a_strings[0]) == NOSTR) {
		syslog(LOG_INFO, "FILE exiting due to logout\n");
		finish(0);
	} else if (home != NOSTR) {
		errstring = "You are already logged in.";
		error(t, "", BUG);
	} else if (*name == '\0' || (p = getpwnam(downcase(name))) == (struct passwd *)0) {
syslog(LOG_INFO, "FILE: login() no user '%s'\n", name);
		errstring = "Login incorrect.";
		error(t, "", UNK);
	} else if (p->pw_passwd == NOSTR || *p->pw_passwd == '\0') {
	/* User MUST have a passwd */
		errstring = "Invalid account";
		error(t, "", MSC);
	} else if (p->pw_passwd != NOSTR && *p->pw_passwd != '\0' &&
#ifdef PRIVHOSTS
/*      allow logins without passwords from privileged hosts */
		   !privileged_host &&
#endif
		   (a->a_strings[1] == NOSTR ||
		    strcmp(crypt(a->a_strings[1], p->pw_passwd), p->pw_passwd)
		     != 0)) {
syslog(LOG_INFO, "FILE: %s %s failed", name, a->a_strings[1]);
		error(t, "", IP);
	} else if (p->pw_uid != 0 && access(NOLOGIN, 0) == 0) {
		errstring = "All logins are disabled, system shutting down";
		error(t, "", MSC);
	} else {
syslog(LOG_INFO, "FILE: login() pw ok\n");
		home = savestr(p->pw_dir);
		cwd = savestr(home);
		umask(0);
#if defined(BSD42) || defined(linux)
		(void)initgroups(p->pw_name, p->pw_gid);
#else /*!BSD42*/
		(void)setgid(p->pw_gid);
#endif /*!BSD42*/
		(void)setuid(p->pw_uid);
		(void)sprintf(response, "%s %s/%c%s%c", p->pw_name, p->pw_dir,
				   CHNL, fullname(p), CHNL);
				   
		strncpy(mylogin.cl_user, p->pw_name, sizeof(mylogin.cl_user));
		if ((ufd = open(FILEUTMP, 1)) >= 0) {
			(void)lseek(ufd,
			  (long)(mylogin.cl_cnum * sizeof(struct chlogin)),
			    0);
			(void)write(ufd, (char *)&mylogin, sizeof(mylogin));
			(void)close(ufd);
		}
syslog(LOG_INFO, "FILE: login() responding\n");
		respond(t, response);
		syslog(LOG_NOTICE, "FILE: logged in as %s from host %s\n",
		  p->pw_name, chaos_name(mylogin.cl_haddr));
#if 0
		{
		  char str[50];
		  
		  sprintf(str, "/usr/local/f @%s",
			  chaos_name(mylogin.cl_haddr));
		  strcat(str, " |/usr/local/logger -t FILE -p %d",
			 LOG_NOTICE);
		  system(str);
		}
#endif
	}
syslog(LOG_INFO, "FILE: login() done\n");
}
/*
 * Generate the full name from the password file entry, according to
 * the current "finger" program conventions.  This code was sort of
 * lifted from finger and needs to be kept in sync with it...
 */
char *
fullname(p)
register struct passwd *p;
{
	static char fname[100];
	register char *cp;
	register char *gp;

	gp = p->pw_gecos;
	cp = fname;
	if (*gp == '*')
		gp++;
	while (*gp != '\0' && *gp != ',') {
		if( *gp == '&' )  {
			char *lp = p->pw_name;

			if (islower(*lp))
				*cp++ = toupper(*lp++);
			while (*lp)
				*cp++ = *lp++;
		} else
			*cp++ = *gp;
		gp++;
	}
	*cp = '\0';
	for (gp = cp; gp > fname && *--gp != ' '; )
		;
	if (gp == fname)
		return fname;
	*cp++ = ',';
	*cp++ = ' ';
	*gp = '\0';
	strcpy(cp, fname);
	return ++gp;
}
char *
downcase(string)
char *string;
{
	register char *cp;

	for (cp = string; *cp; cp++)
		if (isupper(*cp))
			*cp = tolower(*cp);
	return string;
}

/*
 * Here are the major top level commands that create transfers,
 * either of data or directory contents
 */
/*
 * Open a file.
 * Unless a probe is specified, a transfer is created, and started.
 */
fileopen(t)
register struct transaction *t;
{
	register struct file_handle *f = t->t_fh;
	register struct xfer *x;
	int errcode, fd;
	long options = t->t_args->a_options;
	int bytesize = t->t_args->a_numbers[O_BYTESIZE];
	int ifexists = t->t_args->a_exists[O_IFEXISTS];
	int ifnexists = t->t_args->a_exists[O_IFNEXISTS];
	char *pathname = t->t_args->a_strings[0];
	char *realname = NOSTR, *dirname = NOSTR, *qfasl = FALSE;
	char *tempname = NOSTR;
	char response[CHMAXDATA + 1];
	long nbytes;
	struct tm *tm;

	if ((errcode = parsepath(pathname, &dirname, &realname, 0)) != 0)
		goto openerr;
	if (options & ~
	    (O_RAW|O_READ|O_WRITE|O_PROBE|O_DEFAULT|O_PRESERVE|O_BINARY|O_CHARACTER|O_PROBEDIR)
	    ) {
		syslog(LOG_ERR, "FILE:UOO: 0%O\n", options);
		errcode = ICO;
		goto openerr;
	}
	errcode = 0;
	if ((options & (O_READ|O_WRITE|O_PROBE)) == 0) {
		if (f == FNULL)
			options |= O_PROBE;
		else if (f->f_type == F_INPUT)
			options |= O_READ;
		else
			options |= O_WRITE;
	}
	switch (options & (O_READ|O_WRITE|O_PROBE)) {
	case O_READ:
		if (f == FNULL || f->f_type != F_INPUT) {
			errstring = "Bad file handle on READ";
			errcode = BUG;
		}
		if (ifnexists == O_XCREATE) {
			errstring = "IF-DOES-NOT-EXIST CREATE illegal for READ";
			errcode = ICO;	
		} else if (ifnexists == O_XNONE)
			ifnexists = O_XERROR;
		ifexists = O_XNONE;
		break;
	case O_WRITE:
		if (f == FNULL || f->f_type != F_OUTPUT) {
			errstring = "Bad file handle on WRITE";
			errcode = BUG;
		} else if (ifexists == O_XNEWVERSION)
			errcode = UUO;
		else {
			if (ifexists == O_XNONE)
				ifexists = O_XSUPERSEDE;
			if (ifnexists == O_XNONE)
				ifnexists = ifexists == O_XOVERWRITE ||
					    ifexists == O_XAPPEND ?
					    O_XERROR : O_XCREATE;
		}
		break;
	case O_PROBE:
		if (options & O_PROBEDIR)
			realname = dirname;
		if (f != FNULL) {
			errstring = "File handle supplied on PROBE";
			errcode = BUG;
		}
		ifnexists = O_XERROR;
		ifexists = O_XNONE;
		break;
	}
	if (errcode == 0 && f != NULL && f->f_xfer != XNULL) {
		errstring = "File handle in OPEN is already in use.";
		errcode = BUG;
	}
	if (errcode != 0)
		goto openerr;
	switch (options & (O_PROBE|O_READ|O_WRITE)) {
	case O_PROBE:
		if (stat(realname, &sbuf) < 0) {
			switch (errno) {
			case EACCES:
				errstring = SEARCHDIR;
				errcode = ATD;
				break;
			case ENOTDIR:
				errstring = PATHNOTDIR;
				errcode = DNF;
				break;
			case ENOENT:
				if (access(dirname, 0) != 0)
					errcode = DNF;
				else
					errcode = FNF;
				break;
			default:
				errcode = MSC;
			}
		}
		break;
	case O_WRITE:
		fd = 0;	/* Impossible value */
		if (stat(realname, &sbuf) == 0) {
			/*
			 * The file exists.  Disallow writing directories.
			 * Open the real file for append, overwrite and truncate,
			 * otherwise fall through to the file nonexistent case.
			 */
			if ((sbuf.st_mode & S_IFMT) == S_IFDIR) {
				errstring = "File to be written is a directory";
				errcode = IOD;
				break;
			} else switch (ifexists) {
			case O_XERROR:
				errcode = FAE;
				break;
			case O_XTRUNCATE:
				fd = creat(realname, 0644);
				break;
			case O_XOVERWRITE:
				fd = open(realname, 1);
				break;
			case O_XAPPEND:
				if ((fd = open(realname, 1)) > 0)
					lseek(fd, 0, 2);
				break;
			case O_XSUPERSEDE:
			case O_XRENDEL:
			case O_XRENAME:
				/*
				 * These differ only at close time.
				 */		
				break;
			}
		} else {
			/*
			 * The stat above failed. Make sure the file really doesn't
			 * exist. Otherwise fall through to the error processing
			 * below.
			 */
			if (errno != ENOENT || access(dirname, 1) != 0)
				fd = -1;
			else switch (ifnexists) {
			case O_XERROR:
				errcode = FNF;
				break;
			case O_XCREATE:
				if (ifexists == O_XAPPEND ||
				    ifexists == O_XOVERWRITE ||
				    ifexists == O_XTRUNCATE)
					fd = creat(realname, 0644);
				break;
			}
		}
		if (errcode)
			break;
		if (fd == 0) {
			/*
			 * ifexists is either SUPERSEDE, RENDEL or RENAME, so
			 * we use temporary files.
			 */
			if ((tempname = tempfile(dirname)) == NOSTR)
				fd = -1;
			else
				fd = creat(tempname, 0644);
		}
		/*
		 * An error occurred either in stat, creat or open on the
		 * realname, or on access or creat on the tempname.
		 */
		if (fd < 0)
			switch (errno) {
			case EACCES:
				errcode = ATD;
				if (access(dirname, 1) < 0)
					errstring = SEARCHDIR;
				else if (access(dirname, 2) < 0)
					errstring = WRITEDIR;
				else {
					errcode = ATF;
					errstring = WRITEFILE;
				}
				break;
			case ENOENT:
				errcode = DNF;
				break;
			case ENOTDIR:
				errstring = PATHNOTDIR;
				errcode = DNF;
				break;
			case EISDIR:	/* impossible */
				errstring = "File to be written is a directory";
				errcode = IOD;
				break;
			case ETXTBSY:
				errstring = "File to be written is being executed";
				errcode = LCK;
				break;	
			case ENFILE:
				errstring = "No file descriptors available";
				errcode = NER;
				break;
			case ENOSPC:
				errstring = "No free space to create directory entry";
				errcode = NMR;
				break;
			default:
				errcode = MSC;
			}
		else if (fstat(fd, &sbuf) < 0)
			fatal(FSTAT);
		break;
	case O_READ:
		if ((fd = open(realname, 0)) < 0) {
			switch (errno) {
			case EACCES:
				if (access(dirname, 1) == 0) {
					errcode = ATF;
					errstring = READFILE;
				} else {
					errcode = ATD;
					errstring = SEARCHDIR;
				}
				break;
			case ENOENT:
				if (access(dirname, 0) < 0)
					errcode = DNF;
				else
					errcode = FNF;
				break;
			case ENOTDIR:
				errstring = PATHNOTDIR;
				errcode = DNF;
				break;
			case ENFILE:
				errstring = "No file descriptors available";
				errcode = NER;
				errtype = E_RECOVERABLE;
				break;
			default:
				errcode = MSC;
			}
		} else if (fstat(fd, &sbuf) < 0)
			fatal(FSTAT);
		else if (options & O_DEFAULT) {
			unsigned short ss[2];

			if (read(fd, (char *)ss, sizeof(ss)) == sizeof(ss) &&
			    (ss[0] == QBIN1 && ss[1] == QBIN2 ||
			     ss[0] == LBIN1 && ss[1] < LBIN2LIMIT))
				options |= O_BINARY;
			else
				options |= O_CHARACTER;
			(void)lseek(fd, 0L, 0);
		}
		break;
	}
	if (options & O_BINARY) {
		qfasl = TRUE;
		if (bytesize == 0)
			bytesize = 16;
		else if (bytesize < 0 || bytesize > 16) {
			errcode = IBS;
		}
	}
	if (errcode != 0) {
		if (errcode == MSC)
			errstring = strerror(errno);
		goto openerr;
	}
	tm = localtime(&sbuf.st_mtime);
	nbytes = options & O_CHARACTER || bytesize <= 8 ? sbuf.st_size :
		(sbuf.st_size + 1) / 2;
	if (protocol > 0)
		(void)sprintf(response,
			"%02d/%02d/%02d %02d:%02d:%02d %ld %s%s%s%c%s%c",
			tm->tm_mon+1, tm->tm_mday, tm->tm_year,
			tm->tm_hour, tm->tm_min, tm->tm_sec, nbytes,
			qfasl, options & O_DEFAULT ? " " : "",
			options & O_DEFAULT ? (options & O_CHARACTER ?
					       TRUE : FALSE) : "",
			CHNL, realname, CHNL);
	else
		(void)sprintf(response,
			"%d %02d/%02d/%02d %02d:%02d:%02d %ld %s%c%s%c",
			-1, tm->tm_mon+1, tm->tm_mday, tm->tm_year,
			tm->tm_hour, tm->tm_min, tm->tm_sec, nbytes,
			qfasl, CHNL, realname, CHNL);
	if (options & (O_READ|O_WRITE)) {
		x = makexfer(t, options);
		x->x_state = X_PROCESS;
		x->x_bytesize = bytesize;
		x->x_fd = fd;
		x->x_realname = realname;
		x->x_dirname = dirname;
		x->x_atime = sbuf.st_atime;
		x->x_mtime = sbuf.st_mtime;
		if (options & O_WRITE)
			x->x_tempname = tempname;
		realname = dirname = tempname = NOSTR;			
		if ((errcode = startxfer(x)) != 0)
			xflush(x);
	}
	if (errcode == 0)
		respond(t, response);
openerr:
	if (errcode)
		error(t, f != FNULL ? f->f_name : "", errcode);
	if (dirname)
		free(dirname);
	if (realname)
		free(realname);
	if (tempname)
		free(tempname);
}
/*
 * Make up a temporary file name given a directory string.
 * We fail (return NOSTR) either if we can't find an unused name
 * (basically impossible) or if the search path is bogus.
 */
char *
tempfile(dname)
char *dname;
{
	register char *cp = malloc((unsigned)(strlen(dname) +
				   2 + sizeof("#FILEpppppdd")));
	register int i;
	static int lastnum;

	if (cp == NOSTR)
		fatal(NOMEM);
	for (i = 0; i < 100; i++) {
		int uniq = lastnum + i;

		if (uniq > 99)
			uniq -= 100;
		(void)sprintf(cp, "%s/#FILE%05d%02d", dname, mypid, uniq);
		if (access(cp, 0) != 0)
			if (errno == ENOENT) {
				/*
				 * We could be losing here if the directory doesn't exist,
				 * but that will be caught later anyway.
				 */
				lastnum = ++uniq;
				return cp;
			} else
				break;
	}
	free(cp);
	return NOSTR;	/* Our caller checks errno */
}

getprops(t)
register struct transaction *t;
{
	register struct cmdargs *a = t->t_args;
	register struct xfer *x;
	int errcode = 0;
	char *dirname, *realname, *tempname = 0;

	if (t->t_fh->f_xfer != XNULL) {
		errstring = "File handle already in use";
		errcode = BUG;
	} else if (a->a_strings[0][0]) {
		register struct file_handle *f;

		for (f = file_handles; f; f = f->f_next)
			if (strncmp(a->a_strings[0], f->f_name, FHLEN) == 0)
				break;
		if (f == FNULL) {
			errcode = BUG;
			(void)sprintf(errstring = errbuf,
				"Unknown file handle: %s", a->a_strings[0]);
		} else if ((x = f->f_xfer) == XNULL) {
			errcode = BUG;
			(void)sprintf(errstring = errbuf,
				"File handle: %s has no associated file", a->a_strings[0]);
		} else {
			dirname = savestr(x->x_dirname);
			realname = savestr(x->x_realname);
			tempname = savestr(x->x_tempname);
		}
	} else
		errcode = parsepath(a->a_strings[1], &dirname, &realname, 0);
	if (errcode == 0)
		if (stat(realname, &sbuf) < 0)
			switch (errno) {
			case EACCES:
				errstring = SEARCHDIR;
				errcode = ATD;
				break;
			case ENOTDIR:
				errstring = PATHNOTDIR;
				errcode = DNF;
				break;
			case ENOENT:
				if (access(dirname, 0) != 0)
					errcode = DNF;
				else
					errcode = FNF;
				break;
			default:
				errcode = MSC;
			}
		else {
			if (!(a->a_options & (O_BINARY | O_CHARACTER)))
			 	a->a_options |= O_CHARACTER;
			x = makexfer(t, a->a_options | O_PROPERTIES);
			x->x_realname = realname;
			x->x_dirname = dirname;
			x->x_state = X_IDLE;
			x->x_tempname = tempname;
			if ((errcode = startxfer(x)) == 0) {
				t->t_command = &propcom;
				afree(a);
				t->t_args = ANULL;
				xcommand(t);
				return;
			}
			xflush(x);
		}
	error(t, t->t_fh->f_name, errcode);
}

/*
 * Format of output is a line of changeable property names,
 * followed by a line of file name followed by lines
 * for individual properties.
 */
propopen(x, t)
register struct xfer *x;
register struct transaction *t;
{
	register char *cp;

	respond(t, "");
	cp = x->x_bptr = x->x_bbuf;
	cp = getsprops(&sbuf, cp);
	*cp++ = '\n';
	strcpy(cp, x->x_realname);
	while (*cp)
		cp++;
	*cp++ = '\n';
	x->x_bptr = cp;
	x->x_left = x->x_bptr - x->x_bbuf;
	x->x_bptr = x->x_bbuf;
	x->x_state = X_PROCESS;
	return;
}
propread(x)
register struct xfer *x;
{
	register struct property *p;
	register char *cp;

	if (x->x_realname == 0)
		return 0;
	if (stat(x->x_tempname ? x->x_tempname : x->x_realname, &sbuf) < 0)
		return -1;
	free(x->x_realname);
	x->x_realname = 0;
	cp = x->x_bbuf;
	for (p = properties; p->p_indicator; p++)
		if (!(p->p_flags & P_GLOBAL)) {
			register char *ep = cp;
			strcpy(ep, p->p_indicator);
			while (*ep)
				ep++;
			*ep++ = ' ';
			if (ep = (*p->p_get)(&sbuf, ep)) {
				*ep++ = '\n';
				cp = ep;
			}
		}
	*cp++ = '\n';
	return cp - x->x_bbuf;
}
/*
 * Implement the directory command.
 * This consists of the top level command and the routines for generating
 * the property list of a directory entry.  See chngprop and friends.
 */
directory(t)
register struct transaction *t;
{
	register struct cmdargs *a = t->t_args;
	register struct xfer *x;
	int errcode = 0;
	char *dirname, *realname;

	if (a->a_options & ~(O_DELETED|O_FAST|O_SORTED)) {
		syslog(LOG_ERR, "FILE:UOO: 0%o\n", a->a_options);
		errcode = ICO;
	} else if (t->t_fh->f_xfer != XNULL) {
		errstring = "File handle already in use";
		errcode = BUG;
	} else if ((errcode = parsepath(a->a_strings[0], &dirname, &realname, 0)) == 0) {
		if (!(a->a_options & (O_BINARY | O_CHARACTER)))
		 	a->a_options |= O_CHARACTER;
		x = makexfer(t, a->a_options | O_DIRECTORY);
		x->x_realname = realname;
		x->x_dirname = dirname;
		x->x_state = X_IDLE;
		if ((errcode = startxfer(x)) == 0) {
			t->t_command = &dircom;
			afree(t->t_args);
			t->t_args = ANULL;
			xcommand(t);
			return;
		}
		xflush(x);
	}
	error(t, t->t_fh->f_name, errcode);
}
/*
 * Start up a directory transfer by first doing the glob and responding to the
 * directory command.
 * Note that this happens as the first work in a transfer process.
 */
diropen(ax, t)
struct xfer *ax;
register struct transaction *t;
{
	register struct xfer *x = ax;
	struct stat *s = (struct stat *)0;
	int errcode;

	x->x_glob = glob(x->x_realname);
	if ((errcode = globerr) != 0)
		goto derror;
	if (x->x_glob) {
		char **badfile = NOBLK;
		int baderrno;
		/*
		 * We still need to check for the case where the only
		 * matching files (which don't necessarily exists) are
		 * in non-existent directories.
		 */
		for (x->x_gptr = x->x_glob; *x->x_gptr; x->x_gptr++)
			if (stat(*x->x_gptr, &sbuf) == 0) {
				s = &sbuf;
				break;
			} else if (badfile == NOBLK) {
				baderrno = errno;
				badfile = x->x_gptr;
			}
		/*
		 * If no existing files were found, and an erroneous
		 * file was found, scan for a file that is not simply
		 * non-existent.  If such a file is found, then we
		 * must return an error.
		 */
		if (*x->x_gptr == NOSTR && badfile != NOBLK)
			for (x->x_gptr = badfile; *x->x_gptr; baderrno = errno)
				switch(baderrno) {
					register char *cp;
				case ENOENT:
					cp = rindex(*x->x_gptr, '/');
					if (cp) {
						register char c;

						c = *cp;
						*cp = '\0';
						if (access(*x->x_gptr, 0) < 0) {
							errcode = DNF;
							goto derror;
						}
						*cp = c;
					}
					if (*++x->x_gptr != NOSTR)
						access(*x->x_gptr, 0);
					break;
				case EACCES:
					errcode = ATD;
					errstring = SEARCHDIR;
					goto derror;
				case ENOTDIR:
					errcode = DNF;
					errstring = PATHNOTDIR;
					goto derror;
				default:
					errcode = MSC;
					errstring = strerror(baderrno);
					goto derror;
				}
	}
	{
		register struct property *p;
		register char *cp;

		respond(t, "");
		/*
		 * We create the firest record to be returned, which
		 * is the properties of the file system as a whole.
		 */
		x->x_bptr = x->x_bbuf;
		*x->x_bptr++ = '\n';
		for (p = properties; p->p_indicator; p++)
			if (p->p_flags & P_GLOBAL) {
				cp = x->x_bptr;
				strcpy(cp, p->p_indicator);
				while (*cp)
					cp++;
				*cp++ = ' ';
				if (cp = (*p->p_get)(s, cp)) {
					*cp++ = '\n';
					x->x_bptr = cp;
				}
			}
		*x->x_bptr++ = '\n';
		x->x_left = x->x_bptr - x->x_bbuf;
		x->x_bptr = x->x_bbuf;
		x->x_state = X_PROCESS;
		return;
	}
derror:
	error(t, t->t_fh->f_name, errcode);
	x->x_state = X_DERROR;
#ifndef SELECT
	(void)write(ctlpipe[1], (char *)&ax, sizeof(x));
#endif
}
/*
 * Assemble a directory entry record in the buffer for this transfer.
 * This is actually analogous to doing a disk read on the directory
 * file.
 */
dirread(x)
register struct xfer *x;
{
	if (x->x_glob == NOBLK)
		return 0;
	while (*x->x_gptr != NOSTR && stat(*x->x_gptr, &sbuf) < 0)
		x->x_gptr++;
	if (*x->x_gptr == NOSTR)
		return 0;
	(void)sprintf(x->x_bbuf, "%s\n", *x->x_gptr);
	x->x_gptr++;
	x->x_bptr = x->x_bbuf + strlen(x->x_bbuf);
	if (!(x->x_options & O_FAST)) {
		register struct property *p;
		register char *cp;

		for (p = properties; p->p_indicator; p++)
			if (!(p->p_flags & P_GLOBAL)) {
				cp = x->x_bptr;
				strcpy(cp, p->p_indicator);
				while (*cp)
					cp++;
				*cp++ = ' ';
				if (cp = (*p->p_get)(&sbuf, cp)) {
					*cp++ = '\n';
					x->x_bptr = cp;
				}
			}
	}
	*x->x_bptr++ = '\n';
	return x->x_bptr - x->x_bbuf;
}

/*
 * Utilities for managing transfer tasks
 * Make a transfer task and initialize it;
 */
 struct xfer *
makexfer(t, options)
register struct transaction *t;
long options;
{
	register struct xfer *x;

	if ((x = salloc(xfer)) == XNULL)
		fatal(NOMEM);
	x->x_fh = t->t_fh;
	t->t_fh->f_xfer = x;
#ifdef SELECT
	x->x_work = TNULL;
#endif
	x->x_flags = 0;
	x->x_options = options;
	if (options & O_WRITE)
		x->x_room = FSBSIZE;
	else
		x->x_room = CHMAXDATA;
	x->x_bptr = x->x_bbuf;
	x->x_pptr = x->x_pbuf;
	x->x_left = 0;
	x->x_next = xfers;
	x->x_realname = x->x_dirname = x->x_tempname = NOSTR;
	x->x_glob = (char **)0;
	xfers = x;
	return x;
}
/*
 * Issue the command on its xfer.
 */
xcommand(t)
register struct transaction *t;
{
	register struct file_handle *f;
	register struct xfer *x;

	syslog(LOG_INFO, "FILE: transfer command: %s\n", t->t_command->c_name);
	if ((f = t->t_fh) == FNULL || (x = f->f_xfer) == XNULL) {
		errstring = "No transfer in progress on this file handle";
		error(t, f->f_name, BUG);
	} else {
#ifdef SELECT
		xqueue(t, x);
#else
		sendcmd(t, x);
#endif
	}
}
#ifdef SELECT
/*
 * Queue up the transaction onto the transfer.
 */
xqueue(t, x)
register struct transaction *t;
register struct xfer *x;
{
	register struct transaction **qt;

	for (qt = &x->x_work; *qt; qt = &(*qt)->t_next)
		;
	t->t_next = TNULL;
	*qt = t;
}
#endif
/*
 * Flush the transfer - just make the file handle not busy
 */
xflush(x)
register struct xfer *x;
{
	register struct xfer **xp;

	for (xp = &xfers; *xp && *xp != x; xp = &(*xp)->x_next)
		;
	if (*xp == XNULL)
		fatal("Missing xfer");
	*xp = x->x_next;
	x->x_fh->f_xfer = XNULL;
	xfree(x);
}
/*
 * Free storage associated with xfer struct.
 */
xfree(x)
register struct xfer *x;
{

#ifdef SELECT
	register struct transaction *t;

	while ((t = x->x_work) != TNULL) {
		x->x_work = t->t_next;
		tfree(t);
	}
#endif
	if (x->x_realname)
		free(x->x_realname);
	if (x->x_glob)
		blkfree(x->x_glob);
	sfree(x);
}
/*
 * Here are commands that operate on existing transfers.  There execution
 * is queued and performed on the context of the transfer.
 * (Closing a READ or DIRECTORY transfer is special).
 */
/*
 * Perform the close on the transfer.
 * Closing a write transfer is easy since the CLOSE command can truly
 * be queued.  Closing a READ or DIRECTORY is harder since the transfer
 * is most likely blocked on writing the data into the connection and thus
 * will not process a queued command.  Since the user side will not
 * drain the connection (reading until it sees a synchromous mark) until
 * the response to the CLOSE is received, we would deadlock unless we
 * interrupted the transfer task.
 * Basically we mark the transfer closed and make a state change if
 * appropriate.
 */
fileclose(x, t)
register struct xfer *x;
register struct transaction *t;
{
	x->x_flags |= X_CLOSE;
	switch (x->x_options & (O_READ | O_WRITE | O_DIRECTORY | O_PROPERTIES)) {
	case O_READ:
	case O_DIRECTORY:
	case O_PROPERTIES:
		/*
		 * On READing transfers we brutally force the X_DONE
		 * state, causing the close response right away.
		 */
		x->x_state = X_DONE;
		break;
	case O_WRITE:
		switch (x->x_state) {
		case X_PROCESS:
		case X_WSYNC:
		case X_RETRY:
			/*
			 * Do nothing, keep going until SYNCMARK.
			 */
			break;
		case X_ERROR:
			x->x_state = X_WSYNC;
			break;
		case X_BROKEN:
		case X_RSYNC:
			x->x_state = X_DONE;
		}
	}
	x->x_close = t;
#ifndef SELECT
	(void)signal(SIGHUP, SIG_IGN);
#endif
}

/*
 * The actual work and response to a close is called expicitly
 * from the transfer task when it has finished.
 */
xclose(ax)
struct xfer *ax;
{
	register struct xfer *x = ax;
	register struct transaction *t = x->x_close;
	char response[CHMAXDATA];
	int errcode = 0;
	struct tm *tm;
	
#ifndef SELECT
	(void)write(ctlpipe[1], (char *)&ax, sizeof(x)); 
#endif 
        if (x->x_options & (O_DIRECTORY|O_PROPERTIES)) {
		respond(t, NOSTR);
		return;
	}
	/*
	 * If writing a file, rename the temp file.
	 */
	if (x->x_options & O_WRITE &&
	    x->x_tempname && !(x->x_flags & X_DELETE)) {
		/*
		 * We know that both names are in the same directory
		 * and that we were already able to create the
		 * temporary file, implying write access to the
		 * directory at that time.
		 */
		if (link(x->x_tempname, x->x_realname) == 0)
			(void)unlink(x->x_tempname);
		else switch (errno) {
		/* Removed out from under me! */
		case ENOENT:
			errstring = "Temporary file has disappeared";
			errcode = MSC;
			break;
		case EEXIST:
			backfile(x->x_realname);
			if (link(x->x_tempname, x->x_realname) == 0)
				(void)unlink(x->x_tempname);
			else {
				errcode = MSC;
				errstring = "Can't rename temporary file";
			}
			break;
		default:
			/*
			 * Something strange has happened.
			 */
			errstring = strerror(errno);
			errcode = MSC;
			break;
		}
	}
	if (errcode == 0) {
		if (fstat(x->x_fd, &sbuf) < 0) {
			syslog(LOG_ERR, "fd:%d, pid:%d, mypid:%d, errno:%d\n", x->x_fd,
					getpid(), mypid, errno);
			fatal("Fstat in xclose 1");
		}
		if (x->x_options & O_PRESERVE ||
		    x->x_flags & (X_ATIME|X_MTIME)) {
			time_t timep[2];

			timep[0] = (x->x_options&O_PRESERVE ||
				    x->x_flags&X_ATIME) ? x->x_atime :
				    sbuf.st_atime;
			timep[1] = (x->x_options&O_PRESERVE ||
				    x->x_flags&X_MTIME) ? x->x_mtime :
				    sbuf.st_mtime;
			/*
			 * No error checking is done here since CLOSE
			 * can't really fail anyway.
			 */
			utime(x->x_realname, timep);
			if (fstat(x->x_fd, &sbuf) < 0)
				fatal("Fstat in xclose 2");
		}
		tm = localtime(&sbuf.st_mtime);
		if (protocol > 0)
			(void)sprintf(response,
				"%02d/%02d/%02d %02d:%02d:%02d %ld%c%s%c",
				tm->tm_mon+1, tm->tm_mday, tm->tm_year,
				tm->tm_hour, tm->tm_min, tm->tm_sec,
				sbuf.st_size, CHNL,
				x->x_realname, CHNL);
		else
			(void)sprintf(response,
				"%d %02d/%02d/%02d %02d:%02d:%02d %ld%c%s%c",
				-1, tm->tm_mon+1, tm->tm_mday,
				tm->tm_year, tm->tm_hour, tm->tm_min,
				tm->tm_sec, sbuf.st_size, CHNL,
				x->x_realname, CHNL);
		respond(t, response);
	} else
		error(t, x->x_fh->f_name, errcode);
	(void)close(x->x_fd);
}
/*
 * Rename a file to its backup file.
 * If this fails, its just too bad.
 */
backfile(file)
register char *file;
{
	register char *name = rindex(file, '/');
	register char *end, *back;

	if (name == NOSTR)
		name = file;
	else
		name++;
	end = name + strlen(name);
	if ((back = malloc((unsigned)(strlen(file) + 2))) == NOSTR)
		fatal(NOMEM);
	strcpy(back, file);
#if !defined(BSD42) && !defined(linux)
	if (end - name >= DIRSIZ - 1)
		back[name - file + DIRSIZ - 1] = '\0';
#endif
	strcat(back, "~");
	/*
	 * Get rid of the previous backup copy.
	 * Rename current copy to backup name and if rename succeeded,
	 * remove current copy.
	 */
	(void)unlink(back);
	if (link(file, back) == 0)
		(void)unlink(file);
	free(back);
}

/*
 * Change the file position of the file.
 * Transfer must be a READ and either in process or hung at EOF.
 */
filepos(x, t)
register struct xfer *x;
register struct transaction *t;
{
	if ((x->x_options & O_READ) == 0) {
		errstring = "Not a reading file handle for FILEPOS";
		error(t, x->x_fh->f_name, BUG);
	} else if (!(x->x_state == X_PROCESS || x->x_state == X_REOF ||
		     x->x_state == X_SEOF)) {
		errstring = "Incorrect transfer state for FILEPOS";
		error(t, x->x_fh->f_name, BUG);
	} else {
		long new = t->t_args->a_numbers[0];
		long old = tell(x->x_fd);
		long size = lseek(x->x_fd, 0L, 2);

		/* Adjust for bytesize */
		new *= (x->x_bytesize == 16 ? 2 : 1);

		if (new < 0 || new > size) {
			(void)lseek(x->x_fd, old, 0);
			errstring = "Illegal byte position for this file";
			error(t, x->x_fh->f_name, FOR);
		} else {
			x->x_room = CHMAXDATA;
			x->x_pptr = x->x_pbuf;
			x->x_left = 0;
			x->x_flags &= ~X_EOF;
			x->x_state = X_PROCESS;
			(void)lseek(x->x_fd, new, 0);
			respond(t, NOSTR);
			if (syncmark(x->x_fh) < 0)
				fatal("Broken data connection");
#ifdef LOG_VERBOSE
			syslog(LOG_INFO,
			       "pid: %ld, x: %X, fd: %ld, size: %ld, old: %ld, new: %ld, pos: %ld\n",
			       getpid(), x, x->x_fd, size, old, new, tell(x->x_fd));
#endif
		}
	}
}

/*
 * Continue a transfer which is in the asynchronously marked state.
 */
filecontinue(x, t)
register struct xfer *x;
register struct transaction *t;
{
	if (x->x_state != X_ERROR) {
		errstring = "CONTINUE received when not in error state.";
		error(t, x->x_fh->f_name, BUG);
	} else {
		x->x_state = X_RETRY;
		respond(t, NOSTR);
	}
}

/*
 * Here are commands that are usually top-level but can also be
 * issued in reference to an existing transfer.
 */
/*
 * Delete the given file.
 * If the delete is on a file handle, delete the file that is open.
 * If it open for writing and has a tempname delete the tempname, NOT the
 * realname.
 */
delete(t)
register struct transaction *t;
{
	register struct file_handle *f = t->t_fh;
	register struct xfer *x;
	char *file, *dir = NOSTR, *real = NOSTR, *fhname;
	struct stat sbuf;
	int errcode;

	if (f != FNULL)
		if (t->t_args != ANULL && t->t_args->a_strings[0] != NOSTR) {
			errstring =
				"Both a file handle and filename in DELETE";
			error(t, f->f_name, BUG);
		} else if ((x = f->f_xfer) == XNULL) {
			errstring =
				"No transfer when DELETE on file handle";
			error(t, f->f_name, BUG);
		} else if (f->f_xfer->x_options & (O_PROPERTIES|O_DIRECTORY)) {
			errstring =
				"Trying to DELETE a directory list transfer";
			error(t, f->f_name, BUG);
		} else {
			fhname = f->f_name;
			if (unlink(x->x_tempname ? x->x_tempname : x->x_realname) < 0)
				goto badunlink;
			f->f_xfer->x_flags |= X_DELETE;
			respond(t, NOSTR);
#ifndef SELECT
			t = makecmd(&delcom, f);
			sendcmd(t, f->f_xfer);
#endif			
		}
	else if (t->t_args == ANULL ||
		 (file = t->t_args->a_strings[0]) == NOSTR) {
		errstring = "No file handle or file name in DELETE";
		error(t, "", BUG);
	} else if ((errcode = parsepath(file, &dir, &real, 0)) != 0)
		error(t, "", errcode);
	else if (stat(real, &sbuf) < 0) {
		switch (errno) {
		case EACCES:
			errcode = ATD;
			errstring = SEARCHDIR;
			break;
		case ENOENT:
			if (access(dir, 0) == 0)
				errcode = FNF;
			else
				errcode = DNF;
			break;
		case ENOTDIR:
			errcode = DNF;
			errstring = PATHNOTDIR;
			break;
		default:
			errcode = MSC;
			errstring = strerror(errno);
		}
		error(t, "", errcode);
	} else if ((sbuf.st_mode & S_IFMT) == S_IFDIR) {
		if (access(dir, 2) != 0) {
			errstring = SEARCHDIR;
			error(t, "", ATD);
		} else if (access(real, 3) != 0) {
			errstring =
				"No search or write permission on directory to be deleted.";
			error(t, "", ATD);
		} else {
			register int pid, rp;
			int st;

			if ((pid = fork()) == 0) {
				(void)close(0);
				(void)close(1);
				(void)close(2);
				(void)open("/dev/null", 2);
				(void)dup(0); (void)dup(0);
				execl("/bin/rmdir", "rmdir", real, 0);
				execl("/usr/bin/rmdir", "rmdir", real, 0);
				exit(1);
			} else if (pid == -1) {
				errstring = "Can't fork subprocess for rmdir";
				error(t, "", NER);
			} else {
				while ((rp = wait(&st)) >= 0)
					if (rp != pid)
						nprocdone++;
					else
						break;
				if (rp != pid)
					fatal("Lost a process!");
				if (st != 0) {
					/*
					 * We are not totally sure this is
					 * the reason but...
					 */
					errstring =
					"Directory to delete is not empty.";
					error(t, "", DNE);
				} else
					respond(t, NOSTR);
			}
		}
	} else if (unlink(real) < 0) {
		fhname = "";
badunlink:
		switch (errno) {
		case EACCES:
			if (access(dir, 1) == 0)
				errstring = WRITEDIR;
			else
				errstring = SEARCHDIR;
			errcode = ATD;
			break;
		case ENOENT:
			if (access(dir, 0) == 0)
				errcode = FNF;
			else
				errcode = DNF;
			break;
		case ENOTDIR:
			errstring = PATHNOTDIR;
			errcode = DNF;
			break;
		default:
			errcode = MSC;
			errstring = strerror(errno);
		}
		error(t, fhname, errcode);
	} else
		respond(t, NOSTR);
	if (dir != NOSTR)
		free(dir);
	if (real != NOSTR)
		free(real);
}	

/*
 * Rename a file.
 */
xrename(t)
register struct transaction *t;
{
	register struct file_handle *f;
	register struct xfer *x;
	int errcode;
	char *file1, *file2, *dir1 = NOSTR, *dir2 = NOSTR,
		*real1 = NOSTR, *real2 = NOSTR;

	file1 = t->t_args->a_strings[0];
	file2 = t->t_args->a_strings[1];
	if ((errcode = parsepath(file1, &dir1, &real1, 0)) != 0)
		error(t, "", errcode);
	else if ((f = t->t_fh) != FNULL)
		if (file2 != NOSTR) {
			errstring =
			"Both file handle and file name specified on RENAME";
			error(t, f->f_name, BUG);
		} else if ((x = f->f_xfer) == XNULL) {
			errstring = "No transfer on file handle for RENAME";
			error(t, f->f_name, BUG);
		} else if (x->x_options & (O_DIRECTORY|O_PROPERTIES)) {
			errstring = "Can't rename in DIRECTORY command.";
			error(t, f->f_name, BUG);
		} else {
			if (x->x_options & O_WRITE) {	
				if (access(dir1, 3) != 0)
					errcode = ATD;
			} else
				errcode = mv(x->x_realname,
					     x->x_dirname, real1, dir1);
			if (errcode)
				error(t, f->f_name, errcode);
			else {
				free(x->x_realname);
				x->x_realname = real1;
				real1 = NOSTR;
				respond(t, NOSTR);
#ifndef SELECT
				t  = makecmd(&rencom, f);
				if ((t->t_args = salloc(cmdargs)) == ANULL)
					fatal(NOMEM);
				ainit(t->t_args);
				t->t_args->a_strings[0] = savestr(real1);
				sendcmd(t, x);
#endif			
			}
		}			
	else if (file2 == NOSTR) {
		errstring = "Missing second filename in RENAME";
		error(t, "", BUG);
	} else if ((errcode = parsepath(file2, &dir2, &real2, 0)) != 0)
		error(t, "", errcode);
	else if ((errcode = mv(real1, dir1, real2, dir2)) != 0)
		error(t, "", errcode);
	else
		respond (t, NOSTR);
	if (dir1)
		free(dir1);
	if (dir2)
		free(dir2);
	if (real1)
		free(real1);
	if (real2)
		free(real2);
}
/*
 * mv, move one file to a new name (works for directory)
 * Second name must not exist.
 * Errors are returned in strings.
 */
mv(from, fromdir, to, todir)
char *from, *fromdir, *to, *todir;
{
	struct stat sbuf;
	int olderrno, didstat;

	/*
	 * We don't want to do more system calls than necessary, but we can't
	 * allow the super-user to unlink directories.
	 */
	if (getuid() == 0) {
		didstat = 1;
		if (stat(from, &sbuf) < 0)
			goto fromstat;
		if ((sbuf.st_mode & S_IFMT) == S_IFDIR)
			return IOD;
	} else
		didstat = 0;
	if (link(from, to) == 0 && unlink(from) == 0)
		return 0;
	olderrno = errno;
	if (didstat == 0 && stat(from, &sbuf) < 0) {
fromstat:
		switch (errno) {
		case EACCES:
			errstring = SEARCHDIR;
			return ATD;
		case ENOENT:
			return access(fromdir, 0) == 0 ? FNF : DNF;
		case ENOTDIR:
			errstring = PATHNOTDIR;
			return DNF;
		default:
			errstring = strerror(errno);
			return MSC;
		}
	}
	if ((sbuf.st_mode & S_IFMT) == S_IFDIR)
		return IOD;
	if (access(fromdir, 2) < 0) {
		errstring = "No permission to modify source directory";
		return ATD;
	}
	if (stat(to, &sbuf) >= 0)
		return REF;
	switch (errno) {
	case EACCES:
		errstring = SEARCHDIR;
		return ATD;
	case ENOTDIR:
		errstring = PATHNOTDIR;
		return DNF;
	case ENOENT:
		if (access(todir, 0) != 0)
			return DNF;
		/*
		 * Everything looks ok. Look at the original errno.
		 */
		if (olderrno == EXDEV) {
			errstring = "Can't rename across UNIX file systems.";
			return RAD;
		}
		errno = olderrno;
	default:
		errstring = strerror(errno);
		return MSC;
	}
}

/*
 * File name completion.
 */
#define SNONE		0
#define SEXACT		1
#define SPREFIX		2
#define SMANY		3
#define	SDEFAULT	4
#define replace(old, new) if (old) free(old); old = new ? savestr(new) : NOSTR

complete(t)
register struct transaction *t;
{
	register char *cp, *tp;
	int errcode, nstate, tstate;
#if !defined(BSD42) && !defined(linux)
	int dfd;
#else
	DIR *dfd;
	struct direct *dirp;
#endif

	char *dfile, *ifile, *ddir, *idir, *dreal, *ireal, *dname, *iname,
		*dtype, *itype, *adir, *aname, *atype;
	union {
		struct direct de;
		char dummy[sizeof(struct direct) + 1];
	} d;
	char response[CHMAXDATA + 1];

	if ((t->t_args->a_options & ~(O_NEWOK|O_OLD|O_DELETED|O_READ|O_WRITE))) {
		error(t, "", UUO);
		return;
	}
	d.dummy[sizeof(struct direct)] = '\0';
	dfile = t->t_args->a_strings[0];
	ifile = t->t_args->a_strings[1];
	adir = ddir = dreal = idir = ireal = NOSTR;
	aname = atype = NOSTR;
	iname = itype = dname = dtype = NOSTR;
	if (dfile[0] != '/') {
		errstring =
			"Default for completion is not an absolute pathname";
		error(t, "", IPS);
	} else if ((errcode = parsepath(dfile, &ddir, &dreal, 1)) != 0)
		error(t, "", errcode);
	else {
		if (ifile[0] != '/') {
			if ((adir = malloc((unsigned)
					(strlen(ddir) + strlen(ifile) + 2)))
			    == NOSTR)
			    	fatal(NOMEM);
			strcpy(adir, ddir);
			strcat(adir, "/");
			strcat(adir, ifile);
			if ((errcode = parsepath(adir,
						 &idir, &ireal, 1)) != 0) {
				error(t, "", errcode);
				goto freeall;
			}
			free(adir);
			adir = idir;
			idir = NOSTR;
		} else if ((errcode = parsepath(ifile,
						&adir, &ireal, 1)) != 0) {
			error(t, "", errcode);
			goto freeall;
		}
		cp = &dreal[strlen(ddir)];
		if (*cp == '/')
			cp++;
		if (*cp != '\0')
			if ((tp = rindex(cp, '.')) == NOSTR)
				dname = savestr(cp);
			else {
				*tp = '\0';
				dname = savestr(cp);
				dtype = savestr(tp+1);
				*tp = '.';
			}
		cp = &ireal[strlen(adir)];
		if (*cp == '/')
			cp++;
		if (*cp != '\0')
			if ((tp = rindex(cp, '.')) == NOSTR)
				iname = savestr(cp);
			else {
				*tp = '\0';
				iname = savestr(cp);
				itype = savestr(tp + 1);
				*tp = '.';
			}
#ifdef LOG_VERBOSE
		syslog(LOG_INFO, "ifile:'%s'\nireal:'%s'\nidir:'%s'\n",
				ifile ? ifile : "!",
				ireal ? ireal : "!",
				idir ? idir : "!");
		syslog(LOG_INFO, "dfile:'%s'\ndreal:'%s'\nddir:'%s'\n",
				dfile ? dfile : "!",
				dreal ? dreal : "!",
				ddir ? ddir : "!");
		syslog(LOG_INFO, "iname:'%s'\nitype:'%s'\n",
				iname ? iname : "!",
				itype ? itype : "!");
		syslog(LOG_INFO, "dname:'%s'\ndtype:'%s'\n",
				dname ? dname : "!",
				dtype ? dtype : "!");
		syslog(LOG_INFO, "adir:'%s'\n",
				adir ? adir : "!");
#endif
#if !defined(BSD42) && !defined(linux)
		if ((dfd = open(adir, 0)) < 0) {
#else
		if( (dfd = opendir(adir)) == NULL ) {
#endif
			switch(errno) {
			case ENOENT:
				errcode = DNF;
				break;
			case ENOTDIR:
				errstring = PATHNOTDIR;
				errcode = DNF;
				break;
			case ENFILE:
				errstring = "No file descriptors available";
				errcode = NER;
				break;
			case EACCES:
				errstring = READDIR;
				errcode = ATD;
				break;
			default:
				errstring = strerror(errno);
				errcode = MSC;
				break;
			}
			error(t, "", errcode);
			goto freeall;
		}
		nstate = tstate = SNONE;
#if !defined(BSD42) && !defined(linux)
		while (read(dfd, (char *)&d.de, sizeof(d.de)) == sizeof(d.de)) { 
			char *ename, *etype;
			int namematch, typematch;

			if (d.de.d_ino == 0 ||
			    (d.de.d_name[0] == '.' &&
			     (d.de.d_name[1] == '\0' ||
			      (d.de.d_name[1] == '.' && d.de.d_name[2] == '\0'))))
				continue;
			ename = d.de.d_name;
#else
		while( (dirp = readdir(dfd)) != NULL ) {
			char *ename, *etype;
			int namematch, typematch;

			if (dirp->d_ino == 0 ||
			    (dirp->d_name[0] == '.' &&
			     (dirp->d_name[1] == '\0' ||
			      (dirp->d_name[1] == '.' && dirp->d_name[2] == '\0'))))
				continue;
			ename = dirp->d_name;
#endif
			if ((etype = rindex(ename, '.')) != NOSTR)
				*etype++ = '\0';
			if ((namematch = prefix(iname, ename)) == SNONE ||
			    (typematch = prefix(itype, etype)) == SNONE)
				continue;
#ifdef LOG_VERBOSE
			syslog(LOG_INFO, "ename:'%s'\netype:'%s'\n",
				ename ? ename : "!",
				etype ? etype : "!");
			syslog(LOG_INFO, "nm:%d, tm:%d, ns:%d, ts:%d\n",
				namematch, typematch, nstate, tstate);
#endif
			if (namematch == SEXACT) {
				if (typematch == SEXACT) {
					nstate = tstate = SEXACT;
					goto gotit;
				}
				if (dtype && strcmp(etype, dtype) == 0)
					tstate = SDEFAULT;
				else if (nstate != SEXACT) {
					replace(atype, etype);
					tstate = SPREFIX;
				} else if (tstate != SDEFAULT) {
					tstate = SMANY;
					incommon(atype, etype);
				}
				nstate = SEXACT;
			} else if (nstate == SNONE) {
				nstate = SPREFIX;
				replace(aname, ename);
				if (typematch == SEXACT && iname != NOSTR)
					tstate = SEXACT;
				else if (dtype && strcmp(etype, dtype) == 0)
					tstate = SDEFAULT;
				else {
					replace(atype, etype);
					tstate = SPREFIX;
				}
			} else if (tstate == SEXACT) {
				if (typematch == SEXACT) {
					nstate = SMANY;
					incommon(aname, ename);
				}
			} else if (typematch == SEXACT && iname != NOSTR) {
				replace(aname, ename);
				tstate = SEXACT;
				nstate = SPREFIX;
			} else if (dtype && strcmp(etype, dtype) == 0)
				if (tstate == SDEFAULT) {
					incommon(aname, ename);
					nstate = SMANY;
				} else {
					replace(aname, ename);
					tstate = SDEFAULT;
					nstate = SPREFIX;
				}
			else if (tstate != SDEFAULT)
				if (nstate == SPREFIX) {
					if (tstate != SDEFAULT) {
						incommon(aname, ename);
						incommon(atype, etype);
						tstate = nstate = SMANY;
					}
				} else if (nstate == SMANY) {
					tstate = SMANY;
					incommon(atype, etype);
				}
#ifdef LOG_VERBOSE
		syslog(LOG_INFO, "aname:'%s'\natype:'%s'\n",
				aname ? aname : "!",
				atype ? atype : "!");
		syslog(LOG_INFO, "nstate: %d\n, tstate: %d\n", nstate, tstate);
#endif
		}	
gotit:
#if !defined(BSD42) && !defined(linux)
		(void)close(dfd);
#else
		closedir(dfd);
#endif
		if (tstate != SEXACT && tstate != SNONE) {
			if (itype) free(itype);
			if (tstate == SDEFAULT) {
				itype = dtype;
				dtype = NOSTR;
			} else {
				itype = atype;
				atype = NOSTR;
			}
		}
		if (nstate != SEXACT && nstate != SNONE) {
			if (iname) free(iname);
			iname = aname;
			aname = NOSTR;
		}
		(void)sprintf(errbuf, "%s%s%s%s%s",
			adir ? adir : "", adir && adir[1] != '\0' ? "/" : "",
			iname ? iname : "", itype ? "." : "",
			itype ? itype : "");
		if ((nstate == SEXACT || nstate == SPREFIX) &&
		    (tstate == SEXACT || tstate == SPREFIX) &&
		    stat(errbuf, &sbuf) == 0 &&
		    (sbuf.st_mode & S_IFMT) == S_IFDIR)
			strcat(errbuf, "/");
		respond(t, sprintf(response, "%s%c%s%c",
			nstate == SNONE || nstate == SMANY || tstate == SMANY ?
				"NIL" : "OLD",
			CHNL, errbuf, CHNL));
	}
freeall:
	if (iname) free(iname);
	if (itype) free(itype);
	if (dname) free(dname);
	if (dtype) free(dtype);
	if (aname) free(aname);
	if (atype) free(atype);
	if (adir) free(adir);
	if (ireal) free(ireal);
	if (dreal) free(dreal);
	if (idir) free(idir);
	if (ddir) free(ddir);	
}
incommon(old, new)
register char *old, *new;
{
	if (old != NOSTR && new != NOSTR) {
		while (*old && *new++ == *old)
			old++;
		*old = 0;
	}
}
prefix(in, new)
register char *in, *new;
{
	if (in == NOSTR)
		return new == NOSTR ? SEXACT : SPREFIX;
	if (new == NOSTR)
		return SNONE;
	while (*in == *new++)
		if (*in++ == '\0')
			return SEXACT;
	return *in == '\0' ? SPREFIX : SNONE;
}

/*
 * Create a directory
 */
crdir(t)
register struct transaction *t;
{
	char *dir = NULL, *file = NULL, *parent = NULL;
	int errcode;
	struct stat sbuf;

	if (errcode = parsepath(t->t_args->a_strings[0], &dir, &file, 1))
		error(t, "", errcode);
	else {
		free(file);
		file = NULL;
		if (errcode = parsepath(dir, &parent, &file, 0))
			error(t, "", errcode);
		else if (access(parent, 3) != 0) {
			if (errno == EACCES) {
				errcode = ATD;
				errstring =
				"Permission denied on parent directory.";
			} else {
				errcode = DNF;
				errstring = "Parent directory doesn't exist.";
			}
			error(t, "", errcode);
		} else if (stat(dir, &sbuf) >= 0) {
			if ((sbuf.st_mode & S_IFMT) == S_IFDIR) {
				errcode = DAE;
				errstring = "Directory already exists."	;
			} else {
				errcode = DAE;
				errstring =
					"File already exists with same name.";
			}
			error(t, "", errcode);
		} else {
			register int pid, rp;
			int st;

			if ((pid = fork()) == 0) {
				(void)close(0);
				(void)close(1);
				(void)close(2);
				(void)open("/dev/null", 2);
				(void)dup(0); (void)dup(0);
				execl("/bin/mkdir", "mkdir", dir, 0);
				execl("/usr/bin/mkdir", "mkdir", dir, 0);
				exit(1);
			}
			while ((rp = wait(&st)) >= 0)
				if (rp != pid)
					nprocdone++;
				else
					break;
			if (rp != pid)
				fatal("Lost a process!");
			if (st != 0) {
				errstring = "UNIX mkdir failed.";
				error(t, "", MSC);
			} else
				respond(t, NOSTR);
		}
	}
	if (file)
		free(file);
	if (dir)
		free(dir);
	if (parent)
		free(parent);
}

/*
 * Expunge directory.  This is rather easy on UNIX.
 */
expunge(t)
register struct transaction *t;
{
	char *dir = NULL, *file = NULL;
	int errcode;
	
	if (errcode = parsepath(t->t_args->a_strings[0], &dir, &file, 1))
		error(t, "", errcode);
	else {
		free(file);
		free(dir);
		respond(t, "0");
	}
}

/*
 * Change properties.  Either of a file or the file on a tranfer.
 */
chngprop(t)
register struct transaction *t;
{
	register struct file_handle *f = t->t_fh;
	register struct xfer *x = XNULL;
	int errcode;
	char *file, *dir, *fhname;
	struct stat sbuf;

	if (f != FNULL) {
		fhname = f->f_name;
		if (t->t_args != ANULL && t->t_args->a_strings[0] != NOSTR &&
		    t->t_args->a_strings[0][0] != '\0') {
			errstring =
			"Both file handle and file name in CHANGE-PROPERTIES";
			error(t, fhname, BUG);
		} else if ((x = f->f_xfer) == XNULL) {
			errstring =
			"No transfer on file handle for CHANGE-PROPERTIES";
			error(t, fhname, BUG);
		} else if (f->f_xfer->x_options & (O_PROPERTIES|O_DIRECTORY)) {
			errstring = "CHANGE-PROPERTIES on directory transfer";
			error(t, fhname, BUG);
		} else {
			file = x->x_options & O_WRITE ? x->x_tempname :
							x->x_realname;
			if (stat(file, &sbuf) < 0)
				fatal(FSTAT);
			goto doit;
		}
	} else
		fhname = "";
	if (errcode = parsepath(t->t_args->a_strings[0], &dir, &file, 0))
		error(t, fhname, errcode);
	else if (stat(file, &sbuf) < 0) {
		switch (errno) {
		case EACCES:
			errcode = ATD;
			errstring = SEARCHDIR;
			break;
		case ENOENT:
			if (access(dir, 0) == 0)
				errcode = FNF;
			else
				errcode = DNF;
			break;
		case ENOTDIR:
			errcode = DNF;
			errstring = PATHNOTDIR;
			break;
		default:
			errcode = MSC;
			errstring = strerror(errno);
		}
		error(t, fhname, errcode);
	} else {
		register struct property *pp;
		register struct plist *plp;
doit:
		for (plp = t->t_args->a_plist; plp; plp = plp->p_next) {
			for (pp = properties; pp->p_indicator; pp++)
				if (strcmp(pp->p_indicator, plp->p_name) == 0)
					if (pp->p_put)
						if (errcode =
						    (*pp->p_put)
						    (&sbuf, file,
						     plp->p_value, x)) {
							error(t,
							      fhname, errcode);
							return;
						} else
							break;
					else {
						errstring = errbuf;
						(void)sprintf(errstring,
						"No a changeable property: %s",
							plp->p_name);
						error(t, fhname, CSP);
						return;
					}
			if (pp->p_indicator == NOSTR) {
				(void)sprintf(errbuf,
					"Unknown property name: %s",
					plp->p_name);
				errstring = errbuf;
				error(t, fhname, UKP);
				return;
			}
		}
		respond(t, NOSTR);
	}
}

/*
 * Property routines - one to get each property gettable by the DIRECTORY
 * command and one to put each property changeable by the change properties
 * command.
 */
char *
getdev(s, cp)
register struct stat *s;
register char *cp;	
{
	(void)sprintf(cp, "%o", s->st_dev);
	while (*cp)
		cp++;
	return cp;
}
/* ARGSUSED */
char *
getblock(s, cp)
struct stat *s;
char *cp;
{
	(void)sprintf(cp, "%d", FSBSIZE);
	while (*cp)
		cp++;
	return cp;
}
char *
getspace(s, cp)
struct stat *s;
char *cp;
{
	long	total;
	long	free;
	long	used;
	int	fd, len;
	struct stat mstbuf;
#ifdef linux
	struct statfs sblock;
#else
#ifdef BSD42
	struct fs sblock;
#else
	struct filsys sblock;
#endif
#endif
	struct mtab {
		char path[32];
		char spec[32];
	} mtab;
	char dev[32 + sizeof("/dev/")];

	if (!s)
		return 0;
	if ((fd = open("/etc/mtab", 0)) < 0)
		return 0;
	while((len = read(fd, (char *)&mtab, sizeof(mtab))) == sizeof(mtab)) {
		strcpy(dev, "/dev/");
		strcat(dev, mtab.spec);
		if (stat(dev, &mstbuf) == 0 &&
		    mstbuf.st_rdev == s->st_dev) {
			break;
		}
	}
	(void)close(fd);
	if(len != sizeof(mtab))
		return 0;
#ifdef linux
	if (statfs(dev, &sblock))
		return 0;
	total = sblock.f_bsize * (sblock.f_blocks - sblock.f_bfree);
	free = sblock.f_bsize * sblock.f_bfree;
	used = total - free;
#else
	if ((fd = open(dev, 0)) < 0)
		return 0;
#ifdef BSD42
	lseek(fd, SBLOCK * DEV_BSIZE, 0);
#else
	lseek(fd, 1L<<BSHIFT, 0);
#endif
	len = read(fd, (char *)&sblock, sizeof(sblock));
	(void)close(fd);
	if (len != sizeof(sblock))
		return 0;
#ifdef BSD42
	total = sblock.fs_dsize;
	free = sblock.fs_cstotal.cs_nbfree * sblock.fs_frag +
	    sblock.fs_cstotal.cs_nffree;
#else
	total = (long) sblock.s_fsize - (long)sblock.s_isize;
	free = sblock.s_tfree;
#endif
	used = total - free;
#endif

	(void)
	sprintf(cp, "%s (%s): %ld free, %ld/%ld used (%ld%%)", mtab.path, mtab.spec,
		free, used, total, (100L * used + total / 2) / total);
	while (*cp)
		cp++;
	return cp;
}
/*
 * We don't account for indirect blocks...
 */
char *
getbsize(s, cp)
register struct stat *s;
register char *cp;	
{
	(void)sprintf(cp, "%ld", (s->st_size + FSBSIZE - 1) / FSBSIZE);
	while (*cp)
		cp++;
	return cp;
}
/* ARGSUSED */
char *
getbyte(s, cp)
struct stat *s;
char *cp;
{
	*cp++ = '8';
	*cp = '\0';
	return cp;
}
char *
getsize(s, cp)
register struct stat *s;
register char *cp;	
{
	(void)sprintf(cp, "%ld", s->st_size);
	while (*cp)
		cp++;
	return cp;
}
char *
getmdate(s, cp)
register struct stat *s;
register char *cp;	
{
	struct tm *tm;

	tm = localtime(&s->st_mtime);
	(void)sprintf(cp, "%02d/%02d/%02d %02d:%02d:%02d",
			tm->tm_mon+1, tm->tm_mday, tm->tm_year,
			tm->tm_hour, tm->tm_min, tm->tm_sec);
	while (*cp)
		cp++;
	return cp;
}
char *
getrdate(s, cp)
register struct stat *s;
register char *cp;	
{
	struct tm *tm;

	tm = localtime(&s->st_atime);
	(void)sprintf(cp, "%02d/%02d/%02d %02d:%02d:%02d",
			tm->tm_mon+1, tm->tm_mday, tm->tm_year,
			tm->tm_hour, tm->tm_min, tm->tm_sec);
	while (*cp)
		cp++;
	return cp;
}
/*
char *
getcdate(s, cp)
register struct stat *s;
register char *cp;	
{
	struct tm *tm;

	tm = localtime(&s->st_mtime);
	(void)sprintf(cp, "%02d/%02d/%02d %02d:%02d:%02d",
			tm->tm_mon+1, tm->tm_mday, tm->tm_year,
			tm->tm_hour, tm->tm_min, tm->tm_sec);
	while (*cp)
		cp++;
	return cp;
}
*/
char *
getdir(s, cp)
register struct stat *s;
register char *cp;	
{
	if ((s->st_mode & S_IFMT) == S_IFDIR) {
		*cp++ = 'T';
		*cp = '\0';
		return cp;
	} else
		return 0;
}
char *
getname(s, cp)
register struct stat *s;
register char *cp;	
{
	register struct passwd *pw;

	if (pw = getpwuid(s->st_uid))
		(void) sprintf(cp, "%s", pw->pw_name);
	else
		(void) sprintf(cp, "#%d", s->st_uid);
	while (*cp)
		cp++;
	return cp;
}
char *
getprot(s, cp)
register struct stat *s;
register char *cp;	
{
	(void)sprintf(cp, "0%o", s->st_mode & 07777);
	while (*cp)
		cp++;
	return cp;
}
/* ARGSUSED */
char *
getsprops(s, cp)
struct stat *s;
register char *cp;
{
	register struct property *pp;
	register char *p;

	for (p = cp, pp = properties; pp->p_indicator; pp++)
		if (pp->p_put) {
			if (p != cp)
				*p++ = ' ';
			strcpy(p, pp->p_indicator);
			while (*p)
				p++;
		}
	if (p != cp && p[-1] == ' ')
		p--;
	*p = '\0';
	return p;
}
putprot(s, file, newprot)
register struct stat *s;
char *file;
char *newprot;
{
	register char *cp;
	int mode;

	for (mode = 0, cp = newprot; *cp; cp++)
		if (*cp < '0' || *cp > '7') {
			(void)
			sprintf(errstring = errbuf,
			       "Illegal protection mode, must be octal: %s",
			       newprot);
			return IPV;
		} else {
			mode <<= 3;
			mode |= *cp & 7;
		}
	if (mode > 07777 || mode < 0) {
		(void)sprintf(errstring = errbuf,
			      "Illegal protection mode, must be octal: %s",
			      newprot);
		return IPV;
	}
	if (mode != (s->st_mode & 07777) && chmod(file, mode) < 0) {
		errstring = "No permission to change protection modes";
		return ATF;
	}
	return 0;
}

putname(s, file, newname)
register struct stat *s;
char *file;
char *newname;
{
	register struct passwd *pw;

	if ((pw = getpwnam(downcase(newname))) == NULL) {
		(void)sprintf(errstring = errbuf, "Unknown user name: %s",
			      newname);
		return IPV;
	} else if (s->st_uid != pw->pw_uid &&
		   chown(file, pw->pw_uid, s->st_gid) < 0) {
		errstring = "No permission to change author";
		return ATF;
	}
	return 0;
}

/*
 * This should be deferred until close time.
 * Actually it should be done twice - now and at close time.
 */
putmdate(s, file, newtime, x)
register struct stat *s;
char *file;
char *newtime;
register struct xfer *x;
{
	time_t mtime;

	if (parsetime(newtime, &mtime)) {
		errstring = "Illegal modification date format";
		return IPV;
	}
	if (mtime != s->st_mtime) {
		time_t timep[2];

		timep[0] = s->st_atime;
		timep[1] = mtime;
		if (utime(file, timep) < 0) {
			errstring =
				"No permission to change modification time";
			return ATF;
		} else if (x) {
			x->x_mtime = mtime;
			x->x_flags |= X_MTIME;
#ifndef SELECT
			{
				register struct transaction *t;

				t  = makecmd(&mdatcom, x->x_fh);
				if ((t->t_args = salloc(cmdargs)) == ANULL)
					fatal(NOMEM);
				ainit(t->t_args);
				t->t_args->a_numbers[0] = mtime;
				sendcmd(t, x);
			}
#endif			

		}
	}
	return 0;
}

int
putrdate(s, file, newtime, x)
register struct stat *s;
char *file;
char *newtime;
register struct xfer *x;
{
	time_t atime;

	if (parsetime(newtime, &atime)) {
		errstring = "Illegal reference date format";
		return IPV;
	}
	if (atime != s->st_atime) {
		time_t timep[2];

		timep[1] = s->st_mtime;
		timep[0] = atime;
		if (utime(file, timep) < 0) {
			errstring = "No permission to change reference date";
			return ATF;
		} else if (x) {
			x->x_mtime = atime;
			x->x_flags |= X_ATIME;
#ifndef SELECT
			{
				register struct transaction *t;

				t  = makecmd(&adatcom, x->x_fh);
				if ((t->t_args = salloc(cmdargs)) == ANULL)
					fatal(NOMEM);
				ainit(t->t_args);
				t->t_args->a_numbers[0] = atime;
				sendcmd(t, x);
			}
#endif			
		}
	}
	return 0;
}

static	int	dmsize[12] =
{
	31,
	28,
	31,
	30,
	31,
	30,
	31,
	31,
	30,
	31,
	30,
	31
};

int
parsetime(cp, t)
register char *cp;
register time_t *t;
{
	register int i;
	char *tnum();
	int month, day, year, hour, minute, second;

	if (!(cp = tnum(cp, '/', &month)) ||
	    !(cp = tnum(cp, '/', &day)) ||
	    !(cp = tnum(cp, ' ', &year)) ||
	    !(cp = tnum(cp, ':', &hour)) ||
	    !(cp = tnum(cp, ':', &minute)) ||
	    !(cp = tnum(cp, '\0', &second)) ||
	    month < 1 || month > 12 ||
	    day < 1 || day > 31 ||
	    year < 70 || year > 99 ||
	    hour < 0 || hour > 23 ||
	    minute < 0 || minute > 59 ||
	    second < 0 || second > 59)
		return 1;
	year += 1900;
	*t = 0;
#define dysize(i) (((i%4)==0) ? 366 : 365)
	for (i = 1970; i < year; i++)
		*t += dysize(i);
	if (dysize(year) == 366 && month >= 3)
		(*t)++;
	while (--month)
		*t += dmsize[month - 1];
	*t += day - 1;
	*t *= 24;
	*t += hour;
	*t *= 60;
	*t += minute;
	*t *= 60;
	*t += second;
	/*
	 * Now convert to GMT
	 */
	*t += (long)timeinfo.timezone * 60;
	if(localtime(t)->tm_isdst)
		*t -= 60*60;
	return 0;
}

char *
tnum(cp, delim, ip)
register char *cp;
char delim;
int *ip;
{
	register int i;

	if (isdigit(*cp)) {
		i = *cp - '0';
		if (isdigit(*++cp)) {			
			i *= 10;
			i += *cp - '0';
			cp++;
		}
		if (*cp == delim) {
			*ip = i;
			return ++cp;
		}
	}
	return 0;
}
		

/*
 * Parse the pathname into directory and full name parts.
 * Returns text messages if anything is wrong.
 * Checking is done for well-formed pathnames, .. components,
 * and leading tildes.  Wild carding is not done here. Should it?
 */
parsepath(path, dir, real, blankok)
register char *path;
char **dir, **real;
int blankok;
{
	register char *cp;
	int errcode;
	char *wd, save;

	if (*path == '~') {
		for (cp = path; *cp != '\0' && *cp != '/'; cp++)
			;
		if (cp == path + 1) {
			if ((wd = home) == NOSTR) {
				errstring = "Can't expand '~', not logged in.";
				return NLI;
			}
		} else {
			struct passwd *pw;

			save = *cp;
			*cp = '\0';
			if ((pw = getpwnam(path+1)) == NULL) {
				*cp = save;
				(void)sprintf(errstring = errbuf,
					"Unknown user name: %s after '~'.",
					path+1);
				return IPS;	/* Invalid pathname syntax */
			}
			*cp = save;
			wd = pw->pw_dir;
		}
		path = cp;
		while (*path == '/')
			path++;
	} else if (*path == '/')
		wd = "";
	else if ((wd = cwd) == NOSTR) {
		errstring = "Relative pathname when no working directory";
		return IPS;
	}
	if ((cp = malloc((unsigned)(strlen(wd) + strlen(path) + 2))) == NOSTR)
		fatal(NOMEM);
	strcpy(cp, wd);
	if (wd[0] != '\0')
		(void)strcat(cp, "/");
	(void)strcat(cp, path);
	if (errcode = dcanon(cp, blankok)) {
		(void)free(cp);
		return errcode;
	}
	*real = cp;
	if ((cp = rindex(cp, '/')) == NOSTR)
		fatal("Parsepath");
	if (cp == *real)
		cp++;
	save = *cp;
	*cp = '\0';
	*dir = savestr(*real);
	*cp = save;
	return 0;
}

/*
 * dcanon - canonicalize the pathname, removing excess ./ and ../ etc.
 *	we are of course assuming that the file system is standardly
 *	constructed (always have ..'s, directories have links)
 * Stolen from csh (My old code).
 */
dcanon(cp, blankok)
char *cp;
int blankok;
{
	register char *p, *sp;
	register int slash;

	if (*cp != '/')
		fatal("dcanon");
	for (p = cp; *p; ) {		/* for each component */
		sp = p;			/* save slash address */
		while(*++p == '/')	/* flush extra slashes */
			;
		if (p != ++sp)
			strcpy(sp, p);
		p = sp;			/* save start of component */
		if (*sp == '\0') { 	/* if component is null */
			if (--sp != cp)	/* if path is not one char (i.e. /) */
				if (blankok)
					break;
				else
					return IPS;
			break;
		}
		slash = 0;
		if (*p) while(*++p)	/* find next slash or end of path */
			if (*p == '/') {
				slash = 1;
				*p = 0;
				break;
			}
/* Can't do this now since we also parse {foo,abc,xyz}
		if (p - sp > DIRSIZ) {
			(void)sprintf(errstring = errbuf,
			"Pathname component: '%s', longer than %d characters",
				sp, DIRSIZ);
			return IPS;
		}
*/
		if (sp[0] == '.' && sp[1] == '\0') {
			if (slash) {
				strcpy(sp, ++p);
				p = --sp;
			}
		} else if (sp[0] == '.' && sp[1] == '.' && sp[2] == '\0') {
			if (--sp != cp)
				while (*--sp != '/')
					;
			if (slash) {
				strcpy(++sp, ++p);
				p = --sp;
			} else if (cp == sp)
				*++sp = '\0';
			else
				*sp = '\0';
		} else if (slash)
			*p = '/';
	}
	return 0;
}

/*
 * File handle error routine.
 */
fherror(f, code, type, message)
register struct file_handle *f;
char *message;
{
	register struct file_error *e = &errors[code];
	struct chpacket pkt;
	register int len;

	pkt.cp_op = ASYNOP;
	(void)sprintf(pkt.cp_data, "TIDNO %s ERROR %s %c %s",
		f->f_name, e->e_code, type, message ? message : e->e_string);
	len = 1 + strlen(pkt.cp_data);
	if (f->f_type == F_OUTPUT) {
		if (write(1, (char *)&pkt, len) != len) 
			fatal(CTLWRITE);
	} else if (write(f->f_fd, (char *)&pkt, len) != len)
		return -1;
	return 0;
}


/*
 * Function to perform a unit of work on a transfer.
 * The point here is to do exactly one network operation.
 * Multiple disk/file operations can be performed.
 * If the transfer should be discarded completely, X_FLUSH
 * is returned.  If the transfer must have further commands
 * to continue, X_HANG is returned.  X_CONTINUE is returned if the
 * transfer is happy to continue what it is doing.
 *
 * Transfers go through several states, which differ greatly depending
 * on the direction of transfer.
 *
 * READ (disk to net) transfers:
 * 1. X_PROCESS		Initial state and data transfer state until:
 *			Normally - DATA packet sent: stay in X_PROCESS
 * 			- EOF read from disk, last data sent: go to X_REOF
 *			- Fatal error read from disk: go to X_ABORT
 *			- Error writing to net: go to X_BROKEN
 *			- CLOSE received: goto X_DONE
 * 2. X_REOF		After reading EOF from disk. Next:
 *			Normally - EOF packet sent: go to X_SEOF
 * 			- Error writing EOF to net: goto X_BROKEN
 *			- FILEPOS received: goto X_PROCESS
 *			- CLOSE received: goto X_DONE
 * 3. X_SEOF		After sending EOF to net.
 *			Normally - Wait for command - stay in X_SEOF
 *			- FILEPOS received: goto X_PROCESS
 *			- CLOSE received: goto X_DONE
 * 4. X_ABORT		After disk error.
 *			Normally - Wait for command - stay in X_ABORT
 *			- CLOSE received: goto X_DONE
 * 5. X_BROKEN		After net error.
 *			Normally - Wait for command - stay in X_BROKEN
 *			- CLOSE received: goto X_DONE
 * 6. X_DONE		Respond to close, send SYNC MARK.
 *			Normally - Flush connection.
 *
 * WRITE (net to disk) transfers:
 * 1. X_PROCESS		Initial state and data ransfer stats until:
 *			Normally - DATA packets read: stay in X_PROCESS
 *			- EOF read from net, last disk write: goto X_WSYNC
 *			- Error reading from net:
 *			  Send ASYNCMARK on control conn, goto X_BROKEN or
 *			  or X_DONE is CLOSE already arrived.
 *			- Error (recoverable) writing to disk:
 *			  (Remember if already got EOF)
 *			  Send ASYNCMARK on ctl, goto X_ERROR
 *			- Error (fatal) writingto disk, goto X_WSYNC
 *			- Read SYNCMARK, goto X_RSYNC
 * 2. X_WSYNC		Waiting for SYNCMARK
 *			Normally - Wait for SYNCMARK - stay in X_WSYNC
 *			CLOSE arriving just marks conn.
 *			- SYNCMARK arrives:
 *			  If CLOSE arrived, goto X_DONE, else goto X_RSYNC
 *			- Error reading from net:
 *			  Send ASYNCMARK on ctl, goto X_BROKEN
 *			- DATA or EOF read from net:
 *			  If no error so far, send ASYNCMARK - fatal,
 *			  otherwise ignore, in any case stay in X_WSYNC.
 * 3. X_ERROR		Waiting for CONTINUE or CLOSE
 *			Normally wait for command
 *			- CONTINUE arrives - goto X_RETRY
 *			- CLOSE arrives, goto X_WSYNC
 * 4. X_RETRY		Retry disk write
 *			- Another error - send ASYNCMARK, goto X_ERROR
 *			- Successful, goto X_PROCESS
 * 5. X_RSYNC		Wait for CLOSE
 *			- Close arrives, goto X_DONE
 * 6. X_BROKEN		Wait for CLOSE
 *			- CLOSE arrives, goto X_DONE
 * 7. X_DONE		Respond to CLOSE, if not BROKEN, send SYNCMARK.
 *
 */
dowork(x)
register struct xfer *x;
{
	/*
 	 * Now, do a unit of work on the transfer, which should be one
	 * packet read or written to the network.
	 * Note that this only provides task multiplexing as far as use of the
	 * network goes, not anything else.  However the non-network work
	 * is usually just a single disk operation which is probably not worth
	 * worrying about.
	 */
	switch (x->x_options & (O_READ | O_WRITE | O_DIRECTORY | O_PROPERTIES)) {
	case O_READ:
	case O_PROPERTIES:
	case O_DIRECTORY:
		switch (x->x_state) {
		case X_DERROR:
			return X_FLUSH;
		case X_DONE:
			/*
			 * Note we must respond to the close before sending the
			 * SYNCMARK since otherwise we would likely block on
			 * the data connection while the other end was blocked
			 * waiting for the CLOSE response.
			 */
			xclose(x);
			return X_SYNCMARK;
		case X_BROKEN:
		case X_ABORT:
		case X_SEOF:
		case X_IDLE:
			return X_HANG;
		case X_REOF:
			/*
			 * We have read an EOF from the disk/directory and
			 * flushed the last data packet into the net. So
			 * now we send the EOF packet and wait for further
			 * instructions (FILEPOS or CLOSE);
			 */			
			if (xpweof(x) < 0)
				x->x_state = X_BROKEN;
			else
				x->x_state = X_SEOF;
			return X_CONTINUE;
		case X_PROCESS:
			break;
		default:
			fatal("Bad state of transfer process");
		}
		while (x->x_room != 0) {
			if (x->x_left == 0) {
				long pos = tell(x->x_fd);
				register int n;

#ifdef LOG_VERBOSE
				syslog(LOG_INFO, "Before read pos: %ld\n", tell(x->x_fd));
#endif
				n = x->x_options & O_DIRECTORY ? dirread(x) :
					    x->x_options & O_PROPERTIES ? propread(x) :
					    read(x->x_fd, x->x_bbuf, FSBSIZE);
				
#ifdef LOG_VERBOSE
				syslog(LOG_INFO, "pid: %ld, x: %X, fd: %ld, Read: %ld\n",
					getpid(), x, x->x_fd, n);
#endif
				switch (n) {
				case 0:
					if (xpwrite(x) < 0)
						x->x_state = X_BROKEN;
					else
						x->x_state = X_REOF;
					return X_CONTINUE;
				case -1:
					if (fherror(x->x_fh, MSC, E_FATAL,
						    strerror(errno)) < 0)
						x->x_state = X_BROKEN;
					else
						x->x_state = X_ABORT;
					return X_CONTINUE;
				default:
					x->x_left = n;
					x->x_bptr = x->x_bbuf;
				}
			}
			switch (x->x_options & (O_CHARACTER|O_RAW|O_SUPER)) {
			case 0:
				if (x->x_bytesize <= 8) {
					register char *from = x->x_bptr;
					register char *to = x->x_pptr;
					
					do {
						*to++ = *from++;
						*to++ = '\0';
						x->x_room -=2;
					} while (--x->x_left && x->x_room);
					x->x_bptr = from;
					x->x_pptr = to;
					break;
				}
			case O_CHARACTER | O_RAW:
				{
					/* Could use VAX movcl3 here... */
					register int n;
					register char *from = x->x_bptr;
					register char *to = x->x_pptr;
					
					n = MIN(x->x_room, x->x_left);
					x->x_left -= n;
					x->x_room -= n;
					do *to++ = *from++; while (--n);
					x->x_bptr = from;
					x->x_pptr = to;
				}
				break;
			case O_CHARACTER:	/* default ascii */
			case O_CHARACTER | O_SUPER:
				to_lispm(x);
				break;
			}
		}
		if (xpwrite(x) < 0)
			x->x_state = X_BROKEN;
		else {
			x->x_pptr = x->x_pbuf;
			x->x_room = CHMAXDATA;
		}
		return X_CONTINUE;
	case O_WRITE:
		switch (x->x_state) {
		case X_DONE:
			xclose(x);
			return X_FLUSH;
		case X_BROKEN:
		case X_RSYNC:
		case X_ERROR:
			return X_HANG;
		case X_WSYNC:
			switch (xpread(x)) {
			case -1:
				x->x_state = x->x_flags & X_CLOSE ?
					     X_DONE : X_BROKEN;
				break;
			case -2:
				x->x_state = x->x_flags & X_CLOSE ?
					     X_DONE : X_RSYNC;
				break;
			default:
				/*
				 * Ignore other packets.
				 */
				 ;
			}
			return X_CONTINUE;
		case X_RETRY:
			if (xbwrite(x) < 0)
				goto writerr;	/* Kludge alert */
			x->x_state = x->x_flags & X_EOF ? X_WSYNC : X_PROCESS;
			return X_CONTINUE;
		case X_PROCESS:
			{
				register int n;
				
				switch (n = xpread(x)) {
				case 0:
					x->x_flags |= X_EOF;
					if (xbwrite(x) < 0)
						goto writerr;
					x->x_state = X_WSYNC;
					return X_CONTINUE;
				case -1:
					(void)fherror(x->x_fh, NET, E_FATAL,
						"Data connection error");
					x->x_state = x->x_flags & X_CLOSE ?
						     X_DONE : X_BROKEN;
					return X_CONTINUE;
				case -2:
					/*
					 * SYNCMARK before EOF, don't bother
					 * flushing disk buffer.
					 */
					x->x_state = x->x_flags & X_CLOSE ?
						     X_DONE : X_RSYNC;
					return X_CONTINUE;
				default:
					x->x_left = n;
					x->x_pptr = x->x_pbuf;
					break;
				}
			}
			break;
		default:
			fatal("Bad transfer task state");
		}
		/*
		 * Yow, a regular old packet full of data.
		 */		
		while (x->x_left) {
			switch (x->x_options & (O_CHARACTER|O_RAW|O_SUPER)) {
			case 0:
				if (x->x_bytesize <= 8) {
					register char *from = x->x_pptr;
					register char *to = x->x_bptr;
					
					do {
						*to++ = *from++;
						from++;
						x->x_left -=2;
					} while (--x->x_room && x->x_left);
					x->x_pptr = from;
					x->x_bptr = to;
					break;
				}
				/* Fall into... */
			case O_CHARACTER | O_RAW:
				{
					register int n;
					register char *from = x->x_pptr;
					register char *to = x->x_bptr;

					n = MIN(x->x_left, x->x_room);
					x->x_left -= n;
					x->x_room -= n;
					do *to++ = *from++; while (--n);
					x->x_pptr = from;
					x->x_bptr = to;
				}
				break;
			case O_CHARACTER:
			case O_CHARACTER | O_SUPER:
				from_lispm(x);
				break;
			}
			if (x->x_room == 0)
				if (xbwrite(x) >= 0) {
					x->x_bptr = x->x_bbuf;
					x->x_room = FSBSIZE;
				} else {
writerr:
					if (errno == ENOSPC &&
					    (x->x_flags & X_CLOSE) == 0) {
						(void)fherror(x->x_fh, NMR,
							E_RECOVERABLE,
						"File system out of space");
						x->x_state = X_ERROR;
					} else {
						(void)fherror(x->x_fh, MSC, E_FATAL,
							strerror(errno));
						x->x_state = X_WSYNC;
					}
				}
		}
		return X_CONTINUE;
	}
	/* NOTREACHED */
}

/*
 * Character set conversion routines.
 */
to_lispm(x)
register struct xfer *x;
{
	register int c;

	while (x->x_left && x->x_room) {
		c = *x->x_bptr++ & 0377;
		x->x_left--;
		switch (c) {
		case 0210:	/* Map SAIL symbols back */
		case 0211:
		case 0212:
		case 0213:
		case 0214:
		case 0215:
		case 0377:	/* Give back infinity */
			c &= 0177;
			break;
		case '\n':	/* Map canonical newline to lispm */
			c = CHNL;
			break;
		case 015:	/* Reverse linefeed map kludge */
			c = 0212;
		case 010:	/* Map the format effectors back */
		case 011:
		case 013:
		case 014:
		case 0177:
			c |= 0200;
		}
		*x->x_pptr++ = c;
		x->x_room--;
	}
}

/*
 * This is the translation between the LISPM character set and
 * UNIX.
 * Several notes:
 * 010 through 015 and 0177 are mapped to above 0200 since UNIX needs this
 * range for the lispm format effectors.
 * The lispm format effectors 0210 through 0215 are mapped to the UNIX
 * ones, with the exception that 0212 maps to 015 since 0215 must map to 012.
 * 0177 is mapped to 0377 since its also a symbol.
 */
from_lispm(x)
register struct xfer *x;
{
	register int c;

	while (x->x_left && x->x_room) {
		c = *x->x_pptr++ & 0377;
		switch (c) {
		case 010:	/* Map these SAIL symbols out of the way. */
		case 011:
		case 012:
		case 013:
		case 014:
		case 015:
		case 0177:
			c |= 0200;
			break;
		case 0212:	/* Map LINE to CR */
			c = 015;
			break;
		case 0215:	/* Map lispm canonical newline to UNIX's */
			c = '\n';
			break;
		case 0210:	/* Map format effectors to their right place */
		case 0211:
		case 0213:
		case 0214:
		case 0377:
			c &= 0177;
			break;
		}
		x->x_left--;
		*x->x_bptr++ = c;
		x->x_room--;
	}
}		

/*
 * Write out the local disk buffer, doing the appropriate error
 * processing here, returning non zero if we got an error.
 */
xbwrite(x)
register struct xfer *x;
{
	register int ret;
	register int n;

	if ((n = x->x_bptr - x->x_bbuf) == 0)
		return 0;
	if ((ret = write(x->x_fd, x->x_bbuf, n)) <= 0) {
		syslog(LOG_ERR, "FILE: write error %d (%d) to file\n",
			ret, errno);
		return -1;
	}
#ifdef LOG_VERBOSE
	syslog(LOG_INFO,"FILE: wrote %d bytes to file\n", ret);
#endif
	return 0;
}

/*
 * Write an eof packet on a transfer.
 */
xpweof(x)
register struct xfer *x;
{
	char op = EOFOP;

	if (write(x->x_fh->f_fd, &op, 1) != 1)
		return -1;
#ifdef LOG_VERBOSE
	syslog(LOG_INFO, "FILE: wrote EOF to net\n");
#endif
	return 0;
}

/*
 * Write a transfer's packet.
 */
xpwrite(x)
register struct xfer *x;
{
	register int len;

	len = x->x_pptr - x->x_pbuf;
	if (len == 0)
		return 0;
	x->x_op = x->x_options & O_BINARY ? DWDOP : DATOP;
	len++;
#ifdef LOG_VERBOSE
	syslog(LOG_INFO, "FILE: writing (%d) %d bytes to net\n",
		x->x_op & 0377, len);
#endif
	if (write(x->x_fh->f_fd, (char *)&x->x_pkt, len) != len)
		return -1;
	return 0;
}

/*
 * Read a packet from the net, returning 0 for EOF packets, < 0 for errors,
 * and > 0 for data.
 */
xpread(x)
register struct xfer *x;
{
	register int n;

loop:	
	n = read(x->x_fh->f_fd, (char *)&x->x_pkt, sizeof(x->x_pkt));
#ifdef LOG_VERBOSE
	syslog(LOG_INFO, "FILE: read (%d) %d bytes from net\n",
		x->x_op & 0377, n);
#endif
	if (n < 0)
		return -1;
	if (n == 0)
		fatal("Net Read returns 0");
	switch (x->x_op) {
	case EOFOP:
		return 0;
	case SYNOP:
		return -2;
	case DATOP:
	case DWDOP:
		if (n == 1) {
			syslog(LOG_ERR, "FILE: zero size data packet\n");
			goto loop;	/* Zero size data packet!? */
		}
		return n - 1;
	default:
		syslog(LOG_ERR, "FILE: bad opcode in data connection: %d\n",
			x->x_op & 0377);
		fatal("Bad opcode on data connection");
		/* NOTREACHED */
	}
}	

/*
 * End this program right here.
 * Nothing really to do since system closes everything.
 */
void finish(int arg)
{
	register int ufd;

	if (getpid() == mypid && (ufd = open(FILEUTMP, 1)) >= 0) {
		mylogin.cl_user[0] = '\0';
		(void)lseek(ufd, (long)(mylogin.cl_cnum * sizeof(mylogin)), 0);
		(void)write(ufd, (char *)&mylogin, sizeof(mylogin));
		(void)close(ufd);
	}	
	/* Should send close packet here */
	exit(0);
}
/*
 * Start the transfer task running.
 * Returns errcode if an error occurred, else 0
 */
startxfer(ax)
struct xfer *ax;
{
	register struct xfer *x = ax;
	
#ifdef SELECT
put on runq or something, set up which fd and why to wait....
#else
	register int i;
	int pfd[2];

	if (pipe(pfd) < 0) {
		errstring = "Can't create pipe for transfer process";
		return NER;
	}
	x->x_pfd = pfd[1];
	switch (x->x_pid = fork()) {
	case -1:
		(void)close(pfd[0]);
		(void)close(pfd[1]);
		errstring = "Can't create transfer process";
		return NER;
	default:
		if (!(x->x_options & (O_PROPERTIES|O_DIRECTORY)))
			(void)close(x->x_fd);
		x->x_fd = -1;
		(void)close(pfd[0]);
		return 0;
	/*
	 * Child process.
	 */
	case 0:
		for (i = 3; i < 20; i++)
			if (i != pfd[0] && i != x->x_fh->f_fd &&
			    i != ctlpipe[1] && i != x->x_fd)
				(void)close(i);
		x->x_pfd = pfd[0];
		myxfer = x;
		setjmp(closejmp);
		for (;;) {
			while ((x->x_flags & X_CLOSE) == 0) {
				off_t nread;

				(void)signal(SIGHUP, interrupt);
				if (ioctl(x->x_pfd, FIONREAD, (char *)&nread) < 0)
					fatal("Failing FIONREAD");
				if (nread == 0)
					break;
				(void)signal(SIGHUP, SIG_IGN);
				rcvcmd(x);
			}
#ifdef LOG_VERBOSE
			syslog(LOG_INFO, "Switch pos: %ld, status: %ld\n",
			       tell(x->x_fd), x->x_state);
#endif
			switch (dowork(x)) {
			case X_SYNCMARK:
				syncmark(x->x_fh);	/* Ignore errors */
				break;
			case X_FLUSH:		/* Totally done */
				break;
			case X_CONTINUE:	/* In process */
				continue;
			case X_HANG:		/* Need more instructions */
				(void)signal(SIGHUP, SIG_IGN);
				rcvcmd(x);
				syslog(LOG_INFO, "Hang pos: %ld\n", tell(x->x_fd));
				continue;
			}
			syslog(LOG_INFO, "FILE: subproc exiting\n");
			exit(0);
		}
	}
	/* NOTREACHED */
#endif
}

#ifndef SELECT

mdatfh(x, t)
register struct xfer *x;
register struct transaction *t;
{
	x->x_mtime = t->t_args->a_numbers[0];
	x->x_flags |= X_MTIME;
	tfree(t);
}
adatfh(x, t)
register struct xfer *x;
register struct transaction *t;
{
	x->x_atime = t->t_args->a_numbers[0];
	x->x_flags |= X_ATIME;
	tfree(t);
}

delfh(x, t)
register struct xfer *x;
register struct transaction *t;
{
	x->x_flags |= X_DELETE;
	tfree(t);
}

renfh(x, t)
register struct xfer *x;
register struct transaction *t;
{
	free(x->x_realname);
	x->x_realname = t->t_args->a_strings[0];
	t->t_args->a_strings[0] = NOSTR;
	tfree(t);
}

/*
 * Check the control pipe to see if any transfer processes exited, and
 * pick them up.
 */
xcheck()
{
	register int nprocs;
	struct xfer *x;
	off_t nread = 0;

	if (ctlpipe[0] <= 0)
		if (pipe(ctlpipe) < 0)
			fatal("Can't create control pipe");
	nprocs = 0;
	if (ioctl(ctlpipe[0], FIONREAD, (char *)&nread) < 0)
		fatal("Failing FIONREAD on control pipe");
	while (nread) {
		if (read(ctlpipe[0], (char *)&x, sizeof(x)) != sizeof(x))
			fatal("Read error on control pipe");
		nprocs++;
		(void)close(x->x_pfd);
		xflush(x);
		nread -= sizeof(x);
	}
	nprocs -= nprocdone;
	nprocdone = 0;
	while (nprocs--)
		if (wait((int *)0) < 0)
			fatal("Lost a subprocess");
}

struct transaction *
makecmd(c, f)
struct command *c;
struct file_handle *f;
{
	register struct transaction *t = salloc(transaction);

	if (t == TNULL)
		fatal(NOMEM);
	tinit(t);
	t->t_fh = f;
	t->t_tid = savestr("XXXXX");
	t->t_command = c;
	return t;
}

/*
 * Rather than queuing up a transaction on a transfer that is a subtask
 * of this process, send it to the subprocess.
 */
sendcmd(t, x)
register struct transaction *t;
register struct xfer *x;
{
	register struct cmdargs *a;
	struct pmesg pm;

	pm.pm_cmd = t->t_command;
	strncpy(pm.pm_tid, t->t_tid, TIDLEN);
	pm.pm_tid[TIDLEN] = '\0';
	if (a = t->t_args) {
		pm.pm_args = 1;
		pm.pm_n = a->a_numbers[0];
		pm.pm_strlen = a->a_strings[0] ? strlen(a->a_strings[0]) : 0;
	} else
		pm.pm_args = 0;
	if (write(x->x_pfd, (char *)&pm, sizeof(pm)) != sizeof(pm) ||
	    a && a->a_strings[0] &&
	    write(x->x_pfd, a->a_strings[0], (unsigned)pm.pm_strlen) != pm.pm_strlen) {
		errstring = "No transfer process to receive command";
		error(t, t->t_fh->f_name, BUG);
	} else {
		/*
		 * Reading transfers need to be interrupted since they might be
		 * hung on output to the net.
		 */
		if ((t->t_command->c_func == fileclose ||
		     t->t_command->c_func == filepos) && x->x_options & O_READ)
			(void)kill(x->x_pid, SIGHUP);
		syslog(LOG_INFO, "FILE: send %s command\n", t->t_command->c_name);
		tfree(t);
	}
}

/*
 * Read a message from the pipe and queue it up.
 */
rcvcmd(x)
register struct xfer *x;
{
	register struct transaction *t;
	register int n;
	struct pmesg pm;

	if ((n = read(x->x_pfd, (char *)&pm, sizeof(pm))) !=
	    sizeof(pm))
		fatal("Pipe read botch1: %d %d", n, errno);
	if ((t = salloc(transaction)) == TNULL)
		fatal(NOMEM);
	t->t_tid = savestr(pm.pm_tid);
	t->t_command = pm.pm_cmd;
	t->t_fh = x->x_fh;
	if (pm.pm_args) {
		if ((t->t_args = salloc(cmdargs)) == ANULL)
			fatal(NOMEM);
		ainit(t->t_args);
		t->t_args->a_numbers[0] = pm.pm_n;
		if (pm.pm_strlen) {
			if ((t->t_args->a_strings[0] = malloc(pm.pm_strlen + 1)) == NOSTR)
				fatal(NOMEM);
			if ((n = read(x->x_pfd, t->t_args->a_strings[0], pm.pm_strlen)) !=
			    pm.pm_strlen)
				fatal("Pipe read botch2");
			t->t_args->a_strings[0][pm.pm_strlen] = '\0';
		}
	} else
		t->t_args = ANULL;
	syslog(LOG_INFO, "FILE: rcvd %s command\n", t->t_command->c_name);
	(*t->t_command->c_func)(x, t);
}

/*
 * Interrupt the transfer process which is potentially blocked on network
 * output
 */
void interrupt(int arg)
{
	off_t nread;

#if !defined(BSD42) && !defined(linux)
	(void)signal(SIGHUP, interrupt);
#endif
	syslog(LOG_INFO, "Interrupt!\n");
	if (ioctl(myxfer->x_pfd, FIONREAD, (char *)&nread) < 0)
		fatal("Failing FIONREAD");
	if (nread != 0)
		longjmp(closejmp, 0);
}
#endif

