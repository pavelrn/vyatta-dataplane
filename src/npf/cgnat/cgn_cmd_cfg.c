/*
 * Copyright (c) 2019, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/**
 * @file cgn_cmd_cfg.c - CGNAT config
 *
 * -----------------------------------------------
 * Policy config
 * -----------------------------------------------
 *
 * cgn-cfg policy add <policy-name>
 *   priority=<priority>
 *   src-addr=<prefix/length>
 *   pool=<pool-name>
 *   log-group=<group-name>
 *   log-all={yes | no}
 *
 * cgn-cfg policy delete <policy-name>
 * cgn-cfg policy attach <policy-name> <interface-name>
 * cgn-cfg policy detach <policy-name> <interface-name>
 *
 * -----------------------------------------------
 * Other config
 * -----------------------------------------------
 *
 * cgn-cfg hairpinning {on | off}
 */

#include <errno.h>
#include <netinet/in.h>
#include <linux/if.h>

#include "commands.h"
#include "compiler.h"
#include "config.h"
#include "if_var.h"
#include "util.h"
#include "vplane_log.h"

#include "npf/npf_addr.h"

#include "npf/cgnat/cgn.h"
#include "npf/cgnat/cgn_if.h"
#include "npf/cgnat/cgn_limits.h"
#include "npf/cgnat/cgn_policy.h"
#include "npf/cgnat/cgn_sess_state.h"
#include "npf/cgnat/cgn_session.h"
#include "npf/cgnat/cgn_cmd_cfg.h"


/*
 * Interface list to handle interface config replay.  Entries are identified
 * by name
 */
struct cgn_cfg_if_list {
	struct cds_list_head  if_list;
	int                   if_list_count;
};

struct cgn_cfg_if_list_entry {
	struct cds_list_head  le_node;
	char                  le_ifname[IFNAMSIZ];
	char                  *le_buf;
	char                  **le_argv;
	int                   le_argc;
};

static struct cgn_cfg_if_list *cgn_cfg_list;

/* Create list */
static struct cgn_cfg_if_list *cgn_cfg_if_list_create(void)
{
	struct cgn_cfg_if_list *if_list;

	if_list = zmalloc_aligned(sizeof(*if_list));
	if (!if_list)
		return NULL;

	CDS_INIT_LIST_HEAD(&if_list->if_list);
	if_list->if_list_count = 0;

	return if_list;
}

/* Add a command to the list */
static int
cgn_cfg_if_list_add(struct cgn_cfg_if_list *if_list, const char *ifname,
		    int argc, char *argv[])
{
	struct cgn_cfg_if_list_entry *le;
	int i, size;

	if (strlen(ifname) + 1 > IFNAMSIZ)
		return -EINVAL;

	le = zmalloc_aligned(sizeof(*le));
	if (!le)
		return -ENOMEM;

	memcpy(le->le_ifname, ifname, strlen(ifname) + 1);

	/* Determine space required for arg strings */
	for (size = 0, i = 0; i < argc; i++)
		size += (strlen(argv[i]) + 1);

	if (!size) {
		free(le);
		return -EINVAL;
	}

	le->le_buf = malloc(size);
	le->le_argv = malloc(argc * sizeof(void *));
	le->le_argc = argc;

	if (!le->le_buf || !le->le_argv) {
		free(le->le_buf);
		free(le->le_argv);
		free(le);
		return -ENOMEM;
	}

	char *ptr = le->le_buf;

	for (i = 0; i < argc; i++) {
		size = strlen(argv[i]) + 1;
		memcpy(ptr, argv[i], size);
		le->le_argv[i] = ptr;
		ptr += size;
	}

	cds_list_add_tail(&le->le_node, &if_list->if_list);
	if_list->if_list_count++;

	return 0;
}

/* Remove entry from list, and free it */
static int
cgn_cfg_if_list_del(struct cgn_cfg_if_list *if_list,
		    struct cgn_cfg_if_list_entry *le)
{
	if (!if_list || if_list->if_list_count == 0)
		return -ENOENT;

	cds_list_del(&le->le_node);
	if_list->if_list_count--;

	if (le->le_buf)
		free(le->le_buf);
	if (le->le_argv)
		free(le->le_argv);
	free(le);

	return 0;
}

/* Destroy list */
static int cgn_cfg_if_list_destroy(struct cgn_cfg_if_list **if_list)
{
	if (!*if_list || (*if_list)->if_list_count)
		return -EINVAL;

	free(*if_list);
	*if_list = NULL;
	return 0;
}

/*
 * Interface has been created.  Replay any relevant commands on the interface
 * list.
 */
void
cgn_event_if_index_set(struct ifnet *ifp, uint32_t ifindex __unused)
{
	struct cgn_cfg_if_list_entry *le, *tmp;

	if (!cgn_cfg_list)
		return;

	cds_list_for_each_entry_safe(le, tmp, &cgn_cfg_list->if_list,
				     le_node) {

		if (strcmp(ifp->if_name, le->le_ifname) != 0)
			continue;

		/* Replay command */
		cmd_cgn(NULL, le->le_argc, le->le_argv);

		/* Remove from list and free */
		cgn_cfg_if_list_del(cgn_cfg_list, le);
	}

	if (!cgn_cfg_list->if_list_count)
		cgn_cfg_if_list_destroy(&cgn_cfg_list);
}

/*
 * Interface has been deleted.  Discard any saved commands.
 */
void
cgn_event_if_index_unset(struct ifnet *ifp, uint32_t ifindex __unused)
{
	struct cgn_cfg_if_list_entry *le, *tmp;

	if (!cgn_cfg_list)
		return;

	cds_list_for_each_entry_safe(le, tmp, &cgn_cfg_list->if_list,
				     le_node) {

		if (strcmp(ifp->if_name, le->le_ifname) != 0)
			continue;

		cgn_cfg_if_list_del(cgn_cfg_list, le);
	}

	if (!cgn_cfg_list->if_list_count)
		cgn_cfg_if_list_destroy(&cgn_cfg_list);
}

/* Initialize command replay list */
static int cgn_cfg_replay_init(void)
{
	if (!cgn_cfg_list) {
		cgn_cfg_list = cgn_cfg_if_list_create();
		if (!cgn_cfg_list)
			return -ENOMEM;

	}
	return 0;
}

/*
 * Iterate through argv/argc looking for "intf=dp0p1", extract the interface
 * name, and lookup the ifp pointer.  Does not change argv.
 */
static char *
cgn_cfg_ifname_from_arg(char *if_name, int sz, int argc, char **argv)
{
	char *c, *item, *value;
	int i;

	if (sz < IFNAMSIZ)
		return NULL;

	for (i = 0; i < argc; i++) {
		if (!strstr(argv[i], "intf="))
			continue;

		c = strchr(argv[i], '=');
		if (!c)
			return NULL;

		/* Duplicate the string so we can write to it */
		item = strdup(argv[i]);
		if (!item)
			return NULL;

		c = strchr(item, '=');
		if (!c) {
			free(item);
			return NULL;
		}

		*c = '\0';
		value = c + 1;

		strncpy(if_name, value, sz-1);
		if_name[sz] = '\0';

		free(item);
		return if_name;
	}
	return NULL;
}

/*
 * Attach policy to interface
 *
 * cgn-cfg policy attach intf=dp0p1 name=POLICY1
 */
static int cgn_policy_cfg_attach(FILE *f, int argc, char **argv)
{
	const char *name = NULL;
	struct ifnet *ifp;
	struct cgn_policy *cp;
	char ifname[IFNAMSIZ+1];
	char *c, *item, *value;
	int i;

	if (argc < 5)
		goto usage;

	/* Extract interface name from "intf=dp0p1" arg list */
	if (!cgn_cfg_ifname_from_arg(ifname, sizeof(ifname), argc, argv))
		goto usage;

	/* Does interface exist? */
	ifp = ifnet_byifname(ifname);

	if (!ifp) {
		/* No.  Store command for later replay */
		if (cgn_cfg_replay_init() != 0) {
			RTE_LOG(ERR, CGNAT,
				"Could not set up cgn replay cache\n");
			goto err_out;
		}

		cgn_cfg_if_list_add(cgn_cfg_list, ifname, argc, argv);

		RTE_LOG(ERR, CGNAT,
			"Caching cgn command for interface %s\n", ifname);
		return 0;
	}

	/*
	 * Parse item/value pairs.  We ignore any we do not understand.
	 */
	for (i = 0; i < argc; i++) {
		c = strchr(argv[i], '=');
		if (!c)
			continue;

		item = argv[i];
		*c = '\0';
		value = c + 1;

		if (!strcmp(item, "intf"))
			continue;

		if (!strcmp(item, "name"))
			name = value;
	}

	if (!name)
		goto usage;

	cp = cgn_policy_lookup(name);
	if (!cp)
		return -EEXIST;

	/* Add policy to cgn interface list, and take reference on policy */
	cgn_if_add_policy(ifp, cp);

	return 0;

usage:
	if (f)
		fprintf(f, "%s: policy attach name=<policy-name> "
			"intf=<intf-name>",
			__func__);
err_out:
	return -1;
}

/*
 * Detach policy from interface
 *
 * cgn policy detach name=POLICY1 intf=dpT21
 */
static int cgn_policy_cfg_detach(FILE *f, int argc, char **argv)
{
	struct cgn_policy *cp;
	const char *name = NULL;
	char *ifname = NULL;
	struct ifnet *ifp;
	char *c, *item, *value;
	int i;

	if (argc < 5)
		goto usage;

	/*
	 * Parse item/value pairs.  We ignore any we do not understand.
	 */
	for (i = 0; i < argc; i++) {
		c = strchr(argv[i], '=');
		if (!c)
			continue;

		item = argv[i];
		*c = '\0';
		value = c + 1;

		if (!strcmp(item, "intf"))
			ifname = value;

		else if (!strcmp(item, "name"))
			name = value;
	}

	if (!name || !ifname)
		goto usage;

	/* Does interface exist? */
	ifp = ifnet_byifname(ifname);
	if (!ifp)
		return -EEXIST;

	/*
	 * Policy may have been removed from the hash table before now, so
	 * search list
	 */
	cp = cgn_if_find_policy_by_name(ifp, name);
	if (!cp)
		return 0;

	/*
	 * Delete policy from interface list and release reference on policy
	 */
	cgn_if_del_policy(ifp, cp);

	/* If policy list is now empty, then free cgn intf */
	cgn_if_gc_intf(ifp, false);

	return 0;

usage:
	if (f)
		fprintf(f, "%s: policy detach <policy-name> <intf-name>",
			__func__);

	return -1;
}

/*
 * cgn-cfg policy ...
 */
static int cgn_policy_cfg(FILE *f, int argc, char **argv)
{
	int rc = 0;

	if (argc < 3)
		goto usage;

	/* Policy */
	if (strcmp(argv[2], "add") == 0)
		rc = cgn_policy_cfg_add(f, argc, argv);

	else if (strcmp(argv[2], "delete") == 0)
		rc = cgn_policy_cfg_delete(f, argc, argv);

	else if (strcmp(argv[2], "attach") == 0)
		rc = cgn_policy_cfg_attach(f, argc, argv);

	else if (strcmp(argv[2], "detach") == 0)
		rc = cgn_policy_cfg_detach(f, argc, argv);
	else
		goto usage;

	return rc;
usage:
	if (f)
		fprintf(f, "%s: cgn-cfg policy {add|delete} ... ",
			__func__);

	return -1;
}

/*
 * cgn-cfg hairpinning [on|off]
 */
static int cgn_hairpinning_cfg(FILE *f, int argc, char **argv)
{
	if (argc < 3)
		goto usage;

	/* Policy */
	if (strcmp(argv[2], "on") == 0)
		cgn_hairpinning_gbl = true;
	else
		cgn_hairpinning_gbl = false;

	return 0;
usage:
	if (f)
		fprintf(f, "%s: cgn-cfg hairpinning {on|off}",
			__func__);

	return -1;
}

/*
 * cgn-cfg max-sessions <num>
 */
static int cgn_max_sessions_cfg(FILE *f, int argc, char **argv)
{
	int tmp;

	if (argc < 3)
		goto usage;

	tmp = cgn_arg_to_int(argv[2]);
	if (tmp < 0 || tmp > CGN_SESSIONS_MAX)
		return -1;

	if (tmp == 0)
		tmp = CGN_SESSIONS_MAX;
	cgn_sessions_max = tmp;

	return 0;
usage:
	if (f)
		fprintf(f, "%s: cgn-cfg max-sessions <num>",
			__func__);

	return -1;
}

/*
 * cgn-cfg max-dest-per-session <num>
 *
 * cs_sess2_used is a an atomic int16, so the value cgn_dest_sessions_max must
 * never be greater than USHRT_MAX - 1 to avoid wrap.
 */
static int cgn_max_dest_sessions_cfg(FILE *f, int argc, char **argv)
{
	int tmp;

	if (argc < 3)
		goto usage;

	assert(CGN_DEST_SESSIONS_MAX < USHRT_MAX);

	tmp = cgn_arg_to_int(argv[2]);
	if (tmp < 0 || tmp > CGN_DEST_SESSIONS_MAX || tmp > (USHRT_MAX - 1))
		return -1;

	if (tmp == 0)
		tmp = CGN_DEST_SESSIONS_MAX;
	cgn_dest_sessions_max = (int16_t)tmp;

	return 0;
usage:
	if (f)
		fprintf(f, "%s: cgn-cfg max-dest-per-session <num>",
			__func__);

	return -1;
}

/*
 * Session timeouts
 */
static int cgn_session_timeouts_cfg(FILE *f, int argc, char **argv)
{
	char *c, *item, *value;
	int i, tmp;

	/* Move past "cgn-cfg session-timeouts" */
	argc -= 2;
	argv += 2;

	if (argc < 2)
		goto usage;

	/*
	 * Parse item/value pairs.  We ignore any we do not understand.
	 */
	for (i = 0; i < argc; i++) {
		c = strchr(argv[i], '=');
		if (!c)
			continue;

		item = argv[i];
		*c = '\0';
		value = c + 1;

		tmp = cgn_arg_to_int(value);
		if (tmp < 0)
			goto usage;

		if (!strcmp(item, "other-opening"))
			cgn_sess_other_etime[CGN_ETIME_OPENING] = tmp;

		else if (!strcmp(item, "other-estab"))
			cgn_sess_other_etime[CGN_ETIME_ESTBD] = tmp;

		else if (!strcmp(item, "udp-opening"))
			cgn_sess_udp_etime[CGN_ETIME_OPENING] = tmp;

		else if (!strcmp(item, "udp-estab"))
			cgn_sess_udp_etime[CGN_ETIME_ESTBD] = tmp;

		else if (!strcmp(item, "tcp-opening"))
			cgn_sess_tcp_etime[CGN_ETIME_TCP_OPENING] = tmp;

		else if (!strcmp(item, "tcp-estab"))
			cgn_sess_tcp_etime[CGN_ETIME_TCP_ESTBD] = tmp;

		else if (!strcmp(item, "tcp-closing"))
			cgn_sess_tcp_etime[CGN_ETIME_TCP_CLOSING] = tmp;
	}

	return 0;
usage:
	if (f)
		fprintf(f, "%s: cgn-cfg "
			"session-timeouts <item> <value> ...",
			__func__);

	return -1;
}


/*
 * cgn-cfg [policy | hairpinning] ...
 * cgn-ut  ...
 */
int cmd_cgn(FILE *f, int argc, char **argv)
{
	int rc = 0;

	if (argc < 2)
		goto usage;

	if (strcmp(argv[1], "policy") == 0)
		rc = cgn_policy_cfg(f, argc, argv);

	else if (strcmp(argv[1], "hairpinning") == 0)
		rc = cgn_hairpinning_cfg(f, argc, argv);

	else if (strcmp(argv[1], "max-sessions") == 0)
		rc = cgn_max_sessions_cfg(f, argc, argv);

	else if (strcmp(argv[1], "max-dest-per-session") == 0)
		rc = cgn_max_dest_sessions_cfg(f, argc, argv);

	else if (strcmp(argv[1], "session-timeouts") == 0)
		rc = cgn_session_timeouts_cfg(f, argc, argv);

	else
		goto usage;

	return rc;

usage:
	if (f)
		fprintf(f, "%s: cgn-cfg {policy} {add|delete} ... ",
			__func__);

	return -1;
}

int cmd_cgn_ut(FILE *f, int argc, char **argv)
{
	return cmd_cgn(f, argc, argv);
}
