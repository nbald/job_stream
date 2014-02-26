#ifndef JOB_STREAM_PROCESSOR_H_
#define JOB_STREAM_PROCESSOR_H_

#include "job.h"
#include "message.h"
#include "module.h"
#include "yaml.h"

#include <boost/mpi.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <deque>
#include <functional>
#include <memory>
#include <list>

namespace job_stream {
namespace processor {
    class AnyUniquePtr;
}
}

namespace std {
    void swap(job_stream::processor::AnyUniquePtr& p1,
            job_stream::processor::AnyUniquePtr& p2);
}

namespace job_stream {
namespace processor {

/* Debug flag when testing library; 
  1 = basic messages (min output),
  2 = very, very verbose */
extern const int JOB_STREAM_DEBUG;


/** Generic thread-safe queue class with unique_ptrs. */
template<typename T>
class ThreadSafeQueue {
public:
    void push(std::unique_ptr<T> value) {
        boost::mutex::scoped_lock lock(this->mutex);
        this->queue.push_back(std::move(value));
    }

    bool pop(std::unique_ptr<T>& value) {
        boost::mutex::scoped_lock lock(this->mutex);
        if (this->queue.empty()) {
            return false;
        }
        value = std::move(this->queue.front());
        this->queue.pop_front();
        return true;
    }

    size_t size() {
        boost::mutex::scoped_lock lock(this->mutex);
        return this->queue.size();
    }

private:
    std::deque<std::unique_ptr<T>> queue;
    boost::mutex mutex;
};



/** A cheat for a unique_ptr to any class which can be retrieved later and is
    properly deleted.  Application is responsible for keeping track of the type
    though.  This just prevents you from needing to write a tricky destructor,
    so you can focus on logic. */
class AnyUniquePtr {
public:
    AnyUniquePtr() {}

    template<class T>
    AnyUniquePtr(std::unique_ptr<T> ptr) : 
            _impl(new AnyPtrImpl<T>(std::move(ptr))) {}

    AnyUniquePtr(AnyUniquePtr&& other) : _impl(std::move(other._impl)) {}

    virtual ~AnyUniquePtr() {}

    operator bool() const {
        return (bool)this->_impl;
    }

    template<class T>
    AnyUniquePtr& operator=(std::unique_ptr<T> ptr) {
        this->_impl.reset(new AnyPtrImpl<T>(std::move(ptr)));
    }

    template<class T>
    T* get() { 
        if (!this->_impl) {
            return 0;
        }
        return (T*)this->_impl->get();
    }

    void reset() {
        this->_impl.reset();
    }

    template<class T>
    void reset(T* ptr) {
        this->_impl.reset(new AnyPtrImpl<T>(std::unique_ptr<T>(ptr)));
    }

    void swapWith(AnyUniquePtr& other) {
        std::swap(this->_impl, other._impl);
    }

private:
    struct AnyPtrImplBase {
        AnyPtrImplBase(void* ptr) : void_ptr(ptr) {}
        virtual ~AnyPtrImplBase() {}

        void* get() { return void_ptr; }

    protected:
        void* void_ptr;
    };

    template<class T>
    struct AnyPtrImpl : public AnyPtrImplBase {
        AnyPtrImpl(std::unique_ptr<T> ptr) : AnyPtrImplBase((void*)ptr.get()), 
                t_ptr(std::move(ptr)) {}

        virtual ~AnyPtrImpl() {}
    private:
        std::unique_ptr<T> t_ptr;
    };

    std::unique_ptr<AnyPtrImplBase> _impl;
};



/** Holds information about a received MPI message, so that we process them in
    order. */
struct MpiMessage {
    /** The MPI message tag that this message belongs to */
    int tag;

    MpiMessage(int tag, AnyUniquePtr data);

    /** Kind of a factory method - depending on tag, deserialize message into
        the desired message class, and put it in data. */
    MpiMessage(int tag, std::string&& message);

    MpiMessage(MpiMessage&& other) : tag(other.tag) {
        std::swap(this->data, other.data);
        std::swap(this->encodedMessage, other.encodedMessage);
    }

    ~MpiMessage() {}

    std::string serialized() const;

    template<typename T>
    T* getTypedData() const {
        this->_ensureDecoded();
        return this->data.get<T>();
    }

private:
    /** An owned version of the data in this message - deleted manually in
        destructor.  We would use unique_ptr, but we don't know the type of our 
        data! */
    mutable AnyUniquePtr data;

    /** The string representing this message, serialized. */
    mutable std::string encodedMessage;

    /** If data == 0, populate it by deserializing encodedMessage. */
    void _ensureDecoded() const;
};



struct ProcessorReduceInfo {
    /** The number of child reduceTags this one is waiting for */
    int childTagCount;

    /** Any messages waiting on this tag to resume (childTagCount != 0, waiting
        for 0). */
    std::vector<std::shared_ptr<MpiMessage>> messagesWaiting;

    /** The workCount from our reduction; used to tell when a ring is done
        processing and can be settled (marked done). */
    uint64_t workCount;

    /** The parent's reduceTag */
    uint64_t parentTag;

    /* The ReduceBase that contains our tag.  We track this so we can call 
       done on it.  NULL for global ring, of course.*/
    job::ReducerBase* reducer;

    ProcessorReduceInfo() : childTagCount(0), parentTag(0), reducer(0), 
            workCount(0) {}
};



/** Information tied to an in-progress send */
struct ProcessorSendInfo {
    boost::mpi::request request;
    std::vector<uint64_t> reduceTags;

    ProcessorSendInfo(boost::mpi::request request,
            std::vector<uint64_t> reduceTags) : request(request),
                reduceTags(std::move(reduceTags)) {}
};



/** Handles communication and job dispatch, as well as input streaming */
class Processor {
    friend class job::SharedBase;
    friend class module::Module;

public:
    /* MPI message tags */
    enum ProcessorSendTag {
        TAG_WORK,
        TAG_REDUCE_WORK,
        TAG_DEAD_RING_TEST,
        TAG_DEAD_RING_IS_DEAD,
        TAG_STEAL,
        /** A group of messages - tag, body, tag, body, etc. */
        TAG_GROUP,
        /** Placeholder for number of tags */
        TAG_COUNT,
    };

    /*  Profiler categories.  User time is the most important distinction; 
        others are internal. */
    enum ProcessorTimeType {
        TIME_USER,
        TIME_SYSTEM,
        TIME_COMM,
        //Placeholder for number of types
        TIME_COUNT,
    };

    /** Used to keep track of time spent not working (vs working). */
    class WorkTimer {
    public:
        WorkTimer(Processor* p, ProcessorTimeType timeType) : processor(p) {
            this->processor->_pushWorkTimer(timeType);
        }
        ~WorkTimer() {
            this->processor->_popWorkTimer();
        }

    private:
        Processor* processor;
    };

    static void addJob(const std::string& typeName, 
            std::function<job::JobBase* ()> allocator);
    static void addReducer(const std::string& typeName, 
            std::function<job::ReducerBase* ()> allocator);

    Processor(std::unique_ptr<boost::mpi::environment> env, 
            boost::mpi::communicator world, 
            const YAML::Node& config);
    ~Processor();


    /** Add work to our workInQueue.  We now own wr. */
    void addWork(std::unique_ptr<message::WorkRecord> wr);
    /** Allocate and return a new tag for reduction operations. */
    uint64_t getNextReduceTag();
    /** Return this Processor's rank */
    int getRank() const { return this->world.rank(); }
    /** Run all modules defined in config; inputLine (already trimmed) 
        determines whether we are using one row of input (the inputLine) or 
        stdin (if empty) */
    void run(const std::string& inputLine);
    /** Start a dead ring test for the given reduceTag */
    void startRingTest(uint64_t reduceTag, uint64_t parentTag, 
            job::ReducerBase* reducer);

protected:
    job::JobBase* allocateJob(module::Module* parent, const std::string& id, 
            const YAML::Node& config);
    job::ReducerBase* allocateReducer(module::Module* parent, 
            const YAML::Node& config);
    /** Called in the middle of a job / reducer; update all asynchronous MPI
        operations to ensure our buffers are full */
    void checkMpi();
    /** Called to reduce a childTagCount on a ProcessorReduceInfo for a given
        reduceTag.  Optionally dispatch messages pending. */
    void decrReduceChildTag(uint64_t reduceTag);
    /** Called to handle a ring test message, possibly within tryReceive, or
        possibly in the main work loop. */
    void handleRingTest(std::shared_ptr<MpiMessage> message, bool isWork);
    /** If we have an input thread, join it */
    void joinThreads();
    /** We got a steal message; decode it and maybe give someone work. */
    void maybeAllowSteal(const std::string& messageBuffer);
    /** Listen for input events and put them on workOutQueue.  When this thread 
        is finished, it emits a TAG_DEAD_RING_TEST message for 0. */
    void processInputThread_main(const std::string& inputLine);
    /** Process a previously received mpi message.  Passed non-const so that it
        can be steal-constructored (rvalue) */
    void process(std::shared_ptr<MpiMessage> message);
    /** Try to receive the current request, or make a new one */
    bool tryReceive();

private:
    struct _WorkTimerRecord {
        uint64_t tsStart;
        uint64_t timeChild;
        uint64_t clkStart;
        uint64_t clksChild;
        int timeType;

        _WorkTimerRecord(uint64_t start, uint64_t clock, int type) 
                : tsStart(start), clkStart(clock), timeChild(0), clksChild(0),
                    timeType(type) {}
    };

    /** Array containing how many cpu clocks were spent in each type of 
        operation.  Indexed by ProcessorTimeType */
    std::unique_ptr<uint64_t[]> clksByType;
    /** Our environment */
    std::unique_ptr<boost::mpi::environment> env;
    /** Running count of messages by tag */
    std::unique_ptr<uint64_t[]> msgsByTag;
    /* The stdin management thread; only runs on node 0 */
    boost::thread* processInputThread;
    /* Buffers corresponding to requests */
    message::_Message recvBuffer;
    /* The current message receiving requests (null when dead) */
    boost::optional<boost::mpi::request> recvRequest;
    /* The current number of assigned tags for reductions */
    uint64_t reduceTagCount;
    std::map<uint64_t, ProcessorReduceInfo> reduceInfoMap;
    /* We have to keep track of how many 
    /* The root module defined by the main config file */
    std::unique_ptr<job::JobBase> root;
    /* Set when eof is reached on stdin (or input line), or if our index is not
       zero. */
    bool sawEof;
    /** Currently pending outbound nonBlocking requests */
    std::list<ProcessorSendInfo> sendRequests;
    /* Set when we send ring test for 0 */
    bool sentEndRing0;
    /* True until quit message is received (ring 0 is completely closed). */
    bool shouldRun;
    /** Array containing how much time was spent in each type of operation.
        Indexed by ProcessorTimeType */
    std::unique_ptr<uint64_t[]> timesByType;
    //Any work waiting to be done on this Processor.  First element in queue
    //is current work.  We use shared_ptr so we can pass it around and still
    //read from first element for e.g. reduce tags.
    std::list<std::shared_ptr<MpiMessage>> workInQueue;
    /* workOutQueue gets redistributed to all workers; MPI is not implicitly
       thread-safe, that is why this queue exists.  Used for input only at
       the moment. */
    ThreadSafeQueue<message::WorkRecord> workOutQueue;
    int workTarget;
    std::vector<_WorkTimerRecord> workTimers;
    boost::mpi::communicator world;

    /* Enqueue a line of input (stdin or argv) to system */
    void _enqueueInputWork(const std::string& line);
    /* Return rank + 1, modulo world size */
    int _getNextRank();
    /* Increment and wrap workTarget, return new value */
    int _getNextWorker();
    /** Send in a non-blocking manner (asynchronously, receiving all the while).
        reduceTags are any tags that should be kept alive by the fact that this
        is in the process of being sent.
        */
    void _nonBlockingSend(int tag, message::Header header, std::string payload);
    /** Push down a new timer section */
    void _pushWorkTimer(ProcessorTimeType userWork);
    /** Pop the last timer section */
    void _popWorkTimer();
};

}//processor
}//job_stream

#endif//JOB_STREAM_PROCESSOR_H_
