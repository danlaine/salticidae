/**
 * Copyright (c) 2018 Cornell University.
 *
 * Author: Ted Yin <tederminant@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _SALTICIDAE_NETWORK_H
#define _SALTICIDAE_NETWORK_H

#include "salticidae/event.h"
#include "salticidae/netaddr.h"
#include "salticidae/msg.h"
#include "salticidae/conn.h"

#ifdef __cplusplus
namespace salticidae {
/** Network of nodes who can send async messages.  */
template<typename OpcodeType>
class MsgNetwork: public ConnPool {
    public:
    using Msg = MsgBase<OpcodeType>;
    /* match lambdas */
    template<typename T>
    struct callback_traits:
        public callback_traits<decltype(&T::operator())> {};
    
    /* match plain functions */
    template<typename ReturnType, typename MsgType, typename ConnType>
    struct callback_traits<ReturnType(MsgType, ConnType)> {
        using ret_type = ReturnType;
        using conn_type = typename std::remove_reference<ConnType>::type::type;
        using msg_type = typename std::remove_reference<MsgType>::type;
    };
    
    /* match function pointers */
    template<typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(*)(Args...)>:
        public callback_traits<ReturnType(Args...)> {};
    
    /* match const member functions */
    template<typename ClassType, typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(ClassType::*)(Args...) const>:
        public callback_traits<ReturnType(Args...)> {};
    
    /* match member functions */
    template<typename ClassType, typename ReturnType, typename... Args>
    struct callback_traits<ReturnType(ClassType::*)(Args...)>:
        public callback_traits<ReturnType(Args...)> {};

    class Conn: public ConnPool::Conn {
        friend MsgNetwork;
        enum MsgState {
            HEADER,
            PAYLOAD
        };

        Msg msg;
        MsgState msg_state;

        protected:
#ifdef SALTICIDAE_MSG_STAT
        mutable std::atomic<size_t> nsent;
        mutable std::atomic<size_t> nrecv;
        mutable std::atomic<size_t> nsentb;
        mutable std::atomic<size_t> nrecvb;
#endif

        public:
        Conn(): msg_state(HEADER)
#ifdef SALTICIDAE_MSG_STAT
            , nsent(0), nrecv(0), nsentb(0), nrecvb(0)
#endif
        {}

        MsgNetwork *get_net() {
            return static_cast<MsgNetwork *>(get_pool());
        }

#ifdef SALTICIDAE_MSG_STAT
        size_t get_nsent() const { return nsent; }
        size_t get_nrecv() const { return nrecv; }
        size_t get_nsentb() const { return nsentb; }
        size_t get_nrecvb() const { return nrecvb; }
        void clear_msgstat() const {
            nsent.store(0, std::memory_order_relaxed);
            nrecv.store(0, std::memory_order_relaxed);
            nsentb.store(0, std::memory_order_relaxed);
            nrecvb.store(0, std::memory_order_relaxed);
        }
#endif

        protected:
        void on_read() override;
    };

    using conn_t = ArcObj<Conn>;
#ifdef SALTICIDAE_MSG_STAT
    // TODO: a lock-free, thread-safe, fine-grained stat
#endif

    private:
    std::unordered_map<
        typename Msg::opcode_t,
        std::function<void(const Msg &msg, const conn_t &)>> handler_map;
    using queue_t = MPSCQueueEventDriven<std::pair<Msg, conn_t>>;
    queue_t incoming_msgs;

    protected:

    ConnPool::Conn *create_conn() override { return new Conn(); }

    public:

    class Config: public ConnPool::Config {
        friend MsgNetwork;
        size_t _burst_size;

        public:
        Config(): Config(ConnPool::Config()) {}
        Config(const ConnPool::Config &config):
            ConnPool::Config(config), _burst_size(1000) {}

        Config &burst_size(size_t x) {
            _burst_size = x;
            return *this;
        }
    };

    virtual ~MsgNetwork() { stop_workers(); }

    MsgNetwork(const EventContext &ec, const Config &config):
            ConnPool(ec, config) {
        incoming_msgs.set_capacity(65536);
        incoming_msgs.reg_handler(ec, [this, burst_size=config._burst_size](queue_t &q) {
            std::pair<Msg, conn_t> item;
            size_t cnt = 0;
            while (q.try_dequeue(item))
            {
                auto &msg = item.first;
                auto &conn = item.second;
#ifdef SALTICIDAE_CBINDINGS_INJECT_CALLBACK
                salticidae_injected_msg_callback(&msg, conn.get());
#else
                auto it = handler_map.find(msg.get_opcode());
                if (it == handler_map.end())
                    SALTICIDAE_LOG_WARN("unknown opcode: %s",
                                        get_hex(msg.get_opcode()).c_str());
                else /* call the handler */
                {
                    SALTICIDAE_LOG_DEBUG("got message %s from %s",
                            std::string(msg).c_str(),
                            std::string(*conn).c_str());
#ifdef SALTICIDAE_MSG_STAT
                    conn->nrecv++;
                    conn->nrecvb += msg.get_length();
#endif
                    it->second(msg, conn);
                }
#endif
                if (++cnt == burst_size) return true;
            }
            return false;
        });
    }

    template<typename Func>
    typename std::enable_if<std::is_constructible<
        typename callback_traits<Func>::msg_type, DataStream &&>::value>::type
    reg_handler(Func handler) {
        using callback_t = callback_traits<Func>;
        set_handler(callback_t::msg_type::opcode,
            [handler](const Msg &msg, const conn_t &conn) {
            handler(typename callback_t::msg_type(msg.get_payload()),
                    static_pointer_cast<typename callback_t::conn_type>(conn));
        });
    }

    template<typename Func>
    inline void set_handler(OpcodeType opcode, Func handler) {
        handler_map[opcode] = handler;
    }

    template<typename MsgType>
    void send_msg(MsgType &&msg, const conn_t &conn);
    inline void _send_msg(Msg &&msg, const conn_t &conn);
    inline void _send_msg_dispatcher(const Msg &msg, const conn_t &conn);

    using ConnPool::listen;
    conn_t connect(const NetAddr &addr) {
        return static_pointer_cast<Conn>(ConnPool::connect(addr));
    }
};

/** Simple network that handles client-server requests. */
template<typename OpcodeType>
class ClientNetwork: public MsgNetwork<OpcodeType> {
    public:
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;

    private:
    std::unordered_map<NetAddr, typename MsgNet::conn_t> addr2conn;

    public:
    class Conn: public MsgNet::Conn {
        friend ClientNetwork;

        public:
        Conn() = default;

        ClientNetwork *get_net() {
            return static_cast<ClientNetwork *>(ConnPool::Conn::get_pool());
        }

        protected:
        void on_setup() override;
        void on_teardown() override;
    };

    using conn_t = ArcObj<Conn>;

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }

    public:
    using Config = typename MsgNet::Config;
    ClientNetwork(const EventContext &ec, const Config &config):
        MsgNet(ec, config) {}

    using MsgNet::send_msg;
    template<typename MsgType>
    void send_msg(MsgType &&msg, const NetAddr &addr);
};

/** Peer-to-peer network where any two nodes could hold a bi-diretional message
 * channel, established by either side. */
template<typename OpcodeType = uint8_t,
        OpcodeType OPCODE_PING = 0xf0,
        OpcodeType OPCODE_PONG = 0xf1>
class PeerNetwork: public MsgNetwork<OpcodeType> {
    public:
    using MsgNet = MsgNetwork<OpcodeType>;
    using Msg = typename MsgNet::Msg;
    using unknown_callback_t = std::function<void(const NetAddr &)>;

    enum IdentityMode {
        IP_BASED,
        IP_PORT_BASED
    };

    class Conn: public MsgNet::Conn {
        friend PeerNetwork;
        NetAddr peer_id;
        TimerEvent ev_timeout;
        void reset_timeout(double timeout);

        public:
        Conn() = default;

        PeerNetwork *get_net() {
            return static_cast<PeerNetwork *>(ConnPool::Conn::get_pool());
        }

        const NetAddr &get_peer() { return peer_id; }

        protected:
        void stop() override {
            ev_timeout.clear();
            MsgNet::Conn::stop();
        }

        void on_setup() override;
        void on_teardown() override;
    };

    using conn_t = ArcObj<Conn>;

    private:
    struct Peer {
        /** connection addr, may be different due to passive mode */
        NetAddr addr;
        /** the underlying connection, may be invalid when connected = false */
        conn_t conn;
        TimerEvent ev_ping_timer;
        TimerEvent ev_retry_timer;
        bool ping_timer_ok;
        bool pong_msg_ok;
        bool connected;

        Peer() = delete;
        Peer(NetAddr addr, conn_t conn, const EventContext &ec):
            addr(addr), conn(conn),
            ev_ping_timer(
                TimerEvent(ec, std::bind(&Peer::ping_timer, this, _1))),
            connected(false) {}
        ~Peer() {}
        Peer &operator=(const Peer &) = delete;
        Peer(const Peer &) = delete;

        void ping_timer(TimerEvent &);
        void reset_ping_timer();
        void send_ping();
        void clear_all_events() {
            if (ev_ping_timer)
                ev_ping_timer.del();
        }
        void reset_conn(conn_t conn);
    };

    std::unordered_map<NetAddr, BoxObj<Peer>> id2peer;
    std::unordered_map<NetAddr, BoxObj<Peer>> id2upeer;
    unknown_callback_t unknown_peer_cb;

    const IdentityMode id_mode;
    double retry_conn_delay;
    double ping_period;
    double conn_timeout;
    uint16_t listen_port;
    bool allow_unknown_peer;

    struct MsgPing {
        static const OpcodeType opcode;
        DataStream serialized;
        uint16_t port;
        MsgPing(uint16_t port) {
            serialized << htole(port);
        }
        MsgPing(DataStream &&s) {
            s >> port;
            port = letoh(port);
        }
    };

    struct MsgPong {
        static const OpcodeType opcode;
        DataStream serialized;
        uint16_t port;
        MsgPong(uint16_t port) {
            serialized << htole(port);
        }
        MsgPong(DataStream &&s) {
            s >> port;
            port = letoh(port);
        }
    };

    void msg_ping(MsgPing &&msg, const conn_t &conn);
    void msg_pong(MsgPong &&msg, const conn_t &conn);
    void _ping_msg_cb(const conn_t &conn, uint16_t port);
    void _pong_msg_cb(const conn_t &conn, uint16_t port);
    bool check_new_conn(const conn_t &conn, uint16_t port);
    void start_active_conn(const NetAddr &paddr);
    static void tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout);
    Peer *get_peer(const NetAddr &id) const;

    protected:
    ConnPool::Conn *create_conn() override { return new Conn(); }
    virtual double gen_conn_timeout() {
        return gen_rand_timeout(retry_conn_delay);
    }

    public:

    class Config: public MsgNet::Config {
        friend PeerNetwork;
        double _retry_conn_delay;
        double _ping_period;
        double _conn_timeout;
        bool _allow_unknown_peer;
        IdentityMode _id_mode;

        public:
        Config(): Config(typename MsgNet::Config()) {}

        Config(const typename MsgNet::Config &config):
            MsgNet::Config(config),
            _retry_conn_delay(2),
            _ping_period(30),
            _conn_timeout(180),
            _allow_unknown_peer(false),
            _id_mode(IP_PORT_BASED) {}


        Config &retry_conn_delay(double x) {
            _retry_conn_delay = x;
            return *this;
        }

        Config &ping_period(double x) {
            _ping_period = x;
            return *this;
        }

        Config &conn_timeout(double x) {
            _conn_timeout = x;
            return *this;
        }

        Config &id_mode(IdentityMode x) {
            _id_mode = x;
            return *this;
        }

        Config &allow_unknown_peer(bool x) {
            _allow_unknown_peer = x;
            return *this;
        }
    };

    PeerNetwork(const EventContext &ec, const Config &config):
        MsgNet(ec, config),
        id_mode(config._id_mode),
        retry_conn_delay(config._retry_conn_delay),
        ping_period(config._ping_period),
        conn_timeout(config._conn_timeout),
        allow_unknown_peer(config._allow_unknown_peer) {
        this->reg_handler(generic_bind(&PeerNetwork::msg_ping, this, _1, _2));
        this->reg_handler(generic_bind(&PeerNetwork::msg_pong, this, _1, _2));
    }

    ~PeerNetwork() { this->stop_workers(); }

    void add_peer(const NetAddr &paddr);
    void del_peer(const NetAddr &paddr);
    bool has_peer(const NetAddr &paddr) const;
    const conn_t get_peer_conn(const NetAddr &paddr) const;
    using MsgNet::send_msg;
    template<typename MsgType>
    void send_msg(MsgType &&msg, const NetAddr &paddr);
    inline void _send_msg(Msg &&msg, const NetAddr &paddr);
    template<typename MsgType>
    void multicast_msg(MsgType &&msg, const std::vector<NetAddr> &paddrs);
    inline void _multicast_msg(Msg &&msg, const std::vector<NetAddr> &paddrs);

    void listen(NetAddr listen_addr);
    conn_t connect(const NetAddr &addr) = delete;
    template<typename Func>
    void reg_unknown_peer_handler(Func cb) { unknown_peer_cb = cb; }
};

/* this callback is run by a worker */
template<typename OpcodeType>
void MsgNetwork<OpcodeType>::Conn::on_read() {
    ConnPool::Conn::on_read();
    auto &recv_buffer = this->recv_buffer;
    auto mn = get_net();
    while (self_ref)
    {
        if (msg_state == Conn::HEADER)
        {
            if (recv_buffer.size() < Msg::header_size) break;
            /* new header available */
            msg = Msg(recv_buffer.pop(Msg::header_size));
            msg_state = Conn::PAYLOAD;
        }
        if (msg_state == Conn::PAYLOAD)
        {
            size_t len = msg.get_length();
            if (recv_buffer.size() < len) break;
            /* new payload available */
            msg.set_payload(recv_buffer.pop(len));
            msg_state = Conn::HEADER;
#ifndef SALTICIDAE_NOCHECKSUM
            if (!msg.verify_checksum())
            {
                SALTICIDAE_LOG_WARN("checksums do not match, dropping the message");
                return;
            }
#endif
            auto conn = static_pointer_cast<Conn>(self());
            while (!mn->incoming_msgs.enqueue(std::make_pair(msg, conn), false))
                std::this_thread::yield();
        }
    }
}

template<typename OpcodeType>
template<typename MsgType>
void MsgNetwork<OpcodeType>::send_msg(MsgType &&msg, const conn_t &conn) {
    return _send_msg(MsgType(std::move(msg)), conn);
}

template<typename OpcodeType>
inline void MsgNetwork<OpcodeType>::_send_msg(Msg &&msg, const conn_t &conn) {
    this->disp_tcall->async_call(
            [this, msg=std::move(msg), conn](ThreadCall::Handle &) {
        try {
            this->_send_msg_dispatcher(msg, conn);
        } catch (...) { this->recoverable_error(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::tcall_reset_timeout(ConnPool::Worker *worker,
                                    const conn_t &conn, double timeout) {
    worker->get_tcall()->async_call([worker, conn, t=timeout](ThreadCall::Handle &) {
        try {
            if (!conn->ev_timeout) return;
            conn->ev_timeout.del();
            conn->ev_timeout.add(t);
            SALTICIDAE_LOG_DEBUG("reset connection timeout %.2f", t);
        } catch (...) { worker->error_callback(std::current_exception()); }
    });
}

/* begin: functions invoked by the dispatcher */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::on_setup() {
    MsgNet::Conn::on_setup();
    auto pn = get_net();
    auto conn = static_pointer_cast<Conn>(this->self());
    auto worker = this->worker;
    assert(!ev_timeout);
    ev_timeout = TimerEvent(worker->get_ec(), [worker, conn](TimerEvent &) {
        try {
            SALTICIDAE_LOG_INFO("peer ping-pong timeout");
            conn->worker_terminate();
        } catch (...) { worker->error_callback(std::current_exception()); }
    });
    /* the initial ping-pong to set up the connection */
    tcall_reset_timeout(worker, conn, pn->conn_timeout);
    pn->send_msg(MsgPing(pn->listen_port), conn);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Conn::on_teardown() {
    MsgNet::Conn::on_teardown();
    auto pn = get_net();
    auto p = pn->get_peer(peer_id);
    if (!p) return;
    if (this != p->conn.get()) return;
    p->ev_ping_timer.del();
    p->connected = false;
    //p->conn = nullptr;
    SALTICIDAE_LOG_INFO("connection lost: %s", std::string(*this).c_str());
    // try to reconnect
    p->ev_retry_timer = TimerEvent(pn->disp_ec,
            [pn, peer_id = this->peer_id](TimerEvent &) {
        try {
            pn->start_active_conn(peer_id);
        } catch (...) { pn->disp_error_cb(std::current_exception()); }
    });
    p->ev_retry_timer.add(pn->gen_conn_timeout());
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_conn(conn_t new_conn) {
    if (conn != new_conn)
    {
        if (conn)
        {
            //SALTICIDAE_LOG_DEBUG("moving send buffer");
            //new_conn->move_send_buffer(conn);
            SALTICIDAE_LOG_INFO("terminating old connection %s", std::string(*conn).c_str());
            conn->disp_terminate();
        }
        addr = new_conn->get_addr();
        conn = new_conn;
    }
    clear_all_events();
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::reset_ping_timer() {
    assert(ev_ping_timer);
    ev_ping_timer.del();
    ev_ping_timer.add(gen_rand_timeout(conn->get_net()->ping_period));
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::send_ping() {
    auto pn = conn->get_net();
    ping_timer_ok = false;
    pong_msg_ok = false;
    tcall_reset_timeout(conn->worker, conn, pn->conn_timeout);
    pn->send_msg(MsgPing(pn->listen_port), conn);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::Peer::ping_timer(TimerEvent &) {
    ping_timer_ok = true;
    if (pong_msg_ok)
    {
        reset_ping_timer();
        send_ping();
    }
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::check_new_conn(const conn_t &conn, uint16_t port) {
    if (conn->peer_id.is_null())
    {   /* passive connections can eventually have ids after getting the port
           number in IP_BASED_PORT mode */
        assert(id_mode == IP_PORT_BASED);
        conn->peer_id.ip = conn->get_addr().ip;
        conn->peer_id.port = port;
    }
    const auto &id = conn->peer_id;
    auto it = id2peer.find(id);
    if (it == id2peer.end())
    {   /* found an unknown peer */
        const auto &addr = conn->get_addr();
        this->user_tcall->async_call([this, id](ThreadCall::Handle &) {
            unknown_peer_cb(id);
        });
        if (allow_unknown_peer)
        {
            auto it2 = id2upeer.find(id);
            if (it2 == id2upeer.end())
                it = id2upeer.insert(std::make_pair(id, new Peer(addr, nullptr, this->disp_ec))).first;
        }
        else
        {
            conn->disp_terminate();
            return true;
        }
    }
    auto p = it->second.get();
    if (p->connected)
    {
        if (conn != p->conn)
        {
            conn->disp_terminate();
            return true;
        }
        return false;
    }
    p->reset_conn(conn);
    p->connected = true;
    p->reset_ping_timer();
    p->send_ping();
    if (p->connected)
    {
        auto color_begin = "";
        auto color_end = "";
        if (logger.is_tty())
        {
            color_begin = TTY_COLOR_BLUE;
            color_end = TTY_COLOR_RESET;
        }
        SALTICIDAE_LOG_INFO("%sPeerNetwork: established connection with %s via %s%s",
            color_begin,
            std::string(conn->peer_id).c_str(), std::string(*conn).c_str(),
            color_end);
    }
    return false;
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::start_active_conn(const NetAddr &addr) {
    auto p = get_peer(addr);
    if (p->connected) return;
    auto conn = static_pointer_cast<Conn>(MsgNet::_connect(addr));
    //assert(p->conn == nullptr);
    p->conn = conn;
    conn->peer_id = addr;
    if (id_mode == IP_BASED)
        conn->peer_id.port = 0;
}

template<typename O, O _, O __>
typename PeerNetwork<O, _, __>::Peer *PeerNetwork<O, _, __>::get_peer(const NetAddr &addr) const {
    auto it = id2peer.find(addr);
    if (it != id2peer.end()) return it->second.get();
    it = id2upeer.find(addr);
    if (it != id2upeer.end()) return it->second.get();
    return nullptr;
}

template<typename OpcodeType>
inline void MsgNetwork<OpcodeType>::_send_msg_dispatcher(const Msg &msg, const conn_t &conn) {
    bytearray_t msg_data = msg.serialize();
    SALTICIDAE_LOG_DEBUG("wrote message %s to %s",
                std::string(msg).c_str(),
                std::string(*conn).c_str());
#ifdef SALTICIDAE_MSG_STAT
    conn->nsent++;
    conn->nsentb += msg.get_length();
#endif
    conn->write(std::move(msg_data));
}
/* end: functions invoked by the dispatcher */

/* begin: functions invoked by the user loop */
template<typename O, O _, O __>
void PeerNetwork<O, _, __>::msg_ping(MsgPing &&msg, const conn_t &conn) {
    uint16_t port = msg.port;
    this->disp_tcall->async_call([this, conn, port](ThreadCall::Handle &) {
        try {
            if (conn->get_mode() == ConnPool::Conn::DEAD) return;
            SALTICIDAE_LOG_INFO("ping from %s, port %u",
                                std::string(*conn).c_str(), ntohs(port));
            if (check_new_conn(conn, port)) return;
            send_msg(MsgPong(this->listen_port), conn);
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::msg_pong(MsgPong &&msg, const conn_t &conn) {
    uint16_t port = msg.port;
    this->disp_tcall->async_call([this, conn, port](ThreadCall::Handle &) {
        try {
            if (conn->get_mode() == ConnPool::Conn::DEAD) return;
            auto p = get_peer(conn->peer_id);
            if (!p)
            {
                SALTICIDAE_LOG_WARN("pong message discarded");
                return;
            }
            if (check_new_conn(conn, port)) return;
            p->pong_msg_ok = true;
            if (p->ping_timer_ok)
            {
                p->reset_ping_timer();
                p->send_ping();
            }
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::listen(NetAddr listen_addr) {
    auto ret = *(static_cast<std::exception_ptr *>(
            this->disp_tcall->call([this, listen_addr](ThreadCall::Handle &h) {
        std::exception_ptr err = nullptr;
        try {
            MsgNet::_listen(listen_addr);
            listen_port = listen_addr.port;
        } catch (...) {
            err = std::current_exception();
        }
        h.set_result(std::move(err));
    }).get()));
    if (ret) std::rethrow_exception(ret);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::add_peer(const NetAddr &addr) {
    this->disp_tcall->async_call([this, addr](ThreadCall::Handle &) {
        try {
            auto it = id2peer.find(addr);
            if (it != id2peer.end())
                throw PeerNetworkError(SALTI_ERROR_PEER_ALREADY_EXISTS);
            auto it2 = id2upeer.find(addr);
            if (it2 != id2peer.end())
            { /* move to the known peer set */
                auto p = std::move(it2->second);
                id2upeer.erase(it2);
                id2peer.insert(std::make_pair(addr, std::move(p)));
            }
            else
                id2peer.insert(std::make_pair(addr, new Peer(addr, nullptr, this->disp_ec)));
            start_active_conn(addr);
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::del_peer(const NetAddr &addr) {
    this->disp_tcall->async_call([this, addr](ThreadCall::Handle &) {
        try {
            auto it = id2peer.find(addr);
            if (it == id2peer.end())
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            it->second->conn->disp_terminate();
            id2peer.erase(it);
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O _, O __>
const typename PeerNetwork<O, _, __>::conn_t
PeerNetwork<O, _, __>::get_peer_conn(const NetAddr &paddr) const {
    auto ret = *(static_cast<std::pair<conn_t, std::exception_ptr> *>(
            this->disp_tcall->call([this, paddr](ThreadCall::Handle &h) {
        conn_t conn;
        std::exception_ptr err = nullptr;
        try {
            auto p = get_peer(paddr);
            if (!p)
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            conn = p->conn;
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) {
            err = std::current_exception();
        }
        h.set_result(std::make_pair(std::move(conn), err));
    }).get()));
    if (ret.second) std::rethrow_exception(ret.second);
    return std::move(ret.first);
}

template<typename O, O _, O __>
bool PeerNetwork<O, _, __>::has_peer(const NetAddr &paddr) const {
    return *(static_cast<bool *>(this->disp_tcall->call(
                [this, paddr](ThreadCall::Handle &h) {
        h.set_result(id2peer.count(paddr));
    }).get()));
}

template<typename O, O _, O __>
template<typename MsgType>
void PeerNetwork<O, _, __>::send_msg(MsgType &&msg, const NetAddr &paddr) {
    return _send_msg(MsgType(std::move(msg)), paddr);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::_send_msg(Msg &&msg, const NetAddr &paddr) {
    this->disp_tcall->async_call(
                [this, msg=std::move(msg), paddr](ThreadCall::Handle &) {
        try {
            auto p = get_peer(paddr);
            if (!p)
                throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
            this->_send_msg_dispatcher(msg, p->conn);
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) { this->recoverable_error(std::current_exception()); }
    });
}

template<typename O, O _, O __>
template<typename MsgType>
void PeerNetwork<O, _, __>::multicast_msg(MsgType &&msg, const std::vector<NetAddr> &paddrs) {
    return _multicast_msg(MsgType(std::move(msg)), paddrs);
}

template<typename O, O _, O __>
void PeerNetwork<O, _, __>::_multicast_msg(Msg &&msg, const std::vector<NetAddr> &paddrs) {
    this->disp_tcall->async_call(
                [this, msg=std::move(msg), paddrs](ThreadCall::Handle &) {
        try {
            for (auto &addr: paddrs)
            {
                auto p = get_peer(addr);
                if (!p)
                    throw PeerNetworkError(SALTI_ERROR_PEER_NOT_EXIST);
                this->_send_msg_dispatcher(msg, p->conn);
            }
        } catch (const PeerNetworkError &) {
            this->recoverable_error(std::current_exception());
        } catch (...) { this->recoverable_error(std::current_exception()); }
    });
}

/* end: functions invoked by the user loop */

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::Conn::on_setup() {
    MsgNet::Conn::on_setup();
    assert(this->get_mode() == Conn::PASSIVE);
    const auto &addr = this->get_addr();
    auto cn = get_net();
    cn->addr2conn.erase(addr);
    cn->addr2conn.insert(
        std::make_pair(addr,
                        static_pointer_cast<Conn>(this->self())));
}

template<typename OpcodeType>
void ClientNetwork<OpcodeType>::Conn::on_teardown() {
    MsgNet::Conn::on_teardown();
    get_net()->addr2conn.erase(this->get_addr());
}

template<typename OpcodeType>
template<typename MsgType>
void ClientNetwork<OpcodeType>::send_msg(MsgType &&msg, const NetAddr &addr) {
    this->disp_tcall->async_call(
            [this, addr, msg=std::forward<MsgType>(msg)](ThreadCall::Handle &) {
        try {
            auto it = addr2conn.find(addr);
            if (it != addr2conn.end())
                send_msg(std::move(msg), it->second);
        } catch (...) { this->disp_error_cb(std::current_exception()); }
    });
}

template<typename O, O OPCODE_PING, O _>
const O PeerNetwork<O, OPCODE_PING, _>::MsgPing::opcode = OPCODE_PING;

template<typename O, O _, O OPCODE_PONG>
const O PeerNetwork<O, _, OPCODE_PONG>::MsgPong::opcode = OPCODE_PONG;

}

#ifdef SALTICIDAE_CBINDINGS
using msgnetwork_t = salticidae::MsgNetwork<_opcode_t>;
using msgnetwork_config_t = msgnetwork_t::Config;
using msgnetwork_conn_t = msgnetwork_t::conn_t;

using peernetwork_t = salticidae::PeerNetwork<_opcode_t>;
using peernetwork_config_t = peernetwork_t::Config;
using peernetwork_conn_t = peernetwork_t::conn_t;
#endif

#else

#ifdef SALTICIDAE_CBINDINGS
typedef struct msgnetwork_t msgnetwork_t;
typedef struct msgnetwork_config_t msgnetwork_config_t;
typedef struct msgnetwork_conn_t msgnetwork_conn_t;

typedef struct peernetwork_t peernetwork_t;
typedef struct peernetwork_config_t peernetwork_config_t;
typedef struct peernetwork_conn_t peernetwork_conn_t;
#endif

#endif

#ifdef SALTICIDAE_CBINDINGS
typedef enum msgnetwork_conn_mode_t {
    CONN_MODE_ACTIVE,
    CONN_MODE_PASSIVE,
    CONN_MODE_DEAD
} msgnetwork_conn_mode_t;

typedef enum peernetwork_id_mode_t {
    ID_MODE_IP_BASED,
    ID_MODE_IP_PORT_BASED
} peernetwork_id_mode_t;

#ifdef __cplusplus
extern "C" {
#endif

void salticidae_injected_msg_callback(const msg_t *msg, msgnetwork_conn_t *conn);

// MsgNetwork
msgnetwork_config_t *msgnetwork_config_new();
void msgnetwork_config_free(const msgnetwork_config_t *self);
void msgnetwork_config_burst_size(msgnetwork_config_t *self, size_t burst_size);
void msgnetwork_config_max_listen_backlog(msgnetwork_config_t *self, int backlog);
void msgnetwork_config_conn_server_timeout(msgnetwork_config_t *self, double timeout);
void msgnetwork_config_seg_buff_size(msgnetwork_config_t *self, size_t size);
void msgnetwork_config_nworker(msgnetwork_config_t *self, size_t nworker);
void msgnetwork_config_queue_capacity(msgnetwork_config_t *self, size_t cap);

msgnetwork_t *msgnetwork_new(const eventcontext_t *ec, const msgnetwork_config_t *config);
void msgnetwork_free(const msgnetwork_t *self);
void msgnetwork_send_msg_by_move(msgnetwork_t *self, msg_t *_moved_msg, const msgnetwork_conn_t *conn);
msgnetwork_conn_t *msgnetwork_connect(msgnetwork_t *self, const netaddr_t *addr);
msgnetwork_conn_t *msgnetwork_conn_copy(const msgnetwork_conn_t *self);
void msgnetwork_conn_free(const msgnetwork_conn_t *self);
void msgnetwork_listen(msgnetwork_t *self, const netaddr_t *listen_addr);
void msgnetwork_start(msgnetwork_t *self);
void msgnetwork_terminate(msgnetwork_t *self, const msgnetwork_conn_t *conn);

typedef void (*msgnetwork_msg_callback_t)(const msg_t *, const msgnetwork_conn_t *, void *userdata);
void msgnetwork_reg_handler(msgnetwork_t *self, _opcode_t opcode, msgnetwork_msg_callback_t cb, void *userdata);

typedef void (*msgnetwork_conn_callback_t)(const msgnetwork_conn_t *, bool, void *userdata);
void msgnetwork_reg_conn_handler(msgnetwork_t *self, msgnetwork_conn_callback_t cb, void *userdata);

msgnetwork_t *msgnetwork_conn_get_net(const msgnetwork_conn_t *conn);
msgnetwork_conn_mode_t msgnetwork_conn_get_mode(const msgnetwork_conn_t *conn);
netaddr_t *msgnetwork_conn_get_addr(const msgnetwork_conn_t *conn);

// PeerNetwork

peernetwork_config_t *peernetwork_config_new();
void peernetwork_config_free(const peernetwork_config_t *self);
void peernetwork_config_retry_conn_delay(peernetwork_config_t *self, double t);
void peernetwork_config_ping_period(peernetwork_config_t *self, double t);
void peernetwork_config_conn_timeout(peernetwork_config_t *self, double t);
void peernetwork_config_id_mode(peernetwork_config_t *self, peernetwork_id_mode_t mode);
msgnetwork_config_t *peernetwork_config_as_msgnetwork_config(peernetwork_config_t *self);

peernetwork_t *peernetwork_new(const eventcontext_t *ec, const peernetwork_config_t *config);
void peernetwork_free(const peernetwork_t *self);
void peernetwork_add_peer(peernetwork_t *self, const netaddr_t *paddr);
void peernetwork_del_peer(peernetwork_t *self, const netaddr_t *paddr);
bool peernetwork_has_peer(const peernetwork_t *self, const netaddr_t *paddr);
const peernetwork_conn_t *peernetwork_get_peer_conn(const peernetwork_t *self, const netaddr_t *paddr);
msgnetwork_t *peernetwork_as_msgnetwork(peernetwork_t *self);
msgnetwork_conn_t *msgnetwork_conn_new_from_peernetwork_conn(const peernetwork_conn_t *conn);
peernetwork_conn_t *peernetwork_conn_copy(const peernetwork_conn_t *self);
void peernetwork_conn_free(const peernetwork_conn_t *self);
void peernetwork_send_msg_by_move(peernetwork_t *self, msg_t * _moved_msg, const netaddr_t *paddr);
void peernetwork_multicast_msg_by_move(peernetwork_t *self, msg_t *_moved_msg, const netaddr_array_t *paddrs);
void peernetwork_listen(peernetwork_t *self, const netaddr_t *listen_addr);

#ifdef __cplusplus
}
#endif
#endif

#endif
