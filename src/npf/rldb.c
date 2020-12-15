/*
 * Copyright (c) 2020, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include <rte_acl.h>
#include <rte_mempool.h>
#include <rte_jhash.h>

#include "npf_rte_acl.h"

#include "urcu.h"
#include "util.h"
#include "vplane_log.h"

#include "rldb.h"

#define RLDB_ERR(args...) RTE_LOG(ERR, DATAPLANE, args)

#define RLDB_MAX_RULES    (1 << 13)
#define RLDB_MAX_ELEMENTS (2 * RLDB_MAX_RULES)

#define GLOBAL_MIN_BUCKETS (2 << 6)
#define GLOBAL_MAX_BUCKETS (2 << 10)

#define RLDB_MIN_BUCKETS (1 << 16)
#define RLDB_MAX_BUCKETS (1 << 17)

struct rldb_db_handle {
	npf_match_ctx_t *match_ctx;
	uint32_t flags;
	uint16_t af;
	struct rte_acl_rule *acl_rules;
	struct cds_lfht *ht;
	struct cds_lfht_node ht_node;
	struct rldb_stats stats;
	/* --- cacheline 1 boundary (64 bytes) was 40 bytes ago --- */
	char name[RLDB_NAME_MAX];
};

struct rldb_rule_handle {
	uint32_t rule_no;
	struct cds_lfht_node ht_node;
	struct rldb_rule_spec rule;
};

static struct rte_mempool *rldb_mempool;
static struct cds_lfht *rldb_global_ht;

static bool rldb_disabled;

static rte_atomic32_t rldb_counter;

/*
 * initialize infrastructure for rule database
 */
int rldb_init(void)
{
	int rc;
	rldb_mempool = rte_mempool_create("rldb_pool", RLDB_MAX_ELEMENTS,
					  sizeof(struct rldb_rule_handle),
					  0, 0, NULL, NULL, NULL, NULL,
					  rte_socket_id(), 0);

	if (!rldb_mempool) {
		RLDB_ERR("Could not allocate rldb pool\n");
		return -ENOMEM;
	}

	rldb_global_ht = cds_lfht_new(GLOBAL_MIN_BUCKETS,
				      GLOBAL_MIN_BUCKETS,
				      GLOBAL_MAX_BUCKETS,
				      CDS_LFHT_AUTO_RESIZE, NULL);

	if (!rldb_global_ht) {
		RLDB_ERR("Could not allocate rldb id hashtable\n");
		rc = -ENOMEM;
		goto error;
	}

	rldb_disabled = false;

	return 0;

error:
	rldb_cleanup();
	return rc;
}

static int rldb_name_match(struct cds_lfht_node *node, const void *key)
{
	const char *key_name = key;

	struct rldb_db_handle *db = caa_container_of(node,
						     struct rldb_db_handle,
						     ht_node);

	if (strcmp(key_name, db->name) == 0)
		return 1;

	return 0;
}

static void rldb_db_handle_destroy(struct rldb_db_handle *db)
{
	if (!db)
		return;

	cds_lfht_destroy(db->ht, NULL);

	if (db->match_ctx)
		npf_rte_acl_destroy(db->af, &db->match_ctx);

	free(db);
}

/*
 * Borrowed from nat_pool.c:
 * ----8<----
 * rte_jhash reads from memory in 4-byte chunks.  If the length of 'name' is
 * not a multiple of 4 bytes then it may try and read memory that is not
 * mapped.  Issue was detected by valgrind.
 * ---->8-----
 *
 * Also spotted by AddressSanitizer: global-buffer-overflow
 */
static uint32_t rldb_name_hash(const char *name, size_t name_len)
{
	char buf[name_len+3];

	memcpy(buf, name, name_len);
	return rte_jhash(buf, name_len, 0);
}

/*
 * create rule database of specified name
 */
int rldb_create(const char *name, uint32_t flags, struct rldb_db_handle **_db)
{
	uint32_t hash;
	struct rldb_db_handle *db = NULL;
	size_t name_len;
	struct cds_lfht_node *node;
	int id, rc = 0;

	if (!name)
		return -EINVAL;

	if (rldb_disabled) {
		RLDB_ERR("RLDB is not initialized\n");
		return -ENODEV;
	}

	name_len = strnlen(name, RLDB_NAME_MAX);
	if (name_len == RLDB_NAME_MAX || name_len == 0)
		return -EINVAL;

	db = zmalloc_aligned(sizeof(*db));
	if (!db) {
		RLDB_ERR("Could not allocate memory for rldb: \"%s\".\n", name);
		rc = -ENOMEM;
		goto error;
	}

	id = rte_atomic32_add_return(&rldb_counter, 1);
	snprintf(db->name, RLDB_NAME_MAX, "%s-%d", name, id);

	if (flags & NPFRL_FLAG_V4_PFX)
		db->af = AF_INET;
	else if (flags & NPFRL_FLAG_V6_PFX)
		db->af = AF_INET6;
	else {
		rc = -EAFNOSUPPORT;
		goto error;
	}

	db->flags = flags;

	db->ht = cds_lfht_new(RLDB_MIN_BUCKETS,
			      RLDB_MIN_BUCKETS,
			      RLDB_MAX_BUCKETS, CDS_LFHT_AUTO_RESIZE, NULL);

	if (!db->ht) {
		RLDB_ERR("Could not allocate rldb hashtable\n");
		rc = -ENOMEM;
		goto error;
	}

	cds_lfht_node_init(&db->ht_node);

	hash = rldb_name_hash(name, name_len);
	node = cds_lfht_add_unique(rldb_global_ht, hash, rldb_name_match,
				   &db->name, &db->ht_node);
	if (node != &db->ht_node) {
		RLDB_ERR("Could not add rldb: database with the name \"%s\" "
			 "already exists.\n", name);
		rc = -EEXIST;
		goto error;
	}

	rc = npf_rte_acl_init(db->af, db->name, RLDB_MAX_RULES,
			      &db->match_ctx);
	if (rc < 0) {
		RLDB_ERR
		    ("Could not add rldb (%s): NPF rte_acl could not be "
		     "initialized\n", name);
		goto error;
	}

	*_db = db;

	return 0;

error:
	if (db) {
		cds_lfht_del(rldb_global_ht, &db->ht_node);
		rldb_db_handle_destroy(db);
	}

	return rc;
}

/*
 * add rule to the specified database
 */
int rldb_add_rule(struct rldb_db_handle *db __rte_unused,
		  uint32_t rule_no __rte_unused,
		  struct rldb_rule_spec const *in_spec __rte_unused,
		  struct rldb_rule_handle **out_rule __rte_unused)
{
	return 0;
}

/*
 * delete rule from the specified database
 */
int rldb_del_rule(struct rldb_db_handle *db __rte_unused,
		  struct rldb_rule_handle *rule __rte_unused)
{
	return 0;
}

/*
 * find rule by rule number
 */
int rldb_find_rule(struct rldb_db_handle *db __rte_unused,
		   uint32_t rule_no __rte_unused,
		   struct rldb_rule_handle **out_rule __rte_unused)
{
	return 0;
}

/*
 * match packets against rules in the specified database
 */
int rldb_match(struct rldb_db_handle *db __rte_unused,
	       /* array of packets to be matched */
	       struct rte_mbuf *m[] __rte_unused,
	       /* number of packets */
	       uint32_t num_packets __rte_unused,
	       struct rldb_result *results __rte_unused)
{
	return 0;
}

/*
 * get statistics at database level
 */
int rldb_get_stats(struct rldb_db_handle *db __rte_unused,
		   struct rldb_stats *stats __rte_unused)
{
	return 0;
}

/*
 * clear statistics at database level
 */
int rldb_clear_stats(struct rldb_db_handle *db __rte_unused)
{
	return 0;
}

/*
 * walk rule database
 */
void rldb_walk(struct rldb_db_handle *db, rldb_walker_t walker, void *userdata)
{
	struct cds_lfht_iter iter;
	struct rldb_rule_handle *rh;

	if (!db || !walker)
		return;

	if (rldb_disabled) {
		RLDB_ERR("RLDB is not initialized\n");
		return;
	}

	cds_lfht_for_each_entry(db->ht, &iter, rh, ht_node) {
		if (walker(rh, userdata) < 0)
			return;
	}
}

#define PREFIX_STRLEN (INET6_ADDRSTRLEN + sizeof("/128"))

static const char *rldb_prefix_str(uint16_t family, union rldb_pfx *rldb_pfx,
				   char *buf, size_t blen)
{
	char addrbuf[INET6_ADDRSTRLEN];
	const char *addrstr;
	uint32_t count;
	int16_t prefix_len = -1;

	switch (family) {
	case AF_INET:
		addrstr =
		    inet_ntop(family, (void *)&rldb_pfx->v4_pfx.npfrl_bytes[0],
			      addrbuf, sizeof(addrbuf));
		prefix_len = rldb_pfx->v4_pfx.npfrl_plen;
		break;
	case AF_INET6:
		addrstr =
		    inet_ntop(family, (void *)&rldb_pfx->v6_pfx.npfrl_bytes[0],
			      addrbuf, sizeof(addrbuf));
		prefix_len = rldb_pfx->v6_pfx.npfrl_plen;
		break;
	default:
		addrstr = NULL;
	}

	count = snprintf(buf, blen, "%s", addrstr ? : "[bad address]");
	if (prefix_len >= 0)
		snprintf(buf + count, blen - count, "/%d", prefix_len);

	return buf;
}

static const char *rldb_port_range(struct rldb_l4port_range *pr, char *buf,
				   size_t blen)
{
	int rc;

	if (pr->npfrl_loport == pr->npfrl_hiport)
		rc = snprintf(buf, blen, "%u", pr->npfrl_loport);
	else
		rc = snprintf(buf, blen, "%u-%u", pr->npfrl_loport,
			      pr->npfrl_hiport);

	if (rc < 0)
		snprintf(buf, blen, "[bad port-range]");

	return buf;
}

static void rldb_dump_rule_spec(struct rldb_rule_spec *rule, json_writer_t *wr)
{
	char prefix_buf[PREFIX_STRLEN];
	uint16_t af = 0;

	if (rule->rldb_flags & NPFRL_FLAG_V4_PFX)
		af = AF_INET;
	else if (rule->rldb_flags & NPFRL_FLAG_V6_PFX)
		af = AF_INET6;

	jsonw_uint_field(wr, "priority", rule->rldb_priority);
	jsonw_uint_field(wr, "flags", rule->rldb_flags);

	jsonw_string_field(wr, "src_addr",
			   rldb_prefix_str(af, &rule->rldb_src_addr, prefix_buf,
					   sizeof(prefix_buf)));

	jsonw_string_field(wr, "dst_addr",
			   rldb_prefix_str(af, &rule->rldb_dst_addr, prefix_buf,
					   sizeof(prefix_buf)));

	jsonw_uint_field(wr, "proto", rule->rldb_proto.npfrl_proto);

	jsonw_string_field(wr, "sport",
			   rldb_port_range(&rule->rldb_src_port_range,
					   prefix_buf, sizeof(prefix_buf)));
	jsonw_string_field(wr, "dport",
			   rldb_port_range(&rule->rldb_dst_port_range,
					   prefix_buf, sizeof(prefix_buf)));
}

/*
 * dump rule database in json form
 */
void rldb_dump(struct rldb_db_handle *db, json_writer_t *wr)
{
	struct cds_lfht_iter iter;
	struct rldb_rule_handle *rh;
	struct rldb_stats *stats;

	if (!db || !wr)
		return;

	if (rldb_disabled) {
		RLDB_ERR("RLDB is not initialized\n");
		return;
	}

	jsonw_string_field(wr, "name", db->name);
	jsonw_uint_field(wr, "flags", db->flags);

	/* stats */
	stats = &db->stats;

	jsonw_name(wr, "stats");
	jsonw_start_object(wr);
	jsonw_uint_field(wr, "rules_added", stats->rldb_rules_added);
	jsonw_uint_field(wr, "rules_deleted", stats->rldb_rules_deleted);
	jsonw_uint_field(wr, "rule_cnt", stats->rldb_rule_cnt);
	jsonw_uint_field(wr, "transaction_cnt", stats->rldb_transaction_cnt);

	jsonw_name(wr, "error-counters");
	jsonw_start_object(wr);
	jsonw_uint_field(wr, "rule_add_failed",
			 db->stats.rldb_err.rule_add_failed);
	jsonw_uint_field(wr, "rule_del_failed",
			 db->stats.rldb_err.rule_del_failed);
	jsonw_uint_field(wr, "transaction_failed",
			 db->stats.rldb_err.transaction_failed);
	jsonw_end_object(wr);

	jsonw_end_object(wr);

	/* rules */

	jsonw_name(wr, "rules");

	jsonw_start_array(wr);
	cds_lfht_for_each_entry(db->ht, &iter, rh, ht_node) {
		jsonw_start_object(wr);
		jsonw_uint_field(wr, "rule_no", rh->rule_no);
		rldb_dump_rule_spec(&rh->rule, wr);
		jsonw_end_object(wr);
	}
	jsonw_end_array(wr);
}

/*
 * destroy specified rule database
 */
int rldb_destroy(struct rldb_db_handle *db)
{
	if (!db)
		return -EINVAL;

	if (rldb_disabled) {
		RLDB_ERR("RLDB is not initialized\n");
		return -ENODEV;
	}

	cds_lfht_del(rldb_global_ht, &db->ht_node);
	rldb_db_handle_destroy(db);

	return 0;
}

/*
 * clean up infrastructure set up for rule database
 */
int rldb_cleanup(void)
{
	int rc = 0;
	struct cds_lfht_iter iter;
	struct rldb_db_handle *db;

	if (rldb_global_ht) {
		cds_lfht_for_each_entry(rldb_global_ht, &iter, db, ht_node) {
			rldb_destroy(db);
		}

		cds_lfht_destroy(rldb_global_ht, NULL);
	}

	if (rldb_mempool)
		rte_mempool_free(rldb_mempool);

	rldb_mempool = NULL;
	rldb_global_ht = NULL;

	rldb_disabled = true;

	return rc;
}