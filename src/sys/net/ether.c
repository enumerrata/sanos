//
// ether.c
//
// Copyright (c) 2001 Michael Ringgaard. All rights reserved.
//
// Ethernet network interface
//

#include <net/net.h>

static const struct eth_addr ethbroadcast = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
struct queue *ether_queue;

struct ether_msg
{
  struct pbuf *p;
  struct netif *netif;
};

//
// ether_netif_add
//

struct netif *ether_netif_add(char *name, char *devname, struct ip_addr *ipaddr, struct ip_addr *netmask, struct ip_addr *gw)
{
  struct netif *netif;
  devno_t devno;

  // Open device
  devno = dev_open(devname);
  if (devno == NODEV) return NULL;
  if (device(devno)->driver->type != DEV_TYPE_PACKET) return NULL;

  // Attach device to network interface
  netif = netif_add(name, ipaddr, netmask, gw);
  if (!netif) return NULL;

  netif->output = ether_output;
  netif->state = (void *) devno;

  dev_attach(devno, netif, ether_input);

  kprintf("%s: device %s addr ", name, devname);
  ip_addr_debug_print(ipaddr);
  kprintf(" mask ");
  ip_addr_debug_print(netmask);
  kprintf(" gw ");
  ip_addr_debug_print(gw);
  kprintf("\n");
  
  return netif;
}

//
// ether2str
//
// This function converts an ethernet address to string format.
//

char *ether2str(struct eth_addr *hwaddr, char *s)
{
  sprintf(s, "%02x:%02x:%02x:%02x:%02x:%02x",  
          hwaddr->addr[0], hwaddr->addr[1], hwaddr->addr[2], 
	  hwaddr->addr[3], hwaddr->addr[4], hwaddr->addr[5]);
  return s;
}

//
// ether_output
//
// This function is called by the TCP/IP stack when an IP packet
// should be sent.
//

err_t ether_output(struct netif *netif, struct pbuf *p, struct ip_addr *ipaddr)
{
  struct pbuf *q;
  struct eth_hdr *ethhdr;
  struct eth_addr *dest, mcastaddr;
  struct ip_addr *queryaddr;
  err_t err;
  int i;
  
  // Make room for Ethernet header
  if (pbuf_header(p, 14) != 0)
  {
    // The pbuf_header() call shouldn't fail, but we allocate an extra pbuf just in case
    q = pbuf_alloc(PBUF_LINK, 14, PBUF_RW);
    if (q == NULL) 
    {
      stats.link.drop++;
      stats.link.memerr++;

      return -ENOMEM;
    }

    pbuf_chain(q, p);
    p = q;
  }

  // Construct Ethernet header. Start with looking up deciding which
  // MAC address to use as a destination address. Broadcasts and
  // multicasts are special, all other addresses are looked up in the
  // ARP table.

  queryaddr = ipaddr;
  if (ip_addr_isany(ipaddr) || ip_addr_isbroadcast(ipaddr, &(netif->netmask))) 
    dest = (struct eth_addr *) &ethbroadcast;
  else if (ip_addr_ismulticast(ipaddr)) 
  {
    // Hash IP multicast address to MAC address.
    mcastaddr.addr[0] = HTONS(0x01 << 8);
    mcastaddr.addr[1] = HTONS((0x5E << 8) | (ip4_addr2(ipaddr) & 0x7F));
    mcastaddr.addr[2] = HTONS((ip4_addr3(ipaddr) << 8) | ip4_addr4(ipaddr));
    dest = &mcastaddr;
  } 
  else 
  {
    if (ip_addr_maskcmp(ipaddr, &(netif->ip_addr), &(netif->netmask)))
    {
      // Use destination IP address if the destination is on the same subnet as we are.
      queryaddr = ipaddr;
    }
    else
    {
      // Otherwise we use the default router as the address to send the Ethernet frame to.
      queryaddr = &(netif->gw);
    }

    dest = arp_lookup(queryaddr);
  }

  // If the arp_lookup() didn't find an address, we send out an ARP query for the IP address.
  if (dest == NULL) 
  {
    q = arp_query(netif, &netif->hwaddr, queryaddr);
    if (q != NULL) 
    {
      err = dev_transmit((devno_t) netif->state, q);
      pbuf_free(q);
      return err;
    }

    stats.link.drop++;
    stats.link.memerr++;

    return -ENOMEM;
  }
  ethhdr = p->payload;

  for (i = 0; i < 6; i++)
  {
    ethhdr->dest.addr[i] = dest->addr[i];
    ethhdr->src.addr[i] = netif->hwaddr.addr[i];
  }
  ethhdr->type = htons(ETHTYPE_IP);
  
  stats.link.xmit++;
  return dev_transmit((devno_t) netif->state, p);
}

//
// ether_input
//
// This function should be called when a packet is received
// from the interface. 
//

err_t ether_input(struct netif *netif, struct pbuf *p)
{
  struct ether_msg *msg;

  msg = (struct ether_msg *) kmalloc(sizeof(struct ether_msg));
  if (!msg) return -ENOMEM;

  msg->p = p;
  msg->netif = netif;

  if (enqueue(ether_queue, msg, 0) < 0)
  {
    kprintf("drop (queue full)\n");
    kfree(msg);
    pbuf_free(p);
    stats.link.memerr++;
    stats.link.drop++;
    return -EBUF;
  }

  return 0;
}

//
// ether_task
//
// This task dispatches received packets from the network interfaces 
// to the TCP/IP stack. 
//

void ether_dispatcher(void *arg)
{
  struct ether_msg *msg;
  struct pbuf *p;
  struct netif *netif;
  struct eth_hdr *ethhdr;

  while (1)
  {
    msg = dequeue(ether_queue, INFINITE);
    if (!msg) panic("error retrieving message from ethernet packet queue\n");

    p = msg->p;
    netif = msg->netif;
    kfree(msg);

    if (p != NULL) 
    {
      stats.link.recv++;
      ethhdr = p->payload;

      {
	char s[20];
	char d[20];

	kprintf("packet: src=%s dst=%s type=%04X len=%d\n", ether2str(&ethhdr->src, s), ether2str(&ethhdr->dest, d), htons(ethhdr->type), p->tot_len);
      }
    
      switch (htons(ethhdr->type))
      {
	case ETHTYPE_IP:
	  arp_ip_input(netif, p);
	  pbuf_header(p, -14);
	  netif->input(p, netif);
	  break;

	case ETHTYPE_ARP:
	  p = arp_arp_input(netif, &netif->hwaddr, p);
	  if (p != NULL) 
	  {
            dev_transmit((devno_t) netif->state, p);
	    pbuf_free(p);
	  }
	  break;

	default:
	  pbuf_free(p);
	  break;
      }
    }
  }
}

//
// ether_init
//
// Initializes the ethernet packet dispatcher
//

void ether_init()
{
  struct thread *ethertask;

  ether_queue = alloc_queue(256);
  ethertask = create_task(ether_dispatcher, NULL, PRIORITY_NORMAL);
  mark_thread_ready(ethertask);
}
