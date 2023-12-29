/* bulk_remove.c -- bulk remove files */

/*
 * This file is part of CliFM
 *
 * Copyright (C) 2016-2023, L. Abramovich <leo.clifm@outlook.com>
 * All rights reserved.

 * CliFM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * CliFM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
*/

#include "helpers.h"

#include <dirent.h> /* scandir() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* strnlen() */
#include <sys/stat.h> /* (l)stat() */
#include <fcntl.h> /* unlinkat() */
#include <unistd.h> /* unlinkat() */
#include <errno.h>

#include "aux.h" /* xnmalloc, open_fwrite(), get_cmd_path(), count_dir() */
#include "checks.h" /* is_internal() */
#include "exec.h" /* launch_execv() */
#include "file_operations.h" // open_file() */
#include "messages.h" /* RR_USAGE */
#include "misc.h" /* xerror() */

#define BULK_RM_TMP_FILE_HEADER "# CliFM - Remove files in bulk\n\
# Remove the files you want to be deleted, save and exit\n\
# Just quit the editor without any edit to cancel the operation\n\n"

static int
parse_bulk_remove_params(char *s1, char *s2, char **app, char **target)
{
	if (!s1 || !*s1) { /* No parameters */
		/* TARGET defaults to CWD and APP to default associated app */
		*target = workspaces[cur_ws].path;
		return EXIT_SUCCESS;
	}

	int stat_ret = 0;
	struct stat a;
	if ((stat_ret = stat(s1, &a)) == -1 || !S_ISDIR(a.st_mode)) {
		char *p = get_cmd_path(s1);
		if (!p) { /* S1 is neither a directory nor a valid application */
			int ec = stat_ret != -1 ? ENOTDIR : ENOENT;
			xerror("rr: '%s': %s\n", s1, strerror(ec));
			return ec;
		}
		/* S1 is an application name. TARGET defaults to CWD */
		*target = workspaces[cur_ws].path;
		*app = s1;
		free(p);
		return EXIT_SUCCESS;
	}

	/* S1 is a valid directory */
	size_t tlen = strlen(s1);
	if (tlen > 2 && s1[tlen - 1] == '/')
		s1[tlen - 1] = '\0';
	*target = s1;

	if (!s2 || !*s2) /* No S2. APP defaults to default associated app */
		return EXIT_SUCCESS;

	char *p = get_cmd_path(s2);
	if (p) { /* S2 is a valid application name */
		*app = s2;
		free(p);
		return EXIT_SUCCESS;
	}
	/* S2 is not a valid application name */
	xerror("rr: '%s': %s\n", s2, strerror(ENOENT));
	return ENOENT;
}

static int
create_tmp_file(char **file, int *fd)
{
	size_t tmp_len = strlen(xargs.stealth_mode == 1 ? P_tmpdir : tmp_dir);
	size_t file_len = tmp_len + (sizeof(TMP_FILENAME) - 1) + 2;

	*file = xnmalloc(file_len, sizeof(char));
	snprintf(*file, file_len, "%s/%s", xargs.stealth_mode == 1
		? P_tmpdir : tmp_dir, TMP_FILENAME);

	errno = 0;
	*fd = mkstemp(*file);
	if (*fd == -1) {
		xerror("rr: mkstemp: '%s': %s\n", *file, strerror(errno));
		free(*file);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static char
get_file_suffix(const mode_t type)
{
	switch (type) {
	case DT_DIR: return DIR_CHR;
	case DT_REG: return 0;
	case DT_LNK: return LINK_CHR;
	case DT_SOCK: return SOCK_CHR;
	case DT_FIFO: return FIFO_CHR;
#ifdef SOLARIS_DOORS
	case DT_DOOR: return DOOR_CHR;
//	case DT_PORT: return 0;
#endif /* SOLARIS_DOORS */
#ifdef S_IFWHT
	case DT_WHT: return WHT_CHR;
#endif /* S_IFWHT */
	case DT_UNKNOWN: return UNKNOWN_CHR;
	default: return 0;
	}
}

static void
print_file(FILE *fp, const char *name, const mode_t type)
{
#ifndef _DIRENT_HAVE_D_TYPE
	UNUSED(type);
	char s = 0;
	struct stat a;
	if (lstat(name, &a) != -1)
		s = get_file_suffix(a.st_mode);
#else
	char s = get_file_suffix(type);
#endif /* !_DIRENT_HAVE_D_TYPE */

	if (s)
		fprintf(fp, "%s%c\n", name, s);
	else
		fprintf(fp, "%s\n", name);
}

static int
write_files_to_tmp(struct dirent ***a, filesn_t *n, const char *target,
	char *tmp_file)
{
	int fd = 0;
	FILE *fp = open_fwrite(tmp_file, &fd);
	if (!fp) {
		err('e', PRINT_PROMPT, "%s: rr: fopen: '%s': %s\n", PROGRAM_NAME,
			tmp_file, strerror(errno));
		return errno;
	}

	fprintf(fp, "%s", _(BULK_RM_TMP_FILE_HEADER));

	if (target == workspaces[cur_ws].path) {
		filesn_t i;
		for (i = 0; i < files; i++)
			print_file(fp, file_info[i].name, file_info[i].type);
	} else {
		if (count_dir(target, CPOP) <= 2) {
			int tmp_err = EXIT_FAILURE;
			xerror(_("%s: '%s': Directory empty\n"), PROGRAM_NAME, target);
			fclose(fp);
			return tmp_err;
		}

		*n = scandir(target, a, NULL, alphasort);
		if (*n == -1) {
			int tmp_err = errno;
			xerror("rr: '%s': %s", target, strerror(errno));
			fclose(fp);
			return tmp_err;
		}

		filesn_t i;
		for (i = 0; i < *n; i++) {
			if (SELFORPARENT((*a)[i]->d_name))
				continue;
#ifndef _DIRENT_HAVE_D_TYPE
			struct stat attr;
			if (stat((*a)[i]->d_name, &attr) == -1)
				continue;
			print_file(fp, (*a)[i]->d_name, get_dt(attr.st_mode));
#else
			print_file(fp, (*a)[i]->d_name, (*a)[i]->d_type);
#endif /* !_DIRENT_HAVE_D_TYPE */
		}
	}

	fclose(fp);
	return EXIT_SUCCESS;
}

static int
open_tmp_file(struct dirent ***a, const filesn_t n, char *tmp_file, char *app)
{
	int exit_status = EXIT_SUCCESS;
	filesn_t i;

	if (!app || !*app) {
		open_in_foreground = 1;
		exit_status = open_file(tmp_file);
		open_in_foreground = 0;

		if (exit_status == EXIT_SUCCESS)
			return EXIT_SUCCESS;

		xerror(_("rr: '%s': Cannot open file\n"), tmp_file);
		goto END;
	}

	char *cmd[] = {app, tmp_file, NULL};
	exit_status = launch_execv(cmd, FOREGROUND, E_NOFLAG);

	if (exit_status == EXIT_SUCCESS)
		return EXIT_SUCCESS;

END:
	for (i = 0; i < n && *a && (*a)[i]; i++)
		free((*a)[i]);
	free(*a);

	return exit_status;
}

static char **
get_files_from_tmp_file(const char *tmp_file, const char *target, const filesn_t n)
{
	size_t nfiles = (target == workspaces[cur_ws].path) ? (size_t)files : (size_t)n;
	char **tmp_files = xnmalloc(nfiles + 2, sizeof(char *));

	FILE *fp = fopen(tmp_file, "r");
	if (!fp)
		return (char **)NULL;

	size_t size = 0, i;
	char *line = (char *)NULL;
	ssize_t len = 0;

	i = 0;
	while ((len = getline(&line, &size, fp)) > 0) {
		if (*line == '#' || *line == '\n')
			continue;

		if (line[len - 1] == '\n') {
			line[len - 1] = '\0';
			len--;
		}

		if (len > 0 && (line[len - 1] == '/' || line[len - 1] == '@'
		|| line[len - 1] == '=' || line[len - 1] == '|'
		|| line[len - 1] == '?') ) {
			line[len - 1] = '\0';
			len--;
		}

		tmp_files[i] = savestring(line, (size_t)len);
		i++;
	}

	tmp_files[i] = (char *)NULL;

	free(line);
	fclose(fp);

	return tmp_files;
}

/* If FILE is not found in LIST, returns one; zero otherwise. */
static int
remove_this_file(char *file, char **list)
{
	if (SELFORPARENT(file))
		return 0;

	size_t i;
	for (i = 0; list[i]; i++) {
		if (*file == *list[i] && strcmp(file, list[i]) == 0)
			return 0;
	}

	return 1;
}

static char **
get_remove_files(const char *target, char **tmp_files,
	struct dirent ***a, const filesn_t n)
{
	size_t i, j = 1;
	size_t l = (target == workspaces[cur_ws].path) ? (size_t)files : (size_t)n;
	char **rem_files = xnmalloc(l + 3, sizeof(char *));
	rem_files[0] = savestring("rr", 2);

	if (target == workspaces[cur_ws].path) {
		for (i = 0; i < (size_t)files; i++) {
			if (remove_this_file(file_info[i].name, tmp_files) == 1) {
				rem_files[j] = savestring(file_info[i].name,
					strlen(file_info[i].name));
				j++;
			}
		}
		rem_files[j] = (char *)NULL;
		return rem_files;
	}

	for (i = 0; i < (size_t)n; i++) {
		if (remove_this_file((*a)[i]->d_name, tmp_files) == 1) {
			char p[PATH_MAX];
			if (*target == '/') {
				snprintf(p, sizeof(p), "%s/%s", target, (*a)[i]->d_name);
			} else {
				snprintf(p, sizeof(p), "%s/%s/%s", workspaces[cur_ws].path,
					target, (*a)[i]->d_name);
			}
			rem_files[j] = savestring(p, strnlen(p, sizeof(p)));
			j++;
		}
		free((*a)[i]);
	}

	free(*a);
	rem_files[j] = (char *)NULL;

	return rem_files;
}

static int
diff_files(char *tmp_file, const filesn_t n)
{
	FILE *fp = fopen(tmp_file, "r");
	if (!fp) {
		xerror("br: '%s': %s\n", tmp_file, strerror(errno));
		return 0;
	}

	char line[PATH_MAX + 6];
	memset(line, '\0', sizeof(line));

	filesn_t c = 0;
	while (fgets(line, (int)sizeof(line), fp)) {
		if (*line != '#' && *line != '\n')
			c++;
	}

	fclose(fp);

	return (c < n ? 1 : 0);
}

static int
nothing_to_do(char **tmp_file, struct dirent ***a, const filesn_t n, const int fd)
{
	puts(_("rr: Nothing to do"));
	if (unlinkat(fd, *tmp_file, 0) == 1)
		xerror("rr: unlink: '%s': %s\n", *tmp_file, strerror(errno));
	close(fd);
	free(*tmp_file);

	filesn_t i = n;
	while (--i >= 0)
		free((*a)[i]);
	free(*a);

	return EXIT_SUCCESS;
}

int
bulk_remove(char *s1, char *s2)
{
	if (s1 && IS_HELP(s1)) {
		puts(_(RR_USAGE));
		return EXIT_SUCCESS;
	}

	char *app = (char *)NULL, *target = (char *)NULL;
	int fd = 0, ret = 0, i = 0;
	filesn_t n = 0;

	if ((ret = parse_bulk_remove_params(s1, s2, &app, &target)) != EXIT_SUCCESS)
		return ret;

	char *tmp_file = (char *)NULL;
	if ((ret = create_tmp_file(&tmp_file, &fd)) != EXIT_SUCCESS)
		return ret;

	struct dirent **a = (struct dirent **)NULL;
	if ((ret = write_files_to_tmp(&a, &n, target, tmp_file)) != EXIT_SUCCESS)
		goto END;

	struct stat attr;
	stat(tmp_file, &attr);
	time_t old_t = attr.st_mtime;

	if ((ret = open_tmp_file(&a, n, tmp_file, app)) != EXIT_SUCCESS)
		goto END;

	stat(tmp_file, &attr);
	filesn_t num = (target == workspaces[cur_ws].path) ? files : n - 2;
	if (old_t == attr.st_mtime || diff_files(tmp_file, num) == 0)
		return nothing_to_do(&tmp_file, &a, n, fd);

	char **rfiles = get_files_from_tmp_file(tmp_file, target, n);
	if (!rfiles)
		goto END;

	char **rem_files = get_remove_files(target, rfiles, &a, n);
	if (!rem_files)
		goto FREE_N_EXIT;

	ret = remove_files(rem_files);

	for (i = 0; rem_files[i]; i++)
		free(rem_files[i]);
	free(rem_files);

FREE_N_EXIT:
	for (i = 0; rfiles[i]; i++)
		free(rfiles[i]);
	free(rfiles);

END:
	if (unlinkat(fd, tmp_file, 0) == -1) {
		err('w', PRINT_PROMPT, "rr: unlink: '%s': %s\n",
			tmp_file, strerror(errno));
	}

	close(fd);
	free(tmp_file);
	return ret;
}
