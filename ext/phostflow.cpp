#include "phostflow.h"
#include "phost.h"

#include "../run/params.h"

/* Implementation for pHost Flow */

extern void add_to_event_queue(Event* ev);
extern double get_current_time();
extern DCExpParams params;

PHostFlow::PHostFlow(uint32_t id, double start_time, uint32_t size, Host *s, Host *d)
    : Flow(id, start_time, size, s, d) {
    this->remaining_packets = size_in_pkt;

    this->capabilities_sent = 0;
    this->next_seq_no = 0;
    this->timed_out = false;
    this->timeout_seqno = 0;
}

void PHostFlow::start_flow() {
    ((PHost*) this->src)->start(this);
}

void PHostFlow::receive(Packet* p) {
    PHostToken* t;
    switch (p->type) {
        // sender side
        case CAPABILITY_PACKET:
            t = new PHostToken(this, p->seq_no, get_current_time() + params.capability_timeout);
            ((PHost*) src)->received_capabilities.push(t);
            break;
        case ACK_PACKET:
            // flow finished
            finished = true;
            finish_time = get_current_time();
            add_to_event_queue(new FlowFinishedEvent(get_current_time(), this));
            break;

        // receiver side
        case RTS_PACKET:
            ((PHost*) dst)->active_receiving_flows.push(this);
            break;
        case NORMAL_PACKET:
            receive_data_pkt(p);
            break;

        default:
            assert(false);
            break;
    }

    delete p;
}

void PHostFlow::receive_data_pkt(Packet* p) {
    timed_out = false;
    timeout_seqno = 0;

    uint32_t seq = p->seq_no;
    bool found = false;
    for (auto it = window.begin(); it != window.end(); it++) {
        if (*it == seq) {
            window.erase(it);
            found = true;
            break;
        }
    }

    assert(found);

    remaining_packets--;

    // send ack if done
    if (remaining_packets == 0) {
        assert(window.size() == 0);
        add_to_event_queue(
            new PacketQueuingEvent(
                get_current_time(), 
                new PlainAck(this, 0, params.hdr_size, this->dst, this->src), 
                this->dst->queue
            )
        );
    }
}

// timeouts are receiver side
void PHostFlow::set_timeout(double time) {
    if (retx_event == NULL || retx_event->cancelled) {
        retx_event = new RetxTimeoutEvent(time, this);
        add_to_event_queue(retx_event);
    }
}

void PHostFlow::handle_timeout() {
    this->timed_out = true;

    if (((PHost*) dst)->capa_proc_evt == NULL || ((PHost*) dst)->capa_proc_evt->cancelled) {
        ((PHost*) dst)->capa_proc_evt = new PHostTokenProcessingEvent(get_current_time(), ((PHost*) dst));
        add_to_event_queue(((PHost*) dst)->capa_proc_evt);
    }
}
