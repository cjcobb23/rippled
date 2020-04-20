
//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_CORE_REPORTINGETL_H_INCLUDED
#define RIPPLE_CORE_REPORTINGETL_H_INCLUDED

#include <ripple/app/main/Application.h>
#include <ripple/core/JobQueue.h>
#include <ripple/net/InfoSub.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/resource/Charge.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/Role.h>
#include <ripple/rpc/impl/Handler.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/Tuning.h>

#include "org/xrpl/rpc/v1/xrp_ledger.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <mutex>
#include <queue>

namespace ripple {

class ReportingETL
{
    //TODO some logging in the queue
    class LedgerIndexQueue
    {
        std::queue<uint32_t> queue_;

        std::mutex mtx_;

        std::condition_variable cv_;

        std::atomic_bool stopping_ = false;

    public:
        void
        push(uint32_t idx)
        {
            std::unique_lock<std::mutex> lck(mtx_);
            if (queue_.size() > 0)
            {
                auto last = queue_.back();
                if (idx <= last)
                {
                    std::cout << "push old index = " << idx << std::endl;
                    return;
                }
                assert(idx > last);
                if (idx > last + 1)
                {
                    for (size_t i = last + 1; i < idx; ++i)
                    {
                        queue_.push(i);
                    }
                }
            }
            queue_.push(idx);
            cv_.notify_all();
        }

        uint32_t
        pop()
        {
            std::unique_lock<std::mutex> lck(mtx_);
            cv_.wait(
                lck, [this]() { return this->queue_.size() > 0 || stopping_; });
            if (stopping_)
                return 0;
            uint32_t next = queue_.front();
            queue_.pop();
            return next;
        }

        void
        stop()
        {
            std::unique_lock<std::mutex> lck(mtx_);
            stopping_ = true;
            cv_.notify_all();
        }
    };
    public:
template <class T>
struct ThreadSafeQueue
{

    std::queue<T> queue_;

    std::mutex m_;
    std::condition_variable cv_;

    void push(T const& elt)
    {
        std::unique_lock<std::mutex> lck(m_);
        queue_.push(elt);
        cv_.notify_all();
    }

    T pop()
    {
        std::unique_lock<std::mutex> lck(m_);
        //TODO: is this able to be aborted?
        cv_.wait(lck,[this](){ return !queue_.empty();});
        auto ret = queue_.front();
        queue_.pop();
        return ret;
    }
};

private:
    Application& app_;

    std::thread worker_;

    std::thread subscriber_;

    std::unique_ptr<org::xrpl::rpc::v1::XRPLedgerAPIService::Stub> stub_;

    uint32_t currentIndex_ = 0;

    //TODO stopping logic needs to be better
    //There are a variety of loops and mutexs in play
    //Sometimes, the software can't stop
    std::atomic_bool stopping_ = false;

    std::shared_ptr<Ledger> ledger_;

    LedgerIndexQueue queue_;

    std::string ip_;
    std::string wsPort_;

    beast::Journal journal_;

    enum LoadMethod { ITERATIVE, BUFFER, PARALLEL, ASYNC};

    LoadMethod method_ = ASYNC;

    bool onlyDownload_ = false;

    bool flushDuringDownload_ = false;

    size_t flushInterval_ = 0;

    size_t parallelism_ = 16;

    bool asyncFlush_ = true;

    bool updateViaDiff_ = false;

    bool useLedgerEntry_ = true;

    //TODO better names for these functions
    void loadIterative();

    void loadParallel();

    void loadBuffer();

    void loadAsync();
    
    void doInitialLedgerLoad();

    std::vector<TxMeta> loadNextLedger();

    void storeLedger();

    void continousUpdate();

    void diffLedgers();

    void doAsyncFlush();

    void runGapHandler();

    void updateViaDiff(uint32_t have, uint32_t want);
    
    std::thread flusher_;
    

    ThreadSafeQueue<std::shared_ptr<SLE>> flushQueue_;

    std::thread gapHandler_;

    ThreadSafeQueue<uint32_t> gaps_;


public:
    ReportingETL(Application& app)
        : app_(app), journal_(app.journal("ReportingETL"))
    {
        // if present, get endpoint from config
        if (app_.config().exists("reporting"))
        {
            Section section = app_.config().section("reporting");

            std::pair<std::string, bool> ipPair = section.find("source_ip");
            if (!ipPair.second)
                return;

            std::pair<std::string, bool> portPair =
                section.find("source_grpc_port");
            if (!portPair.second)
                return;

            std::pair<std::string, bool> wsPortPair =
                section.find("source_ws_port");
            if (!wsPortPair.second)
                return;

            std::pair<std::string, bool> startIndexPair =
                section.find("start_index");

            if (startIndexPair.second)
            {
                currentIndex_ = std::stoi(startIndexPair.first);
                queue_.push(currentIndex_);
            }

            std::pair<std::string, bool> loadMethod = section.find("load_method");
            if(loadMethod.second)
            {
                if(loadMethod.first == "parallel")
                    method_ = PARALLEL;
                else if(loadMethod.first == "iterative")
                    method_ = ITERATIVE;
                else if(loadMethod.first == "buffer")
                    method_ = BUFFER;
                else if(loadMethod.first == "async")
                    method_ = ASYNC;
            }

            std::pair<std::string, bool> onlyDownload = section.find("download");
            if(onlyDownload.second)
            {
                if(onlyDownload.first == "true")
                    onlyDownload_ = true;
            }


            std::pair<std::string, bool> flush = section.find("flush");
            if(flush.second)
            {
                if(flush.first == "true")
                    flushDuringDownload_ = true;
            }
            std::pair<std::string, bool> flushInterval = section.find("flush_interval");
            if(flushInterval.second)
            {
                flushInterval_ = std::stoi(flushInterval.first);
            }

            std::pair<std::string, bool> p = section.find("parallelism");
            if(p.second)
                parallelism_ = std::stoi(p.first);

            std::pair<std::string, bool> asyncFlush = section.find("async_flush");
            if(asyncFlush.second)
            {
                if(asyncFlush.first == "true")
                    asyncFlush_ = true;
            }

            std::pair<std::string, bool> updateViaDiff =
                section.find("update_via_diff");
            if (updateViaDiff.second)
            {
                if (updateViaDiff.first == "true")
                    updateViaDiff_ = true;
            }

            std::pair<std::string, bool> useLedgerEntry =
                section.find("use_ledger_entry");
            if (useLedgerEntry.second)
            {
                if (useLedgerEntry.first == "false")
                    useLedgerEntry_ = false;
            }

            try
            {
                stub_ = org::xrpl::rpc::v1::XRPLedgerAPIService::NewStub(
                    grpc::CreateChannel(
                        beast::IP::Endpoint(
                            boost::asio::ip::make_address(ipPair.first),
                            std::stoi(portPair.first))
                            .to_string(),
                        grpc::InsecureChannelCredentials()));
                std::cout << "made stub" << std::endl;
                ip_ = ipPair.first;
                wsPort_ = wsPortPair.first;
            }
            catch (std::exception const& e)
            {
                std::cout << "Exception while creating stub = " << e.what()
                          << std::endl;
            }
        }
    }

    ~ReportingETL()
    {
        JLOG(journal_.debug()) << "Stopping Reporting ETL";
        stopping_ = true;
        if (subscriber_.joinable())
            subscriber_.join();
        
        JLOG(journal_.debug()) << "Joined subscriber thread";
        if (worker_.joinable())
            worker_.join();

        JLOG(journal_.debug()) << "Joined worker thread";
    }

    void
    run()
    {
        std::cout << "starting reporting etl" << std::endl;
        assert(app_.config().reporting());
        assert(app_.config().standalone());
        if (!stub_)
        {
            std::cout << "stub not created. aborting reporting etl"
                      << std::endl;
            return;
        }
        stopping_ = false;
        doSubscribe();
        doWork();
    }

private:
    void
    doWork();

    void
    doSubscribe();
};

}  // namespace ripple
#endif
