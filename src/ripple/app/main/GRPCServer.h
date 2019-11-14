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

#ifndef RIPPLE_CORE_GRPCSERVER_H_INCLUDED
#define RIPPLE_CORE_GRPCSERVER_H_INCLUDED

#include <ripple/rpc/Role.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/Handler.h>
#include <ripple/rpc/impl/Tuning.h>
#include <ripple/net/InfoSub.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/core/JobQueue.h>
#include <ripple/resource/Charge.h>

#include <grpcpp/grpcpp.h>
#include "rpc/v1/xrp_ledger.grpc.pb.h"

namespace ripple {

//Interface that CallData implements
class Processor
{
    public:

    virtual ~Processor() {}

    // process a request that has arrived. Can only be called once per instance
    virtual void process() = 0;

    //store an iterator to this object
    //all Processor objects are stored in a std::list, and the iterator points
    //to that objects position in the list. When object finishes processing a 
    //request, the iterator is used to delete the object from the list
    virtual void set_iter(
            std::list<std::shared_ptr<Processor>>::iterator const& it) = 0;

    //get iterator to this object. see above comment
    virtual std::list<std::shared_ptr<Processor>>::iterator get_iter() = 0;

    //abort processing this request. called when server shutsdown
    virtual void abort() = 0;

    //create a new instance of this CallData object, with the same type
    //(same template parameters) as original. This is called when a CallData
    //object starts processing a request. Creating a new instance allows the 
    //server to handle additional requests while the first is being processed
    virtual std::shared_ptr<Processor> clone() = 0;

    //true if this object has finished processing the request. Object will be
    //deleted once this function returns true
    virtual bool isFinished() = 0;
};

namespace {

//helper function. strips port from endpoint string
std::string getEndpoint(std::string const& peer)
{
    std::size_t first = peer.find_first_of(":");
    std::size_t last = peer.find_last_of(":");
    std::string peer_clean(peer);
    if(first != last)
    {
        peer_clean = peer.substr(first+1);
    }
    return peer_clean;
}

} //namespace

class GRPCServerImpl final {

    private:

    //list of current RPC requests being processed or listened for
    std::list<std::shared_ptr<Processor>> requests_;

    //CompletionQueue returns events that have occurred, or events that have
    //been cancelled
    std::unique_ptr<grpc::ServerCompletionQueue> cq_;

    //The gRPC service defined by the .proto files
    rpc::v1::XRPLedgerAPIService::AsyncService service_;

    //gRPC server
    std::unique_ptr<grpc::Server> server_;

    //referernce to ripple::Application
    Application& app_;

    //address of where to run the server
    std::string server_address_;


    //typedef for function to bind a listener
    template <class Request, class Response>
    using BindListener = std::function<void(
            rpc::v1::XRPLedgerAPIService::AsyncService&,
            grpc::ServerContext*,
            Request*,
            grpc::ServerAsyncResponseWriter<Response>*,
            grpc::CompletionQueue*,
            grpc::ServerCompletionQueue*,
            void*)>;

    //typedef for actual handler (that populates a response)
    template <class Request, class Response>
    using Handler = std::function<std::pair<Response,grpc::Status>(
            RPC::ContextGeneric<Request>&)>;

    public:
    ~GRPCServerImpl() {}

    //server_address is of the form "ip:port"
    //example = "0.0.0.0:50051"
    GRPCServerImpl(Application& app, std::string const & server_address) :
        app_(app), server_address_(server_address) {}
  
    void shutdown()
    {
        server_->Shutdown();
        // Always shutdown the completion queue after the server.
        cq_->Shutdown(); 
    }
  
    //setup the server and begin handling rpcs
    void Run();
  
    private:
  
    // Class encompasing the state and logic needed to serve a request.
    template <class Request,class Response>
    class CallData :
        public Processor, 
        public std::enable_shared_from_this<CallData<Request,Response>>
    {
        private:
        // The means of communication with the gRPC runtime for an asynchronous
        // server.
        rpc::v1::XRPLedgerAPIService::AsyncService& service_;

        // The producer-consumer queue for asynchronous server notifications.
        grpc::ServerCompletionQueue& cq_;

        // Context for the rpc, allowing to tweak aspects of it such as the use
        // of compression, authentication, as well as to send metadata back to
        // the client.
        grpc::ServerContext ctx_;

        // Possible states of the RPC
        enum CallStatus { PROCESSING, FINISH };
        // The current serving state.
        CallStatus status_;

        // reference to Application
        Application& app_;

        // iterator pointing to this object in the requests list
        // for lifetime management
        boost::optional<std::list<std::shared_ptr<Processor>>::iterator> iter_;

        // mutex for signaling abort
        std::mutex mut_;

        // whether the call should be aborted, due to server shutdown
        bool aborted_;

        // What we get from the client.
        Request request_;

        // What we send back to the client.
        Response reply_;

        // The means to get back to the client.
        grpc::ServerAsyncResponseWriter<Response> responder_;

        // Function that creates a listener for specific request type
        BindListener<Request,Response> bind_listener_;

        // Function that processes a request
        Handler<Request,Response> handler_;

        //Condition required for this RPC
        RPC::Condition required_condition_;

        //Load type for this RPC
        Resource::Charge load_type_;


        public:

        virtual ~CallData() {}
  
        // Take in the "service" instance (in this case representing an
        // asynchronous server) and the completion queue "cq" used for
        // asynchronous communication with the gRPC runtime.
        CallData(
                rpc::v1::XRPLedgerAPIService::AsyncService& service,
                grpc::ServerCompletionQueue& cq,
                Application& app,
                BindListener<Request,Response> bind_listener,
                Handler<Request,Response> handler,
                RPC::Condition required_condition,
                Resource::Charge load_type);
   
  
        void process() override;
  
        bool isFinished() override
        {
            //checking the status while a request is in the middle of being
            //processed will lead to indeterminate results. Lock here to 
            //sequence checking status and processing
            std::lock_guard<std::mutex> lock(mut_);
            return status_ == CallStatus::FINISH;
        }
  
        virtual void abort() override
        {
            std::lock_guard<std::mutex> lock(mut_);
            aborted_ = true;
        }
  
        void set_iter(std::list<std::shared_ptr<Processor>>::iterator const & it) override
        {
            iter_ = it;
        }
  
        std::list<std::shared_ptr<Processor>>::iterator get_iter() override
        {
            if(!iter_)
                BOOST_ASSERT(false);
  
            return iter_.get();
        }
  
        std::shared_ptr<Processor> clone() override;
  
        private:
  
        //process the request. Called inside the coroutine passed to JobQueue
        void process(std::shared_ptr<JobQueue::Coro> coro);
  
        //return load type of this RPC
        Resource::Charge getLoadType()
        {
            return load_type_;
        }
  
        //return the Role required for this RPC
        //for now, we are only supporting RPC's that require Role::USER for gRPC
        Role getRole()
        {
            return Role::USER;
        }
  
        //register endpoint with ResourceManager and return usage
        Resource::Consumer getUsage()
        {
            std::string peer = getEndpoint(ctx_.peer());
            boost::optional<beast::IP::Endpoint> endpoint =
                beast::IP::Endpoint::from_string_checked(peer);
            return app_.getResourceManager().newInboundEndpoint(endpoint.get());
        }

    }; //CallData
  
  
    //Create a CallData object for each RPC
    void setup();
  
    //make a CallData instance, returned as shared_ptr to base class (Processor)
    template <class Request, class Response>
    std::shared_ptr<Processor> makeCallData(
            BindListener<Request,Response> bl,
            Handler<Request,Response> handler,
            RPC::Condition condition,
            Resource::Charge load_type)
    {
        auto ptr = std::make_shared<CallData<Request,Response>>(
                service_,*cq_,app_,bl,handler, condition, load_type);
        return std::static_pointer_cast<Processor>(ptr);
    }
  
    //make CallData instance and push to requests list
    template <class Request,class Response>
    void makeAndPush(
            BindListener<Request,Response> bl,
            Handler<Request,Response> handler,
            RPC::Condition condition,
            Resource::Charge load_type)
    {
        auto ptr = makeCallData(bl,handler,condition,load_type);
        requests_.push_front(ptr);
        ptr->set_iter(requests_.begin());
    }
  
    //the main event loop
    void HandleRpcs();
  
}; //GRPCServerImpl


class GRPCServer
{
    std::string server_address = "0.0.0.0:50051";

    public:
    GRPCServer(Application& app) : impl_(app,server_address) {};

    void run()
    {
        thread_ = std::thread([this]()
                {
                    this->impl_.Run();
                });
    }
    ~GRPCServer()
    {
        impl_.shutdown();
        thread_.join();
    }

    private:
    GRPCServerImpl impl_;
    std::thread thread_;
};
} //namespace ripple
#endif
