// Copyright (c) 2017-2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//******************************************************************************
//******************************************************************************

#include <xbridge/util/settings.h>
#include <xbridge/util/txlog.h>
#include <xbridge/xuiconnector.h>

#include <util/system.h>

#include <string>
#include <sstream>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>

boost::mutex txlogLocker;

//******************************************************************************
//******************************************************************************
// static
std::string TXLOG::m_logFileName;

//******************************************************************************
//******************************************************************************
TXLOG::TXLOG()
    : std::basic_stringstream<char, std::char_traits<char>,
                    boost::pool_allocator<char> >()
{
    *this << "\n"
          << boost::posix_time::second_clock::local_time()
          << " [0x" << boost::this_thread::get_id() << "] ";
}

//******************************************************************************
//******************************************************************************
// static
std::string TXLOG::logFileName()
{
    return m_logFileName;
}

//******************************************************************************
//******************************************************************************
TXLOG::~TXLOG()
{
    boost::mutex::scoped_lock l(txlogLocker);

    // const static std::string path     = settings().logPath().size() ? settings().logPath() : settings().appPath();
    const static bool logToFile       = true; // !path.empty();
    static boost::gregorian::date day =
            boost::gregorian::day_clock::local_day();
    if (m_logFileName.empty())
    {
        m_logFileName    = makeFileName();
    }

    // std::cout << str().c_str();

    try
    {
        if (logToFile)
        {
            boost::gregorian::date tmpday =
                    boost::gregorian::day_clock::local_day();

            if (day != tmpday)
            {
                m_logFileName = makeFileName();
                day = tmpday;
            }

            std::ofstream file(m_logFileName.c_str(), std::ios_base::app);
            file << str().c_str();
        }
    }
    catch (std::exception &)
    {
    }
}

//******************************************************************************
//******************************************************************************
// static
std::string TXLOG::makeFileName()
{
    boost::filesystem::path directory = GetDataDir(false) / "log-tx";
    boost::filesystem::create_directory(directory);

    auto lt = boost::posix_time::second_clock::local_time();
    auto df = new boost::gregorian::date_facet("%Y%m%dT%H%M%S");
    std::ostringstream ss;
    ss.imbue(std::locale(ss.getloc(), df));
    ss << lt.date();
    return directory.string() + "/" +
            "xbridgep2p_" + ss.str() + ".log";
}
