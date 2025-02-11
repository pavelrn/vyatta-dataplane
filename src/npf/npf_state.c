/*
 * Copyright (c) 2017-2021, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 */

/*	$NetBSD: npf_state.c,v 1.12 2012/08/15 19:47:38 rmind Exp $	*/

/*-
 * Copyright (c) 2010-2012 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: (LGPL-2.1-only AND BSD-2-Clause-NETBSD)
 *
 * This material is based upon work partially supported by The
 * NetBSD Foundation under a contract with Mindaugas Rasiukevicius.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NPF state engine to track sessions.
 */

#include <assert.h>
#include <ctype.h>
#include <rte_branch_prediction.h>
#include <rte_spinlock.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "json_writer.h"
#include "vrf_internal.h"
#include "npf/npf.h"
#include "npf/config/npf_config.h"
#include "npf/config/npf_ruleset_type.h"
#include "npf/npf_cache.h"
#include "npf/npf_ruleset.h"
#include "npf/npf_state.h"
#include "npf/npf_timeouts.h"
#include "npf/npf_vrf.h"
#include "npf/rproc/npf_rproc.h"
#include "npf_shim.h"
#include "npf/npf_pack.h"
#include "npf/npf_rc.h"

/*
 * TCP state name.
 *
 * Logger uses the upper-case form shown here.
 * npf commands use the lower-case form.
 * json uses use the lower-case form, plus hyphens replaced with underscores.
 */
static const char *npf_state_tcp_name[NPF_TCP_NSTATES] = {
	[NPF_TCPS_NONE]		= "NONE",
	[NPF_TCPS_SYN_SENT]	= "SYN-SENT",
	[NPF_TCPS_SIMSYN_SENT]	= "SIMSYN-SENT",
	[NPF_TCPS_SYN_RECEIVED]	= "SYN-RECEIVED",
	[NPF_TCPS_ESTABLISHED]	= "ESTABLISHED",
	[NPF_TCPS_FIN_SENT]	= "FIN-SENT",
	[NPF_TCPS_FIN_RECEIVED]	= "FIN-RECEIVED",
	[NPF_TCPS_CLOSE_WAIT]	= "CLOSE-WAIT",
	[NPF_TCPS_FIN_WAIT]	= "FIN-WAIT",
	[NPF_TCPS_CLOSING]	= "CLOSING",
	[NPF_TCPS_LAST_ACK]	= "LAST-ACK",
	[NPF_TCPS_TIME_WAIT]	= "TIME-WAIT",
	[NPF_TCPS_RST_RECEIVED]	= "RST-RECEIVED",
	[NPF_TCPS_CLOSED]	= "CLOSED",
};

static const uint8_t npf_generic_fsm[SESSION_STATE_SIZE][NPF_FLOW_SZ] = {
	[SESSION_STATE_NONE] = {
		[NPF_FLOW_FORW]		= SESSION_STATE_NEW,
	},
	[SESSION_STATE_NEW] = {
		[NPF_FLOW_FORW]		= SESSION_STATE_NEW,
		[NPF_FLOW_BACK]		= SESSION_STATE_ESTABLISHED,
	},
	[SESSION_STATE_ESTABLISHED] = {
		[NPF_FLOW_FORW]		= SESSION_STATE_ESTABLISHED,
		[NPF_FLOW_BACK]		= SESSION_STATE_ESTABLISHED,
	},
};

static struct npf_state_stats *stats;
static bool npf_state_icmp_strict;

static inline void stats_inc_tcp(enum tcp_session_state tcp_state)
{
	if (likely(tcp_state <= NPF_TCPS_LAST))
		stats[dp_lcore_id()].ss_tcp_ct[tcp_state]++;
}

static inline void stats_dec_tcp(enum tcp_session_state tcp_state)
{
	if (likely(tcp_state <= NPF_TCPS_LAST))
		stats[dp_lcore_id()].ss_tcp_ct[tcp_state]--;
}

static inline void stats_inc(enum npf_proto_idx proto_idx,
			     enum dp_session_state state)
{
	if (likely(state <= SESSION_STATE_LAST &&
		   proto_idx <= NPF_PROTO_IDX_LAST))
		stats[dp_lcore_id()].ss_ct[proto_idx][state]++;
}

static inline void stats_dec(enum npf_proto_idx proto_idx,
			     enum dp_session_state state)
{
	if (likely(state <= SESSION_STATE_LAST &&
		   proto_idx <= NPF_PROTO_IDX_LAST))
		stats[dp_lcore_id()].ss_ct[proto_idx][state]--;
}

/* state stats - create/destroy */
void npf_state_stats_create(void)
{
	stats = zmalloc_aligned((get_lcore_max() + 1) *
		sizeof(struct npf_state_stats));
}

void npf_state_stats_destroy(void)
{
	free(stats);
}

/* Control strict icmp echo direction checks */
void npf_state_set_icmp_strict(bool value)
{
	npf_state_icmp_strict = value;
}

/*
 * npf_sess_state_init: initialise the state structure.
 *
 * Should normally be called on a first packet, which also determines the
 * direction in a case of connection-orientated protocol.  Returns true on
 * success and false otherwise (e.g. if protocol is not supported).
 */
bool
npf_state_init(vrfid_t vrfid, enum npf_proto_idx proto_idx, npf_state_t *nst)
{
	static_assert(SESSION_STATE_LAST < 255,
		      "session state last is too big");
	static_assert(NPF_TCPS_LAST < 255,
		      "npf tcps last is too big");

	rte_spinlock_init(&nst->nst_lock);

	/* Take reference on vrf npf timeout struct */
	struct npf_timeout *to = vrf_get_npf_timeout_rcu(vrfid);
	if (!to)
		return false;

	npf_timeout_ref_get(to);
	rcu_assign_pointer(nst->nst_to, to);

	if (proto_idx == NPF_PROTO_IDX_TCP) {
		nst->nst_tcp_state = NPF_TCPS_NONE;
		nst->nst_gen_state = SESSION_STATE_NONE;
		stats_inc_tcp(NPF_TCPS_NONE);
	} else {
		nst->nst_gen_state = SESSION_STATE_NONE;
		stats_inc(proto_idx, SESSION_STATE_NONE);
	}

	return true;
}

/* Called from npf_session_destroy */
void npf_state_destroy(npf_state_t *nst, enum npf_proto_idx proto_idx)
{
	struct npf_timeout *to;

	if (proto_idx == NPF_PROTO_IDX_TCP)
		stats_dec_tcp(nst->nst_tcp_state);
	else
		stats_dec(proto_idx, nst->nst_gen_state);

	to = rcu_dereference(nst->nst_to);
	to = rcu_cmpxchg_pointer(&nst->nst_to, to, NULL);

	/* Release reference on vrf npf timeout struct */
	if (to)
		npf_timeout_ref_put(to);
}

/*
* Set generic session state.
*/
static inline void
npf_state_set_gen(npf_state_t *nst, enum npf_proto_idx proto_idx,
		  enum dp_session_state state, bool *state_changed)
{
	if (unlikely(nst->nst_gen_state != state)) {
		enum dp_session_state old_state = nst->nst_gen_state;

		stats_dec(proto_idx, old_state);
		stats_inc(proto_idx, state);

		nst->nst_gen_state = state;
		*state_changed = true;
	}
}

/*
* Set TCP session state.
*/
static inline void
npf_state_set_tcp(npf_state_t *nst, enum tcp_session_state state,
		  bool *state_changed)
{
	if (unlikely(nst->nst_tcp_state != state)) {
		enum tcp_session_state old_state = nst->nst_tcp_state;

		stats_dec_tcp(old_state);
		stats_inc_tcp(state);

		nst->nst_tcp_state = state;
		nst->nst_gen_state = npf_state_tcp2gen(state);
		*state_changed = true;
	}
}

/*
 * State inspect for sessions other than TCP and ICMP
 */
static inline int
npf_state_inspect_other(npf_session_t *se, npf_state_t *nst,
			enum npf_proto_idx proto_idx,
			enum npf_flow_dir flow_dir)
{
	bool state_changed = false;
	enum dp_session_state old_state, new_state;

	rte_spinlock_lock(&nst->nst_lock);

	old_state = nst->nst_gen_state;

	new_state = npf_generic_fsm[nst->nst_gen_state][flow_dir];

	npf_state_set_gen(nst, proto_idx, new_state, &state_changed);

	rte_spinlock_unlock(&nst->nst_lock);

	if (state_changed)
		npf_session_gen_state_change(se, nst, old_state, new_state,
					     proto_idx);

	return 0;
}

/*
 * State inspect for TCP sessions
 */
static inline int
npf_state_inspect_tcp(const npf_cache_t *npc, struct rte_mbuf *nbuf,
		      npf_session_t *se, npf_state_t *nst,
		      enum npf_flow_dir flow_dir)
{
	bool state_changed = false;
	enum tcp_session_state old_state, new_state;
	int rc = 0;

	rte_spinlock_lock(&nst->nst_lock);

	old_state = nst->nst_tcp_state;

	new_state = npf_state_tcp(npc, nbuf, nst, flow_dir, &rc);

	if (rc == 0)
		npf_state_set_tcp(nst, new_state, &state_changed);

	rte_spinlock_unlock(&nst->nst_lock);

	if (state_changed)
		npf_session_tcp_state_change(se, nst, old_state, new_state);

	return rc;
}

/*
 * State inspect for ICMP sessions
 */
static inline int
npf_state_inspect_icmp(const npf_cache_t *npc, npf_session_t *se,
		       npf_state_t *nst, enum npf_flow_dir flow_dir)
{
	bool state_changed = false;
	enum dp_session_state old_state, new_state;
	int rc = 0;

	rte_spinlock_lock(&nst->nst_lock);

	old_state = nst->nst_gen_state;

	/*
	 * If a ping session does not exist, it can only be created by an ICMP
	 * echo request. If it exists, the fwd direction will conditionally
	 * ('strict' enabled) only pass requests and the backward only
	 * replies.  Note, the 'strict' bit needs to be disabled because of MS
	 * Windows clients.
	 */
	if ((npf_state_icmp_strict || old_state == SESSION_STATE_NONE) &&
	    unlikely((flow_dir == NPF_FLOW_FORW) ^
		     npf_iscached(npc, NPC_ICMP_ECHO_REQ)))
		rc = -NPF_RC_ICMP_ECHO;

	if (rc == 0) {
		new_state = npf_generic_fsm[nst->nst_gen_state][flow_dir];

		npf_state_set_gen(nst, NPF_PROTO_IDX_ICMP, new_state,
				  &state_changed);
	}

	rte_spinlock_unlock(&nst->nst_lock);

	if (state_changed)
		npf_session_gen_state_change(se, nst, old_state, new_state,
					     NPF_PROTO_IDX_ICMP);

	return rc;
}

/*
 * npf_state_inspect: inspect the packet according to the protocol state.
 *
 * Return 0 if packet is considered to match the state (e.g. for TCP, the
 * packet belongs to the tracked connection) and return code (< 0) otherwise.
 */
int npf_state_inspect(const npf_cache_t *npc, struct rte_mbuf *nbuf,
		      npf_session_t *se, npf_state_t *nst,
		      enum npf_proto_idx proto_idx, bool forw)
{
	enum npf_flow_dir flow_dir = forw ? NPF_FLOW_FORW : NPF_FLOW_BACK;
	int rc = 0;

	switch (proto_idx) {
	case NPF_PROTO_IDX_UDP:
	case NPF_PROTO_IDX_OTHER:
		rc = npf_state_inspect_other(se, nst, proto_idx, flow_dir);
		break;
	case NPF_PROTO_IDX_TCP:
		rc = npf_state_inspect_tcp(npc, nbuf, se, nst, flow_dir);
		break;
	case NPF_PROTO_IDX_ICMP:
		rc = npf_state_inspect_icmp(npc, se, nst, flow_dir);
		break;
	};

	return rc;
}

/*
 * Mark (non-TCP) session state as 'closed' for the period that it is going
 * through garbage collection.
 */
void npf_state_set_gen_closed(npf_state_t *nst, npf_session_t *se, bool lock,
			      enum npf_proto_idx proto_idx)
{
	enum dp_session_state old_state;
	bool state_changed = false;

	if (lock)
		rte_spinlock_lock(&nst->nst_lock);

	old_state = nst->nst_gen_state;

	npf_state_set_gen(nst, proto_idx, SESSION_STATE_CLOSED, &state_changed);

	if (lock)
		rte_spinlock_unlock(&nst->nst_lock);

	if (state_changed)
		npf_session_gen_state_change(se, nst, old_state,
					     SESSION_STATE_CLOSED, proto_idx);
}

/*
 * Mark TCP session state as 'closed' for the period that it is going through
 * garbage collection.
 */
void npf_state_set_tcp_closed(npf_state_t *nst, npf_session_t *se, bool lock)
{
	enum tcp_session_state old_state;
	bool state_changed = false;

	if (lock)
		rte_spinlock_lock(&nst->nst_lock);

	old_state = nst->nst_tcp_state;

	npf_state_set_tcp(nst, NPF_TCPS_CLOSED, &state_changed);

	if (lock)
		rte_spinlock_unlock(&nst->nst_lock);

	if (state_changed)
		npf_session_tcp_state_change(se, nst, old_state,
					     NPF_TCPS_CLOSED);
}

/*
 * Update a dataplane session other than TCP (if present) state/timeout with
 * the current NPF protocol state.
 *
 * This is called during NPF activation and protocol state changes.
 */
void npf_state_update_gen_session(struct session *s,
				  enum npf_proto_idx proto_idx,
				  const npf_state_t *nst)
{
	if (unlikely(!s))
		return;

	uint32_t timeout;
	enum dp_session_state gen_state = nst->nst_gen_state;

	timeout = npf_gen_timeout_get(nst, gen_state, proto_idx,
				      s->se_custom_timeout);

	/* Protocol state and gen state are the same */
	session_set_protocol_state_timeout(s, gen_state, gen_state, timeout);
}

/*
 * Update a dataplane TCP session state/timeout with the current NPF protocol
 * state.
 */
void npf_state_update_tcp_session(struct session *s, const npf_state_t *nst)
{
	if (unlikely(!s))
		return;

	uint32_t timeout;
	enum tcp_session_state tcp_state = nst->nst_tcp_state;
	enum dp_session_state gen_state = npf_state_tcp2gen(tcp_state);

	timeout = npf_tcp_timeout_get(nst, tcp_state, s->se_custom_timeout);

	/* Protocol state and gen state are different */
	session_set_protocol_state_timeout(s, tcp_state, gen_state, timeout);
}

const char *npf_state_get_tcp_name(enum tcp_session_state state)
{
	if (state <= NPF_TCPS_LAST)
		return npf_state_tcp_name[state];
	return "none";
}

/*
 * Json strings are lower case, with underscores in place of hyphens.
 */
static void npf_str_to_json_name(const char *src, char *dst, int len)
{
	int i;

	for (i = 0; i < len-1 && src[i] != '\0'; i++) {
		if (src[i] == '-')
			dst[i] = '_';
		else
			dst[i] = tolower(src[i]);
	}
	dst[i] = '\0';
}

/*
 * Log strings are upper case, with hyphens in place of underscores
 */
static void npf_str_to_log_name(const char *src, char *dst, int len)
{
	int i;

	for (i = 0; i < len-1 && src[i] != '\0'; i++)
		if (src[i] == '_')
			dst[i] = '-';
		else
			dst[i] = toupper(src[i]);
	dst[i] = '\0';
}

/*
 * Generic state name used in summary stats
 *
 * For UDP, ICMP, and other we are not interested in SESSION_STATE_NONE or
 * SESSION_STATE_TERMINATING.
 *
 * Note that these names are different from those returned by
 * dp_session_state_name.
 */
static const char *npf_state_name_summary_json(enum dp_session_state state)
{
	switch (state) {
	case SESSION_STATE_NEW:
		return "new";
	case SESSION_STATE_ESTABLISHED:
		return "established";
	case SESSION_STATE_CLOSED:
		return "closed";
	case SESSION_STATE_TERMINATING:
	case SESSION_STATE_NONE:
		break;
	};
	return "none";
}

/* convert CLI TCP state to numerical value */
enum tcp_session_state npf_map_str_to_tcp_state(const char *name)
{
	enum tcp_session_state state;
	char upper[40];

	npf_str_to_log_name(name, upper, sizeof(upper));

	for (state = NPF_TCPS_FIRST;
	     state <= NPF_TCPS_LAST; state++)
		if (strcmp(upper, npf_state_tcp_name[state]) == 0)
			return state;

	return NPF_TCPS_NONE;
}

/*
 * Test the packet to see if it matches a custom session timeout.
 */
uint32_t npf_state_get_custom_timeout(vrfid_t vrfid, npf_cache_t *npc,
				      struct rte_mbuf *nbuf)
{
	/* Test the packet */
	struct npf_config *npf_config = vrf_get_npf_conf_rcu(vrfid);
	const npf_ruleset_t *npf_rs =
			npf_get_ruleset(npf_config, NPF_RS_CUSTOM_TIMEOUT);
	npf_rule_t *rl =
		npf_ruleset_inspect(npc, nbuf, npf_rs, NULL, NULL, PFIL_IN);

	/* The custom timeout handle is stored as a tag */
	bool tag_present = false;
	uint32_t tag_val = (rl) ? npf_rule_rproc_tag(rl, &tag_present) : 0;

	return (tag_present) ? tag_val : 0;
}

void npf_state_stats_json(json_writer_t *json)
{
	enum npf_proto_idx proto;
	uint32_t tmp;
	uint32_t i;
	char name[40];

	/* Temporary fixup until vplane-config-npf is updated */
	FOREACH_DP_LCORE(i) {
		stats[i].ss_tcp_ct[NPF_TCPS_CLOSED] +=
			stats[i].ss_tcp_ct[NPF_TCPS_NONE];
	}

	FOREACH_DP_LCORE(i) {
		for (proto = NPF_PROTO_IDX_FIRST;
				proto <= NPF_PROTO_IDX_LAST; proto++)
			stats[i].ss_ct[proto][SESSION_STATE_CLOSED] +=
				stats[i].ss_ct[proto][SESSION_STATE_NONE];
	}

	jsonw_name(json, "tcp");
	jsonw_start_object(json);

	enum tcp_session_state tcp_state;

	for (tcp_state = NPF_TCPS_FIRST; tcp_state <= NPF_TCPS_LAST;
	     tcp_state++) {
		/* Copy state name to name[] buffer */
		npf_str_to_json_name(npf_state_get_tcp_name(tcp_state),
				     name, sizeof(name));

		tmp = 0;
		FOREACH_DP_LCORE(i)
			tmp += stats[i].ss_tcp_ct[tcp_state];
		jsonw_uint_field(json, name, tmp);
	}

	jsonw_end_object(json);

	/*
	 * udp, icmp and other
	 */
	for (proto = NPF_PROTO_IDX_FIRST;
	     proto <= NPF_PROTO_IDX_LAST; proto++) {
		enum dp_session_state state;

		if (proto == NPF_PROTO_IDX_TCP)
			continue;

		jsonw_name(json, npf_get_protocol_name_from_idx(proto));
		jsonw_start_object(json);

		for (state = SESSION_STATE_FIRST;
		     state <= SESSION_STATE_LAST;  state++) {

			/* Copy state name to name[] buffer */
			npf_str_to_json_name(npf_state_name_summary_json(state),
					     name, sizeof(name));

			tmp = 0;
			FOREACH_DP_LCORE(i)
				tmp +=  stats[i].ss_ct[proto][state];
			jsonw_uint_field(json, name, tmp);
		}

		jsonw_end_object(json);
	}
}

#ifdef _NPF_TESTING
void
npf_state_dump(const npf_state_t *nst __unused)
{
	const struct npf_tcp_window *fst = &nst->nst_tcp_win[NPF_FLOW_FORW];
	const struct npf_tcp_window *tst = &nst->nst_tcp_win[NPF_FLOW_BACK];

	printf("\tstate (%p) %d:\n\t\t"
	    "F { end %u maxend %u mwin %u wscale %u }\n\t\t"
	    "T { end %u maxend %u mwin %u wscale %u }\n",
	    nst, nst->nst_tcp_state,
	    fst->nst_end, fst->nst_maxend, fst->nst_maxwin, fst->nst_wscale,
	    tst->nst_end, tst->nst_maxend, tst->nst_maxwin, tst->nst_wscale
	);
}
#endif

/* Update non-TCP session state from a connsync restore or update */
void npf_state_pack_update_gen(npf_state_t *nst,
			       struct npf_pack_session_state *pst,
			       enum npf_proto_idx proto_idx,
			       bool *state_changed)
{
	rte_spinlock_lock(&nst->nst_lock);

	npf_state_set_gen(nst, proto_idx, pst->pst_gen_state, state_changed);

	rte_spinlock_unlock(&nst->nst_lock);
}

/* Update TCP session state from a connsync restore or update */
void npf_state_pack_update_tcp(npf_state_t *nst,
			       struct npf_pack_session_state *pst,
			       bool *state_changed)
{
	struct npf_pack_tcp_window *ptw;
	struct npf_tcp_window *ntw;
	enum npf_flow_dir fl;

	rte_spinlock_lock(&nst->nst_lock);

	for (fl = NPF_FLOW_FIRST; fl <= NPF_FLOW_LAST; fl++) {
		ptw = &pst->pst_tcp_win[fl];
		ntw = &nst->nst_tcp_win[fl];

		ntw->nst_end = ptw->ptw_end;
		ntw->nst_maxend = ptw->ptw_maxend;
		ntw->nst_maxwin = ptw->ptw_maxwin;
		ntw->nst_wscale = ptw->ptw_wscale;
	}

	npf_state_set_tcp(nst, pst->pst_tcp_state, state_changed);

	rte_spinlock_unlock(&nst->nst_lock);
}

/* Copy non-TCP session state to a protobuf-c message */
int npf_state_pack_gen_pb(npf_state_t *nst, NPFSessionStateMsg *nss)
{
	if (!nst || !nss)
		return -EINVAL;

	nss->has_nss_state = 1;
	nss->nss_state = nst->nst_gen_state;
	return 0;
}

/* Restore non-TCP session state from a protobuf-c message */
int npf_state_restore_gen_pb(npf_state_t *nst, NPFSessionStateMsg *nss)
{
	if (!nst || !nss)
		return -EINVAL;

	if (nss->nss_state > SESSION_STATE_LAST)
		return -EINVAL;

	nst->nst_gen_state = nss->nss_state;
	return 0;
}

/* update non-TCP session state from a protobuf-c message */
int npf_state_update_gen_pb(npf_state_t *nst, NPFSessionStateMsg *nss,
			    enum npf_proto_idx proto_idx, bool *state_changed)
{
	if (!nst || !nss)
		return -EINVAL;

	if (nss->nss_state > SESSION_STATE_LAST)
		return -EINVAL;

	rte_spinlock_lock(&nst->nst_lock);
	npf_state_set_gen(nst, proto_idx, nss->nss_state, state_changed);
	rte_spinlock_unlock(&nst->nst_lock);
	return 0;
}

/* Copy TCP session state to a protobuf-c message */
int npf_state_pack_tcp_pb(npf_state_t *nst, NPFSessionStateMsg *nss)
{
	int i;
	const struct npf_tcp_window *tcp_win;

	if (!nst || !nss)
		return -EINVAL;

	nss->has_nss_state = 1;
	nss->nss_state = nst->nst_tcp_state;

	nss->n_nss_tcpwins = NPF_FLOW_SZ;
	for (i = NPF_FLOW_FIRST; i <= NPF_FLOW_LAST; i++) {
		tcp_win = &nst->nst_tcp_win[i];
		nss->nss_tcpwins[i]->has_tw_end = 1;
		nss->nss_tcpwins[i]->tw_end = tcp_win->nst_end;
		nss->nss_tcpwins[i]->has_tw_maxend = 1;
		nss->nss_tcpwins[i]->tw_maxend = tcp_win->nst_maxend;
		nss->nss_tcpwins[i]->has_tw_maxwin = 1;
		nss->nss_tcpwins[i]->tw_maxwin = tcp_win->nst_maxwin;
		nss->nss_tcpwins[i]->has_tw_wscale = 1;
		nss->nss_tcpwins[i]->tw_wscale = tcp_win->nst_wscale;
	}
	return 0;
}

/* restore npf_tcp_window from protobuf-c message */
void npf_state_restore_tcpwin_pb(struct npf_tcp_window *tcp_win,
				 TCPWindowMsg *pb_tcp_win)
{
	if (pb_tcp_win->has_tw_end)
		tcp_win->nst_end = pb_tcp_win->tw_end;
	if (pb_tcp_win->has_tw_maxend)
		tcp_win->nst_maxend = pb_tcp_win->tw_maxend;
	if (pb_tcp_win->has_tw_maxwin)
		tcp_win->nst_maxwin = pb_tcp_win->tw_maxwin;
	if (pb_tcp_win->has_tw_wscale)
		tcp_win->nst_wscale = pb_tcp_win->tw_wscale;
}

/* restore initial tcp state from protobuf-c message - no locking is needed */
int npf_state_restore_tcp_pb(npf_state_t *nst, NPFSessionStateMsg *nss)
{
	int i;

	if (!nst || !nss)
		return -EINVAL;

	if (nss->nss_state > NPF_TCPS_LAST)
		return -EINVAL;

	nst->nst_tcp_state = nss->nss_state;

	for (i = NPF_FLOW_FIRST; i <= NPF_FLOW_LAST; i++)
		npf_state_restore_tcpwin_pb(&nst->nst_tcp_win[i],
					    nss->nss_tcpwins[i]);
	return 0;
}

/* update tcp state from protobuf-c message. */
int npf_state_update_tcp_pb(npf_state_t *nst, NPFSessionStateMsg *nss,
			    bool *state_changed)
{
	int i;

	if (!nst || !nss)
		return -EINVAL;

	if (nss->nss_state > NPF_TCPS_LAST)
		return -EINVAL;

	rte_spinlock_lock(&nst->nst_lock);
	for (i = NPF_FLOW_FIRST; i <= NPF_FLOW_LAST; i++)
		npf_state_restore_tcpwin_pb(&nst->nst_tcp_win[i],
					    nss->nss_tcpwins[i]);
	npf_state_set_tcp(nst, nss->nss_state, state_changed);
	rte_spinlock_unlock(&nst->nst_lock);
	return 0;
}
