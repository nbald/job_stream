
#include "message.h"
#include "serialization.h"

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>
#include <sys/time.h>

namespace job_stream {
namespace message {

uint64_t Location::getCurrentTimeMs() {
    timeval ts; gettimeofday(&ts, 0);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_usec / 1000);
}


WorkRecord::WorkRecord(const std::string& serialized) {
    serialization::decode(serialized, *this);

    //We were received, so...
    this->source.tsReceived = Location::getCurrentTimeMs();
}


WorkRecord::WorkRecord(const std::vector<std::string>& target, 
        const std::string& work) : reduceTag(0), reduceHomeRank(0), work(work) {
    this->source.hostname = boost::asio::ip::host_name();
    this->source.target = target;
    this->source.tsSent = Location::getCurrentTimeMs();
}


void WorkRecord::chainFrom(const WorkRecord& wr) {
    this->route.insert(this->route.end(), wr.route.begin(), wr.route.end());
    this->route.push_back(wr.source);
    this->reduceHomeRank = wr.reduceHomeRank;
    this->reduceTag = wr.reduceTag;
}


template<typename T>
std::string getAsString(const std::string& payload) {
    T decoded;
    serialization::decode(payload, decoded);
    return boost::lexical_cast<std::string>(decoded);
}


std::string WorkRecord::getWorkAsString() const {
#define TRY_TYPE(T) \
        try { \
            return getAsString<T>(this->work); \
        } \
        catch (const std::exception& e) { \
            /* Do nothing, conversion failed */ \
        }

    TRY_TYPE(std::string);
    TRY_TYPE(uint64_t);
    TRY_TYPE(int64_t);
    TRY_TYPE(int);
    TRY_TYPE(unsigned short);
    TRY_TYPE(short);
    TRY_TYPE(unsigned char);
    TRY_TYPE(char);
    TRY_TYPE(float);
    TRY_TYPE(double);

    std::ostringstream ss;
    ss << "Unrecognized work type for string output: " << this->work;
    throw std::runtime_error(ss.str());

#undef TRY_TYPE
}

} //message
} //job_stream
