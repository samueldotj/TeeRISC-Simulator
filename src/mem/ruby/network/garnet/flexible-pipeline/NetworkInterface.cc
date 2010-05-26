/*
 * Copyright (c) 2008 Princeton University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Niket Agarwal
 */

#include <cmath>

#include "mem/ruby/network/garnet/flexible-pipeline/NetworkInterface.hh"
#include "mem/ruby/buffers/MessageBuffer.hh"
#include "mem/ruby/network/garnet/flexible-pipeline/flitBuffer.hh"
#include "mem/ruby/slicc_interface/NetworkMessage.hh"

NetworkInterface::NetworkInterface(int id, int virtual_networks, GarnetNetwork *network_ptr)
{
        m_id = id;
        m_net_ptr = network_ptr;
        m_virtual_networks  = virtual_networks;
        m_vc_per_vnet = m_net_ptr->getVCsPerClass();
        m_num_vcs = m_vc_per_vnet*m_virtual_networks;

        m_vc_round_robin = 0;
        m_ni_buffers.setSize(m_num_vcs);
        inNode_ptr.setSize(m_virtual_networks);
        outNode_ptr.setSize(m_virtual_networks);

        for(int i =0; i < m_num_vcs; i++)
                m_ni_buffers[i] = new flitBuffer(); // instantiating the NI flit buffers

         m_vc_allocator.setSize(m_virtual_networks);
         for(int i = 0; i < m_virtual_networks; i++)
         {
                 m_vc_allocator[i] = 0;
         }

         for(int i = 0; i < m_num_vcs; i++)
         {
                m_out_vc_state.insertAtBottom(new OutVcState(i));
                m_out_vc_state[i]->setState(IDLE_, g_eventQueue_ptr->getTime());
         }
}

NetworkInterface::~NetworkInterface()
{
        m_out_vc_state.deletePointers();
        m_ni_buffers.deletePointers();
        delete outSrcQueue;
}

void NetworkInterface::addInPort(NetworkLink *in_link)
{
        inNetLink = in_link;
        in_link->setLinkConsumer(this);
}

void NetworkInterface::addOutPort(NetworkLink *out_link)
{
        outNetLink = out_link;
        outSrcQueue = new flitBuffer();
        out_link->setSourceQueue(outSrcQueue);
        out_link->setSource(this);
}

void NetworkInterface::addNode(Vector<MessageBuffer*>& in,  Vector<MessageBuffer*>& out)
{
        ASSERT(in.size() == m_virtual_networks);
        inNode_ptr = in;
        outNode_ptr = out;
        for (int j = 0; j < m_virtual_networks; j++)
        {
                inNode_ptr[j]->setConsumer(this);  // So that protocol injects messages into the NI
        }
}

void NetworkInterface::request_vc(int in_vc, int in_port, NetDest destination, Time request_time)
{
        inNetLink->grant_vc_link(in_vc, request_time);
}

bool NetworkInterface::flitisizeMessage(MsgPtr msg_ptr, int vnet)
{
        NetworkMessage *net_msg_ptr = dynamic_cast<NetworkMessage*>(msg_ptr.ref());
        NetDest net_msg_dest = net_msg_ptr->getInternalDestination();
        Vector<NodeID> dest_nodes = net_msg_dest.getAllDest(); // gets all the destinations associated with this message.
        int num_flits = (int) ceil((double) m_net_ptr->MessageSizeType_to_int(net_msg_ptr->getMessageSize())/m_net_ptr->getFlitSize() ); // Number of flits is dependent on the link bandwidth available. This is expressed in terms of bytes/cycle or the flit size

        for(int ctr = 0; ctr < dest_nodes.size(); ctr++) // loop because we will be converting all multicast messages into unicast messages
        {
                int vc = calculateVC(vnet); // this will return a free output virtual channel

                if(vc == -1)
                {
                        // did not find a free output vc
                        return false ;
                }
                MsgPtr new_msg_ptr = *(msg_ptr.ref());
                NodeID destID = dest_nodes[ctr];

                NetworkMessage *new_net_msg_ptr = dynamic_cast<NetworkMessage*>(new_msg_ptr.ref());
                if(dest_nodes.size() > 1)
                {
                        NetDest personal_dest;
                        for(int m = 0; m < (int) MachineType_NUM; m++)
                        {
                                if((destID >= MachineType_base_number((MachineType) m)) && destID < MachineType_base_number((MachineType) (m+1)))
                                {
                                        // calculating the NetDest associated with this destination ID
                                        personal_dest.clear();
                                        personal_dest.add((MachineID) {(MachineType) m, (destID - MachineType_base_number((MachineType) m))});
                                        new_net_msg_ptr->getInternalDestination() = personal_dest;
                                        break;
                                }
                        }
                        net_msg_dest.removeNetDest(personal_dest);
                        net_msg_ptr->getInternalDestination().removeNetDest(personal_dest); // removing the destination from the original message to reflect that a message with this particular destination has been flitisized and an output vc is acquired
                }
                for(int i = 0; i < num_flits; i++)
                {
                        flit *fl = new flit(i, vc, vnet, num_flits, new_msg_ptr);
                        m_ni_buffers[vc]->insert(fl);
                }

                m_out_vc_state[vc]->setState(VC_AB_, g_eventQueue_ptr->getTime());
                outNetLink->request_vc_link(vc, new_net_msg_ptr->getInternalDestination(), g_eventQueue_ptr->getTime()); // setting an output vc request for the next hop. It is only when an output vc is acquired at the next hop that this flit will be ready to traverse the link and into the next hop
        }

        return true ;
}

// An output vc has been granted at the next hop to one of the vc's. We have to update the state of the vc to reflect this
void NetworkInterface::grant_vc(int out_port, int vc, Time grant_time)
{

        assert(m_out_vc_state[vc]->isInState(VC_AB_, grant_time));
        m_out_vc_state[vc]->grant_vc(grant_time);
        g_eventQueue_ptr->scheduleEvent(this, 1);
}

// The tail flit corresponding to this vc has been buffered at the next hop and thus this vc is now free
void NetworkInterface::release_vc(int out_port, int vc, Time release_time)
{
        assert(m_out_vc_state[vc]->isInState(ACTIVE_, release_time));
        m_out_vc_state[vc]->setState(IDLE_, release_time);
        g_eventQueue_ptr->scheduleEvent(this, 1);
}

// Looking for a free output vc
int NetworkInterface::calculateVC(int vnet)
{
        int vc_per_vnet;
        if(m_net_ptr->isVNetOrdered(vnet))
                vc_per_vnet = 1;
        else
                vc_per_vnet = m_vc_per_vnet;

        for(int i = 0; i < vc_per_vnet; i++)
        {
                int delta = m_vc_allocator[vnet];
                m_vc_allocator[vnet]++;
                if(m_vc_allocator[vnet] == vc_per_vnet)
                        m_vc_allocator[vnet] = 0;

                if(m_out_vc_state[(vnet*m_vc_per_vnet) + delta]->isInState(IDLE_, g_eventQueue_ptr->getTime()))
                {
                        return ((vnet*m_vc_per_vnet) + delta);
                }
        }
        return -1;
}

/*
 * The NI wakeup checks whether there are any ready messages in the protocol buffer. If yes, it picks that up, flitisizes it into a number of flits and puts it into an output
 * buffer and schedules the output link.
 * On a wakeup it also checks whether there are flits in the input link. If yes, it picks them up and if the flit is a tail, the NI inserts the corresponding message into
 * the protocol buffer.
 */

void NetworkInterface::wakeup()
{
        MsgPtr msg_ptr;

        //Checking for messages coming from the protocol
        for (int vnet = 0; vnet < m_virtual_networks; vnet++) // can pick up a message/cycle for each virtual net
        {
                while(inNode_ptr[vnet]->isReady()) // Is there a message waiting
                {
                        msg_ptr = inNode_ptr[vnet]->peekMsgPtr();
                        if(flitisizeMessage(msg_ptr, vnet))
                        {
                                inNode_ptr[vnet]->pop();
                        }
                        else
                        {
                                break;
                        }
                }
        }

        scheduleOutputLink();
        checkReschedule();

/*********** Picking messages destined for this NI **********/

        if(inNetLink->isReady())
        {
                flit *t_flit = inNetLink->consumeLink();
                if(t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_)
                {
                        DEBUG_EXPR(NETWORK_COMP, HighPrio, m_id);
                        DEBUG_MSG(NETWORK_COMP, HighPrio, "Message got delivered");
                        DEBUG_EXPR(NETWORK_COMP, HighPrio, g_eventQueue_ptr->getTime());
                        if(!m_net_ptr->isNetworkTesting()) // When we are doing network only testing, the messages do not have to be buffered into the message buffers
                        {
                                outNode_ptr[t_flit->get_vnet()]->enqueue(t_flit->get_msg_ptr(), 1); // enqueueing for protocol buffer. This is not required when doing network only testing
                        }
                        inNetLink->release_vc_link(t_flit->get_vc(), g_eventQueue_ptr->getTime() + 1); // signal the upstream router that this vc can be freed now
                }
                delete t_flit;
        }
}

// This function look at the NI buffers and if some buffer has flits which are ready to traverse the link in the next cycle and also the downstream output vc associated with this flit has buffers left, the link is scheduled for the next cycle
void NetworkInterface::scheduleOutputLink()
{
        int vc = m_vc_round_robin;
        m_vc_round_robin++;
        if(m_vc_round_robin == m_num_vcs)
                m_vc_round_robin = 0;

        for(int i = 0; i < m_num_vcs; i++)
        {
                vc++;
                if(vc == m_num_vcs)
                        vc = 0;
                if(m_ni_buffers[vc]->isReady())
                {
                        if(m_out_vc_state[vc]->isInState(ACTIVE_, g_eventQueue_ptr->getTime()) && outNetLink->isBufferNotFull_link(vc))  // models buffer backpressure
                        {
                                flit *t_flit = m_ni_buffers[vc]->getTopFlit();  // Just removing the flit
                                t_flit->set_time(g_eventQueue_ptr->getTime() + 1);
                                outSrcQueue->insert(t_flit);
                                g_eventQueue_ptr->scheduleEvent(outNetLink, 1); // schedule the out link
                                return;
                        }
                }
        }
}

void NetworkInterface::checkReschedule()
{
        for(int vnet = 0; vnet < m_virtual_networks; vnet++)
        {
                if(inNode_ptr[vnet]->isReady()) // Is there a message waiting
                {
                        g_eventQueue_ptr->scheduleEvent(this, 1);
                        return;
                }
        }
        for(int vc = 0; vc < m_num_vcs; vc++)
        {
                if(m_ni_buffers[vc]->isReadyForNext())
                {
                        g_eventQueue_ptr->scheduleEvent(this, 1);
                        return;
                }
        }
}

void NetworkInterface::printConfig(std::ostream& out) const
{
        out << "[Network Interface " << m_id << "] - ";
        out << "[inLink " << inNetLink->get_id() << "] - ";
        out << "[outLink " << outNetLink->get_id() << "]" << std::endl;
}

void NetworkInterface::print(std::ostream& out) const
{
        out << "[Network Interface]";
}