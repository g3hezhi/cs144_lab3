/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"


/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
                     uint8_t * packet/* lent */,
                     unsigned int len,
                     char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("\n*** -> Received packet of length %d \n",len);

  if (len <  sizeof(sr_ethernet_hdr_t)) {
    fprintf(stderr, "***** -> Failed to process ETHERNET header, insufficient length\n");
    return;
  }
    
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet; 
    
  uint16_t ether_type = ntohs(eth_hdr->ether_type);
  struct sr_if *interface_detail = sr_get_interface(sr, interface);
    
  if (ether_type == ethertype_arp) 
  {
      printf("***** -> Going to sr_handle_ARP_packet \n");
      sr_handle_arp_packet(sr, packet, len, interface);

  } else if (ether_type == ethertype_ip) {

      printf("***** -> Going to sr_handle_IP_packet \n");
      sr_handle_ip_packet(sr, packet, len, interface_detail);
  }

}/* end sr_ForwardPacket */


void sr_handle_ip_packet(struct sr_instance *sr, 
                         uint8_t *packet,
                         unsigned int len,
                         struct sr_if *interface){

  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_handle_ip_packet() ====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_if *dest_interface = sr_get_interface_byIP(sr,ihdr->ip_dst);

  unsigned int check_len1 = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
  unsigned int check_len2 = sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl*4);
  uint16_t sum = ihdr->ip_sum;
  ihdr->ip_sum = 0;
  uint16_t ck_sum = cksum(ihdr,ihdr->ip_hl*4);
  ihdr->ip_sum = sum;

  if(len < check_len1 || len < check_len2){
    printf("*** -> ERROR!!!! -> not enough length or check sum not mach\n");
  }
  if (sum != ck_sum){
   printf("ERROR!!!!!!!!!!!!!!!!!!!!!1, check sum not match");}
  /* find address , directly forward*/
  if(!dest_interface){
    printf("***** -> Find dest_interface address, Going to sr_ip_forward \n");
    sr_ip_forward(sr,packet,len);
  
  /*if address is not found*/
  } else {
    printf("***** -> IP address not found, checking for ICMP \n");
    
    if(ihdr->ip_p == ip_protocol_icmp)
    {
      printf("****** -> It's a ICMP message \n");
      sr_icmp_handler(sr,packet,len);

    } else if (ihdr->ip_p == 0x0006 || ihdr->ip_p == 0x0001){
      /* icmp type   unreachable = 3
         icmp code = unreachable = 3  */
      printf("****** -> IP address not found and not a ICMP msg, Preparing icmp 3 3 \n");
      sr_send_icmp(sr,packet, len, 3, 3);
    }
  }
}/* end sr_handle_ip packets*/



void sr_ip_forward(struct sr_instance *sr, 
                   uint8_t *packet, 
                   unsigned int len){

  assert(sr);
  assert(packet);
  printf("\n==== sr_ip_forward ====\n");

  sr_ip_hdr_t *ihdr =(sr_ip_hdr_t *) (sizeof(sr_ethernet_hdr_t) + packet);
  ihdr->ip_ttl--;
  ihdr->ip_sum = 0;
  ihdr->ip_sum = cksum(ihdr,ihdr->ip_hl*4);
  struct sr_rt *lpm = sr_lpm(sr, ihdr->ip_dst);

  if(ihdr->ip_ttl == 0){
    /* 
    icmp type : time excceded = 11
    icmp code : time exceeded_ttl = 0
    */
    printf("**** -> IP packet TTL == 0 \n");
    sr_send_icmp(sr,packet,len, 11, 0);
    return;
  }
  
  if (!lpm)
  {
    /*
    icmp type : unreachable = 3
    icmp code : unreachable-net = 0
    */
    printf("**** -> IP packet not LMP, preparing ICMP 3 0 Destination Net unreachable\n");
    sr_send_icmp(sr, packet, len, 3, 0);
    return;
  }

  printf("**** -> Sending ip Packet L:185\n");
  struct sr_if *out_interface =  sr_get_interface(sr, lpm->interface);
  sr_sending(sr, packet, len, out_interface, lpm->gw.s_addr);

} /* end sr_ip_forward */


void sr_send_icmp(struct sr_instance *sr,
                  uint8_t *packet,
                  unsigned int len,
                  uint8_t type,
                  uint8_t code)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_send_icmp() ====\n");

  sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet; 
  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_rt *lpm = sr_lpm(sr,ihdr->ip_src);
  struct sr_if *out_interface = sr_get_interface(sr,lpm->interface);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t)+ sizeof(ihdr->ip_hl *4) + packet);
  /*
    handle ICMP according to following type and code. 
    Type 
    unreachable     3
    time exceed     11
    echo            0

    code
    unreachable_host = 1
    unreachable_net = 0
    unreachable_port = 3
    time_exceed = 0
    echo reply = 0 */

  /* type = unreachable*/
  if(type == 3){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));
    /* icmp code = unrachable_port = 3*/
    if(code == 3){
      new_ihdr->ip_src = ihdr->ip_dst;
    }else{
      new_ihdr->ip_src = out_interface->ip;
    }

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /*type = time exceed*/
  }else if (type == 11){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_src = out_interface->ip;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /* echo reply*/
  }else if (type == 0){

    /*update ip hearder*/
    uint32_t ip_dst = ihdr->ip_src;
    ihdr->ip_src = ihdr->ip_dst;
    ihdr->ip_dst = ip_dst;

    /*update icmp header*/

    ichdr->icmp_type = 0;
    ichdr->icmp_code = 0;
    ichdr->icmp_sum = 0;
    ichdr->icmp_sum = cksum(ichdr,ntohs(ihdr->ip_len)-(ihdr->ip_hl*4));
    /*update ethernet header*/
    memset(ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(ehdr->ether_dhost,0,ETHER_ADDR_LEN);  

    sr_sending(sr,packet,len,out_interface,lpm->gw.s_addr);
  }
}



void sr_icmp_handler(struct sr_instance *sr,
                     uint8_t *packet,
                     unsigned int len)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_icmp_handler() =====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t)+packet);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl * 4) + packet);
  unsigned int check_len = sizeof(sr_icmp_hdr_t) + (ihdr->ip_hl*4) + sizeof(sr_ethernet_hdr_t);
  uint16_t sum = ichdr->icmp_sum;
  ichdr->icmp_sum = 0;
  uint16_t check_sum = cksum(ichdr,ntohs(ihdr->ip_len) - (ihdr->ip_hl*4));
  ichdr->icmp_sum = sum;

  if( len < check_len || sum != check_sum){
    fprintf(stderr,"**** -> Faill to produce ICMP header , not enough length or check sum not mach \n");
  }

  /* when type is echo request = 8 , and code is echo request = 0*/
  if (ichdr->icmp_type == 8 && ichdr->icmp_code == 0){
    printf("**** -> type is echo request = 8 , and code is echo request = 0\n");
    /* send echo replay type = 0 , echo reply code = 0*/
    sr_send_icmp(sr,packet,len,0,0);
  }
}/* end sr_icmp_handler */


void sr_sending(struct sr_instance *sr,
                uint8_t *packet,
                unsigned int len,
                struct sr_if *interface,
                uint32_t ip){
  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_sending() ==== \n");

  struct sr_arpentry *arp = sr_arpcache_lookup(&(sr->cache),ip);

  if(arp){
    printf("**** -> IP->MAC mapping is in the cache. L:369\n");
    sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet;

    memcpy(ehdr->ether_dhost,arp->mac,ETHER_ADDR_LEN);
    memcpy(ehdr->ether_shost,interface->addr,ETHER_ADDR_LEN);

    sr_send_packet(sr,packet,len,interface->name);

  }else{
    printf("**** -> IP->MAC mapping NOT in the cache. L:378\n");
    struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache),ip,packet,len,interface->name);
    sr_handle_arpreq(sr,request);

  }
}/* end sr_sending */


void sr_handle_arp_packet(struct sr_instance *sr, 
                          uint8_t *packet,
                          unsigned int len,
                          char *receiving_interface)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(receiving_interface);
    printf("\n==== handle_arp_packet() ====\n");

    /*Check packet length*/
    if (len <  sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t))
    {
      fprintf(stderr, "**** -> ERROR: Incorrect packet length\n");
      return;
    }

    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)( packet + sizeof(sr_ethernet_hdr_t));
    struct sr_if *receive_interface = sr_get_interface(sr, receiving_interface);
    struct sr_if *sender_interface  = sr_get_interface_byIP(sr, arp_hdr->ar_tip); 

    /*Check interface whether in router's IP address*/
    if (!receive_interface)
    {
      fprintf(stderr, "**** -> ERROR: Invalid interface\n");
      return;
    }

    /* Get arp_opcode: request or replay to me*/
    if (ntohs(arp_hdr->ar_op) == arp_op_request){           /* Request to me, send a reply*/
        printf("***** -> this is a arp request, preparing a reply L:419\n");
        sr_handle_arp_send_reply_to_requester(sr, packet, receive_interface, sender_interface);
  
    } else if (ntohs(arp_hdr->ar_op) == arp_op_reply){    /* Reply to me, cache it */
     
        printf("***** -> This is a REPLY to me, CACHE it L:422 \n");
        sr_handle_arp_cache_reply(sr, packet, receive_interface);
    } 
}/* end sr_handle_arp_packet */


void sr_handle_arp_cache_reply(struct sr_instance *sr,
                               uint8_t *packet,
                               struct sr_if *interface_info)
{
    printf("\n==== sr_handle_arp_cache_reply() ==== \n");
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Cache it */
    struct sr_arpreq *requests = sr_arpcache_insert(&(sr->cache), arp_hdr->ar_sha, arp_hdr->ar_sip); 

    printf("*** -> Go through my request queue for this IP and send outstanding packets if there are any \n");
    /* Go through my request queue for this IP and send outstanding packets if there are any*/
    if(requests)
    {
      struct sr_packet *pkts         = NULL;
      struct sr_if *dest_if          = NULL;
      sr_ethernet_hdr_t *pkt_eth_hdr = NULL; 
      
      pkts = requests->packets;
      while(pkts)
      {
	printf("**** -> Iterating request queue\n");
        pkt_eth_hdr = (sr_ethernet_hdr_t *)(pkts->buf);
        dest_if = sr_get_interface(sr, pkts->iface);

        /* source and desti mac addresss switched*/
        memcpy(pkt_eth_hdr->ether_shost, dest_if->addr, ETHER_ADDR_LEN);
        memcpy(pkt_eth_hdr->ether_dhost, arp_hdr->ar_sha, ETHER_ADDR_LEN);
        sr_send_packet(sr, pkts->buf, pkts->len, pkts->iface);
        pkts = pkts->next;
      }

      sr_arpreq_destroy(&(sr->cache), requests);
    }

}/* end sr_handle_arp_send_reply */


void sr_handle_arp_send_reply_to_requester(struct sr_instance *sr,
                                           uint8_t *packet,
                                           struct sr_if *receive_interface,
                                           struct sr_if *sender_interface)
{ 
    printf("\n==== sr_handle_arp_send_reply_to_requester() =====\n");
    printf("**** -> Preparing packet....\n");  
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Consttruct a ARP reply*/
    unsigned int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t); 
    uint8_t *reply = (uint8_t *) malloc(packet_len); 
    sr_ethernet_hdr_t *new_ether_hdr = (sr_ethernet_hdr_t *) reply;
    sr_arp_hdr_t *new_arp_hdr = (sr_arp_hdr_t *)(reply + sizeof(sr_ethernet_hdr_t));

    /* Construct the ethernet header */
    new_ether_hdr->ether_type = eth_hdr->ether_type;
    memcpy(new_ether_hdr->ether_dhost, eth_hdr->ether_shost, ETHER_ADDR_LEN);
    memcpy(new_ether_hdr->ether_shost, receive_interface->addr,ETHER_ADDR_LEN);

    /* Construct the ARP header */
    new_arp_hdr->ar_hrd = arp_hdr->ar_hrd;      /* format of hardware address   */
    new_arp_hdr->ar_pro = arp_hdr->ar_pro;      /* format of protocol address   */
    new_arp_hdr->ar_hln = arp_hdr->ar_hln;     /* length of hardware address   */
    new_arp_hdr->ar_pln = arp_hdr->ar_pln;      /* length of protocol address   */
    new_arp_hdr->ar_op  = htons(arp_op_reply);  /* ARP opcode (command)         */
    new_arp_hdr->ar_sip = sender_interface->ip;   /* Sender IP address            */
    new_arp_hdr->ar_tip = arp_hdr->ar_sip;      /* Target IP address            */
    memcpy(new_arp_hdr->ar_sha, receive_interface->addr, ETHER_ADDR_LEN); /* sender hardware address      */
    memcpy(new_arp_hdr->ar_tha, arp_hdr->ar_sha, ETHER_ADDR_LEN);  /* target hardware address      */

    /* ARP replies are sent directly to the requester?s MAC address*/
    printf("***** -> Finsihed packeting, going sr_sr_send_packet()\n");
    sr_send_packet(sr, reply, packet_len, sender_interface->name);
    free(reply);
} /* end sr_handle_arp_manage_reply */


/* HELPERS */
struct sr_if *sr_get_interface_byIP(struct sr_instance *sr,
                                    uint32_t ip){
  struct sr_if *if_walker = 0;
  assert(sr);

  if_walker = sr->if_list;
  while(if_walker){/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"


/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
                     uint8_t * packet/* lent */,
                     unsigned int len,
                     char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("\n*** -> Received packet of length %d \n",len);

  if (len <  sizeof(sr_ethernet_hdr_t)) {
    fprintf(stderr, "***** -> Failed to process ETHERNET header, insufficient length\n");
    return;
  }
    
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet; 
    
  uint16_t ether_type = ntohs(eth_hdr->ether_type);
  struct sr_if *interface_detail = sr_get_interface(sr, interface);
    
  if (ether_type == ethertype_arp) 
  {
      printf("***** -> Going to sr_handle_ARP_packet \n");
      sr_handle_arp_packet(sr, packet, len, interface);

  } else if (ether_type == ethertype_ip) {

      printf("***** -> Going to sr_handle_IP_packet \n");
      sr_handle_ip_packet(sr, packet, len, interface_detail);
  }

}/* end sr_ForwardPacket */


void sr_handle_ip_packet(struct sr_instance *sr, 
                         uint8_t *packet,
                         unsigned int len,
                         struct sr_if *interface){

  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_handle_ip_packet() ====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_if *dest_interface = sr_get_interface_byIP(sr,ihdr->ip_dst);

  unsigned int check_len1 = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
  unsigned int check_len2 = sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl*4);
  uint16_t sum = ihdr->ip_sum;
  ihdr->ip_sum = 0;
  uint16_t ck_sum = cksum(ihdr,ihdr->ip_hl*4);
  ihdr->ip_sum = sum;

  if(len < check_len1 || len < check_len2){
    printf("*** -> ERROR!!!! -> not enough length or check sum not mach\n");
  }
  if (sum != ck_sum){
   printf("ERROR!!!!!!!!!!!!!!!!!!!!!1, check sum not match");}
  /* find address , directly forward*/
  if(!dest_interface){
    printf("***** -> Find dest_interface address, Going to sr_ip_forward \n");
    sr_ip_forward(sr,packet,len);
  
  /*if address is not found*/
  } else {
    printf("***** -> IP address not found, checking for ICMP \n");
    
    if(ihdr->ip_p == ip_protocol_icmp)
    {
      printf("****** -> It's a ICMP message \n");
      sr_icmp_handler(sr,packet,len);

    } else if (ihdr->ip_p == 0x0006 || ihdr->ip_p == 0x0001){
      /* icmp type   unreachable = 3
         icmp code = unreachable = 3  */
      printf("****** -> IP address not found and not a ICMP msg, Preparing icmp 3 3 \n");
      sr_send_icmp(sr,packet, len, 3, 3);
    }
  }
}/* end sr_handle_ip packets*/



void sr_ip_forward(struct sr_instance *sr, 
                   uint8_t *packet, 
                   unsigned int len){

  assert(sr);
  assert(packet);
  printf("\n==== sr_ip_forward ====\n");

  sr_ip_hdr_t *ihdr =(sr_ip_hdr_t *) (sizeof(sr_ethernet_hdr_t) + packet);
  ihdr->ip_ttl--;
  ihdr->ip_sum = 0;
  ihdr->ip_sum = cksum(ihdr,ihdr->ip_hl*4);
  struct sr_rt *lpm = sr_lpm(sr, ihdr->ip_dst);

  if(ihdr->ip_ttl == 0){
    /* 
    icmp type : time excceded = 11
    icmp code : time exceeded_ttl = 0
    */
    printf("**** -> IP packet TTL == 0 \n");
    sr_send_icmp(sr,packet,len, 11, 0);
    return;
  }
  
  if (!lpm)
  {
    /*
    icmp type : unreachable = 3
    icmp code : unreachable-net = 0
    */
    printf("**** -> IP packet not LMP, preparing ICMP 3 0 Destination Net unreachable\n");
    sr_send_icmp(sr, packet, len, 3, 0);
    return;
  }

  printf("**** -> Sending ip Packet L:185\n");
  struct sr_if *out_interface =  sr_get_interface(sr, lpm->interface);
  sr_sending(sr, packet, len, out_interface, lpm->gw.s_addr);

} /* end sr_ip_forward */


void sr_send_icmp(struct sr_instance *sr,
                  uint8_t *packet,
                  unsigned int len,
                  uint8_t type,
                  uint8_t code)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_send_icmp() ====\n");

  sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet; 
  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_rt *lpm = sr_lpm(sr,ihdr->ip_src);
  struct sr_if *out_interface = sr_get_interface(sr,lpm->interface);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t)+ sizeof(ihdr->ip_hl *4) + packet);
  /*
    handle ICMP according to following type and code. 
    Type 
    unreachable     3
    time exceed     11
    echo            0

    code
    unreachable_host = 1
    unreachable_net = 0
    unreachable_port = 3
    time_exceed = 0
    echo reply = 0 */

  /* type = unreachable*/
  if(type == 3){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));
    /* icmp code = unrachable_port = 3*/
    if(code == 3){
      new_ihdr->ip_src = ihdr->ip_dst;
    }else{
      new_ihdr->ip_src = out_interface->ip;
    }

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /*type = time exceed*/
  }else if (type == 11){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_src = out_interface->ip;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /* echo reply*/
  }else if (type == 0){

    /*update ip hearder*/
    uint32_t ip_dst = ihdr->ip_src;
    ihdr->ip_src = ihdr->ip_dst;
    ihdr->ip_dst = ip_dst;

    /*update icmp header*/

    ichdr->icmp_type = 0;
    ichdr->icmp_code = 0;
    ichdr->icmp_sum = 0;
    ichdr->icmp_sum = cksum(ichdr,ntohs(ihdr->ip_len)-(ihdr->ip_hl*4));
    /*update ethernet header*/
    memset(ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(ehdr->ether_dhost,0,ETHER_ADDR_LEN);  

    sr_sending(sr,packet,len,out_interface,lpm->gw.s_addr);
  }
}



void sr_icmp_handler(struct sr_instance *sr,
                     uint8_t *packet,
                     unsigned int len)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_icmp_handler() =====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t)+packet);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl * 4) + packet);
  unsigned int check_len = sizeof(sr_icmp_hdr_t) + (ihdr->ip_hl*4) + sizeof(sr_ethernet_hdr_t);
  uint16_t sum = ichdr->icmp_sum;
  ichdr->icmp_sum = 0;
  uint16_t check_sum = cksum(ichdr,ntohs(ihdr->ip_len) - (ihdr->ip_hl*4));
  ichdr->icmp_sum = sum;

  if( len < check_len || sum != check_sum){
    fprintf(stderr,"**** -> Faill to produce ICMP header , not enough length or check sum not mach \n");
  }

  /* when type is echo request = 8 , and code is echo request = 0*/
  if (ichdr->icmp_type == 8 && ichdr->icmp_code == 0){
    printf("**** -> type is echo request = 8 , and code is echo request = 0\n");
    /* send echo replay type = 0 , echo reply code = 0*/
    sr_send_icmp(sr,packet,len,0,0);
  }
}/* end sr_icmp_handler */


void sr_sending(struct sr_instance *sr,
                uint8_t *packet,
                unsigned int len,
                struct sr_if *interface,
                uint32_t ip){
  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_sending() ==== \n");

  struct sr_arpentry *arp = sr_arpcache_lookup(&(sr->cache),ip);

  if(arp){
    printf("**** -> IP->MAC mapping is in the cache. L:369\n");
    sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet;

    memcpy(ehdr->ether_dhost,arp->mac,ETHER_ADDR_LEN);
    memcpy(ehdr->ether_shost,interface->addr,ETHER_ADDR_LEN);

    sr_send_packet(sr,packet,len,interface->name);

  }else{
    printf("**** -> IP->MAC mapping NOT in the cache. L:378\n");
    struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache),ip,packet,len,interface->name);
    sr_handle_arpreq(sr,request);

  }
}/* end sr_sending */


void sr_handle_arp_packet(struct sr_instance *sr, 
                          uint8_t *packet,
                          unsigned int len,
                          char *receiving_interface)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(receiving_interface);
    printf("\n==== handle_arp_packet() ====\n");

    /*Check packet length*/
    if (len <  sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t))
    {
      fprintf(stderr, "**** -> ERROR: Incorrect packet length\n");
      return;
    }

    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)( packet + sizeof(sr_ethernet_hdr_t));
    struct sr_if *receive_interface = sr_get_interface(sr, receiving_interface);
    struct sr_if *sender_interface  = sr_get_interface_byIP(sr, arp_hdr->ar_tip); 

    /*Check interface whether in router's IP address*/
    if (!receive_interface)
    {
      fprintf(stderr, "**** -> ERROR: Invalid inter/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"


/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
                     uint8_t * packet/* lent */,
                     unsigned int len,
                     char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("\n*** -> Received packet of length %d \n",len);

  if (len <  sizeof(sr_ethernet_hdr_t)) {
    fprintf(stderr, "***** -> Failed to process ETHERNET header, insufficient length\n");
    return;
  }
    
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet; 
    
  uint16_t ether_type = ntohs(eth_hdr->ether_type);
  struct sr_if *interface_detail = sr_get_interface(sr, interface);
    
  if (ether_type == ethertype_arp) 
  {
      printf("***** -> Going to sr_handle_ARP_packet \n");
      sr_handle_arp_packet(sr, packet, len, interface);

  } else if (ether_type == ethertype_ip) {

      printf("***** -> Going to sr_handle_IP_packet \n");
      sr_handle_ip_packet(sr, packet, len, interface_detail);
  }

}/* end sr_ForwardPacket */


void sr_handle_ip_packet(struct sr_instance *sr, 
                         uint8_t *packet,
                         unsigned int len,
                         struct sr_if *interface){

  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_handle_ip_packet() ====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_if *dest_interface = sr_get_interface_byIP(sr,ihdr->ip_dst);

  unsigned int check_len1 = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
  unsigned int check_len2 = sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl*4);
  uint16_t sum = ihdr->ip_sum;
  ihdr->ip_sum = 0;
  uint16_t ck_sum = cksum(ihdr,ihdr->ip_hl*4);
  ihdr->ip_sum = sum;

  if(len < check_len1 || len < check_len2){
    printf("*** -> ERROR!!!! -> not enough length or check sum not mach\n");
  }
  if (sum != ck_sum){
   printf("ERROR!!!!!!!!!!!!!!!!!!!!!1, check sum not match");}
  /* find address , directly forward*/
  if(!dest_interface){
    printf("***** -> Find dest_interface address, Going to sr_ip_forward \n");
    sr_ip_forward(sr,packet,len);
  
  /*if address is not found*/
  } else {
    printf("***** -> IP address not found, checking for ICMP \n");
    
    if(ihdr->ip_p == ip_protocol_icmp)
    {
      printf("****** -> It's a ICMP message \n");
      sr_icmp_handler(sr,packet,len);

    } else if (ihdr->ip_p == 0x0006 || ihdr->ip_p == 0x0001){
      /* icmp type   unreachable = 3
         icmp code = unreachable = 3  */
      printf("****** -> IP address not found and not a ICMP msg, Preparing icmp 3 3 \n");
      sr_send_icmp(sr,packet, len, 3, 3);
    }
  }
}/* end sr_handle_ip packets*/



void sr_ip_forward(struct sr_instance *sr, 
                   uint8_t *packet, 
                   unsigned int len){

  assert(sr);
  assert(packet);
  printf("\n==== sr_ip_forward ====\n");

  sr_ip_hdr_t *ihdr =(sr_ip_hdr_t *) (sizeof(sr_ethernet_hdr_t) + packet);
  ihdr->ip_ttl--;
  ihdr->ip_sum = 0;
  ihdr->ip_sum = cksum(ihdr,ihdr->ip_hl*4);
  struct sr_rt *lpm = sr_lpm(sr, ihdr->ip_dst);

  if(ihdr->ip_ttl == 0){
    /* 
    icmp type : time excceded = 11
    icmp code : time exceeded_ttl = 0
    */
    printf("**** -> IP packet TTL == 0 \n");
    sr_send_icmp(sr,packet,len, 11, 0);
    return;
  }
  
  if (!lpm)
  {
    /*
    icmp type : unreachable = 3
    icmp code : unreachable-net = 0
    */
    printf("**** -> IP packet not LMP, preparing ICMP 3 0 Destination Net unreachable\n");
    sr_send_icmp(sr, packet, len, 3, 0);
    return;
  }

  printf("**** -> Sending ip Packet L:185\n");
  struct sr_if *out_interface =  sr_get_interface(sr, lpm->interface);
  sr_sending(sr, packet, len, out_interface, lpm->gw.s_addr);

} /* end sr_ip_forward */


void sr_send_icmp(struct sr_instance *sr,
                  uint8_t *packet,
                  unsigned int len,
                  uint8_t type,
                  uint8_t code)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_send_icmp() ====\n");

  sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet; 
  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_rt *lpm = sr_lpm(sr,ihdr->ip_src);
  struct sr_if *out_interface = sr_get_interface(sr,lpm->interface);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t)+ sizeof(ihdr->ip_hl *4) + packet);
  /*
    handle ICMP according to following type and code. 
    Type 
    unreachable     3
    time exceed     11
    echo            0

    code
    unreachable_host = 1
    unreachable_net = 0
    unreachable_port = 3
    time_exceed = 0
    echo reply = 0 */

  /* type = unreachable*/
  if(type == 3){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));
    /* icmp code = unrachable_port = 3*/
    if(code == 3){
      new_ihdr->ip_src = ihdr->ip_dst;
    }else{
      new_ihdr->ip_src = out_interface->ip;
    }

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /*type = time exceed*/
  }else if (type == 11){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_src = out_interface->ip;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /* echo reply*/
  }else if (type == 0){

    /*update ip hearder*/
    uint32_t ip_dst = ihdr->ip_src;
    ihdr->ip_src = ihdr->ip_dst;
    ihdr->ip_dst = ip_dst;

    /*update icmp header*/

    ichdr->icmp_type = 0;
    ichdr->icmp_code = 0;
    ichdr->icmp_sum = 0;
    ichdr->icmp_sum = cksum(ichdr,ntohs(ihdr->ip_len)-(ihdr->ip_hl*4));
    /*update ethernet header*/
    memset(ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(ehdr->ether_dhost,0,ETHER_ADDR_LEN);  

    sr_sending(sr,packet,len,out_interface,lpm->gw.s_addr);
  }
}



void sr_icmp_handler(struct sr_instance *sr,
                     uint8_t *packet,
                     unsigned int len)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_icmp_handler() =====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t)+packet);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl * 4) + packet);
  unsigned int check_len = sizeof(sr_icmp_hdr_t) + (ihdr->ip_hl*4) + sizeof(sr_ethernet_hdr_t);
  uint16_t sum = ichdr->icmp_sum;
  ichdr->icmp_sum = 0;
  uint16_t check_sum = cksum(ichdr,ntohs(ihdr->ip_len) - (ihdr->ip_hl*4));
  ichdr->icmp_sum = sum;

  if( len < check_len || sum != check_sum){
    fprintf(stderr,"**** -> Faill to produce ICMP header , not enough length or check sum not mach \n");
  }

  /* when type is echo request = 8 , and code is echo request = 0*/
  if (ichdr->icmp_type == 8 && ichdr->icmp_code == 0){
    printf("**** -> type is echo request = 8 , and code is echo request = 0\n");
    /* send echo replay type = 0 , echo reply code = 0*/
    sr_send_icmp(sr,packet,len,0,0);
  }
}/* end sr_icmp_handler */


void sr_sending(struct sr_instance *sr,
                uint8_t *packet,
                unsigned int len,
                struct sr_if *interface,
                uint32_t ip){
  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_sending() ==== \n");

  struct sr_arpentry *arp = sr_arpcache_lookup(&(sr->cache),ip);

  if(arp){
    printf("**** -> IP->MAC mapping is in the cache. L:369\n");
    sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet;

    memcpy(ehdr->ether_dhost,arp->mac,ETHER_ADDR_LEN);
    memcpy(ehdr->ether_shost,interface->addr,ETHER_ADDR_LEN);

    sr_send_packet(sr,packet,len,interface->name);

  }else{
    printf("**** -> IP->MAC mapping NOT in the cache. L:378\n");
    struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache),ip,packet,len,interface->name);
    sr_handle_arpreq(sr,request);

  }
}/* end sr_sending */


void sr_handle_arp_packet(struct sr_instance *sr, 
                          uint8_t *packet,
                          unsigned int len,
                          char *receiving_interface)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(receiving_interface);
    printf("\n==== handle_arp_packet() ====\n");

    /*Check packet length*/
    if (len <  sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t))
    {
      fprintf(stderr, "**** -> ERROR: Incorrect packet length\n");
      return;
    }

    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)( packet + sizeof(sr_ethernet_hdr_t));
    struct sr_if *receive_interface = sr_get_interface(sr, receiving_interface);
    struct sr_if *sender_interface  = sr_get_interface_byIP(sr, arp_hdr->ar_tip); 

    /*Check interface whether in router's IP address*/
    if (!receive_interface)
    {
      fprintf(stderr, "**** -> ERROR: Invalid interface\n");
      return;
    }
/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"


/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
                     uint8_t * packet/* lent */,
                     unsigned int len,
                     char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("\n*** -> Received packet of length %d \n",len);

  if (len <  sizeof(sr_ethernet_hdr_t)) {
    fprintf(stderr, "***** -> Failed to process ETHERNET header, insufficient length\n");
    return;
  }
    
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet; 
    
  uint16_t ether_type = ntohs(eth_hdr->ether_type);
  struct sr_if *interface_detail = sr_get_interface(sr, interface);
    
  if (ether_type == ethertype_arp) 
  {
      printf("***** -> Going to sr_handle_ARP_packet \n");
      sr_handle_arp_packet(sr, packet, len, interface);

  } else if (ether_type == ethertype_ip) {

      printf("***** -> Going to sr_handle_IP_packet \n");
      sr_handle_ip_packet(sr, packet, len, interface_detail);
  }

}/* end sr_ForwardPacket */


void sr_handle_ip_packet(struct sr_instance *sr, 
                         uint8_t *packet,
                         unsigned int len,
                         struct sr_if *interface){

  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_handle_ip_packet() ====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_if *dest_interface = sr_get_interface_byIP(sr,ihdr->ip_dst);

  unsigned int check_len1 = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
  unsigned int check_len2 = sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl*4);
  uint16_t sum = ihdr->ip_sum;
  ihdr->ip_sum = 0;
  uint16_t ck_sum = cksum(ihdr,ihdr->ip_hl*4);
  ihdr->ip_sum = sum;

  if(len < check_len1 || len < check_len2){
    printf("*** -> ERROR!!!! -> not enough length or check sum not mach\n");
  }
  if (sum != ck_sum){
   printf("ERROR!!!!!!!!!!!!!!!!!!!!!1, check sum not match");}
  /* find address , directly forward*/
  if(!dest_interface){
    printf("***** -> Find dest_interface address, Going to sr_ip_forward \n");
    sr_ip_forward(sr,packet,len);
  
  /*if address is not found*/
  } else {
    printf("***** -> IP address not found, checking for ICMP \n");
    
    if(ihdr->ip_p == ip_protocol_icmp)
    {
      printf("****** -> It's a ICMP message \n");
      sr_icmp_handler(sr,packet,len);

    } else if (ihdr->ip_p == 0x0006 || ihdr->ip_p == 0x0001){
      /* icmp type   unreachable = 3
         icmp code = unreachable = 3  */
      printf("****** -> IP address not found and not a ICMP msg, Preparing icmp 3 3 \n");
      sr_send_icmp(sr,packet, len, 3, 3);
    }
  }
}/* end sr_handle_ip packets*/



void sr_ip_forward(struct sr_instance *sr, 
                   uint8_t *packet, 
                   unsigned int len){

  assert(sr);
  assert(packet);
  printf("\n==== sr_ip_forward ====\n");

  sr_ip_hdr_t *ihdr =(sr_ip_hdr_t *) (sizeof(sr_ethernet_hdr_t) + packet);
  ihdr->ip_ttl--;
  ihdr->ip_sum = 0;
  ihdr->ip_sum = cksum(ihdr,ihdr->ip_hl*4);
  struct sr_rt *lpm = sr_lpm(sr, ihdr->ip_dst);

  if(ihdr->ip_ttl == 0){
    /* 
    icmp type : time excceded = 11
    icmp code : time exceeded_ttl = 0
    */
    printf("**** -> IP packet TTL == 0 \n");
    sr_send_icmp(sr,packet,len, 11, 0);
    return;
  }
  
  if (!lpm)
  {
    /*
    icmp type : unreachable = 3
    icmp code : unreachable-net = 0
    */
    printf("**** -> IP packet not LMP, preparing ICMP 3 0 Destination Net unreachable\n");
    sr_send_icmp(sr, packet, len, 3, 0);
    return;
  }

  printf("**** -> Sending ip Packet L:185\n");
  struct sr_if *out_interface =  sr_get_interface(sr, lpm->interface);
  sr_sending(sr, packet, len, out_interface, lpm->gw.s_addr);

} /* end sr_ip_forward */


void sr_send_icmp(struct sr_instance *sr,
                  uint8_t *packet,
                  unsigned int len,
                  uint8_t type,
                  uint8_t code)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_send_icmp() ====\n");

  sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet; 
  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_rt *lpm = sr_lpm(sr,ihdr->ip_src);
  struct sr_if *out_interface = sr_get_interface(sr,lpm->interface);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t)+ sizeof(ihdr->ip_hl *4) + packet);
  /*
    handle ICMP according to following type and code. 
    Type 
    unreachable     3
    time exceed     11
    echo            0

    code
    unreachable_host = 1
    unreachable_net = 0
    unreachable_port = 3
    time_exceed = 0
    echo reply = 0 */

  /* type = unreachable*/
  if(type == 3){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));
    /* icmp code = unrachable_port = 3*/
    if(code == 3){
      new_ihdr->ip_src = ihdr->ip_dst;
    }else{
      new_ihdr->ip_src = out_interface->ip;
    }

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /*type = time exceed*/
  }else if (type == 11){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_src = out_interface->ip;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /* echo reply*/
  }else if (type == 0){

    /*update ip hearder*/
    uint32_t ip_dst = ihdr->ip_src;
    ihdr->ip_src = ihdr->ip_dst;
    ihdr->ip_dst = ip_dst;

    /*update icmp header*/

    ichdr->icmp_type = 0;
    ichdr->icmp_code = 0;
    ichdr->icmp_sum = 0;
    ichdr->icmp_sum = cksum(ichdr,ntohs(ihdr->ip_len)-(ihdr->ip_hl*4));
    /*update ethernet header*/
    memset(ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(ehdr->ether_dhost,0,ETHER_ADDR_LEN);  

    sr_sending(sr,packet,len,out_interface,lpm->gw.s_addr);
  }
}



void sr_icmp_handler(struct sr_instance *sr,
                     uint8_t *packet,
                     unsigned int len)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_icmp_handler() =====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t)+packet);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl * 4) + packet);
  unsigned int check_len = sizeof(sr_icmp_hdr_t) + (ihdr->ip_hl*4) + sizeof(sr_ethernet_hdr_t);
  uint16_t sum = ichdr->icmp_sum;
  ichdr->icmp_sum = 0;
  uint16_t check_sum = cksum(ichdr,ntohs(ihdr->ip_len) - (ihdr->ip_hl*4));
  ichdr->icmp_sum = sum;

  if( len < check_len || sum != check_sum){
    fprintf(stderr,"**** -> Faill to produce ICMP header , not enough length or check sum not mach \n");
  }

  /* when type is echo request = 8 , and code is echo request = 0*/
  if (ichdr->icmp_type == 8 && ichdr->icmp_code == 0){
    printf("**** -> type is echo request = 8 , and code is echo request = 0\n");
    /* send echo replay type = 0 , echo reply code = 0*/
    sr_send_icmp(sr,packet,len,0,0);
  }
}/* end sr_icmp_handler */


void sr_sending(struct sr_instance *sr,
                uint8_t *packet,
                unsigned int len,
                struct sr_if *interface,
                uint32_t ip){
  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_sending() ==== \n");

  struct sr_arpentry *arp = sr_arpcache_lookup(&(sr->cache),ip);

  if(arp){
    printf("**** -> IP->MAC mapping is in the cache. L:369\n");
    sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet;

    memcpy(ehdr->ether_dhost,arp->mac,ETHER_ADDR_LEN);
    memcpy(ehdr->ether_shost,interface->addr,ETHER_ADDR_LEN);

    sr_send_packet(sr,packet,len,interface->name);

  }else{
    printf("**** -> IP->MAC mapping NOT in the cache. L:378\n");
    struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache),ip,packet,len,interface->name);
    sr_handle_arpreq(sr,request);

  }
}/* end sr_sending */


void sr_handle_arp_packet(struct sr_instance *sr, 
                          uint8_t *packet,
                          unsigned int len,
                          char *receiving_interface)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(receiving_interface);
    printf("\n==== handle_arp_packet() ====\n");

    /*Check packet length*/
    if (len <  sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t))
    {
      fprintf(stderr, "**** -> ERROR: Incorrect packet length\n");
      return;
    }

    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)( packet + sizeof(sr_ethernet_hdr_t));
    struct sr_if *receive_interface = sr_get_interface(sr, receiving_interface);
    struct sr_if *sender_interface  = sr_get_interface_byIP(sr, arp_hdr->ar_tip); 

    /*Check interface whether in router's IP address*/
    if (!receive_interface)
    {
      fprintf(stderr, "**** -> ERROR: Invalid interface\n");
      return;
    }

    /* Get arp_opcode: request or replay to me*/
    if (ntohs(arp_hdr->ar_op) == arp_op_request){           /* Request to me, send a reply*/
        printf("***** -> this is a arp request, preparing a reply L:419\n");
        sr_handle_arp_send_reply_to_requester(sr, packet, receive_interface, sender_interface);
  
    } else if (ntohs(arp_hdr->ar_op) == arp_op_reply){    /* Reply to me, cache it */
     
        printf("***** -> This is a REPLY to me, CACHE it L:422 \n");
        sr_handle_arp_cache_reply(sr, packet, receive_interface);
    } 
}/* end sr_handle_arp_packet */


void sr_handle_arp_cache_reply(struct sr_instance *sr,
                               uint8_t *packet,
                               struct sr_if *interface_info)
{
    printf("\n==== sr_handle_arp_cache_reply() ==== \n");
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Cache it */
    struct sr_arpreq *requests = sr_arpcache_insert(&(sr->cache), arp_hdr->ar_sha, arp_hdr->ar_sip); 

    printf("*** -> Go through my request queue for this IP and send outstanding packets if there are any \n");
    /* Go through my request queue for this IP and send outstanding packets if there are any*/
    if(requests)
    {
      struct sr_packet *pkts         = NULL;
      struct sr_if *dest_if          = NULL;
      sr_ethernet_hdr_t *pkt_eth_hdr = NULL; 
      
      pkts = requests->packets;
      while(pkts)
      {
	printf("**** -> Iterating request queue\n");
        pkt_eth_hdr = (sr_ethernet_hdr_t *)(pkts->buf);
        dest_if = sr_get_interface(sr, pkts->iface);

        /* source and desti mac addresss switched*/
        memcpy(pkt_eth_hdr->ether_shost, dest_if->addr, ETHER_ADDR_LEN);
        memcpy(pkt_eth_hdr->ether_dhost, arp_hdr->ar_sha, ETHER_ADDR_LEN);
        sr_send_packet(sr, pkts->buf, pkts->len, pkts->iface);
        pkts = pkts->next;
      }

      sr_arpreq_destroy(&(sr->cache), requests);
    }

}/* end sr_handle_arp_send_reply */


void sr_handle_arp_send_reply_to_requester(struct sr_instance *sr,
                                           uint8_t *packet,
                                           struct sr_if *receive_interface,
                                           struct sr_if *sender_interface)
{ 
    printf("\n==== sr_handle_arp_send_reply_to_requester() =====\n");
    printf("**** -> Preparing packet....\n");  
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Consttruct a ARP reply*/
    unsigned int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t); 
    uint8_t *reply = (uint8_t *) malloc(packet_len); 
    sr_ethernet_hdr_t *new_ether_hdr = (sr_ethernet_hdr_t *) reply;
    sr_arp_hdr_t *new_arp_hdr = (sr_arp_hdr_t *)(reply + sizeof(sr_ethernet_hdr_t));

    /* Construct the ethernet header */
    new_ether_hdr->ether_type = eth_hdr->ether_type;
    memcpy(new_ether_hdr->ether_dhost, eth_hdr->ether_shost, ETHER_ADDR_LEN);
    memcpy(new_ether_hdr->ether_shost, receive_interface->addr,ETHER_ADDR_LEN);

    /* Construct the ARP header */
    new_arp_hdr->ar_hrd = arp_hdr->ar_hrd;      /* format of hardware address   */
    new_arp_hdr->ar_pro = arp_hdr->ar_pro;      /* format of protocol address   */
    new_arp_hdr->ar_hln = arp_hdr->ar_hln;     /* length of hardware address   */
    new_arp_hdr->ar_pln = arp_hdr->ar_pln;      /* length of protocol address   */
    new_arp_hdr->ar_op  = htons(arp_op_reply);  /* ARP opcode (command)         */
    new_arp_hdr->ar_sip = sender_interface->ip;   /* Sender IP address            */
    new_arp_hdr->ar_tip = arp_hdr->ar_sip;      /* Target IP address            */
    memcpy(new_arp_hdr->ar_sha, receive_interface->addr, ETHER_ADDR_LEN); /* sender hardware address      */
    memcpy(new_arp_hdr->ar_tha, arp_hdr->ar_sha, ETHER_ADDR_LEN);  /* target hardware address      */

    /* ARP replies are sent directly to the requester?s MAC address*/
    printf("***** -> Finsihed packeting, going sr_sr_send_packet()\n");
    sr_send_packet(sr, reply, packet_len, sender_interface->name);
    free(reply);
} /* end sr_handle_arp_manage_reply */


/* HELPERS */
struct sr_if *sr_get_interface_byIP(struct sr_insta/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"


/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
                     uint8_t * packet/* lent */,
                     unsigned int len,
                     char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("\n*** -> Received packet of length %d \n",len);

  if (len <  sizeof(sr_ethernet_hdr_t)) {
    fprintf(stderr, "***** -> Failed to process ETHERNET header, insufficient length\n");
    return;
  }
    
  sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet; 
    
  uint16_t ether_type = ntohs(eth_hdr->ether_type);
  struct sr_if *interface_detail = sr_get_interface(sr, interface);
    
  if (ether_type == ethertype_arp) 
  {
      printf("***** -> Going to sr_handle_ARP_packet \n");
      sr_handle_arp_packet(sr, packet, len, interface);

  } else if (ether_type == ethertype_ip) {

      printf("***** -> Going to sr_handle_IP_packet \n");
      sr_handle_ip_packet(sr, packet, len, interface_detail);
  }

}/* end sr_ForwardPacket */


void sr_handle_ip_packet(struct sr_instance *sr, 
                         uint8_t *packet,
                         unsigned int len,
                         struct sr_if *interface){

  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_handle_ip_packet() ====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_if *dest_interface = sr_get_interface_byIP(sr,ihdr->ip_dst);

  unsigned int check_len1 = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t);
  unsigned int check_len2 = sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl*4);
  uint16_t sum = ihdr->ip_sum;
  ihdr->ip_sum = 0;
  uint16_t ck_sum = cksum(ihdr,ihdr->ip_hl*4);
  ihdr->ip_sum = sum;

  if(len < check_len1 || len < check_len2){
    printf("*** -> ERROR!!!! -> not enough length or check sum not mach\n");
  }
  if (sum != ck_sum){
   printf("ERROR!!!!!!!!!!!!!!!!!!!!!1, check sum not match");}
  /* find address , directly forward*/
  if(!dest_interface){
    printf("***** -> Find dest_interface address, Going to sr_ip_forward \n");
    sr_ip_forward(sr,packet,len);
  
  /*if address is not found*/
  } else {
    printf("***** -> IP address not found, checking for ICMP \n");
    
    if(ihdr->ip_p == ip_protocol_icmp)
    {
      printf("****** -> It's a ICMP message \n");
      sr_icmp_handler(sr,packet,len);

    } else if (ihdr->ip_p == 0x0006 || ihdr->ip_p == 0x0001){
      /* icmp type   unreachable = 3
         icmp code = unreachable = 3  */
      printf("****** -> IP address not found and not a ICMP msg, Preparing icmp 3 3 \n");
      sr_send_icmp(sr,packet, len, 3, 3);
    }
  }
}/* end sr_handle_ip packets*/



void sr_ip_forward(struct sr_instance *sr, 
                   uint8_t *packet, 
                   unsigned int len){

  assert(sr);
  assert(packet);
  printf("\n==== sr_ip_forward ====\n");

  sr_ip_hdr_t *ihdr =(sr_ip_hdr_t *) (sizeof(sr_ethernet_hdr_t) + packet);
  ihdr->ip_ttl--;
  ihdr->ip_sum = 0;
  ihdr->ip_sum = cksum(ihdr,ihdr->ip_hl*4);
  struct sr_rt *lpm = sr_lpm(sr, ihdr->ip_dst);

  if(ihdr->ip_ttl == 0){
    /* 
    icmp type : time excceded = 11
    icmp code : time exceeded_ttl = 0
    */
    printf("**** -> IP packet TTL == 0 \n");
    sr_send_icmp(sr,packet,len, 11, 0);
    return;
  }
  
  if (!lpm)
  {
    /*
    icmp type : unreachable = 3
    icmp code : unreachable-net = 0
    */
    printf("**** -> IP packet not LMP, preparing ICMP 3 0 Destination Net unreachable\n");
    sr_send_icmp(sr, packet, len, 3, 0);
    return;
  }

  printf("**** -> Sending ip Packet L:185\n");
  struct sr_if *out_interface =  sr_get_interface(sr, lpm->interface);
  sr_sending(sr, packet, len, out_interface, lpm->gw.s_addr);

} /* end sr_ip_forward */


void sr_send_icmp(struct sr_instance *sr,
                  uint8_t *packet,
                  unsigned int len,
                  uint8_t type,
                  uint8_t code)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_send_icmp() ====\n");

  sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet; 
  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + packet);
  struct sr_rt *lpm = sr_lpm(sr,ihdr->ip_src);
  struct sr_if *out_interface = sr_get_interface(sr,lpm->interface);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t)+ sizeof(ihdr->ip_hl *4) + packet);
  /*
    handle ICMP according to following type and code. 
    Type 
    unreachable     3
    time exceed     11
    echo            0

    code
    unreachable_host = 1
    unreachable_net = 0
    unreachable_port = 3
    time_exceed = 0
    echo reply = 0 */

  /* type = unreachable*/
  if(type == 3){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));
    /* icmp code = unrachable_port = 3*/
    if(code == 3){
      new_ihdr->ip_src = ihdr->ip_dst;
    }else{
      new_ihdr->ip_src = out_interface->ip;
    }

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /*type = time exceed*/
  }else if (type == 11){
    uint8_t *data = (uint8_t *)malloc(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
    assert(data);

    sr_ip_hdr_t *new_ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + data);
    sr_ethernet_hdr_t *new_ehdr = (sr_ethernet_hdr_t *)data;
    sr_icmp_t3_hdr_t *new_ichdr = (sr_icmp_t3_hdr_t *)(sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + data);

    /*crate ip header*/
    new_ihdr->ip_p = ip_protocol_icmp;
    new_ihdr->ip_hl = sizeof(sr_ip_hdr_t)/4;
    new_ihdr->ip_tos = 0;
    new_ihdr->ip_len = htons(sizeof(sr_icmp_t3_hdr_t) + sizeof(sr_ip_hdr_t));
    new_ihdr->ip_id = htons(0);
    new_ihdr->ip_off = htons(IP_DF);
    new_ihdr->ip_ttl = 64;
    new_ihdr->ip_v = 4;
    new_ihdr->ip_dst = ihdr->ip_src;
    new_ihdr->ip_src = out_interface->ip;
    new_ihdr->ip_sum = 0;
    new_ihdr->ip_sum = cksum(new_ihdr,sizeof(sr_ip_hdr_t));

    /*create icmp header*/
    new_ichdr->icmp_type = type;
    new_ichdr->icmp_code = code;
    new_ichdr->unused = 0;
    new_ichdr->next_mtu = 0;
    new_ichdr->icmp_sum = 0;
    new_ichdr->icmp_sum = cksum(new_ichdr,sizeof(sr_icmp_t3_hdr_t));
    memcpy(new_ichdr->data,ihdr,ICMP_DATA_SIZE);

    /*create ethernet header*/
    new_ehdr->ether_type = htons(ethertype_ip);
    memset(new_ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(new_ehdr->ether_dhost,0,ETHER_ADDR_LEN);

    uint32_t new_length = sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    sr_sending(sr,data,new_length,out_interface,lpm->gw.s_addr);
    free(data);

    /* echo reply*/
  }else if (type == 0){

    /*update ip hearder*/
    uint32_t ip_dst = ihdr->ip_src;
    ihdr->ip_src = ihdr->ip_dst;
    ihdr->ip_dst = ip_dst;

    /*update icmp header*/

    ichdr->icmp_type = 0;
    ichdr->icmp_code = 0;
    ichdr->icmp_sum = 0;
    ichdr->icmp_sum = cksum(ichdr,ntohs(ihdr->ip_len)-(ihdr->ip_hl*4));
    /*update ethernet header*/
    memset(ehdr->ether_shost,0,ETHER_ADDR_LEN);
    memset(ehdr->ether_dhost,0,ETHER_ADDR_LEN);  

    sr_sending(sr,packet,len,out_interface,lpm->gw.s_addr);
  }
}



void sr_icmp_handler(struct sr_instance *sr,
                     uint8_t *packet,
                     unsigned int len)
{
  assert(sr);
  assert(packet);
  printf("\n==== sr_icmp_handler() =====\n");

  sr_ip_hdr_t *ihdr = (sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t)+packet);
  sr_icmp_hdr_t *ichdr = (sr_icmp_hdr_t *)(sizeof(sr_ethernet_hdr_t) + (ihdr->ip_hl * 4) + packet);
  unsigned int check_len = sizeof(sr_icmp_hdr_t) + (ihdr->ip_hl*4) + sizeof(sr_ethernet_hdr_t);
  uint16_t sum = ichdr->icmp_sum;
  ichdr->icmp_sum = 0;
  uint16_t check_sum = cksum(ichdr,ntohs(ihdr->ip_len) - (ihdr->ip_hl*4));
  ichdr->icmp_sum = sum;

  if( len < check_len || sum != check_sum){
    fprintf(stderr,"**** -> Faill to produce ICMP header , not enough length or check sum not mach \n");
  }

  /* when type is echo request = 8 , and code is echo request = 0*/
  if (ichdr->icmp_type == 8 && ichdr->icmp_code == 0){
    printf("**** -> type is echo request = 8 , and code is echo request = 0\n");
    /* send echo replay type = 0 , echo reply code = 0*/
    sr_send_icmp(sr,packet,len,0,0);
  }
}/* end sr_icmp_handler */


void sr_sending(struct sr_instance *sr,
                uint8_t *packet,
                unsigned int len,
                struct sr_if *interface,
                uint32_t ip){
  assert(sr);
  assert(packet);
  assert(interface);
  printf("\n==== sr_sending() ==== \n");

  struct sr_arpentry *arp = sr_arpcache_lookup(&(sr->cache),ip);

  if(arp){
    printf("**** -> IP->MAC mapping is in the cache. L:369\n");
    sr_ethernet_hdr_t *ehdr = (sr_ethernet_hdr_t *)packet;

    memcpy(ehdr->ether_dhost,arp->mac,ETHER_ADDR_LEN);
    memcpy(ehdr->ether_shost,interface->addr,ETHER_ADDR_LEN);

    sr_send_packet(sr,packet,len,interface->name);

  }else{
    printf("**** -> IP->MAC mapping NOT in the cache. L:378\n");
    struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache),ip,packet,len,interface->name);
    sr_handle_arpreq(sr,request);

  }
}/* end sr_sending */


void sr_handle_arp_packet(struct sr_instance *sr, 
                          uint8_t *packet,
                          unsigned int len,
                          char *receiving_interface)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(receiving_interface);
    printf("\n==== handle_arp_packet() ====\n");

    /*Check packet length*/
    if (len <  sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t))
    {
      fprintf(stderr, "**** -> ERROR: Incorrect packet length\n");
      return;
    }

    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)( packet + sizeof(sr_ethernet_hdr_t));
    struct sr_if *receive_interface = sr_get_interface(sr, receiving_interface);
    struct sr_if *sender_interface  = sr_get_interface_byIP(sr, arp_hdr->ar_tip); 

    /*Check interface whether in router's IP address*/
    if (!receive_interface)
    {
      fprintf(stderr, "**** -> ERROR: Invalid interface\n");
      return;
    }

    /* Get arp_opcode: request or replay to me*/
    if (ntohs(arp_hdr->ar_op) == arp_op_request){           /* Request to me, send a reply*/
        printf("***** -> this is a arp request, preparing a reply L:419\n");
        sr_handle_arp_send_reply_to_requester(sr, packet, receive_interface, sender_interface);
  
    } else if (ntohs(arp_hdr->ar_op) == arp_op_reply){    /* Reply to me, cache it */
     
        printf("***** -> This is a REPLY to me, CACHE it L:422 \n");
        sr_handle_arp_cache_reply(sr, packet, receive_interface);
    } 
}/* end sr_handle_arp_packet */


void sr_handle_arp_cache_reply(struct sr_instance *sr,
                               uint8_t *packet,
                               struct sr_if *interface_info)
{
    printf("\n==== sr_handle_arp_cache_reply() ==== \n");
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Cache it */
    struct sr_arpreq *requests = sr_arpcache_insert(&(sr->cache), arp_hdr->ar_sha, arp_hdr->ar_sip); 

    printf("*** -> Go through my request queue for this IP and send outstanding packets if there are any \n");
    /* Go through my request queue for this IP and send outstanding packets if there are any*/
    if(requests)
    {
      struct sr_packet *pkts         = NULL;
      struct sr_if *dest_if          = NULL;
      sr_ethernet_hdr_t *pkt_eth_hdr = NULL; 
      
      pkts = requests->packets;
      while(pkts)
      {
	printf("**** -> Iterating request queue\n");
        pkt_eth_hdr = (sr_ethernet_hdr_t *)(pkts->buf);
        dest_if = sr_get_interface(sr, pkts->iface);

        /* source and desti mac addresss switched*/
        memcpy(pkt_eth_hdr->ether_shost, dest_if->addr, ETHER_ADDR_LEN);
        memcpy(pkt_eth_hdr->ether_dhost, arp_hdr->ar_sha, ETHER_ADDR_LEN);
        sr_send_packet(sr, pkts->buf, pkts->len, pkts->iface);
        pkts = pkts->next;
      }

      sr_arpreq_destroy(&(sr->cache), requests);
    }

}/* end sr_handle_arp_send_reply */


void sr_handle_arp_send_reply_to_requester(struct sr_instance *sr,
                                           uint8_t *packet,
                                           struct sr_if *receive_interface,
                                           struct sr_if *sender_interface)
{ 
    printf("\n==== sr_handle_arp_send_reply_to_requester() =====\n");
    printf("**** -> Preparing packet....\n");  
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Consttruct a ARP reply*/
    unsigned int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t); 
    uint8_t *reply = (uint8_t *) malloc(packet_len); 
    sr_ethernet_hdr_t *new_ether_hdr = (sr_ethernet_hdr_t *) reply;
    sr_arp_hdr_t *new_arp_hdr = (sr_arp_hdr_t *)(reply + sizeof(sr_ethernet_hdr_t));

    /* Construct the ethernet header */
    new_ether_hdr->ether_type = eth_hdr->ether_type;
    memcpy(new_ether_hdr->ether_dhost, eth_hdr->ether_shost, ETHER_ADDR_LEN);
    memcpy(new_ether_hdr->ether_shost, receive_interface->addr,ETHER_ADDR_LEN);

    /* Construct the ARP header */
    new_arp_hdr->ar_hrd = arp_hdr->ar_hrd;      /* format of hardware address   */
    new_arp_hdr->ar_pro = arp_hdr->ar_pro;      /* format of protocol address   */
    new_arp_hdr->ar_hln = arp_hdr->ar_hln;     /* length of hardware address   */
    new_arp_hdr->ar_pln = arp_hdr->ar_pln;      /* length of protocol address   */
    new_arp_hdr->ar_op  = htons(arp_op_reply);  /* ARP opcode (command)         */
    new_arp_hdr->ar_sip = sender_interface->ip;   /* Sender IP address            */
    new_arp_hdr->ar_tip = arp_hdr->ar_sip;      /* Target IP address            */
    memcpy(new_arp_hdr->ar_sha, receive_interface->addr, ETHER_ADDR_LEN); /* sender hardware address      */
    memcpy(new_arp_hdr->ar_tha, arp_hdr->ar_sha, ETHER_ADDR_LEN);  /* target hardware address      */

    /* ARP replies are sent directly to the requester?s MAC address*/
    printf("***** -> Finsihed packeting, going sr_sr_send_packet()\n");
    sr_send_packet(sr, reply, packet_len, sender_interface->name);
    free(reply);
} /* end sr_handle_arp_manage_reply */


/* HELPERS */
struct sr_if *sr_get_interface_byIP(struct sr_instance *sr,
                                    uint32_t ip){
  struct sr_if *if_walker = 0;
  assert(sr);

  if_walker = sr->if_list;
  while(if_walker){
    if (ip == if_walker->ip)
    {
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_if *sr_get_interface_byAddr(struct sr_instance *sr,
                                    const unsigned char *addr){
  struct sr_if *if_walker = 0;
  assert(sr);
  assert(addr);

  if_walker = sr->if_list;
  while(if_walker){
    if (!memcpy(if_walker->addr,addr,ETHER_ADDR_LEN)){
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_rt *sr_lpm(struct sr_instance *sr, uint32_t ip){

  assert(sr);
  
  struct sr_rt *lpm = 0;
  struct sr_rt *rt_walker = 0;
  uint32_t lpm_len = 0;

  rt_walker = sr->routing_table;

  while(rt_walker){
    if((rt_walker->mask.s_addr & ip ) == (rt_walker->mask.s_addr & rt_walker->dest.s_addr)){
      if(rt_walker->mask.s_addr >= lpm_len){
        lpm = rt_walker;
        lpm_len = rt_walker->mask.s_addr;
      }
    }
    rt_walker = rt_walker->next;
  }
  return lpm;
}

nce *sr,
                                    uint32_t ip){
  struct sr_if *if_walker = 0;
  assert(sr);

  if_walker = sr->if_list;
  while(if_walker){
    if (ip == if_walker->ip)
    {
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_if *sr_get_interface_byAddr(struct sr_instance *sr,
                                    const unsigned char *addr){
  struct sr_if *if_walker = 0;
  assert(sr);
  assert(addr);

  if_walker = sr->if_list;
  while(if_walker){
    if (!memcpy(if_walker->addr,addr,ETHER_ADDR_LEN)){
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_rt *sr_lpm(struct sr_instance *sr, uint32_t ip){

  assert(sr);
  
  struct sr_rt *lpm = 0;
  struct sr_rt *rt_walker = 0;
  uint32_t lpm_len = 0;

  rt_walker = sr->routing_table;

  while(rt_walker){
    if((rt_walker->mask.s_addr & ip ) == (rt_walker->mask.s_addr & rt_walker->dest.s_addr)){
      if(rt_walker->mask.s_addr >= lpm_len){
        lpm = rt_walker;
        lpm_len = rt_walker->mask.s_addr;
      }
    }
    rt_walker = rt_walker->next;
  }
  return lpm;
}


    /* Get arp_opcode: request or replay to me*/
    if (ntohs(arp_hdr->ar_op) == arp_op_request){           /* Request to me, send a reply*/
        printf("***** -> this is a arp request, preparing a reply L:419\n");
        sr_handle_arp_send_reply_to_requester(sr, packet, receive_interface, sender_interface);
  
    } else if (ntohs(arp_hdr->ar_op) == arp_op_reply){    /* Reply to me, cache it */
     
        printf("***** -> This is a REPLY to me, CACHE it L:422 \n");
        sr_handle_arp_cache_reply(sr, packet, receive_interface);
    } 
}/* end sr_handle_arp_packet */


void sr_handle_arp_cache_reply(struct sr_instance *sr,
                               uint8_t *packet,
                               struct sr_if *interface_info)
{
    printf("\n==== sr_handle_arp_cache_reply() ==== \n");
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Cache it */
    struct sr_arpreq *requests = sr_arpcache_insert(&(sr->cache), arp_hdr->ar_sha, arp_hdr->ar_sip); 

    printf("*** -> Go through my request queue for this IP and send outstanding packets if there are any \n");
    /* Go through my request queue for this IP and send outstanding packets if there are any*/
    if(requests)
    {
      struct sr_packet *pkts         = NULL;
      struct sr_if *dest_if          = NULL;
      sr_ethernet_hdr_t *pkt_eth_hdr = NULL; 
      
      pkts = requests->packets;
      while(pkts)
      {
	printf("**** -> Iterating request queue\n");
        pkt_eth_hdr = (sr_ethernet_hdr_t *)(pkts->buf);
        dest_if = sr_get_interface(sr, pkts->iface);

        /* source and desti mac addresss switched*/
        memcpy(pkt_eth_hdr->ether_shost, dest_if->addr, ETHER_ADDR_LEN);
        memcpy(pkt_eth_hdr->ether_dhost, arp_hdr->ar_sha, ETHER_ADDR_LEN);
        sr_send_packet(sr, pkts->buf, pkts->len, pkts->iface);
        pkts = pkts->next;
      }

      sr_arpreq_destroy(&(sr->cache), requests);
    }

}/* end sr_handle_arp_send_reply */


void sr_handle_arp_send_reply_to_requester(struct sr_instance *sr,
                                           uint8_t *packet,
                                           struct sr_if *receive_interface,
                                           struct sr_if *sender_interface)
{ 
    printf("\n==== sr_handle_arp_send_reply_to_requester() =====\n");
    printf("**** -> Preparing packet....\n");  
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Consttruct a ARP reply*/
    unsigned int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t); 
    uint8_t *reply = (uint8_t *) malloc(packet_len); 
    sr_ethernet_hdr_t *new_ether_hdr = (sr_ethernet_hdr_t *) reply;
    sr_arp_hdr_t *new_arp_hdr = (sr_arp_hdr_t *)(reply + sizeof(sr_ethernet_hdr_t));

    /* Construct the ethernet header */
    new_ether_hdr->ether_type = eth_hdr->ether_type;
    memcpy(new_ether_hdr->ether_dhost, eth_hdr->ether_shost, ETHER_ADDR_LEN);
    memcpy(new_ether_hdr->ether_shost, receive_interface->addr,ETHER_ADDR_LEN);

    /* Construct the ARP header */
    new_arp_hdr->ar_hrd = arp_hdr->ar_hrd;      /* format of hardware address   */
    new_arp_hdr->ar_pro = arp_hdr->ar_pro;      /* format of protocol address   */
    new_arp_hdr->ar_hln = arp_hdr->ar_hln;     /* length of hardware address   */
    new_arp_hdr->ar_pln = arp_hdr->ar_pln;      /* length of protocol address   */
    new_arp_hdr->ar_op  = htons(arp_op_reply);  /* ARP opcode (command)         */
    new_arp_hdr->ar_sip = sender_interface->ip;   /* Sender IP address            */
    new_arp_hdr->ar_tip = arp_hdr->ar_sip;      /* Target IP address            */
    memcpy(new_arp_hdr->ar_sha, receive_interface->addr, ETHER_ADDR_LEN); /* sender hardware address      */
    memcpy(new_arp_hdr->ar_tha, arp_hdr->ar_sha, ETHER_ADDR_LEN);  /* target hardware address      */

    /* ARP replies are sent directly to the requester?s MAC address*/
    printf("***** -> Finsihed packeting, going sr_sr_send_packet()\n");
    sr_send_packet(sr, reply, packet_len, sender_interface->name);
    free(reply);
} /* end sr_handle_arp_manage_reply */


/* HELPERS */
struct sr_if *sr_get_interface_byIP(struct sr_instance *sr,
                                    uint32_t ip){
  struct sr_if *if_walker = 0;
  assert(sr);

  if_walker = sr->if_list;
  while(if_walker){
    if (ip == if_walker->ip)
    {
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_if *sr_get_interface_byAddr(struct sr_instance *sr,
                                    const unsigned char *addr){
  struct sr_if *if_walker = 0;
  assert(sr);
  assert(addr);

  if_walker = sr->if_list;
  while(if_walker){
    if (!memcpy(if_walker->addr,addr,ETHER_ADDR_LEN)){
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_rt *sr_lpm(struct sr_instance *sr, uint32_t ip){

  assert(sr);
  
  struct sr_rt *lpm = 0;
  struct sr_rt *rt_walker = 0;
  uint32_t lpm_len = 0;

  rt_walker = sr->routing_table;

  while(rt_walker){
    if((rt_walker->mask.s_addr & ip ) == (rt_walker->mask.s_addr & rt_walker->dest.s_addr)){
      if(rt_walker->mask.s_addr >= lpm_len){
        lpm = rt_walker;
        lpm_len = rt_walker->mask.s_addr;
      }
    }
    rt_walker = rt_walker->next;
  }
  return lpm;
}

face\n");
      return;
    }

    /* Get arp_opcode: request or replay to me*/
    if (ntohs(arp_hdr->ar_op) == arp_op_request){           /* Request to me, send a reply*/
        printf("***** -> this is a arp request, preparing a reply L:419\n");
        sr_handle_arp_send_reply_to_requester(sr, packet, receive_interface, sender_interface);
  
    } else if (ntohs(arp_hdr->ar_op) == arp_op_reply){    /* Reply to me, cache it */
     
        printf("***** -> This is a REPLY to me, CACHE it L:422 \n");
        sr_handle_arp_cache_reply(sr, packet, receive_interface);
    } 
}/* end sr_handle_arp_packet */


void sr_handle_arp_cache_reply(struct sr_instance *sr,
                               uint8_t *packet,
                               struct sr_if *interface_info)
{
    printf("\n==== sr_handle_arp_cache_reply() ==== \n");
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Cache it */
    struct sr_arpreq *requests = sr_arpcache_insert(&(sr->cache), arp_hdr->ar_sha, arp_hdr->ar_sip); 

    printf("*** -> Go through my request queue for this IP and send outstanding packets if there are any \n");
    /* Go through my request queue for this IP and send outstanding packets if there are any*/
    if(requests)
    {
      struct sr_packet *pkts         = NULL;
      struct sr_if *dest_if          = NULL;
      sr_ethernet_hdr_t *pkt_eth_hdr = NULL; 
      
      pkts = requests->packets;
      while(pkts)
      {
	printf("**** -> Iterating request queue\n");
        pkt_eth_hdr = (sr_ethernet_hdr_t *)(pkts->buf);
        dest_if = sr_get_interface(sr, pkts->iface);

        /* source and desti mac addresss switched*/
        memcpy(pkt_eth_hdr->ether_shost, dest_if->addr, ETHER_ADDR_LEN);
        memcpy(pkt_eth_hdr->ether_dhost, arp_hdr->ar_sha, ETHER_ADDR_LEN);
        sr_send_packet(sr, pkts->buf, pkts->len, pkts->iface);
        pkts = pkts->next;
      }

      sr_arpreq_destroy(&(sr->cache), requests);
    }

}/* end sr_handle_arp_send_reply */


void sr_handle_arp_send_reply_to_requester(struct sr_instance *sr,
                                           uint8_t *packet,
                                           struct sr_if *receive_interface,
                                           struct sr_if *sender_interface)
{ 
    printf("\n==== sr_handle_arp_send_reply_to_requester() =====\n");
    printf("**** -> Preparing packet....\n");  
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t *)packet;
    sr_arp_hdr_t *arp_hdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

    /* Consttruct a ARP reply*/
    unsigned int packet_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t); 
    uint8_t *reply = (uint8_t *) malloc(packet_len); 
    sr_ethernet_hdr_t *new_ether_hdr = (sr_ethernet_hdr_t *) reply;
    sr_arp_hdr_t *new_arp_hdr = (sr_arp_hdr_t *)(reply + sizeof(sr_ethernet_hdr_t));

    /* Construct the ethernet header */
    new_ether_hdr->ether_type = eth_hdr->ether_type;
    memcpy(new_ether_hdr->ether_dhost, eth_hdr->ether_shost, ETHER_ADDR_LEN);
    memcpy(new_ether_hdr->ether_shost, receive_interface->addr,ETHER_ADDR_LEN);

    /* Construct the ARP header */
    new_arp_hdr->ar_hrd = arp_hdr->ar_hrd;      /* format of hardware address   */
    new_arp_hdr->ar_pro = arp_hdr->ar_pro;      /* format of protocol address   */
    new_arp_hdr->ar_hln = arp_hdr->ar_hln;     /* length of hardware address   */
    new_arp_hdr->ar_pln = arp_hdr->ar_pln;      /* length of protocol address   */
    new_arp_hdr->ar_op  = htons(arp_op_reply);  /* ARP opcode (command)         */
    new_arp_hdr->ar_sip = sender_interface->ip;   /* Sender IP address            */
    new_arp_hdr->ar_tip = arp_hdr->ar_sip;      /* Target IP address            */
    memcpy(new_arp_hdr->ar_sha, receive_interface->addr, ETHER_ADDR_LEN); /* sender hardware address      */
    memcpy(new_arp_hdr->ar_tha, arp_hdr->ar_sha, ETHER_ADDR_LEN);  /* target hardware address      */

    /* ARP replies are sent directly to the requester?s MAC address*/
    printf("***** -> Finsihed packeting, going sr_sr_send_packet()\n");
    sr_send_packet(sr, reply, packet_len, sender_interface->name);
    free(reply);
} /* end sr_handle_arp_manage_reply */


/* HELPERS */
struct sr_if *sr_get_interface_byIP(struct sr_instance *sr,
                                    uint32_t ip){
  struct sr_if *if_walker = 0;
  assert(sr);

  if_walker = sr->if_list;
  while(if_walker){
    if (ip == if_walker->ip)
    {
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_if *sr_get_interface_byAddr(struct sr_instance *sr,
                                    const unsigned char *addr){
  struct sr_if *if_walker = 0;
  assert(sr);
  assert(addr);

  if_walker = sr->if_list;
  while(if_walker){
    if (!memcpy(if_walker->addr,addr,ETHER_ADDR_LEN)){
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_rt *sr_lpm(struct sr_instance *sr, uint32_t ip){

  assert(sr);
  
  struct sr_rt *lpm = 0;
  struct sr_rt *rt_walker = 0;
  uint32_t lpm_len = 0;

  rt_walker = sr->routing_table;

  while(rt_walker){
    if((rt_walker->mask.s_addr & ip ) == (rt_walker->mask.s_addr & rt_walker->dest.s_addr)){
      if(rt_walker->mask.s_addr >= lpm_len){
        lpm = rt_walker;
        lpm_len = rt_walker->mask.s_addr;
      }
    }
    rt_walker = rt_walker->next;
  }
  return lpm;
}


    if (ip == if_walker->ip)
    {
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_if *sr_get_interface_byAddr(struct sr_instance *sr,
                                    const unsigned char *addr){
  struct sr_if *if_walker = 0;
  assert(sr);
  assert(addr);

  if_walker = sr->if_list;
  while(if_walker){
    if (!memcpy(if_walker->addr,addr,ETHER_ADDR_LEN)){
      return if_walker;
    }
    if_walker = if_walker->next;
  }
  return 0;
}

struct sr_rt *sr_lpm(struct sr_instance *sr, uint32_t ip){

  assert(sr);
  
  struct sr_rt *lpm = 0;
  struct sr_rt *rt_walker = 0;
  uint32_t lpm_len = 0;

  rt_walker = sr->routing_table;

  while(rt_walker){
    if((rt_walker->mask.s_addr & ip ) == (rt_walker->mask.s_addr & rt_walker->dest.s_addr)){
      if(rt_walker->mask.s_addr >= lpm_len){
        lpm = rt_walker;
        lpm_len = rt_walker->mask.s_addr;
      }
    }
    rt_walker = rt_walker->next;
  }
  return lpm;
}

