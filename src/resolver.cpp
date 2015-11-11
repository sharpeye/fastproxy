/*
 * resolver.cpp
 *
 *  Created on: May 27, 2010
 *      Author: nbryskin
 */

#include <boost/function.hpp>
#include <boost/log/sources/channel_feature.hpp>
#include "resolver.hpp"

logger resolver::log = logger(keywords::channel = "resolver");

struct ub_create_error: std::exception { char const* what() const throw() { return "failed to create unbound context"; } };
struct ub_config_error: std::exception { char const* what() const throw() { return "failed to configure libunbound"; } };

void resolver::init()
{
    dns_init(0, 0);
}

resolver::resolver(asio::io_service& io, const ip::udp::endpoint& outbound, const ip::udp::endpoint& name_server, bool use_unbound_resolve)
    : socket(io)
    , timer(io)
    , udns_context(dns_new(0))
    , unbound_context(ub_ctx_create())
    , use_unbound(use_unbound_resolve)
{
    if (unbound_resolve_enabled())
    {
        if(!unbound_context)
            throw ub_create_error();

        if (ub_ctx_set_option(unbound_context, const_cast<char*>("outgoing-interface:"), const_cast<char*>(outbound.address().to_string().c_str()))) throw ub_config_error();
        if (ub_ctx_set_option(unbound_context, const_cast<char*>("use-syslog:"), const_cast<char*>("yes"))) throw ub_config_error();
        if (ub_ctx_set_option(unbound_context, const_cast<char*>("module-config:"), const_cast<char*>("iterator"))) throw ub_config_error();
        if (ub_ctx_set_option(unbound_context, const_cast<char*>("verbosity:"), const_cast<char*>("1"))) throw ub_config_error();
        if (ub_ctx_set_option(unbound_context, const_cast<char*>("outgoing-range:"), const_cast<char*>("4096"))) throw ub_config_error();
        if (ub_ctx_set_option(unbound_context, const_cast<char*>("num-queries-per-thread:"), const_cast<char*>("4096"))) throw ub_config_error();
        int fd = ub_fd(unbound_context);
        socket.assign(ip::udp::v4(), fd);
    }
    else
    {
        dns_add_serv_s(udns_context, 0);
        dns_add_serv_s(udns_context, name_server.data());
        socket.assign(ip::udp::v4(), dns_open(udns_context));
        socket.bind(outbound);
        socket.connect(name_server);
    }
}

resolver::~resolver()
{
    dns_free(udns_context);
    ub_ctx_delete(unbound_context);
}

bool resolver::unbound_resolve_enabled() const
{
    return use_unbound;
}

void resolver::start()
{
    start_waiting_receive();
}

int resolver::async_resolve(const char* host_name, const callback& completion)
{
    TRACE() << host_name;

    int asyncid = 0;

    if (unbound_resolve_enabled())
    {
        int retval = ub_resolve_async(unbound_context, const_cast<char*>(host_name),
            1 /* TYPE A (IPv4 address) */, 
            1 /* CLASS IN (internet) */, 
            const_cast<callback*>(&completion), &resolver::unbound_finished_resolve_raw, &asyncid);
        if(retval != 0)
            completion(boost::system::error_code(retval, boost::system::get_generic_category()), 0, 0);
    }
    else
    {
        dns_query* query = dns_submit_p(udns_context, host_name, DNS_C_IN, DNS_T_A, 0, dns_parse_a4, &resolver::udns_finished_resolve_raw, const_cast<callback*>(&completion));
        if (query == 0)
            completion(boost::system::error_code(dns_status(udns_context), boost::system::get_generic_category()), 0, 0);
        start_waiting_timer();
    }

    return asyncid;
}

int resolver::cancel(int asyncid)
{
    if (unbound_resolve_enabled())
    {
        return ub_cancel(unbound_context, asyncid);
    }

    return 0;
}

void resolver::start_waiting_receive()
{
    TRACE();
    socket.async_receive(asio::null_buffers(), boost::bind(&resolver::finished_waiting_receive, this, placeholders::error));
}

void resolver::finished_waiting_receive(const boost::system::error_code& ec)
{
    TRACE_ERROR(ec);
    if (ec)
        return;

    if (unbound_resolve_enabled())
    {
        ub_process(unbound_context);
    }
    else
    {
        dns_ioevent(udns_context, 0);
    }

    start_waiting_receive();

    if (!unbound_resolve_enabled())
    {
        start_waiting_timer();
    }
}

void resolver::start_waiting_timer()
{
    int seconds = dns_timeouts(udns_context, -1, 0);
    TRACE() << seconds;
    if (seconds < 0)
        return;
    timer.expires_from_now(asio::deadline_timer::duration_type(0, 0, seconds));
    timer.async_wait(boost::bind(&resolver::finished_waiting_timer, this, placeholders::error));
}

void resolver::finished_waiting_timer(const error_code& ec)
{
    TRACE_ERROR(ec);
    if (ec)
        return;

    dns_ioevent(udns_context, 0);

    start_waiting_timer();
}

void resolver::udns_finished_resolve_raw(dns_ctx* ctx, void* result, void* data)
{
    const callback& completion = *static_cast<const callback*>(data);
    dns_rr_a4& response = *static_cast<dns_rr_a4*>(result);

    int status = dns_status(ctx);
    udns_finished_resolve(status, response, completion);

    free(result);
}

void resolver::unbound_finished_resolve_raw(void* data, int status, ub_result* result)
{
    const callback& completion = *static_cast<const callback*>(data);
    unbound_finished_resolve(status, result, completion);
    ub_resolve_free(result);
}

void resolver::udns_finished_resolve(int status, const dns_rr_a4& response, const callback& completion)
{
    TRACE() << status;
    iterator begin, end;
    boost::system::error_code ec = boost::system::error_code(status, boost::system::get_generic_category());

    std::vector<char*> addrs;

    if (status >= 0)
    {
        for (int i = 0; i < response.dnsa4_nrr; i++)
        {
            addrs.push_back(reinterpret_cast<char*>(response.dnsa4_addr + i));
        }

        begin   = &addrs[0];
        end     = &addrs[addrs.size()];
    }
    completion(ec, begin, end);
}

void resolver::unbound_finished_resolve(int status, ub_result* result, const callback& completion)
{
    TRACE() << status;
    iterator begin, end;
    boost::system::error_code ec;
    if (status == 0)
    {
        if (result->havedata)
        {
            begin = result->data;
            for (end = begin; end; ++end);
        }
        else
        {
            ec = boost::system::error_code(result->rcode ? result->rcode : boost::system::errc::operation_canceled, boost::system::get_generic_category());
        }
    }
    else
    {
        ec = boost::system::error_code(status, boost::system::get_generic_category());
    }
    completion(ec, begin, end);
}
