/*
 * channel.hpp
 *
 *  Created on: May 24, 2010
 *      Author: nbryskin
 */

#ifndef CHANNEL_HPP_
#define CHANNEL_HPP_

#include <functional>
#include "common.hpp"
#include <boost/asio.hpp>
#include <boost/utility.hpp>

using boost::system::error_code;

class session;

class channel : public boost::noncopyable
{
public:
	typedef boost::function<void(const error_code&, std::size_t)> io_handler;
	struct io_handler_wrapper
	{
		io_handler* impl;

		explicit io_handler_wrapper(io_handler& impl)
			: impl(&impl)
		{}

		void operator () (error_code, std::size_t); // To meet the requirements of Read/WriteHandler concept
	};

    // first_input_stat: increment "first_input_time" statistic by elapse from start time
    channel(ip::tcp::socket& input, ip::tcp::socket& output, session& parent_session, const time_duration& input_timeout, bool first_input_stat=false);
    ~channel();

    void start();

    enum state
    {
        created,
        waiting_input,
        waiting_output,
        splicing_input,
        splicing_output,
        finished,
    };
    state get_state() const;

protected:
    void start_waiting();
    void start_waiting_input();
    void start_waiting_output();

    void finished_waiting_input(const error_code& ec, std::size_t);
    void finished_waiting_output(const error_code& ec, std::size_t);

    void input_timeouted(const error_code& ec);

    void splice_from_input();
    void splice_to_output();

    void finished_splice();
    void finish(const error_code& ec);

    void splice(int from, int to, long& spliced, error_code& ec);

private:
    ip::tcp::socket& input;
    ip::tcp::socket& output;
    asio::deadline_timer input_timer;
    time_duration input_timeout;
    int pipe[2];
    long pipe_size;
    session& parent_session;
    static const std::size_t size_of_operation = sizeof(asio::detail::reactive_null_buffers_op<io_handler_wrapper>);
	io_handler input_handler;
    char space_for_input_op[size_of_operation];
	io_handler output_handler;
    char space_for_output_op[size_of_operation];

    // statistics data
    long splices_count;
    long bytes_count;
    state current_state;
    bool first_input;

    static logger log;
};

template<class stream_type>
stream_type& operator << (stream_type& stream, channel::state state)
{
    switch (state)
    {
        case channel::created:
            stream << "created";
            break;
        case channel::waiting_input:
            stream << "waiting_input";
            break;
        case channel::waiting_output:
            stream << "waiting_output";
            break;
        case channel::splicing_input:
            stream << "splicing_input";
            break;
        case channel::splicing_output:
            stream << "splicing_output";
            break;
        case channel::finished:
            stream << "finished";
            break;
        default:
            stream << "unknown status";
    }
    return stream;
}

#endif /* CHANNEL_HPP_ */
