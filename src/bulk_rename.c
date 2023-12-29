/* bulk_rename.c -- bulk rename files */

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

/* The run_action function is taken from NNN's run_selected_plugin function
 * (https://github.com/jarun/nnn/blob/master/src/nnn.c), licensed under BSD-2-Clause.
 * All changes are licensed under GPL-2.0-or-later. */

#include "helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "aux.h" /* press_any_key_to_continue(), abbreviate_file_name(), open_fread() */
#include "checks.h" /* is_file_in_cwd() */
#include "exec.h" /* launch_execv() */
#include "file_operations.h" /* open_file() */
#include "init.h" /* get_sel_files() */
#include "listing.h" /* reload_dirlist() */
#include "messages.h" /* BULK_USAGE */
#include "misc.h" /* xerror(), print_reload_msg() */
#include "readline.h" /* rl_get_y_or_n() */

#define BULK_RENAME_TMP_FILE_HEADER "# CliFM - Rename files in bulk\n\
# Edit file names, save, and quit the editor (you will be\n\
# asked for confirmation)\n\
# Just quit the editor without any edit to cancel the operation\n\n"

/* Error opening tmp file FILE. Err accordingly. */
static int
err_open_tmp_file(const char *file, const int fd)
{
	xerror("br: open: '%s': %s\n", file, strerror(errno));
	if (unlinkat(fd, file, 0) == -1)
		xerror("br: unlink: '%s': %s\n", file, strerror(errno));

	return errno;
}

/* Rename OLDPATH as NEWPATH. */
static int
rename_file(char *oldpath, char *newpath)
{
	/* Some renameat(2) implementations (DragonFly) do not like NEWPATH to
	 * end with a slash (in case of renaming directories). */
	size_t len = strlen(newpath);
	if (len > 1 && newpath[len - 1] == '/')
		newpath[len - 1] = '\0';

	int ret = renameat(XAT_FDCWD, oldpath, XAT_FDCWD, newpath);
	if (ret == 0)
		return 0;

	if (errno != EXDEV) {
		xerror(_("br: Cannot rename '%s' to '%s': %s\n"), oldpath,
			newpath, strerror(errno));
		return errno;
	}

	char *cmd[] = {"mv", "--", oldpath, newpath, NULL};
	return launch_execv(cmd, FOREGROUND, E_NOFLAG);
}

static int
write_renfiles_to_tmp(char ***args, const char *bulk_file, const int fd,
	time_t *mtime_bfr, size_t *total_input)
{
	size_t i;
	struct stat attr;

	FILE *fp = fdopen(fd, "w");
	if (!fp)
		return err_open_tmp_file(bulk_file, fd);

#ifdef HAVE_DPRINTF
	dprintf(fd, BULK_RENAME_TMP_FILE_HEADER);
#else
	fprintf(fp, BULK_RENAME_TMP_FILE_HEADER);
#endif /* HAVE_DPRINTF */

	/* Copy all files to be renamed into the bulk file */
	for (i = 1; (*args)[i]; i++) {
		/* Dequote file name, if necessary */
		if (strchr((*args)[i], '\\')) {
			char *deq_file = unescape_str((*args)[i], 0);
			if (!deq_file) {
				xerror(_("br: '%s': Error unescaping file name\n"), (*args)[i]);
				press_any_key_to_continue(0);
				continue;
			}

			xstrsncpy((*args)[i], deq_file, strlen(deq_file) + 1);
			free(deq_file);
		}

		/* Resolve "./" and "../" */
		if (*(*args)[i] == '.' && ((*args)[i][1] == '/' || ((*args)[i][1] == '.'
		&& (*args)[i][2] == '/') ) ) {
			char *p = realpath((*args)[i], NULL);
			if (!p) {
				xerror("br: '%s': %s\n", (*args)[i], strerror(errno));
				press_any_key_to_continue(0);
				continue;
			}
			free((*args)[i]);
			(*args)[i] = p;
		}

		if (lstat((*args)[i], &attr) == -1) {
			xerror("br: '%s': %s\n", (*args)[i], strerror(errno));
			press_any_key_to_continue(0);
			continue;
		}

		(*total_input)++;

#ifdef HAVE_DPRINTF
		dprintf(fd, "%s\n", (*args)[i]);
#else
		fprintf(fp, "%s\n", (*args)[i]);
#endif /* HAVE_DPRINTF */
	}

	if (*total_input == 0) { /* No valid file name */
		if (unlinkat(fd, bulk_file, 0) == -1)
			xerror("br: unlink: '%s': %s\n", bulk_file, strerror(errno));
		fclose(fp);
		return EXIT_FAILURE;
	}

	/* Store the last modification time of the bulk file. This time
	 * will be later compared to the modification time of the same
	 * file after shown to the user. */
	fstat(fd, &attr);
	*mtime_bfr = (time_t)attr.st_mtime;

	fclose(fp);
	return EXIT_SUCCESS;
}

static size_t
count_modified_names(char **args, FILE *fp)
{
	size_t modified = 0;
	size_t i = 1;

	/* Print what would be done */
	char line[PATH_MAX];
	while (fgets(line, (int)sizeof(line), fp)) {
		if (!*line || *line == '\n' || *line == '#')
			continue;

		size_t line_len = strlen(line);

		if (line[line_len - 1] == '\n')
			line[line_len - 1] = '\0';

		if (args[i] && strcmp(args[i], line) != 0) {
			char *a = abbreviate_file_name(args[i]);
			char *b = abbreviate_file_name(line);

			printf("%s %s->%s %s\n", a ? a : args[i], mi_c, df_c, b ? b : line);

			if (a && a != args[i])
				free(a);
			if (b && b != line)
				free(b);
			modified++;
		}

		i++;
	}

	/* If no file name was modified */
	if (modified == 0) {
		puts(_("br: Nothing to do"));
		return 0;
	}

	return modified;
}

static int
open_bulk_file(char *bulk_file)
{
	open_in_foreground = 1;
	int exit_status = open_file(bulk_file);
	open_in_foreground = 0;

	if (exit_status != EXIT_SUCCESS) {
		xerror("br: %s\n", errno != 0
			? strerror(errno) : _("Error opening temporary file"));
		if (unlink(bulk_file) == -1)
			xerror("br: unlink: '%s': %s\n", bulk_file, strerror(errno));
		return exit_status;
	}

	return EXIT_SUCCESS;
}

static int
check_line_mismatch(FILE *fp, const size_t total_input)
{
	size_t total_modified = 0;
	char tmp_line[PATH_MAX];

	while (fgets(tmp_line, (int)sizeof(tmp_line), fp)) {
		if (!*tmp_line || *tmp_line == '\n' || *tmp_line == '#')
			continue;

		total_modified++;
	}

	if (total_input != total_modified) {
		xerror("%s\n", _("br: Line mismatch in temporary file"));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int
rename_bulk_files(char **args, FILE *fp, int *is_cwd, size_t *renamed,
	const size_t modified)
{
	size_t i = 1;
	int exit_status = EXIT_SUCCESS;
	char line[PATH_MAX];

	while (fgets(line, (int)sizeof(line), fp)) {
		if (!*line || *line == '\n' || *line == '#')
			continue;

		if (!args[i])
			goto CONT;

		size_t len = strlen(line);
		if (line[len - 1] == '\n')
			line[len - 1] = '\0';

		if (strcmp(args[i], line) == 0)
			goto CONT;

		int ret = rename_file(args[i], line);
		if (ret != 0) {
			exit_status = ret;
			if (conf.autols == 1 && modified > 1)
				press_any_key_to_continue(0);
			goto CONT;
		}

		if (*is_cwd == 0 && (is_file_in_cwd(args[i])
		|| is_file_in_cwd(line)))
			*is_cwd = 1;
		(*renamed)++;

CONT:
		i++;
	}

	return exit_status;
}

/* Rename a bulk of files (ARGS) at once. Takes files to be renamed
 * as arguments, and returns zero on success and one on error. The
 * procedude is quite simple: file names to be renamed are copied into
 * a temporary file, which is opened via the mime function and shown
 * to the user to modify it. Once the file names have been modified and
 * saved, modifications are printed on the screen and the user is
 * asked whether to perform the actual bulk renaming or not.
 *
 * This bulk rename method is the same used by the fff filemanager,
 * ranger, and nnn */
int
bulk_rename(char **args)
{
	if (!args || !args[1] || IS_HELP(args[1])) {
		puts(_(BULK_USAGE));
		return EXIT_SUCCESS;
	}

	int exit_status = EXIT_SUCCESS;

	char bulk_file[PATH_MAX];
	snprintf(bulk_file, sizeof(bulk_file), "%s/%s",
		xargs.stealth_mode == 1 ? P_tmpdir : tmp_dir, TMP_FILENAME);

	int fd = mkstemp(bulk_file);
	if (fd == -1) {
		xerror("br: mkstemp: '%s': %s\n", bulk_file, strerror(errno));
		return EXIT_FAILURE;
	}

	/* Write files to be renamed into a tmp file */
	size_t total_input = 0;
	time_t mtime_bfr = 0;
	int ret = write_renfiles_to_tmp(&args, bulk_file, fd,
		&mtime_bfr, &total_input);
	if (ret != EXIT_SUCCESS)
		return ret;

	/* Open the bulk file with the associated text editor */
	if ((ret = open_bulk_file(bulk_file)) != EXIT_SUCCESS)
		return ret;

	FILE *fp;
	struct stat attr;

	if (!(fp = open_fread(bulk_file, &fd)))
		return err_open_tmp_file(bulk_file, fd);

	/* Compare the new modification time to the stored one: if they
	 * match, nothing was modified. */
	if (fstat(fd, &attr) == -1) {
		xerror("br: '%s': %s\n", bulk_file, strerror(errno));
		goto ERROR;
	}
	if (mtime_bfr == (time_t)attr.st_mtime) {
		puts(_("br: Nothing to do"));
		goto ERROR;
	}

	/* Make sure there are as many lines in the bulk file as files
	 * to be renamed. */
	if (check_line_mismatch(fp, total_input) != EXIT_SUCCESS) {
		exit_status = EXIT_FAILURE;
		goto ERROR;
	}

	/* Rewind to the beginning of the bulk file */
	fseek(fp, 0L, SEEK_SET);

	size_t modified = count_modified_names(args, fp);

	/* Ask the user for confirmation */
	if (rl_get_y_or_n("Continue? [y/n] ") == 0)
		goto ERROR;

	/* Rewind again */
	fseek(fp, 0L, SEEK_SET);

	int is_cwd = 0;
	size_t renamed = 0;

	if ((ret = rename_bulk_files(args, fp, &is_cwd,
	&renamed, modified)) != EXIT_SUCCESS)
		exit_status = ret;

	/* Clean stuff, report, and exit */
	if (unlinkat(fd, bulk_file, 0) == -1) {
		exit_status = errno;
		err('w', PRINT_PROMPT, "br: unlink: '%s': %s\n",
			bulk_file, strerror(errno));
	}

	fclose(fp);

	if (sel_n > 0 && have_sel_files())
		/* Just in case a selected file in the current dir was renamed. */
		get_sel_files();

	if (renamed > 0 && is_cwd == 1 && conf.autols == 1)
		reload_dirlist();
	print_reload_msg(_("%zu file(s) renamed\n"), renamed);

	return exit_status;

ERROR:
	if (unlinkat(fd, bulk_file, 0) == -1) {
		xerror("br: unlinkat: '%s': %s\n", bulk_file, strerror(errno));
		exit_status = errno;
	}

	fclose(fp);
	return exit_status;
}
