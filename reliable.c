#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "buffer.h"

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    buffer_t* send_buffer;
    buffer_t* rec_buffer;

    uint32_t next_seq;
    uint32_t exp_seq;

    
    uint16_t flags; // |ACK_ALL|WRITTEN_ALL|EOF_OUT|EOF_IN|

    const struct config_common* config;

};
rel_t *rel_list;

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));


    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->exp_seq = 1; 
    r->next_seq = 1; 
    r->config = cc;
    r->flags = 0;

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;
    

    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;

    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;

    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);

    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);

}

void send_ack(rel_t *r, uint32_t ackno){
    struct ack_packet* ack = (struct ack_packet*) malloc(8);
    memset(ack, 0, 8);
    ack->len = htons(8);
    ack->ackno = htonl(ackno);
    ack->cksum = cksum(ack, 8);
    conn_sendpkt(r->c, (packet_t*) ack, 8);
    free(ack);
}

int is_in_range(rel_t *r, size_t n, packet_t *pkt){

    uint32_t ackno = ntohl(pkt->ackno);
    uint16_t pkt_length = ntohs(pkt->len);


    if((uint16_t) n != pkt_length || 
        n < 8 || 
        n > 512 || 
        pkt_length < 8 || 
        pkt_length > 512 || 
        ackno < 1 || 
        ackno > r->next_seq){
        return 1;
    }else{
        return 0;
    }
}

long getTime(){
    struct timeval now;
    gettimeofday (&now, NULL);
    long now_ms = now.tv_sec * 1000 + now.tv_usec/1000;
    return now_ms;
}


// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{   

    uint32_t ackno = ntohl(pkt->ackno);
    uint32_t seqno = ntohl(pkt->seqno);
    uint16_t pkt_length = ntohs(pkt->len);
    
    if(is_in_range(r, n, pkt)) return;
    
    
    uint16_t old_checksum = pkt->cksum;
	pkt->cksum = 0;
    uint16_t new_checksum = cksum(pkt, pkt_length);

    if(old_checksum != new_checksum)return;
    if(pkt_length != 8 && (pkt_length < 12 || pkt_length > 512))return;
    
    if(pkt_length == 8){
        buffer_remove(r->send_buffer, ackno);
        rel_read(r);    
    }else if(seqno >= r->exp_seq){

            if(!buffer_contains(r->rec_buffer, seqno)){
                 buffer_insert(r->rec_buffer, pkt, 0);
            }

            rel_output(r); 
    }else{
        send_ack(r, r->exp_seq);
    }

   
}

void
rel_read (rel_t *s)
{    
    s->flags = s->flags & 3;
    s->flags = s->flags | (buffer_size(s->send_buffer) == 0) << 3 | (buffer_size(s->send_buffer) == 0) << 2;
    if(s->flags == 0xF){
        rel_destroy(s);
    }else if(s->flags & 1){
        return;
    }

    while (buffer_size(s->send_buffer) < s->config->window){
       
        packet_t* packet = (packet_t*) malloc(sizeof(packet_t));
        memset(packet, 0, sizeof(packet_t));
        int bytes_read = conn_input(s->c, packet->data, 500);

        if(bytes_read == 0){
            break;
        }else if(bytes_read == -1){
            s->flags = (s->flags | 1);  
            packet->len = htons(12);
            packet->seqno = htonl(s->next_seq);
            packet->ackno = htonl(s->exp_seq);
            packet->cksum = cksum(packet, 12);
            (s->next_seq)++;
            conn_sendpkt(s->c, packet, 12);
            buffer_insert(s->send_buffer, packet, getTime());
            break;
        }else{
            packet->len = htons(bytes_read + 12);
            packet->seqno = htonl(s->next_seq);
            packet->ackno = htonl(s->exp_seq);
            packet->cksum = cksum(packet, bytes_read + 12);
            (s->next_seq)++;
            buffer_insert(s->send_buffer, packet, getTime());
            conn_sendpkt(s->c, packet, bytes_read + 12);
        }   
    }
}

void
rel_output (rel_t *r)
{   
    buffer_node_t* buff = buffer_get_first(r->rec_buffer);

    while(buff && ntohs(buff->packet.len) - 12 <= conn_bufspace(r->c) && ntohl(buff->packet.seqno) == r->exp_seq){
        r->exp_seq++;
        if(ntohs(buff->packet.len) == 12){
            r->flags = (r->flags | 2);           
        }
        conn_output(r->c, buff->packet.data, ntohs(buff->packet.len) - 12);
        buff = buff->next;
       
    }

    buffer_remove(r->rec_buffer, r->exp_seq);
    send_ack(r, r->exp_seq);


    r->flags = r->flags & 3;
    r->flags = r->flags | (buffer_size(r->send_buffer) == 0) << 3 | (buffer_size(r->send_buffer) == 0) << 2;
    if(r->flags == 0xF){
        rel_destroy(r);
    }

}

void
rel_timer ()
{
    rel_t *current = rel_list;
    while (current != NULL) {
        buffer_node_t* buff = buffer_get_first(current->send_buffer);
        while(buff){
            if(getTime() - buff->last_retransmit > current->config->timeout){
                conn_sendpkt(current->c, &(buff->packet), ntohs(buff->packet.len));
                buff->last_retransmit = getTime();
            }
            buff = buff->next;
        }
        current = current->next;
    }
}
