#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <execinfo.h>
#include <pwd.h>
#include <grp.h>

#include <iostream>
#include <algorithm>
#include <boost/asio.hpp>
#include <boost/system/linux_error.hpp>
#include <boost/exception/all.hpp>
#include <boost/phoenix/bind.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/channel_logger.hpp>
#include <boost/log/sources/record_ostream.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/filesystem.hpp>

#include <pthread.h>

#include "fastproxy.hpp"
#include "proxy.hpp"
#include "statistics.hpp"

fastproxy* fastproxy::instance_;
logger fastproxy::log = logger(keywords::channel = "fastproxy");

fastproxy::fastproxy()
{
    instance_ = this;
}

fastproxy& fastproxy::instance()
{
    return *instance_;
}

typedef std::vector<std::string> string_vec;
typedef std::vector<ip::tcp::endpoint> endpoint_vec;

namespace boost { namespace asio { namespace ip {
template<class protocol>
bool operator >> (std::basic_istream<char>& stream, ip::basic_endpoint<protocol>& endpoint)
{
	std::string str;
	stream >> str;
	std::string::iterator colon = std::find( str.begin(), str.end(), ':' );
	endpoint.address( ip::address::from_string( std::string( str.begin(), colon ) ) );
	if( colon != str.end() )
		endpoint.port( atoi( &*colon + 1 ) );
	return true;
}
}}}

void fastproxy::parse_config(int argc, char* argv[])
{
    po::options_description desc("Allowed options");
    desc.add_options()
            ("help", "produce help message")
            ("resolve-library", po::value<std::string>()->default_value("unbound"), "DNS library to use for resolve ('udns', 'unbound')")

            ("ingoing-http", po::value<endpoint_vec>()->required(), "http listening addresses")
            ("ingoing-stat", po::value<std::string>()->required(), "statistics listening socket")

            ("outgoing-http", po::value<ip::tcp::endpoint>()->default_value(ip::tcp::endpoint()), "outgoing address for HTTP requests")
            ("outgoing-ns", po::value<ip::udp::endpoint>()->default_value(ip::udp::endpoint()), "outgoing address for NS lookup")

            ("log-level", po::value<int>()->default_value(2), "logging level")
            ("log-channel", po::value<string_vec>(), "logging channel")

            ("receive-timeout", po::value<time_duration::sec_type>()->default_value(3600), "timeout for receive operations (in seconds)")
            ("connect-timeout", po::value<time_duration::sec_type>()->default_value(3), "timeout for connect operation (in seconds)")
            ("resolve-timeout", po::value<time_duration::sec_type>()->default_value(3), "time out for resolve operation for 'unbound' (in seconds)")

            ("udns-name-server", po::value<ip::udp::endpoint>(), "name server address for 'udns' library")

            ("allow-header", po::value<string_vec>()->default_value(string_vec(), "any"), "allowed header for requests")
            ("rename-header", po::value<string_vec>()->default_value(string_vec(), ""), "header rename rule (<original name>:<new name>), only allowed headers are supported")

            ("stat-socket-user", po::value<std::string>()->default_value(getpwuid(getuid())->pw_name), "user for statistics socket")
            ("stat-socket-group", po::value<std::string>()->default_value(getgrgid(getgid())->gr_name), "group for statistics socket")

            ("stop-after-init", po::value<bool>()->default_value(false), "raise SIGSTOP after initialization (Upstart support)")
            ("error-page-dir", po::value<std::string>()->default_value("/etc/fastproxy/errors"), "directory where error pages are located");

    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help"))
        {
            std::cout << desc << std::endl;
            exit(1);
        }

        std::string resolve_library = vm["resolve-library"].as<std::string>();
        if (resolve_library == "udns")
        {
            if (vm.count("udns-name-server") == 0)
            {
                throw boost::program_options::required_option("udns-name-server");
            }
        }
        else if (resolve_library == "unbound")
        {
            // No specific options yet
        }
        else
        {
            throw boost::program_options::invalid_option_value(resolve_library);
        }
        po::notify(vm);
    }
    catch (const boost::program_options::error& exc)
    {
        std::cout << desc << std::endl;
        throw;
    }
}

bool check_channel(const boost::log::value_ref<std::string>& channel)
{
    return fastproxy::instance().check_channel_impl(*channel);
}

bool fastproxy::check_channel_impl(const std::string& channel) const
{
    return channels.find(channel) != channels.end();
}

void fastproxy::init_logging()
{
    if (vm.count("log-channel"))
    {
        auto chans = vm["log-channel"].as<string_vec>();
        channels.insert(chans.begin(), chans.end());
    }

    boost::log::add_common_attributes();
	boost::log::add_console_log
	(
		std::cerr,
		keywords::format = "[%TimeStamp%]: %Channel%: %_%"
	);

    boost::log::core::get()->set_filter
    (
		boost::log::trivial::severity >= vm["log-level"].as<int>() &&
		boost::phoenix::bind( &check_channel, boost::log::expressions::attr<std::string>("Channel"))
    );
}

void fastproxy::init_signals()
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    int s = pthread_sigmask(SIG_BLOCK, &set, 0);
    if (s != 0)
    {
        perror("pthread_sigmask");
        exit(EXIT_FAILURE);
    }

    sw.reset(new signal_waiter(io));
    sw->add_signal(SIGTERM);
    sw->add_signal(SIGQUIT);
    sw->add_signal(SIGINT);
    start_waiting_for_quit();
}

void fastproxy::start_waiting_for_quit()
{
    sw->async_wait(boost::bind(&fastproxy::quit, this, placeholders::error()));
}

void fastproxy::quit(const error_code& ec)
{
    if (ec)
    {
        TRACE_ERROR(ec);
        start_waiting_for_quit();
    }
    else
    {
        io.stop();
    }
}

void fastproxy::init_statistics()
{
    const std::string& stat_sock = vm["ingoing-stat"].as<std::string>();
    boost::filesystem::remove(stat_sock);
    s.reset(new statistics(io, stat_sock));
    errno = 0;
    passwd* pwnam = getpwnam(vm["stat-socket-user"].as<std::string>().c_str());
    if (!pwnam)
    {
        perror(("could not find user " + vm["stat-socket-user"].as<std::string>()).c_str());
        exit(1);
    }
    group* grnam = getgrnam(vm["stat-socket-group"].as<std::string>().c_str());
    if (!grnam)
    {
        perror(("could not find group " + vm["stat-socket-group"].as<std::string>()).c_str());
        exit(1);
    }
    int res = chown(stat_sock.c_str(), pwnam->pw_uid, grnam->gr_gid);
    if (res != 0)
        perror(("chown(" + stat_sock + ", " + vm["stat-socket-user"].as<std::string>() + ", " + 
			vm["stat-socket-group"].as<std::string>() + ")").c_str());
}

void fastproxy::init_proxy()
{
    bool use_unbound_resolve = (vm["resolve-library"].as<std::string>() == "unbound");

    ip::udp::endpoint name_server;

    if (!use_unbound_resolve)
    {
        name_server = vm["udns-name-server"].as<ip::udp::endpoint>();
    }

    p.reset(new proxy(io, vm["ingoing-http"].as<endpoint_vec>(),
            vm["outgoing-http"].as<ip::tcp::endpoint>(),
            vm["outgoing-ns"].as<ip::udp::endpoint>(),
            name_server,
            boost::posix_time::seconds(vm["receive-timeout"].as<time_duration::sec_type>()),
            boost::posix_time::seconds(vm["connect-timeout"].as<time_duration::sec_type>()),
            boost::posix_time::seconds(vm["resolve-timeout"].as<time_duration::sec_type>()),
            vm["allow-header"].as<string_vec>(),
            vm["rename-header"].as<string_vec>(),
            vm["error-page-dir"].as<std::string>(),
            use_unbound_resolve));
}

void fastproxy::init_resolver()
{
    resolver::init();
}

proxy* fastproxy::find_proxy()
{
    return p.get();
}

void fastproxy::init(int argc, char* argv[])
{
    parse_config(argc, argv);
    init_logging();
    init_resolver();
    init_signals();

    init_statistics();
    init_proxy();

    if (vm["stop-after-init"].as<bool>())
        raise(SIGSTOP);
}

void fastproxy::run()
{
    s->start();
    p->start();

//    io.run();
    for (;;)
    {
        if (io.poll() == 0)
        {
            if (io.run_one() == 0)
                break;
            statistics::increment("runs");
        }
        statistics::increment("loops");
    }
}

void terminate()
{
    void* array[50];
    int size = backtrace(array, 50);

    std::cerr << "backtrace returned " << size << " frames" << std::endl;

    char ** messages = backtrace_symbols(array, size);

    for (int i = 0; i < size && messages != NULL; ++i)
    {
        std::cerr << "[bt]: (" << i << ") " << messages[i] << std::endl;
    }
    std::cerr << std::endl;

    free(messages);

    abort();
}


int main(int argc, char* argv[])
{
//    try
//    {
//        std::set_terminate(terminate);

        fastproxy f;
        f.init(argc, argv);
        f.run();
//    }
//    catch (const boost::exception& e)
//    {
//        std::cerr << boost::diagnostic_information(e);
//    }
    return 0;
}
