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

#include <grpcpp/grpcpp.h>

#include "xrp_ledger.grpc.pb.h"

#include <ripple/core/JobQueue.h>
#include <ripple/resource/Charge.h>
#include <ripple/rpc/Role.h>
#include <ripple/rpc/Context.h>
#include <ripple/net/InfoSub.h>

#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/impl/RPCHelpers.h>

#include <boost/exception/diagnostic_information.hpp> 

#include <ripple/rpc/impl/Handler.h>
#include <ripple/rpc/impl/Tuning.h>

#include <ripple/protocol/ErrorCodes.h>


//TODO remove these from header
using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using grpc::StatusCode;
using grpc::CompletionQueue;
using io::xpring::GetAccountInfoRequest;
using io::xpring::AccountInfo;
using io::xpring::XRPLedgerAPI;
using io::xpring::XRPAmount;
using io::xpring::GetFeeRequest;
using io::xpring::Fee;
using io::xpring::SubmitSignedTransactionRequest;
using io::xpring::SubmitSignedTransactionResponse;
using io::xpring::TxRequest;
using io::xpring::TxResponse;
using io::xpring::LedgerSequenceRequest;
using io::xpring::LedgerSequenceResponse;
using io::xpring::FeeResponse;

namespace ripple
{

//Interface that CallData implements
class Processor
{
    public:
    virtual void process() = 0;
    virtual ~Processor() {}

    virtual void set_iter(std::list<std::shared_ptr<Processor>>::iterator const& it) = 0;
    virtual std::list<std::shared_ptr<Processor>>::iterator get_iter() = 0;
    virtual void abort() = 0;

    virtual std::shared_ptr<Processor> clone() = 0;
    virtual bool isFinished() = 0;
};


//Typedefs for function to bind a listener
template <class Request, class Response>
using BindListener = std::function<void(
        XRPLedgerAPI::AsyncService&,
        ServerContext*,
        Request*,
        ServerAsyncResponseWriter<Response>*,
        CompletionQueue*,
        ServerCompletionQueue*,
        void*)>;

//typedef for handler
template <class Request, class Response>
using Handler = std::function<std::pair<Response,Status>(RPC::ContextGeneric<Request>&)>;


//strips port from endpoint
std::string getEndpoint(std::string const& peer)
{
    std::size_t first = peer.find_first_of(":");
    std::size_t last = peer.find_last_of(":");
    std::string peer_clean;
    if(first != last)
    {
        peer_clean = peer.substr(first+1);
    }
    return peer_clean;
}

class GRPCServerImpl final {
 public:
  ~GRPCServerImpl() {}

  void shutdown()
  {
      server_->Shutdown();
      // Always shutdown the completion queue after the server.
      cq_->Shutdown(); 
  }

  GRPCServerImpl(Application& app) : app_(app) {}

  // There is no shutdown handling in this code.
  void Run() {
      std::string server_address("0.0.0.0:50051");

      ServerBuilder builder;
      // Listen on the given address without any authentication mechanism.
      builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
      // Register "service_" as the instance through which we'll communicate with
      // clients. In this case it corresponds to an *asynchronous* service.
      builder.RegisterService(&service_);
      // Get hold of the completion queue used for the asynchronous communication
      // with the gRPC runtime.
      cq_ = builder.AddCompletionQueue();
      // Finally assemble the server.
      server_ = builder.BuildAndStart();
      // Proceed to the server's main loop.
      HandleRpcs();
  }

 private:

  // Class encompasing the state and logic needed to serve a request.
  template <class Request,class Response>
  class CallData : public Processor, public std::enable_shared_from_this<CallData<Request,Response>>
  {
      public:

      // Take in the "service" instance (in this case representing an asynchronous
      // server) and the completion queue "cq" used for asynchronous communication
      // with the gRPC runtime.
      CallData(
              XRPLedgerAPI::AsyncService* service,
              ServerCompletionQueue* cq,
              Application& app,
              BindListener<Request,Response> bind_listener,
              Handler<Request,Response> handler,
              RPC::Condition required_condition,
              Resource::Charge load_type);
 
      virtual ~CallData() {}

      void process() override;


      bool isFinished() override
      {
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

      std::shared_ptr<Processor> clone() override
      {
          return std::static_pointer_cast<Processor>(
                  std::make_shared<CallData<Request,Response>>(
                      this->service_, this->cq_,this->app_,bind_listener_,handler_, required_condition_, load_type_));
      }


      private:

      void process(std::shared_ptr<JobQueue::Coro> coro);

      Resource::Charge getLoadType()
      {
          return load_type_;
      }

      //for now, we are only supporting RPC's that require Role::USER for gRPC
      Role getRole()
      {
          return Role::USER;
      }

      Resource::Consumer getUsage()
      {
          std::string peer = getEndpoint(ctx_.peer());

          boost::optional<beast::IP::Endpoint> endpoint =
              beast::IP::Endpoint::from_string_checked(peer);
          return app_.getResourceManager().newInboundEndpoint(endpoint.get());
      }

      private:
      // The means of communication with the gRPC runtime for an asynchronous
      // server.
      XRPLedgerAPI::AsyncService* service_;
      // The producer-consumer queue for asynchronous server notifications.
      ServerCompletionQueue* cq_;
      // Context for the rpc, allowing to tweak aspects of it such as the use
      // of compression, authentication, as well as to send metadata back to the
      // client.
      ServerContext ctx_;

      // Possible states of the RPC
      enum CallStatus { PROCESSING, FINISH };
      CallStatus status_;  // The current serving state.

      // reference to Application
      Application& app_;

      // iterator to requests list, for lifetime management
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
      ServerAsyncResponseWriter<Response> responder_;

      // Function that creates a listener for specific request type
      BindListener<Request,Response> bind_listener_;

      // Function that processes a request
      Handler<Request,Response> handler_;

      RPC::Condition required_condition_;

      Resource::Charge load_type_;
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
              &service_,cq_.get(),app_,bl,handler, condition, load_type);
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

  //list of current RPC requests being processed or listened for
  std::list<std::shared_ptr<Processor>> requests_;

  std::unique_ptr<ServerCompletionQueue> cq_;
  
  XRPLedgerAPI::AsyncService service_;
  
  std::unique_ptr<grpc::Server> server_;
  
  Application& app_;
}; //GRPCServerImpl

class GRPCServer
{
    public:
    GRPCServer(Application& app) : impl_(app) {};

    void run()
    {
        threads_.emplace_back([this]()
                {
                this->impl_.Run();
                });
    }
    ~GRPCServer()
    {
        impl_.shutdown();
        threads_[0].join();
    }

    private:
    GRPCServerImpl impl_;
    std::vector<std::thread> threads_;
};
} //namespace ripple
#endif
