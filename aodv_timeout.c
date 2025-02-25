/*****************************************************************************
 *
 * Copyright (C) 2001 Uppsala University and Ericsson AB.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors: Erik Nordstr�m, <erik.nordstrom@it.uu.se>
 *          
 *
 *****************************************************************************/

#include <time.h>

#ifdef NS_PORT
#include "ns-2/aodv-uu.h"
#else
#include "defs.h"
#include "aodv_timeout.h"
#include "aodv_socket.h"
#include "aodv_neighbor.h"
#include "aodv_rreq.h"
#include "aodv_hello.h"
#include "aodv_rerr.h"
#include "timer_queue.h"
#include "debug.h"
#include "params.h"
#include "routing_table.h"
#include "seek_list.h"
#include "nl.h"

extern int expanding_ring_search, local_repair;
void route_delete_timeout(void *arg);

#endif

/* These are timeout functions which are called when timers expire... */

void NS_CLASS route_discovery_timeout(void *arg)
{
	struct timeval now;
	seek_list_t *seek_entry;
	rt_table_t *rt, *repair_rt;
	seek_entry = (seek_list_t *) arg;

#define TTL_VALUE seek_entry->ttl

	/* Sanity check... */
	if (!seek_entry)
		return;

	gettimeofday(&now, NULL);

	DEBUG(LOG_DEBUG, 0, "%s", ip_to_str(seek_entry->dest_addr));

	if (seek_entry->reqs < RREQ_RETRIES) {

		if (expanding_ring_search) {

			if (TTL_VALUE < TTL_THRESHOLD)//当ttl小于门槛值
				TTL_VALUE += TTL_INCREMENT;//增大ttl
			else {
				TTL_VALUE = NET_DIAMETER;
				seek_entry->reqs++;
			}
			/* Set a new timer for seeking this destination *///设置一个新的计时器来寻找这个目的地
			timer_set_timeout(&seek_entry->seek_timer,
					  RING_TRAVERSAL_TIME);
		} else {
			seek_entry->reqs++;
			timer_set_timeout(&seek_entry->seek_timer,
					  seek_entry->reqs * 2 *
					  NET_TRAVERSAL_TIME);
		}
		/* AODV should use a binary exponential backoff RREP waiting
		   time. *///AODV应该使用二进制指数回退RREP等待时间。
		DEBUG(LOG_DEBUG, 0, "Seeking %s ttl=%d wait=%d",
		      ip_to_str(seek_entry->dest_addr),
		      TTL_VALUE, 2 * TTL_VALUE * NODE_TRAVERSAL_TIME);

		/* A routing table entry waiting for a RREP should not be expunged
		   before 2 * NET_TRAVERSAL_TIME... *///等待RREP的路由表条目不应该在2 * net_遍历时间之前被删除…
		rt = rt_table_find(seek_entry->dest_addr);

		if (rt && timeval_diff(&rt->rt_timer.timeout, &now) <
		    (2 * NET_TRAVERSAL_TIME))
			rt_table_update_timeout(rt, 2 * NET_TRAVERSAL_TIME);

		rreq_send(seek_entry->dest_addr, seek_entry->dest_seqno,
			  TTL_VALUE, seek_entry->flags);

	} else {

		DEBUG(LOG_DEBUG, 0, "NO ROUTE FOUND!");

#ifdef NS_PORT
		packet_queue_set_verdict(seek_entry->dest_addr, PQ_DROP);
#else
		nl_send_no_route_found_msg(seek_entry->dest_addr);
#endif
		repair_rt = rt_table_find(seek_entry->dest_addr);

		seek_list_remove(seek_entry);

		/* If this route has been in repair, then we should timeout
		   the route at this point. *///如果此路由已在修复中，那么我们应该在此时暂停该路由。
		if (repair_rt && (repair_rt->flags & RT_REPAIR)) {
			DEBUG(LOG_DEBUG, 0, "REPAIR for %s failed!",
			      ip_to_str(repair_rt->dest_addr));
			local_repair_timeout(repair_rt);
		}
	}
}

void NS_CLASS local_repair_timeout(void *arg)
{
	rt_table_t *rt;
	struct in_addr rerr_dest;
	RERR *rerr = NULL;

	rt = (rt_table_t *) arg;

	if (!rt)
		return;

	rerr_dest.s_addr = AODV_BROADCAST;	/* Default destination *///缺省目的时，广播搜索

	/* Unset the REPAIR flag *///取消设置修复标志
	rt->flags &= ~RT_REPAIR;

#ifndef NS_PORT
	nl_send_del_route_msg(rt->dest_addr, rt->next_hop, rt->hcnt);
#endif
	/* Route should already be invalidated. *///路由应该已经无效。

	if (rt->nprec) {

		rerr = rerr_create(0, rt->dest_addr, rt->dest_seqno);//创建rrer

		if (rt->nprec == 1) {
			rerr_dest = FIRST_PREC(rt->precursors)->neighbor;

			aodv_socket_send((AODV_msg *) rerr, rerr_dest,
					 RERR_CALC_SIZE(rerr), 1,
					 &DEV_IFINDEX(rt->ifindex));
		} else {
			int i;

			for (i = 0; i < MAX_NR_INTERFACES; i++) {
				if (!DEV_NR(i).enabled)
					continue;
				aodv_socket_send((AODV_msg *) rerr, rerr_dest,
						 RERR_CALC_SIZE(rerr), 1,
						 &DEV_NR(i));
			}
		}
		DEBUG(LOG_DEBUG, 0, "Sending RERR about %s to %s",
		      ip_to_str(rt->dest_addr), ip_to_str(rerr_dest));
	}
	precursor_list_destroy(rt);

	/* Purge any packets that may be queued *///清除任何可能排队的包
	/* packet_queue_set_verdict(rt->dest_addr, PQ_DROP); */

	rt->rt_timer.handler = &NS_CLASS route_delete_timeout;
	timer_set_timeout(&rt->rt_timer, DELETE_PERIOD);

	DEBUG(LOG_DEBUG, 0, "%s removed in %u msecs",
	      ip_to_str(rt->dest_addr), DELETE_PERIOD);
}


void NS_CLASS route_expire_timeout(void *arg)
{
	rt_table_t *rt;

	rt = (rt_table_t *) arg;

	if (!rt) {
		alog(LOG_WARNING, 0, __FUNCTION__,
		     "arg was NULL, ignoring timeout!");
		return;
	}

	DEBUG(LOG_DEBUG, 0, "Route %s DOWN, seqno=%d",
	      ip_to_str(rt->dest_addr), rt->dest_seqno);

	if (rt->hcnt == 1)
		neighbor_link_break(rt);
	else {
		rt_table_invalidate(rt);
		precursor_list_destroy(rt);
	}

	return;
}

void NS_CLASS route_delete_timeout(void *arg)
{
	rt_table_t *rt;

	rt = (rt_table_t *) arg;

	/* Sanity check: */
	if (!rt)
		return;

	DEBUG(LOG_DEBUG, 0, "%s", ip_to_str(rt->dest_addr));

	rt_table_delete(rt);
}

/* This is called when we stop receiveing hello messages from a
   node. For now this is basically the same as a route timeout. *///当我们停止接收来自节点的hello消息时，将调用它。目前，这基本上与路由超时相同。
void NS_CLASS hello_timeout(void *arg)
{
	rt_table_t *rt;
	struct timeval now;

	rt = (rt_table_t *) arg;

	if (!rt)
		return;

	gettimeofday(&now, NULL);

	DEBUG(LOG_DEBUG, 0, "LINK/HELLO FAILURE %s last HELLO: %d",
	      ip_to_str(rt->dest_addr), timeval_diff(&now,
						     &rt->last_hello_time));

	if (rt && rt->state == VALID && !(rt->flags & RT_UNIDIR)) {

		/* If the we can repair the route, then mark it to be
		   repaired.. */
		if (local_repair && rt->hcnt <= MAX_REPAIR_TTL) {
			rt->flags |= RT_REPAIR;
			DEBUG(LOG_DEBUG, 0, "Marking %s for REPAIR",
			      ip_to_str(rt->dest_addr));
#ifdef NS_PORT
			/* Buffer pending packets from interface queue */
			interfaceQueue((nsaddr_t) rt->dest_addr.s_addr,
				       IFQ_BUFFER);
#endif
		}
		neighbor_link_break(rt);
	}
}

void NS_CLASS rrep_ack_timeout(void *arg)
{
	rt_table_t *rt;

	/* We must be really sure here, that this entry really exists at
	   this point... (Though it should). *///我们必须非常确定，这个条目确实存在于这一点上……(尽管它应该)。
	rt = (rt_table_t *) arg;

	if (!rt)
		return;

	/* When a RREP transmission fails (i.e. lack of RREP-ACK), add to
	   blacklist set... *///当RREP传输失败(即缺乏RREP- ack)，添加到黑名单集…
	rreq_blacklist_insert(rt->dest_addr);

	DEBUG(LOG_DEBUG, 0, "%s", ip_to_str(rt->dest_addr));
}

void NS_CLASS wait_on_reboot_timeout(void *arg)
{
	*((int *) arg) = 0;

	DEBUG(LOG_DEBUG, 0, "Wait on reboot over!!");
}

#ifdef NS_PORT
void NS_CLASS packet_queue_timeout(void *arg)
{
	packet_queue_garbage_collect();
	timer_set_timeout(&PQ.garbage_collect_timer, GARBAGE_COLLECT_TIME);
}
#endif
