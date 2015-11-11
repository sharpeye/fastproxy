/*
 * resolver.hpp
 *
 *  Created on: May 27, 2010
 *      Author: nbryskin
 */

#ifndef RESOLVER_HPP_
#define RESOLVER_HPP_

#include <boost/function.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/bind/placeholders.hpp>
#include <boost/log/sources/channel_logger.hpp>
#include <udns.h>
#include <unbound.h>

#include "common.hpp"

using boost::system::error_code;

class resolver
{
public:
    class iterator
    {
    public:
        iterator(char** ptr=0)
        : ptr(reinterpret_cast<ip::address_v4**>(ptr))
        {
        }
        ip::address_v4& operator * ()
        {
            return **ptr;
        }
        iterator& operator ++ ()
        {
            ptr++;
            return *this;
        }
        operator bool ()
        {
            return *ptr;
        }
    private:
        ip::address_v4** ptr;
    };
    typedef boost::function<void (const boost::system::error_code&, iterator, iterator)> callback;

    static void init();

    resolver(asio::io_service& io, const ip::udp::endpoint& outbound, const ip::udp::endpoint& name_server, bool use_unbound_resolve);
    ~resolver();

    void start();

    int async_resolve(const char* host_name, const callback& completion);
    int cancel(int asyncid);

protected:
    bool unbound_resolve_enabled() const;

    void start_waiting_receive();
    void finished_waiting_receive(const boost::system::error_code& ec);

    void start_waiting_timer();
    void finished_waiting_timer(const error_code& ec);

    static void udns_finished_resolve_raw(dns_ctx* ctx, void* result, void* data);
    static void udns_finished_resolve(int status, const dns_rr_a4& response, const callback& completion);

    static void unbound_finished_resolve_raw(void* data, int status, ub_result* result);
    static void unbound_finished_resolve(int status, ub_result* result, const callback& completion);

private:
    typedef boost::function<void (int, const dns_rr_a4&)> resolve_callback_internal;

    ip::udp::socket socket;
    asio::deadline_timer timer;
    dns_ctx* udns_context;
    ub_ctx* unbound_context;
    bool use_unbound;
    static logger log;
};

#endif /* RESOLVER_HPP_ */
