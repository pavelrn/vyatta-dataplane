/*
 * Copyright (c) 2017-2019, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2015-2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * dataplane UT Interface helpers
 */

#ifndef _DP_TEST_LIB_INTF_H_
#define _DP_TEST_LIB_INTF_H_

#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h> /* conflicts with linux/if_bridge.h */
#include <linux/if_bridge.h>

#include "if_var.h"
#include "bridge_port.h"

#include "dp_test_lib.h"
#include "dp_test_json_utils.h"

#define DP_TEST_INTF_DEF_SRC_MAC "00:00:a6:00:00:01"

/* Needs to match IF_PORT_ID_INVALID */
#define DP_TEST_INTF_INVALID_PORT_ID UCHAR_MAX

#define DP_TEST_INTF_NON_DP_PREAMBLE "nondp"
enum dp_test_intf_type_e {
	DP_TEST_INTF_TYPE_DP, /* Normal dataplane interface i.e dpT0 */
	DP_TEST_INTF_TYPE_BRIDGE,
	DP_TEST_INTF_TYPE_VXLAN,
	DP_TEST_INTF_TYPE_GRE,
	DP_TEST_INTF_TYPE_ERSPAN,
	DP_TEST_INTF_TYPE_VTI,
	DP_TEST_INTF_TYPE_NON_DP,
	DP_TEST_INTF_TYPE_LO,
	DP_TEST_INTF_TYPE_MACVLAN,
	DP_TEST_INTF_TYPE_VFP,
	DP_TEST_INTF_TYPE_SWITCH_PORT,
	DP_TEST_INTF_TYPE_PPP,
	DP_TEST_INTF_TYPE_ERROR, /* Oops, error */
};

enum dp_test_tun_encap_type_e {
	DP_TEST_TUN_ENCAP_TYPE_IP, /* Normal dataplane interface i.e dpT0 */
	DP_TEST_TUN_ENCAP_TYPE_BRIDGE,
	DP_TEST_TUN_ENCAP_TYPE_ERSPAN,
};

enum dp_test_intf_type_e dp_test_intf_type(const char *if_name);

void dp_test_intf_dpdk_init(void);
void dp_test_intf_init(void);
int dp_test_intf_virt_add(const char *if_name);
void dp_test_intf_virt_del(const char *if_name);

void dp_test_intf_create_default_set(json_object *intf_set);
void dp_test_reset_expected_ifconfig(void);
uint8_t dp_test_intf_count(void);
uint8_t dp_test_intf_count_local(void);
uint8_t dp_test_intf_clean_count(void);
uint16_t dp_test_intf2default_dpid(const char *if_name);

/* Get interface information */
enum dp_test_intf_loc_e dp_test_intf_loc(const char *if_name);
char *dp_test_intf_real(const char *test_name, char *real_name);
int dp_test_intf_name2index(const char *if_name);
uint8_t dp_test_intf_name2port(const char *if_name);
unsigned int dp_test_cont_src_ifindex(unsigned int ifindex);
struct ether_addr *dp_test_intf_name2mac(const char *if_name);
char *dp_test_intf_name2mac_str(const char *if_name);
void dp_test_intf_name2addr(const char *if_name, struct dp_test_addr *addr);
void dp_test_intf_name2addr_str(const char *if_name, int family,
				char *addr_str, int buf_len);
#define DP_TEST_INTF_STATE_BRIDGE 0x01
#define DP_TEST_INTF_STATE_PBR    0x02
uint8_t dp_test_intf_name2state(const char *if_name);
void dp_test_intf_name_add_state(const char *if_name, uint8_t state);
void dp_test_intf_name_del_state(const char *if_name, uint8_t state);
void _dp_test_intf_name2vrfid(const char *if_name, uint32_t *vrf_id,
			      const char *file, const char *func, int line);
#define dp_test_intf_name2vrfid(if_name, vrf_id) \
	_dp_test_intf_name2vrfid(if_name, vrf_id, \
				 __FILE__, __func__, __LINE__)
struct ether_addr *dp_test_intf_port2mac(portid_t port_id);
void dp_test_intf_port2name(portid_t port_id, char *if_name);
int dp_test_intf_port2index(portid_t port_id);
void dp_test_intf_add_addr(const char *if_name, struct dp_test_addr *addr);
void dp_test_intf_del_addr(const char *if_name, struct dp_test_addr *addr);
bool dp_test_intf_has_addr(const char *if_name,
			   const struct dp_test_addr *addr);
void dp_test_intf_initial_stats_for_if(const char *ifname,
				       struct if_data *stats);
void dp_test_intf_delta_stats_for_if(const char *ifname,
				     const struct if_data *initial_stats,
				     struct if_data *stats);

/* Create / Delete interfaces */
void _dp_test_intf_bridge_create(const char *br_name,
				 const char *file, const char *func,
				 int line);
#define dp_test_intf_bridge_create(br_name) \
	_dp_test_intf_bridge_create(br_name, \
				    __FILE__, __func__, __LINE__)

void _dp_test_intf_bridge_del(const char *br_name,
			      const char *file, const char *func,
			      int line);
#define dp_test_intf_bridge_del(br_name) \
	_dp_test_intf_bridge_del(br_name, \
				 __FILE__, __func__, __LINE__)

void _dp_test_intf_bridge_add_port(const char *br_name, const char *if_name,
				   const char *file, const char *func,
				   int line);
#define dp_test_intf_bridge_add_port(br_name, if_name) \
	_dp_test_intf_bridge_add_port(br_name, if_name, \
				      __FILE__, __func__, __LINE__)

void _dp_test_intf_bridge_remove_port(const char *br_name, const char *if_name,
				      const char *file, const char *func,
				      int line);
#define dp_test_intf_bridge_remove_port(br_name, if_name) \
	_dp_test_intf_bridge_remove_port(br_name, if_name, \
					 __FILE__, __func__, __LINE__)

void _dp_test_intf_bridge_enable_vlan_filter(const char *br_name,
					     const char *file, const char *func,
					     int line);
#define dp_test_intf_bridge_enable_vlan_filter(br_name) \
	_dp_test_intf_bridge_enable_vlan_filter(br_name, \
						__FILE__, __func__, __LINE__)

void _dp_test_intf_bridge_port_set(const char *br_name,
	const char *if_name, uint16_t pvid,
	struct bridge_vlan_set *vlans,
	struct bridge_vlan_set *untag_vlans,
	uint8_t state,
	const char *file, const char *func,
	int line);
#define dp_test_intf_bridge_port_set_vlans(br_name, if_name, pvid, vlans, \
					   untag_vlans) \
	_dp_test_intf_bridge_port_set(br_name, if_name, pvid, vlans, \
				      untag_vlans, BR_STATE_FORWARDING, \
				      __FILE__, __func__, __LINE__)
#define dp_test_intf_bridge_port_set_vlans_state( \
	br_name, if_name, pvid, vlans, untag_vlans, state)	     \
	_dp_test_intf_bridge_port_set(br_name, if_name, pvid, vlans, \
				      untag_vlans, state, \
				      __FILE__, __func__, __LINE__)

#define dp_test_intf_switch_create(switch_name) \
	_dp_test_intf_bridge_create(switch_name, \
				    __FILE__, __func__, __LINE__)

#define dp_test_intf_switch_del(switch_name)	  \
	_dp_test_intf_bridge_del(switch_name, \
				 __FILE__, __func__, __LINE__)

#define dp_test_intf_switch_add_port(switch_name, if_name)	\
	_dp_test_intf_bridge_add_port(switch_name, if_name, \
				      __FILE__, __func__, __LINE__)

#define dp_test_intf_switch_remove_port(switch_name, if_name) \
	_dp_test_intf_bridge_remove_port(switch_name, if_name, \
					 __FILE__, __func__, __LINE__)

void _dp_test_intf_vxlan_create(const char *vxlan_name, uint32_t vni,
				const char *parent_name,
				const char *file, const char *func,
				int line);
#define dp_test_intf_vxlan_create(vxlan_name, vni, parent_name) \
	_dp_test_intf_vxlan_create(vxlan_name, vni, parent_name, \
				   __FILE__, __func__, __LINE__)

void _dp_test_intf_vxlan_del(const char *vxlan_name, uint32_t vni,
			     const char *file, const char *func,
			     int line);
#define dp_test_intf_vxlan_del(vxlan_name, vni) \
	_dp_test_intf_vxlan_del(vxlan_name, vni, \
			       __FILE__, __func__, __LINE__)

void _dp_test_intf_vif_create(const char *vif_name,
			      const char *parent_name, uint16_t vlan,
			      uint16_t vlan_proto, const char *file,
			      const char *func, int line);
#define dp_test_intf_vif_create(vif_name, parent_name, vlan) \
	_dp_test_intf_vif_create(vif_name, parent_name, vlan, ETH_P_8021Q, \
				__FILE__, __func__, __LINE__)

#define dp_test_intf_vif_create_tag_proto(vif_name, parent_name, vlan,	\
					  vlan_proto)			\
	_dp_test_intf_vif_create(vif_name, parent_name, vlan, vlan_proto, \
				 __FILE__, __func__, __LINE__)


void _dp_test_intf_vif_del(const char *vif_name, uint16_t vlan,
			   uint16_t vlan_prot, const char *file,
			   const char *func, int line);
#define dp_test_intf_vif_del(vif_name, vlan) \
	_dp_test_intf_vif_del(vif_name, vlan, ETH_P_8021Q,	\
			      __FILE__, __func__, __LINE__)

#define dp_test_intf_vif_del_tag_proto(vif_name, vlan, vlan_proto)	\
	_dp_test_intf_vif_del(vif_name, vlan, vlan_proto,		\
			      __FILE__, __func__, __LINE__)

void dp_test_intf_vif_create_incmpl(const char *vif_name,
				    const char *parent_name, uint16_t vlan);
void dp_test_intf_vif_create_incmpl_fin(const char *vif_name,
					const char *parent_name,
					uint16_t vlan);

void _dp_test_intf_macvlan_create(const char *if_name,
				  const char *parent_name,
				  const char *mac_str,
				  const char *file, const char *func,
				  int line);
#define dp_test_intf_macvlan_create(if_name, parent_name, mac_str)	\
	_dp_test_intf_macvlan_create(if_name, parent_name, mac_str,	\
				     __FILE__, __func__, __LINE__)

void _dp_test_intf_macvlan_del(const char *if_name, const char *file,
			       const char *func, int line);
#define dp_test_intf_macvlan_del(if_name)			\
	_dp_test_intf_macvlan_del(if_name, __FILE__, __func__,	\
				  __LINE__)

void dp_test_intf_gre_create(const char *gre_name,
			     const char *gre_local, const char *gre_remote,
			     uint32_t gre_key, uint32_t vrf_id);
void dp_test_intf_gre_l2_create(const char *gre_name,
			     const char *gre_local, const char *gre_remote,
			     uint32_t gre_key);
void dp_test_intf_gre_delete(const char *gre_name,
			     const char *gre_local, const char *gre_remote,
			     uint32_t gre_key, uint32_t vrf_id);
void dp_test_intf_gre_l2_delete(const char *gre_name,
				const char *gre_local, const char *gre_remote,
				uint32_t gre_key);
void dp_test_intf_erspan_create(const char *erspan_name,
				const char *erspan_local,
				const char *erspan_remote,
				uint32_t gre_key, bool gre_seq,
				uint32_t vrf_id);
void dp_test_intf_erspan_delete(const char *erspan_name,
				const char *erspan_local,
				const char *erspan_remote,
				uint32_t gre_key, bool gre_seq,
				uint32_t vrf_id);
void dp_test_intf_vti_create(const char *vti_name,
			     const char *vti_local,
			     const char *vti_remote,
			     uint16_t mark,
			     vrfid_t vrf_id);
void dp_test_intf_vti_delete(const char *vti_name,
			     const char *vti_local,
			     const char *vti_remote,
			     uint16_t mark,
			     vrfid_t vrf_id);

void _dp_test_intf_lord_create(const char *name, vrfid_t vrf_id,
			       const char *file, int line);
void _dp_test_intf_lord_delete(const char *name, vrfid_t vrf_id,
			       const char *file, int line);
#define dp_test_intf_lord_create(name, vrf_id)				\
	_dp_test_intf_lord_create(name, vrf_id, __FILE__, __LINE__)
#define dp_test_intf_lord_delete(name, vrf_id)				\
	_dp_test_intf_lord_delete(name, vrf_id, __FILE__, __LINE__)

void _dp_test_intf_vfp_create(const char *name, vrfid_t vrf_id, bool verfiy,
			      const char *file, const char *func, int line);
#define dp_test_intf_vfp_create(name, vrf_id) \
	_dp_test_intf_vfp_create(name, vrf_id, false, \
				 __FILE__, __func__, __LINE__)

void _dp_test_intf_vfp_delete(const char *name, vrfid_t vrf_id,
			     const char *file, const char *func, int line);
#define dp_test_intf_vfp_delete(name, vrfid)	\
	_dp_test_intf_vfp_delete(name, vrfid, __FILE__, __func__, __LINE__)

void _dp_test_intf_loopback_create(const char *name,
				   const char *file, const char *func,
				   int line);
#define dp_test_intf_loopback_create(name) \
	_dp_test_intf_loopback_create(name, __FILE__, __func__, __LINE__)

void _dp_test_intf_loopback_delete(const char *name,
				   const char *file, const char *func,
				   int line);
#define dp_test_intf_loopback_delete(name) \
	_dp_test_intf_loopback_delete(name, __FILE__, __func__, __LINE__)

void dp_test_intf_nondp_create(const char *name);
void dp_test_intf_nondp_create_incmpl(const char *name);
void dp_test_intf_nondp_create_incmpl_fin(const char *name);
void dp_test_intf_nondp_delete(const char *name);

void dp_test_intf_ppp_create(const char *intf_name, uint32_t vrf_id);
void dp_test_intf_ppp_delete(const char *intf_name, uint32_t vrf_id);

uint8_t dp_test_intf_switch_port_count(void);
bool dp_test_intf_switch_port_over_bkp(const char *real_if_name);
void dp_test_intf_switch_port_activate(const char *real_if_name);

void _dp_test_intf_vrf_master_create(const char *name, vrfid_t vrf_id,
				     uint32_t tableid, const char *file,
				     int line);
void _dp_test_intf_vrf_master_delete(const char *name, vrfid_t vrf_id,
				     uint32_t tableid, const char *file,
				     int line);

vrfid_t _dp_test_translate_vrf_id(vrfid_t vrf_id, const char *file,
			       int line);

#define dp_test_translate_vrf_id(vrf_id) \
	_dp_test_translate_vrf_id(vrf_id, __FILE__, __LINE__)

bool
dp_test_upstream_vrf_lookup_db(uint32_t vrf_id, char *vrf_name,
			       uint32_t *tableid);
bool
dp_test_upstream_vrf_add_db(uint32_t vrf_id, char *vrf_name, uint32_t *tableid);

#endif /* _DP_TEST_LIB_INTF_H_ */
