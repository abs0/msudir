/*
 * $Id: msudir.c,v 1.16 2003/06/06 17:04:04 abs Exp $
 *
 * msudir: (c) 2002, 2003, 2009 DKBrownlee (abs@absd.org).
 * May be freely distributed.
 * Provides easier access to setuid scripts.
 * No warranty, implied or otherwise. Stick no bills. Suggestions welcome.
 *
 */

#ifdef __NetBSD__
#define SYSCTL_HAS_KERN_NGROUPS
#endif

#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <err.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#ifdef SYSCTL_HAS_KERN_NGROUPS
#include <sys/sysctl.h>
#endif

extern char **environ;

#define VERSION		"0.15"

#ifndef CONFIG_FILE
#define CONFIG_FILE      "/usr/local/etc/msudir.conf"
#endif

#define CONF_FLAG_BOOLEAN	1
#define CONF_FLAG_CAN_NULL	2

typedef struct
    {
    const char *key;
    const char *value;
    unsigned	flags;
    } CONFIG;

CONFIG config_list[] = {
    {"basedir", "/usr/local/msudir", 0},
    #define config_basedir config_list[0].value

    {"dirmatchuser", (char *)1, CONF_FLAG_BOOLEAN},
    #define config_dirmatchuser config_list[1].value

    {"path", "/usr/local/bin:/usr/local/sbin:/usr/pkg/bin:/usr/pkg/sbin:/usr/bin:/usr/sbin:/bin:/sbin:/usr/games:/usr/X11R6/bin", 0},
    #define config_path config_list[2].value

    {"rootdir", 0, CONF_FLAG_CAN_NULL},
    #define config_rootdir config_list[3].value

    {"scriptsonly", 0, CONF_FLAG_BOOLEAN},
    #define config_scriptsonly config_list[4].value

    {"fromgroup", 0, CONF_FLAG_CAN_NULL},
    #define config_fromgroup config_list[5].value

    {0, 0} };

static void check_path(const char *path, uid_t uid, mode_t type);
static void cleanstr(char *str);
static void *emalloc(size_t size);
static const char *estrdup(const char *str);
static void msudir(const char *dir, char **argv);
static int ngroups_max(void);
static int read_config(const char *file, const char *dir);
static void stripenv(char **envp);
static char *trim(char *str);
void become(const char *to, const char *shell, const char *from);

int main(int argc, char **argv)
    {
    char *enddir;

    if (argc < 2 || !(enddir = strchr(argv[1], '/')))
	errx(1, "Usage: msudir dir/cmd args");

    *enddir = 0;
    argv[0] = argv[1];
    argv[1] = enddir + 1;
    if (strchr(argv[1], '/'))
	errx(1, "Usage: msudir dir/cmd args (cmd cannot contain a '/')");

    if (read_config(CONFIG_FILE, argv[0]))
	errx(1, "Config file parsing failed.");

    msudir(argv[0], argv + 1);
    return(1);
    }

static void check_path(const char *path, uid_t uid, mode_t type)
    {
    struct	stat	dirstat;

    if (lstat(path, &dirstat))
	err(1, "Unable to stat '%s'", path);

    if ((dirstat.st_mode & S_IFMT) != type)
	errx(1, "'%s' of wrong type (file/directory)", path);
    if (dirstat.st_uid != uid && dirstat.st_uid != 0)
	errx(1, "Incorrect uid for '%s' - %d vs %d", path, (int)dirstat.st_uid,
							   (int)uid);
    if (dirstat.st_mode & 022)
	errx(1, "'%s' cannot be group or other writable", path);
    }

static void cleanstr(char *str)
    {
    while (*str)
	{
	if (!isalnum(*str) && strchr("\"#%'+,-./:=@\\_", *str) == 0)
	    *str = ' ';
	++str;
	}
    }

static void *emalloc(size_t size)
    {
    void *ptr;
    if (!(ptr = malloc(size)))
	errx(1, "Malloc failed");
    return ptr;
    }

static const char *estrdup(const char *str)
    {
    if (!(str = strdup(str)))
	errx(1, "Malloc failed");
    return str;
    }

static void msudir(const char *dir, char **argv)
    {
    char	*ptr;
    char	*destpath;
    char	**arg;
    struct      passwd  *destpwd;

    destpath = emalloc(strlen(config_basedir) + strlen(dir) + strlen(argv[0])
									+ 3);
    sprintf(destpath, "%s/%s", config_basedir, dir);

    if (config_dirmatchuser)
	destpwd = getpwnam(dir);
    else
	{
	struct stat dirstat;
	if (lstat(destpath, &dirstat))
	    errx(1, "Unable to stat '%s'", destpath);
	destpwd = getpwuid(dirstat.st_uid);
	}

    if (destpwd == 0 || destpwd->pw_name == 0)
	errx(1, "Unable to lookup destination account");

    if (config_fromgroup)
	{
	struct group	*grp = getgrnam(config_fromgroup);
	gid_t		*gidset;
	int		numgroups = ngroups_max();
	int		loop;

	if (!grp)
	    errx(1, "Unable to lookup fromgroup '%s'", config_fromgroup);

	if (grp->gr_gid != getgid() && grp->gr_gid != getegid())
	    {
	    gidset = emalloc(numgroups * sizeof(gid_t));
	    if ((numgroups = getgroups(numgroups, gidset)) == -1)
		err(1, "getgroups failed");
	    for (loop = 0 ; loop < numgroups ; ++loop)
		if (gidset[loop] == grp->gr_gid)
		    break;
	    if (loop == numgroups)
		errx(1, "Source user not in fromgroup '%s'", config_fromgroup);
	    }
	}

    if (setgroups(0, 0))
	err(1, "Unable to setgroups(0, 0)");
    if (setgid(destpwd->pw_gid))
	err(1, "Unable to setgid(%d)", destpwd->pw_gid);
    if (setuid(destpwd->pw_uid))
	err(1, "Unable to setuid(%d)", destpwd->pw_uid);

    check_path(config_basedir, 0, S_IFDIR);

    check_path(destpath, destpwd->pw_uid, S_IFDIR);

    sprintf(destpath, "%s/%s/%s", config_basedir, dir, argv[0]);
    check_path(destpath, destpwd->pw_uid, S_IFREG);

    if (destpwd->pw_uid == 0 &&
			    (!config_rootdir || strcmp(dir, config_rootdir)))
	errx(1, "'%s' would setuid root and not set as 'rootdir'", dir);
    if ((ptr = getenv("USER")))
	setenv("OLD_USER", ptr, 1);
    setenv("USER", destpwd->pw_name, 1);
    setenv("PATH", config_path, 1);
    unsetenv("IFS");
    if (config_scriptsonly)
	{
	FILE *fds;
	char buf[3];
	if (!(fds = fopen(destpath, "r")) || ! fgets(buf, sizeof(buf), fds))
	    err(1, "Unable to read '%s'", destpath);
	if (strncmp(buf, "#!", 2))
	    errx(1, "'%s' must begin with '#!' when scriptsonly enabled",
								destpath);
	fclose(fds);
	}
    stripenv(environ);
    for (arg = argv ; *arg ; ++arg)
	cleanstr(*arg);
    for (arg = environ ; *arg ; ++arg)
	cleanstr(*arg);

    /* If we cannot get the current working directory, cd to / */
    if ((ptr = getcwd(0, MAXPATHLEN)))
	free(ptr);
    else
	chdir("/");

    execv(destpath, argv);
    err(1, "exec '%s' failed", destpath);
    }

static int ngroups_max()
    {
#ifdef SYSCTL_HAS_KERN_NGROUPS
    int mib[2], ngroups;
    size_t len;

    mib[0] = CTL_KERN;
    mib[1] = KERN_NGROUPS;
    len = sizeof(ngroups);
    if (sysctl(mib, 2, &ngroups, &len, NULL, 0) == -1)
	err(1, "sysctl kern.ngroups failed");
    return ngroups;
#else
    return 32;
#endif
    }

static int read_config(const char *file, const char *dir)
    {
    FILE	*fds;
    int		line = 0;
    int		error = 0;
    char	readline[1024],
		*ptr,
		*key,
		*value;
    CONFIG	*conf;
    int		ignore = 0;

    if ((fds = fopen(file, "r")) == 0)
	{
	fprintf(stderr, "%s: Unable to open\n", file);
	return -1;
	}
    while (fgets(readline, (int)sizeof(readline), fds))
	{
	++line;
	if ((ptr = strchr(readline, '#')))
	    *ptr = 0;
	if (readline[0] == '[')
	    {
	    if (!(ptr = strchr(readline, ']')))
		{
		fprintf(stderr, "%s:%d - Missing ]\n", file, line);
		error = -1;
		continue;
		}
	    *ptr = 0;
	    ignore = strcmp(readline + 1, dir);
	    }
	else if ((key = strtok(readline, "=")) && (value = strtok(0, "=")))
	    {
	    key = trim(readline);
	    value = trim(value);
	    if (!key)
		{
		fprintf(stderr, "%s:%d - Missing key\n", file, line);
		error = -1;
		continue;
		}
	    for (conf = config_list ; conf->key ; ++conf)
		{
		if (strcmp(conf->key, key) == 0)
		    {
		    if (!value && !(conf->flags&CONF_FLAG_CAN_NULL))
			{
			fprintf(stderr, "%s:%d - Missing value for '%s'\n",
							    file, line, key);
			error = -1;
			break;
			}
		    if (ignore)
			break;
		    else if (conf->flags&CONF_FLAG_BOOLEAN)
			{
			if (strcasecmp(value, "false") == 0 ||
					strcasecmp(value, "off") == 0 ||
					strcasecmp(value, "no") == 0 ||
					strcasecmp(value, "0") == 0)
			    { conf->value = 0; }
			else if (strcasecmp(value, "true") == 0 ||
					strcasecmp(value, "on") == 0 ||
					strcasecmp(value, "yes") == 0 ||
					strcasecmp(value, "1") == 0)
			    { conf->value = (char *)1; }
			else
			    {
			    fprintf(stderr, "%s:%d - '%s' must be boolean\n",
							    file, line, key);
			    error = -1;
			    }
			}
		    else if (value)
			conf->value = estrdup(value);
		    else
			conf->value = 0;
		    break;
		    }
		}
	    if (!conf->key)
		{
		fprintf(stderr, "%s:%d - Unknown key '%s'\n", file, line, key);
		error = -1;
		}
	    }
	}
    fclose(fds);
    return error;
    }

/* stripenv taken from edelkind-bugtraq@episec.com */
static void stripenv(char **envp)
    {
    char **p1, **p2;

    /* the following entries are based on Lawrence R. Rogers'
     * wrapper in cert advisory CA-1995-14 */
    for (p1 = p2 = envp; *p1; p1++)
	{
	if (memcmp(*p1, "LD_", 3) == 0 || memcmp(*p1, "LIBPATH=", 8) == 0 ||
		memcmp(*p1, "ELF_LD_", 7) == 0 || memcmp(*p1, "_RLD", 4) == 0 ||
		memcmp(*p1, "AOUT_LD_", 8) == 0 || memcmp(*p1, "IFS=", 4) == 0)
	    continue;
	*p2++ = *p1;
	}
    *p2 = 0;
    }

static char *trim(char *str)
    {
    char *end;
    while (isspace(*str))
	++str;
    if (!*str)
	return 0;
    for ( end = strchr(str, 0) - 1 ; isspace(*end) ; end--)
	*end = 0;
    return(str);
    }
