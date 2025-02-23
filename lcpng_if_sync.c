/* Hey Emacs use -*- mode: C -*- */
/*
 * Copyright 2021 Pim van Pelt <pim@ipng.nl>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/socket.h>
#include <linux/if.h>

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>

#include <vppinfra/linux/netns.h>

#include <plugins/lcpng/lcpng_interface.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>
#include <vpp/app/version.h>
#include <vnet/format_fns.h>
#include <vnet/udp/udp.h>
#include <vnet/tcp/tcp.h>
#include <vnet/devices/tap/tap.h>
#include <vnet/devices/netlink.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/* helper function to copy forward all sw interface link state flags
 * MTU, and IP addresses into their counterpart LIP interface.
 *
 * This is called upon MTU changes and state changes.
 */
void
lcp_itf_pair_sync_state (lcp_itf_pair_t *lip)
{
  vnet_sw_interface_t *sw;
  vnet_sw_interface_t *sup_sw;
  int curr_ns_fd = -1;
  int vif_ns_fd = -1;
  u32 mtu;
  u32 netlink_mtu;

  if (!lcp_lcp_sync ())
    return;

  sw =
    vnet_get_sw_interface_or_null (vnet_get_main (), lip->lip_phy_sw_if_index);
  if (!sw)
    return;
  sup_sw =
    vnet_get_sw_interface_or_null (vnet_get_main (), sw->sup_sw_if_index);

  if (lip->lip_namespace)
    {
      curr_ns_fd = clib_netns_open (NULL /* self */);
      vif_ns_fd = clib_netns_open (lip->lip_namespace);
      if (vif_ns_fd != -1)
	clib_setns (vif_ns_fd);
    }

  LCP_ITF_PAIR_INFO ("sync_state: %U flags %u sup-flags %u mtu %u sup-mtu %u",
		     format_lcp_itf_pair, lip, sw->flags, sup_sw->flags,
		     sw->mtu[VNET_MTU_L3], sup_sw->mtu[VNET_MTU_L3]);

  /* Linux will not allow children to be admin-up if their parent is
   * admin-down. If child is up but parent is not, force it down.
   */
  int state = sw->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP;

  if (state && !(sup_sw->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP))
    {
      LCP_ITF_PAIR_WARN (
	"sync_state: %U flags %u sup-flags %u mtu %u sup-mtu %u: "
	"forcing state to sup-flags to satisfy netlink",
	format_lcp_itf_pair, lip, sw->flags, sup_sw->flags,
	sw->mtu[VNET_MTU_L3], sup_sw->mtu[VNET_MTU_L3]);
      state = sup_sw->flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP;
    }
  lcp_itf_set_link_state (lip, state);

  /* Linux will clamp MTU of children when the parent is lower. VPP is fine
   * with differing MTUs. VPP assumes that if a subint has MTU of 0, that it
   * inherits from its parent. Linux likes to be more explicit, so we
   * reconcile any differences.
   */
  mtu = sw->mtu[VNET_MTU_L3];
  if (mtu == 0)
    mtu = sup_sw->mtu[VNET_MTU_L3];

  if (sup_sw->mtu[VNET_MTU_L3] < sw->mtu[VNET_MTU_L3])
    {
      LCP_ITF_PAIR_WARN ("sync_state: %U flags %u mtu %u sup-mtu %u: "
			 "clamping to sup-mtu to satisfy netlink",
			 format_lcp_itf_pair, lip, sw->flags,
			 sw->mtu[VNET_MTU_L3], sup_sw->mtu[VNET_MTU_L3]);
      mtu = sup_sw->mtu[VNET_MTU_L3];
    }

  /* Set MTU on all of {sw, tap, netlink}. Only send a netlink message if we
   * really do want to change the MTU.
   */
  vnet_sw_interface_set_mtu (vnet_get_main (), lip->lip_phy_sw_if_index, mtu);
  vnet_sw_interface_set_mtu (vnet_get_main (), lip->lip_host_sw_if_index, mtu);
  if (NULL == vnet_netlink_get_link_mtu (lip->lip_vif_index, &netlink_mtu))
    {
      if (netlink_mtu != mtu)
	vnet_netlink_set_link_mtu (lip->lip_vif_index, mtu);
    }

  /* Linux will remove IPv6 addresses on children when the parent state
   * goes down, so we ensure all IPv4/IPv6 addresses are synced.
   */
  lcp_itf_set_interface_addr (lip);

  if (vif_ns_fd != -1)
    close (vif_ns_fd);

  if (curr_ns_fd != -1)
    {
      clib_setns (curr_ns_fd);
      close (curr_ns_fd);
    }

  return;
}

static walk_rc_t
lcp_itf_pair_walk_sync_state_all_cb (index_t lipi, void *ctx)
{
  lcp_itf_pair_t *lip;
  lip = lcp_itf_pair_get (lipi);
  if (!lip)
    return WALK_CONTINUE;

  lcp_itf_pair_sync_state (lip);
  return WALK_CONTINUE;
}

static walk_rc_t
lcp_itf_pair_walk_sync_state_hw_cb (vnet_main_t *vnm, u32 sw_if_index,
				    void *arg)
{
  const vnet_sw_interface_t *sw;
  lcp_itf_pair_t *lip;

  sw = vnet_get_sw_interface (vnm, sw_if_index);
  if (!sw)
    {
      return WALK_CONTINUE;
    }
  lip = lcp_itf_pair_get (lcp_itf_pair_find_by_phy (sw->sw_if_index));
  if (!lip)
    {
      return WALK_CONTINUE;
    }

  lcp_itf_pair_sync_state (lip);
  return WALK_CONTINUE;
}

void
lcp_itf_pair_sync_state_all ()
{
  lcp_itf_pair_walk (lcp_itf_pair_walk_sync_state_all_cb, 0);
}

void
lcp_itf_pair_sync_state_hw (vnet_hw_interface_t *hi)
{
  if (!hi)
    return;
  LCP_ITF_PAIR_DBG ("sync_state_hw: hi %U", format_vnet_sw_if_index_name,
		    vnet_get_main (), hi->hw_if_index);

  vnet_hw_interface_walk_sw (vnet_get_main (), hi->hw_if_index,
			     lcp_itf_pair_walk_sync_state_hw_cb, NULL);
}

static clib_error_t *
lcp_itf_admin_state_change (vnet_main_t * vnm, u32 sw_if_index, u32 flags)
{
  lcp_itf_pair_t *lip;
  vnet_hw_interface_t *hi;
  vnet_sw_interface_t *si;

  if (!lcp_lcp_sync ())
    return 0;

  LCP_ITF_PAIR_DBG ("admin_state_change: sw %U %u",
		  format_vnet_sw_if_index_name, vnm, sw_if_index,
		  flags);

  // Sync interface state changes into host
  lip = lcp_itf_pair_get (lcp_itf_pair_find_by_phy (sw_if_index));
  if (!lip) return NULL;
  LCP_ITF_PAIR_INFO ("admin_state_change: %U flags %u", format_lcp_itf_pair, lip, flags);

  if (vnet_sw_interface_is_sub (vnm, sw_if_index))
    {
      lcp_itf_pair_sync_state (lip);
      return NULL;
    }

  // When Linux changes link on a parent interface, all of its children also
  // change. If a parent interface changes MTU, all of its children are clamped
  // at that MTU by Linux. Neither holds true in VPP, so we are forced to undo
  // change by walking the sub-interfaces of a phy and syncing their state back
  // into Linux.
  si = vnet_get_sw_interface_or_null (vnm, sw_if_index);
  if (!si)
    return NULL;

  hi = vnet_get_hw_interface_or_null (vnm, si->hw_if_index);
  if (!hi)
    return NULL;
  LCP_ITF_PAIR_DBG ("admin_state_change: si %U hi %U, syncing children",
		    format_vnet_sw_if_index_name, vnm, si->sw_if_index,
		    format_vnet_sw_if_index_name, vnm, hi->sw_if_index);

  lcp_itf_pair_sync_state_hw (hi);

  return NULL;
}   

VNET_SW_INTERFACE_ADMIN_UP_DOWN_FUNCTION(lcp_itf_admin_state_change);

static clib_error_t *
lcp_itf_mtu_change (vnet_main_t *vnm, u32 sw_if_index, u32 flags)
{
  vnet_sw_interface_t *si;
  vnet_hw_interface_t *hi;
  if (!lcp_lcp_sync ())
    return NULL;

  LCP_ITF_PAIR_DBG ("mtu_change: sw %U %u", format_vnet_sw_if_index_name, vnm,
		    sw_if_index, flags);

  if (vnet_sw_interface_is_sub (vnm, sw_if_index))
    {
      lcp_itf_pair_t *lip;
      lip = lcp_itf_pair_get (lcp_itf_pair_find_by_phy (sw_if_index));
      if (lip)
	lcp_itf_pair_sync_state (lip);
      return NULL;
    }

  // When Linux changes link on a parent interface, all of its children also
  // change. If a parent interface changes MTU, all of its children are clamped
  // at that MTU by Linux. Neither holds true in VPP, so we are forced to undo
  // change by walking the sub-interfaces of a phy and syncing their state back
  // into Linux.
  si = vnet_get_sw_interface_or_null (vnm, sw_if_index);
  if (!si)
    return NULL;

  hi = vnet_get_hw_interface_or_null (vnm, si->hw_if_index);
  if (!hi)
    return NULL;
  LCP_ITF_PAIR_DBG ("mtu_change: si %U hi %U, syncing children",
		    format_vnet_sw_if_index_name, vnm, si->sw_if_index,
		    format_vnet_sw_if_index_name, vnm, hi->sw_if_index);

  lcp_itf_pair_sync_state_hw (hi);

  return NULL;
}

VNET_SW_INTERFACE_MTU_CHANGE_FUNCTION (lcp_itf_mtu_change);

// TODO(pim): submit upstream to vnet/devices/netlink.[ch]
typedef struct
{
  u8 *data;
} vnet_netlink_msg_t;

static void
vnet_netlink_msg_init (vnet_netlink_msg_t *m, u16 type, u16 flags,
		       void *msg_data, int msg_len)
{
  struct nlmsghdr *nh;
  u8 *p;
  clib_memset (m, 0, sizeof (vnet_netlink_msg_t));
  vec_add2 (m->data, p, NLMSG_SPACE (msg_len));
  ASSERT (m->data == p);

  nh = (struct nlmsghdr *) p;
  nh->nlmsg_flags = flags | NLM_F_ACK;
  nh->nlmsg_type = type;
  clib_memcpy (m->data + sizeof (struct nlmsghdr), msg_data, msg_len);
}

static void
vnet_netlink_msg_add_rtattr (vnet_netlink_msg_t *m, u16 rta_type,
			     void *rta_data, int rta_data_len)
{
  struct rtattr *rta;
  u8 *p;

  vec_add2 (m->data, p, RTA_SPACE (rta_data_len));
  rta = (struct rtattr *) p;
  rta->rta_type = rta_type;
  rta->rta_len = RTA_LENGTH (rta_data_len);
  clib_memcpy (RTA_DATA (rta), rta_data, rta_data_len);
}
static clib_error_t *
vnet_netlink_msg_send (vnet_netlink_msg_t *m, vnet_netlink_msg_t **replies)
{
  clib_error_t *err = 0;
  struct sockaddr_nl ra = { 0 };
  int len, sock;
  struct nlmsghdr *nh = (struct nlmsghdr *) m->data;
  nh->nlmsg_len = vec_len (m->data);
  char buf[4096];

  if ((sock = socket (AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) == -1)
    return clib_error_return_unix (0, "socket(AF_NETLINK)");

  ra.nl_family = AF_NETLINK;
  ra.nl_pid = 0;

  if ((bind (sock, (struct sockaddr *) &ra, sizeof (ra))) == -1)
    {
      err = clib_error_return_unix (0, "bind");
      goto done;
    }

  if ((send (sock, m->data, vec_len (m->data), 0)) == -1)
    err = clib_error_return_unix (0, "send");

  if ((len = recv (sock, buf, sizeof (buf), 0)) == -1)
    err = clib_error_return_unix (0, "recv");
  for (nh = (struct nlmsghdr *) buf; NLMSG_OK (nh, len);
       nh = NLMSG_NEXT (nh, len))
    {
      if (nh->nlmsg_type == NLMSG_DONE)
	goto done;

      if (nh->nlmsg_type == NLMSG_ERROR)
	{
	  struct nlmsgerr *e = (struct nlmsgerr *) NLMSG_DATA (nh);
	  if (e->error)
	    err = clib_error_return (0, "netlink error %d", e->error);
	  goto done;
	}

      if (replies)
	{
	  vnet_netlink_msg_t msg = { NULL };
	  u8 *p;
	  vec_add2 (msg.data, p, nh->nlmsg_len);
	  clib_memcpy (p, nh, nh->nlmsg_len);
	  vec_add1 (*replies, msg);
	}
    }

done:
  close (sock);
  vec_free (m->data);
  return err;
}

clib_error_t *
vnet_netlink_del_ip4_addr (int ifindex, void *addr, int pfx_len)
{
  vnet_netlink_msg_t m;
  struct ifaddrmsg ifa = { 0 };
  clib_error_t *err = 0;

  ifa.ifa_family = AF_INET;
  ifa.ifa_prefixlen = pfx_len;
  ifa.ifa_index = ifindex;

  vnet_netlink_msg_init (&m, RTM_DELADDR, NLM_F_REQUEST, &ifa,
			 sizeof (struct ifaddrmsg));

  vnet_netlink_msg_add_rtattr (&m, IFA_LOCAL, addr, 4);
  vnet_netlink_msg_add_rtattr (&m, IFA_ADDRESS, addr, 4);
  err = vnet_netlink_msg_send (&m, NULL);
  if (err)
    err = clib_error_return (0, "del ip4 addr %U", format_clib_error, err);
  return err;
}

clib_error_t *
vnet_netlink_del_ip6_addr (int ifindex, void *addr, int pfx_len)
{
  vnet_netlink_msg_t m;
  struct ifaddrmsg ifa = { 0 };
  clib_error_t *err = 0;

  ifa.ifa_family = AF_INET6;
  ifa.ifa_prefixlen = pfx_len;
  ifa.ifa_index = ifindex;

  vnet_netlink_msg_init (&m, RTM_DELADDR, NLM_F_REQUEST, &ifa,
			 sizeof (struct ifaddrmsg));

  vnet_netlink_msg_add_rtattr (&m, IFA_LOCAL, addr, 16);
  vnet_netlink_msg_add_rtattr (&m, IFA_ADDRESS, addr, 16);
  err = vnet_netlink_msg_send (&m, NULL);
  if (err)
    err = clib_error_return (0, "del ip6 addr %U", format_clib_error, err);
  return err;
}
// TODO(pim) move previous block upstream

void
lcp_itf_ip4_add_del_interface_addr (ip4_main_t *im, uword opaque,
				    u32 sw_if_index, ip4_address_t *address,
				    u32 address_length, u32 if_address_index,
				    u32 is_del)
{
  const lcp_itf_pair_t *lip;
  int curr_ns_fd = -1;
  int vif_ns_fd = -1;

  if (!lcp_lcp_sync ())
    return;

  LCP_ITF_PAIR_DBG ("ip4_addr_%s: si:%U %U/%u", is_del ? "del" : "add",
		    format_vnet_sw_if_index_name, vnet_get_main (),
		    sw_if_index, format_ip4_address, address, address_length);

  lip = lcp_itf_pair_get (lcp_itf_pair_find_by_phy (sw_if_index));
  if (!lip)
    return;

  if (lip->lip_namespace)
    {
      curr_ns_fd = clib_netns_open (NULL /* self */);
      vif_ns_fd = clib_netns_open (lip->lip_namespace);
      if (vif_ns_fd != -1)
	clib_setns (vif_ns_fd);
    }

  LCP_ITF_PAIR_DBG ("ip4_addr_%s: %U ip4 %U/%u", is_del ? "del" : "add",
		    format_lcp_itf_pair, lip, format_ip4_address, address,
		    address_length);

  if (is_del)
    vnet_netlink_del_ip4_addr (lip->lip_vif_index, address, address_length);
  else
    vnet_netlink_add_ip4_addr (lip->lip_vif_index, address, address_length);

  if (vif_ns_fd != -1)
    close (vif_ns_fd);

  if (curr_ns_fd != -1)
    {
      clib_setns (curr_ns_fd);
      close (curr_ns_fd);
    }
  return;
}

void
lcp_itf_ip6_add_del_interface_addr (ip6_main_t *im, uword opaque,
				    u32 sw_if_index, ip6_address_t *address,
				    u32 address_length, u32 if_address_index,
				    u32 is_del)
{
  const lcp_itf_pair_t *lip;
  int curr_ns_fd = -1;
  int vif_ns_fd = -1;

  if (!lcp_lcp_sync ())
    return;

  LCP_ITF_PAIR_DBG ("ip6_addr_%s: si:%U %U/%u", is_del ? "del" : "add",
		    format_vnet_sw_if_index_name, vnet_get_main (),
		    sw_if_index, format_ip6_address, address, address_length);

  lip = lcp_itf_pair_get (lcp_itf_pair_find_by_phy (sw_if_index));
  if (!lip)
    return;

  if (lip->lip_namespace)
    {
      curr_ns_fd = clib_netns_open (NULL /* self */);
      vif_ns_fd = clib_netns_open (lip->lip_namespace);
      if (vif_ns_fd != -1)
	clib_setns (vif_ns_fd);
    }
  LCP_ITF_PAIR_DBG ("ip6_addr_%s: %U ip4 %U/%u", is_del ? "del" : "add",
		    format_lcp_itf_pair, lip, format_ip6_address, address,
		    address_length);
  if (is_del)
    vnet_netlink_del_ip6_addr (lip->lip_vif_index, address, address_length);
  else
    vnet_netlink_add_ip6_addr (lip->lip_vif_index, address, address_length);

  if (vif_ns_fd != -1)
    close (vif_ns_fd);

  if (curr_ns_fd != -1)
    {
      clib_setns (curr_ns_fd);
      close (curr_ns_fd);
    }
}

static clib_error_t *
lcp_itf_interface_add_del (vnet_main_t *vnm, u32 sw_if_index, u32 is_create)
{
  const vnet_sw_interface_t *sw;
  uword is_sub;

  is_sub = vnet_sw_interface_is_sub (vnm, sw_if_index);
  LCP_ITF_PAIR_DBG ("interface_%s: [%u] sw %U is_sub %u lcp-auto-subint %u",
		    is_create ? "add" : "del", sw_if_index,
		    format_vnet_sw_if_index_name, vnet_get_main (),
		    sw_if_index, is_sub, lcp_lcp_auto_subint ());

  if (!lcp_lcp_auto_subint ())
    return NULL;

  sw = vnet_get_sw_interface_or_null (vnm, sw_if_index);
  if (!sw)
    return NULL;
  if (!is_sub)
    return NULL;

  LCP_ITF_PAIR_DBG ("interface_%s: sw %U parent %U", is_create ? "add" : "del",
		    format_vnet_sw_if_index_name, vnet_get_main (),
		    sw->sw_if_index, format_vnet_sw_if_index_name,
		    vnet_get_main (), sw->sup_sw_if_index);

  if (is_create)
    {
      const lcp_itf_pair_t *sup_lip;
      u8 *name = 0;

      // If the parent has a LIP auto-create a LIP for this interface
      sup_lip =
	lcp_itf_pair_get (lcp_itf_pair_find_by_phy (sw->sup_sw_if_index));
      if (!sup_lip)
	return NULL;

      name = format (name, "%s.%d", sup_lip->lip_host_name, sw->sub.id);

      LCP_ITF_PAIR_INFO (
	"interface_%s: %U has parent %U, auto-creating LCP with host-if %s",
	is_create ? "add" : "del", format_vnet_sw_if_index_name,
	vnet_get_main (), sw->sw_if_index, format_lcp_itf_pair, sup_lip, name);

      lcp_itf_pair_create (sw->sw_if_index, name, LCP_ITF_HOST_TAP,
			   sup_lip->lip_namespace, NULL);

      vec_free (name);
    }
  else
    {
      const lcp_itf_pair_t *lip;

      // If the interface has a LIP, auto-delete it
      lip = lcp_itf_pair_get (lcp_itf_pair_find_by_phy (sw_if_index));
      if (!lip)
	return NULL;
      lcp_itf_pair_delete (sw_if_index);
    }

  return NULL;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (lcp_itf_interface_add_del);
