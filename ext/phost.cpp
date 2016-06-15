#include <assert.h>
#include <stdlib.h>

#include "../coresim/event.h"
#include "../coresim/flow.h"
#include "../coresim/packet.h"
#include "../coresim/debug.h"

#include "phostflow.h"
#include "phost.h"
#include "factory.h"

#include "../run/params.h"

extern double get_current_time();
extern void add_to_event_queue(Event*);
extern DCExpParams params;

PHostTokenProcessingEvent::PHostTokenProcessingEvent(double time, PHost *h)
    : Event(PHOST_TOKEN_PROCESSING_EVENT, time) {
        this->host = h;
    }

PHostTokenProcessingEvent::~PHostTokenProcessingEvent() {
    if (host->capa_proc_evt == this) {
        host->capa_proc_evt = NULL;
    }
}

void PHostTokenProcessingEvent::process_event() {
    this->host->capa_proc_evt = NULL;
    this->host->send_capability();
}

bool PHostFlowComparator::operator() (PHostToken* a, PHostToken* b){
    if (a->flow == b->flow)
        return a->seqno > b->seqno;
    if (params.deadline && params.schedule_by_deadline) 
        return a->flow->deadline > b->flow->deadline;
    if (a->flow->remaining_packets > b->flow->remaining_packets)
        return true;
    else if (a->flow->remaining_packets == b->flow->remaining_packets)
        return a->flow->start_time > b->flow->start_time;
    else
        return false;
}

bool PHostReceiverFlowComparator::operator() (PHostFlow* a, PHostFlow* b){
    if(params.deadline && params.schedule_by_deadline) {
        return a->deadline > b->deadline;
    }
    else {
        if ((a->size - a->capabilities_sent) > (b->size - b->capabilities_sent)) {
            return true;
        } else if ((a->size - a->capabilities_sent) == (b->size - b->capabilities_sent)) {
            return a->start_time > b->start_time;
        } else {
            return false;
        }
    }
}

PHost::PHost(uint32_t id, double rate, uint32_t queue_type) : SchedulingHost(id, rate, queue_type) {
    this->capa_proc_evt = NULL;
}

// sender side
void PHost::start(Flow* f) {
    // send RTS
    Packet* p = new RTSCTS(true, get_current_time(), f, params.hdr_size, f->src, f->dst);
    add_to_event_queue(new PacketQueuingEvent(get_current_time(), p, this->queue));

    if (this->host_proc_event == NULL || this->host_proc_event->cancelled) {
        this->host_proc_event = new HostProcessingEvent(get_current_time(), this);
        add_to_event_queue(this->host_proc_event);
    }
}

void PHost::send() {
    PHostToken* t;
    double to = get_current_time() + 1.0;
    while (to > get_current_time()) {
        t = received_capabilities.top();
        received_capabilities.pop();
        to = t->timeout;
    }

    t->flow->send(t->seqno);

    if (this->host_proc_event == NULL || this->host_proc_event->cancelled) {
        this->host_proc_event = new HostProcessingEvent(
                get_current_time() 
                + this->queue->get_transmission_delay(
                    params.mss 
                    + params.hdr_size 
                    - INFINITESIMAL_TIME
                    ), 
                this
            );
        add_to_event_queue(this->host_proc_event);
    }
}

//receiver side
void PHost::start_receiving(Flow* f) {
    active_receiving_flows.push((PHostFlow*) f);

    if (capa_proc_evt == NULL || capa_proc_evt->cancelled) {
        capa_proc_evt = new PHostTokenProcessingEvent(get_current_time(), this);
        add_to_event_queue(capa_proc_evt);
    }
}

void PHost::send_capability() {
    Packet* capa;
    PHostFlow* f;
    while(1) {
        f = active_receiving_flows.top();
        if (f->timed_out && f->window.size() >= params.capability_window) {
            // timed out, retx old packets
            capa = new Packet(get_current_time(), f, f->window[f->timeout_seqno], 0, params.hdr_size, f->dst, f->src);
            capa->type = CAPABILITY_PACKET;
            f->timeout_seqno++;
        }
        else if (f->window.size() >= params.capability_window) {
            // wait for a timeout or a received packet
            assert(f->retx_event != NULL);
            active_receiving_flows.pop();
            continue;
        }
        else if (f->capabilities_sent <= f->size_in_pkt) {
            capa = new Packet(get_current_time(), f, f->next_seq_no, 0, params.hdr_size, f->dst, f->src);
            capa->type = CAPABILITY_PACKET;
            f->window.push_back(f->next_seq_no);
            f->next_seq_no += params.mss;
            f->capabilities_sent++;
        }

        if (capa != NULL) {
            f->set_timeout(
                get_current_time() 
                + params.capability_window_timeout 
                * this->queue->get_transmission_delay(params.mss + params.hdr_size)
            );
            add_to_event_queue(new PacketQueuingEvent(get_current_time(), capa, this->queue));
            
            double td = get_current_time() 
                        + this->queue->get_transmission_delay(
                            params.mss 
                            + params.hdr_size 
                            - INFINITESIMAL_TIME
                            ); 
 
            capa_proc_evt = new PHostTokenProcessingEvent(get_current_time() + td, this);
            add_to_event_queue(capa_proc_evt);
            break;
        }
    }
}
