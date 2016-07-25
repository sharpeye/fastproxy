/*
 * proxy.cpp
 *
 *  Created on: May 31, 2010
 *      Author: nbryskin
 */

#include <iostream>
#include <fstream>
#include <functional>
#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>

#include "proxy.hpp"
#include "statistics.hpp"
#include "session.hpp"

logger proxy::log = logger(keywords::channel = "proxy");
typedef std::ios ios;

bool session_less(const session& lhs, const session& rhs)
{
    return lhs.get_id() < rhs.get_id();
}

proxy::proxy(asio::io_service& io, std::vector<ip::tcp::endpoint> inbound, const ip::tcp::endpoint& outbound_http,
             const ip::udp::endpoint& outbound_ns, const ip::udp::endpoint& name_server,
             const time_duration& receive_timeout, const time_duration& connect_timeout,
             const time_duration& resolve_timeout, const std::vector<std::string>& allowed_headers,
             const std::vector<std::string>& rename_headers,
             const std::string error_pages_dir, bool use_unbound_resolve)
    : resolver_(io, outbound_ns, name_server, use_unbound_resolve)
    , outbound_http(outbound_http)
    , receive_timeout(receive_timeout)
    , connect_timeout(connect_timeout)
    , resolve_timeout(resolve_timeout)
    , sessions(std::ptr_fun(session_less))
{
    headers.push_back("");
    lstring empty(headers.back().c_str());

    // Construct header mapping, every header which is mapped implies that
    // it is allowed (and the same map can be used for mapped and allowed
    // headers).
    for (size_t i = 0; i < rename_headers.size(); i++)
    {
        std::vector<std::string> value;

        // Assume that parameters validation was done before and
        // we deal with exactly 2 values separated with ':'.
        boost::split(value, rename_headers[i], boost::is_any_of(":"));

        headers.push_back(value[0]);
        lstring original(headers.back().c_str());

        headers.push_back(value[1]);
        lstring replacement(headers.back().c_str());

        this->allowed_headers[original] = replacement;
    }

    // For allowed (not mapped) headers there is no replacement
    // value. Empty string is used to indicate the case.
    for (size_t i = 0; i < allowed_headers.size(); i++)
    {
        headers.push_back(allowed_headers[i]);
        lstring header(headers.back().c_str());
        if (this->allowed_headers.find(header) == this->allowed_headers.end())
        {
            this->allowed_headers[header] = empty;
        }
    }

    for (int httpec = HTTP_BEGIN; httpec < HTTP_END; ++httpec)
    {
        std::ifstream page_file((boost::format("%1%/%2%.http") % error_pages_dir % httpec).str(), ios::in|ios::binary|ios::ate);
        if (!page_file.is_open())
            continue;
        std::ifstream::pos_type size = page_file.tellg();
        page_file.seekg(0, ios::beg);
        error_pages[httpec - HTTP_BEGIN].resize(size);
        page_file.read(&*(error_pages[httpec - HTTP_BEGIN].begin()), size);
    }

    assert(!inbound.empty());

    for (auto it = inbound.begin(); it != inbound.end(); ++it)
    {
        this->acceptors.push_back(boost::shared_ptr<ip::tcp::acceptor>(new ip::tcp::acceptor(io, *it)));
    }
}

// called by main (parent)
void proxy::start()
{
    for (auto it = this->acceptors.begin(); it != acceptors.end(); ++it)
        start_accept(**it);
    resolver_.start();
    TRACE() << "started";
}

// called by session (child)
resolver& proxy::get_resolver()
{
    return resolver_;
}

// called by session (child)
void proxy::finished_session(session* session, const boost::system::error_code& ec)
{
    TRACE_ERROR(ec) << session->get_id();
    std::size_t c = sessions.erase(*session);
    if (c != 1)
        TRACE() << "erased " << c << " items. total " << sessions.size() << " items";
    assert(c == 1);
}

void proxy::start_accept(ip::tcp::acceptor& acceptor)
{
    std::unique_ptr<session> new_sess(new session(acceptor.get_io_service(), *this));
    acceptor.async_accept(new_sess->socket(), boost::bind(&proxy::handle_accept, this, placeholders::error(), new_sess.get(), boost::ref(acceptor)));
    new_sess.release();
}

void proxy::handle_accept(const boost::system::error_code& ec, session* new_session, ip::tcp::acceptor& acceptor)
{
    std::unique_ptr<session> session_ptr(new_session);
    TRACE_ERROR(ec);
    if (ec)
        return;

    start_accept(acceptor);
    start_session(session_ptr.get());
    session_ptr.release();
}

void proxy::start_session(session* new_session)
{
    TRACE() << new_session;
    bool inserted = sessions.insert(new_session).second;
    assert(inserted);
    new_session->start();
}

void proxy::dump_channels_state() const
{
    for (session_cont::const_iterator it = sessions.begin(); it != sessions.end(); ++it)
    {
        BOOST_LOG_SEV(log, severity_level::debug)
                << it->get_id()
                << " reqch: " << it->get_request_channel().get_state()
                << " rspch: " << it->get_response_channel().get_state()
                << " opened: " << it->get_opened_channels();
    }
}

const ip::tcp::endpoint& proxy::get_outgoing_endpoint() const
{
    return outbound_http;
}

const time_duration& proxy::get_receive_timeout() const
{
    return receive_timeout;
}

const time_duration& proxy::get_connect_timeout() const
{
    return connect_timeout;
}

const time_duration& proxy::get_resolve_timeout() const
{
    return resolve_timeout;
}

const headers_type& proxy::get_allowed_headers() const
{
    return allowed_headers;
}

asio::const_buffer proxy::get_error_page(http_error_code httpec) const
{
    if (httpec < HTTP_BEGIN || httpec >= HTTP_END)
        return asio::const_buffer();
    const std::vector<char>& error_page = error_pages[httpec - HTTP_BEGIN];
    return asio::const_buffer(&*error_page.begin(), error_page.size());
}
