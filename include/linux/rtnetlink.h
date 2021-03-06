#ifndef __LINUX_RTNETLINK_H
#define __LINUX_RTNETLINK_H


#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <uapi/linux/rtnetlink.h>

extern int rtnetlink_send(struct sk_buff *skb, struct net *net, u32 pid, u32 group, int echo);
extern int rtnl_put_cacheinfo(struct sk_buff *skb, struct dst_entry *dst,
			      u32 id, long expires, u32 error);

#ifdef CONFIG_RTNETLINK
void rtnl_set_sk_err(struct net *net, u32 group, int error);
int rtnetlink_put_metrics(struct sk_buff *skb, u32 *metrics);
void rtnl_notify(struct sk_buff *skb, struct net *net, u32 pid,
			u32 group, struct nlmsghdr *nlh, gfp_t flags);
void rtmsg_ifinfo(int type, struct net_device *dev, unsigned change, gfp_t flags);
int rtnl_unicast(struct sk_buff *skb, struct net *net, u32 pid);
#else
static inline int rtnl_unicast(struct sk_buff *skb, struct net *net, u32 pid)
{ return -EIO; }

static inline void
rtmsg_ifinfo(int type, struct net_device *dev, unsigned change, gfp_t flags) {}

static inline void
rtnl_notify(struct sk_buff *skb, struct net *net, u32 pid,
	    u32 group, struct nlmsghdr *nlh, gfp_t flags) {}

static inline int
rtnetlink_put_metrics(struct sk_buff *skb, u32 *metrics) { return -EINVAL; }

static inline void rtnl_set_sk_err(struct net *net, u32 group, int error) {}
#endif

/* RTNL is used as a global lock for all changes to network configuration  */
extern void rtnl_lock(void);
extern void rtnl_unlock(void);
extern int rtnl_trylock(void);
extern int rtnl_is_locked(void);
#ifdef CONFIG_PROVE_LOCKING
extern int lockdep_rtnl_is_held(void);
#else
static inline int lockdep_rtnl_is_held(void)
{
	return 1;
}
#endif /* #ifdef CONFIG_PROVE_LOCKING */

/**
 * rcu_dereference_rtnl - rcu_dereference with debug checking
 * @p: The pointer to read, prior to dereferencing
 *
 * Do an rcu_dereference(p), but check caller either holds rcu_read_lock()
 * or RTNL. Note : Please prefer rtnl_dereference() or rcu_dereference()
 */
#define rcu_dereference_rtnl(p)					\
	rcu_dereference_check(p, lockdep_rtnl_is_held())

/**
 * rtnl_dereference - fetch RCU pointer when updates are prevented by RTNL
 * @p: The pointer to read, prior to dereferencing
 *
 * Return the value of the specified RCU-protected pointer, but omit
 * both the smp_read_barrier_depends() and the ACCESS_ONCE(), because
 * caller holds RTNL.
 */
#define rtnl_dereference(p)					\
	rcu_dereference_protected(p, lockdep_rtnl_is_held())

static inline struct netdev_queue *dev_ingress_queue(struct net_device *dev)
{
	return rtnl_dereference(dev->ingress_queue);
}

extern struct netdev_queue *dev_ingress_queue_create(struct net_device *dev);

#ifdef CONFIG_RTNETLINK
extern void rtnetlink_init(void);
#else
static inline void rtnetlink_init(void) {}
#endif

extern void __rtnl_unlock(void);

#define ASSERT_RTNL() do { \
	if (unlikely(!rtnl_is_locked())) { \
		printk(KERN_ERR "RTNL: assertion failed at %s (%d)\n", \
		       __FILE__,  __LINE__); \
		dump_stack(); \
	} \
} while(0)

#ifdef CONFIG_RTNETLINK
extern int ndo_dflt_fdb_dump(struct sk_buff *skb,
			     struct netlink_callback *cb,
			     struct net_device *dev,
			     int idx);
extern int ndo_dflt_fdb_add(struct ndmsg *ndm,
			    struct nlattr *tb[],
			    struct net_device *dev,
			    const unsigned char *addr,
			     u16 flags);
extern int ndo_dflt_bridge_getlink(struct sk_buff *skb, u32 pid, u32 seq,
				   struct net_device *dev, u16 mode);
extern int ndo_dflt_fdb_del(struct ndmsg *ndm,
			    struct nlattr *tb[],
			    struct net_device *dev,
			    const unsigned char *addr);

#else
static inline int ndo_dflt_fdb_dump(struct sk_buff *skb,
			     struct netlink_callback *cb,
			     struct net_device *dev,
			     int idx) { return -EINVAL; }
static inline int ndo_dflt_fdb_add(struct ndmsg *ndm,
			    struct nlattr *tb[],
			    struct net_device *dev,
			    const unsigned char *addr,
			     u16 flags) { return -EINVAL; }
static inline int ndo_dflt_bridge_getlink(struct sk_buff *skb, u32 pid, u32 seq,
				   struct net_device *dev, u16 mode)
{ return -EINVAL; }
static inline int ndo_dflt_fdb_del(struct ndmsg *ndm,
			    struct nlattr *tb[],
			    struct net_device *dev,
			    const unsigned char *addr)
{ return -EINVAL; }
#endif	/* !CONFIG_RTNETLINK */
#endif	/* __LINUX_RTNETLINK_H */
