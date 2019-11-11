
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

#include <boost/exception/diagnostic_information.hpp> 


using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using grpc::StatusCode;
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



class Processor
{
    public:
    virtual std::shared_ptr<Processor> Proceed() = 0;
    virtual ~Processor() {}

    virtual void setIter(std::list<std::shared_ptr<Processor>>::iterator const& it) = 0;
    virtual std::list<std::shared_ptr<Processor>>::iterator getIter() = 0;
    virtual void abort() = 0;
};

template <class T>
std::shared_ptr<Processor> createCallDataAndListen(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
{
    std::shared_ptr<T> ptr = std::make_shared<T>(service, cq, app);
    ptr->makeListener();
    std::cout << "made listener" << std::endl;
    return std::static_pointer_cast<Processor>(ptr);
}

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
  ~GRPCServerImpl() {

    std::cout << "shutting down grpc" << std::endl;
 //   server_->Shutdown();
    // Always shutdown the completion queue after the server.
 //   cq_->Shutdown();
    std::cout << "shut down grpc" << std::endl;
  }

  void shutdown()
  {
     server_->Shutdown();
    // Always shutdown the completion queue after the server.
    cq_->Shutdown(); 
  }

  GRPCServerImpl(ripple::Application& app) : app_(app) {}

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
    std::cout << "Server listening on " << server_address << std::endl;

    // Proceed to the server's main loop.
    HandleRpcs();
  }

 private:



  // Class encompasing the state and logic needed to serve a request.
  template <class D>
  class CallData : public Processor, public std::enable_shared_from_this<CallData<D>>
  {
  public:
      
    CallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : service_(service), cq_(cq), status_(LISTEN), app_(app),iter_(nullptr),aborted_(false)
    {
    }

    virtual ~CallData() {}

    void Proceed() override
    {
      if (status_ == LISTEN) {
          std::cout << "processing" << std::endl;
          status_ = PROCESS;
        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
          std::shared_ptr<CallData<D>> this_s = this->shared_from_this();
          app_.getJobQueue().postCoro(ripple::JobType::jtRPC, "gRPC-Client",[this_s](std::shared_ptr<ripple::JobQueue::Coro> coro)
                {
                    std::lock_guard<std::mutex> lock(this_s->mut_);

                    //Do nothing if the call has been aborted due to server shutdown
                    if(this_s->aborted_)
                        return;

                    this_s->process(coro);
                });
      } else {
          std::cout << "finishing" << std::endl;
          GPR_ASSERT(status_ == PROCESS);
          status_ = FINISH;
          finish();
      }
    }

    std::shared_ptr<CallData<D>> clone()
    {
        auto ptr = createCallDataAndListen<D>(service_,cq_,app_);
    
        return ptr;
    }

    virtual void makeListener() = 0;
    virtual void process(std::shared_ptr<ripple::JobQueue::Coro>) = 0;
    virtual void finish() {}

    //override these next 3 if different functionality is desired
    virtual ripple::Resource::Charge getLoadType()
    {
        return ripple::Resource::feeReferenceRPC;
    }

    virtual ripple::Role getRole()
    {
        return ripple::Role::USER;
    }

    virtual ripple::Consumer getUsage()
    {
        std::string peer = getEndpoint(ctx_.peer());

        boost::optional<beast::IP::Endpoint> endpoint =
            beast::IP::Endpoint::from_string_checked(peer);
        return app_.getResourceManager().newInboundEndpoint(endpoint.get());
    }


    virtual void abort() override
    {
        std::lock_guard<std::mutex> lock(mut_);
        aborted_ = true;
    }

    void setIter(std::list<std::shared_ptr<Processor>>::iterator const & it) override
    {
        iter_ = it;
    }

    std::list<std::shared_ptr<Processor>>::iterator getIter() override
    {
        return iter_;
    }

    friend class Builder;



    //TODO: return to protected
  //protected:
    // The means of communication with the gRPC runtime for an asynchronous
    // server.
    XRPLedgerAPI::AsyncService* service_;
    // The producer-consumer queue where for asynchronous server notifications.
    ServerCompletionQueue* cq_;
    // Context for the rpc, allowing to tweak aspects of it such as the use
    // of compression, authentication, as well as to send metadata back to the
    // client.
    ServerContext ctx_;

    // Let's implement a tiny state machine with the following states.
    enum CallStatus { LISTEN, PROCESS, FINISH };
    CallStatus status_;  // The current serving state.

    ripple::Application& app_;

    std::list<std::shared_ptr<Processor>>::iterator iter_;
    std::mutex mut_;
    bool aborted_;
  }; //CallData



  

  class AccountInfoCallData : public CallData<AccountInfoCallData>
  {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    AccountInfoCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : CallData(service,cq,app), responder_(&ctx_)
    {
    }



    void makeListener() override 
    {
        // As part of the initial LISTEN state, we *request* that the system
        // start processing GetAccountInfo requests. In this request, "this" acts are
        // the tag uniquely identifying the request (so that different CallData
        // instances can serve different requests concurrently), in this case
        // the memory address of this CallData instance.
        service_->RequestGetAccountInfo(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    } 

    void process(std::shared_ptr<ripple::JobQueue::Coro> coro) override
    {
        try
        {
            auto usage = getUsage();
            if(usage.disconnect())
            {
                Status status{StatusCode::RESOURCE_EXHAUSTED,
                    "usage balance exceeds threshhold"};
                responder_.FinishWithError(status, this);
            }
            else
            {

                auto loadType = getLoadType();
                usage.charge(loadType);

                ripple::RPC::ContextGeneric<GetAccountInfoRequest> context {
                    app.journal("gRPCServer"),getLoadType()
                        request_, app, loadType, app.getOPs(), app.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                std::pair<AccountInfo,Status> result = ripple::doAccountInfoGrpc(context);

                //TODO: what happens if server was shutdown but we try to respond?
                responder_.Finish(result.first, result.second, this);
            }
        }
        catch(std::exception const & ex)
        {
            Status status{StatusCode::INTERNAL,ex.what()};
            responder_.FinishWithError(status,this);
        }
    }

   private:

    // What we get from the client.
    GetAccountInfoRequest request_;
    // What we send back to the client.
    AccountInfo reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<AccountInfo> responder_;
  }; //AccountInfoCallData

  class FeeCallData : public CallData<FeeCallData>
  {
   public:
    FeeCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : CallData(service,cq,app), responder_(&ctx_)
    {
    }



    void makeListener() override 
    {
        service_->RequestGetFee(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    } 

    void process() override
    {
        try
        {
            auto usage = getUsage();
            if(usage.disconnect())
            {
                Status status{StatusCode::RESOURCE_EXHAUSTED,
                    "usage balance exceeds threshhold"};
                responder_.FinishWithError(status, this);
            }
            else
            {getLoadType()

                auto loadType = getLoadType();
                usage.charge(loadType);

                ripple::RPC::ContextGeneric<FeeRequest> context {
                    app.journal("gRPCServer"),
                        request_, app, loadType, app.getOPs(), app.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                reply_ = ripple::doFeeGrpc(context);

                //TODO: what happens if server was shutdown but we try to respond?
                responder_.Finish(reply_, Status::OK, this);
            }
        }
        catch(std::exception const & ex)
        {
            Status status{StatusCode::INTERNAL,ex.what()};
            responder_.FinishWithError(status,this);
        }
    } 

   private:

    // What we get from the client.
    GetFeeRequest request_;
    // What we send back to the client.
    FeeResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<FeeResponse> responder_;
  }; //FeeCallData


  class SubmitCallData : public CallData<SubmitCallData>
  {
    public:
    SubmitCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : CallData(service,cq,app), responder_(&ctx_)
    {
    }

    void makeListener() override
    {

        service_->RequestSubmitSignedTransaction(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    }

    void process() override
    {
        try
        {
            auto usage = getUsage();
            if(usage.disconnect())
            {
                Status status{StatusCode::RESOURCE_EXHAUSTED,
                    "usage balance exceeds threshhold"};
                responder_.FinishWithError(status, this);
            }
            else
            {

                auto loadType = getLoadType();
                usage.charge(loadType);

                ripple::RPC::ContextGeneric<SubmitSignedTransactionRequest> context {
                    app.journal("gRPCServer"),
                        request_, app, loadType, app.getOPs(), app.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                std::pair<SubmitSignedTransactionResponse,Status> result = ripple::doSubmitGrpc(context);

                //TODO: what happens if server was shutdown but we try to respond?
                responder_.Finish(result.first,result.second, this);
            }
        }
        catch(std::exception const & ex)
        {
            Status status{StatusCode::INTERNAL,ex.what()};
            responder_.FinishWithError(status,this);
        }


    }
   
      private:
    // What we get from the client.
    SubmitSignedTransactionRequest request_;
    // What we send back to the client.
    SubmitSignedTransactionResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<SubmitSignedTransactionResponse> responder_;

  }; //SubmitCallData

  class TxCallData : public CallData<TxCallData>
  {
    public:
    TxCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : CallData(service,cq,app), responder_(&ctx_)
    {
    }

    void makeListener() override
    {

        service_->RequestTx(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    }

    void process(std::shared_ptr<ripple::JobQueue::Coro> coro) override
    {
        try
        {
            auto usage = getUsage();
            if(usage.disconnect())
            {
                Status status{StatusCode::RESOURCE_EXHAUSTED,
                    "usage balance exceeds threshhold"};
                responder_.FinishWithError(status, this);
            }
            else
            {

                auto loadType = getLoadType();
                usage.charge(loadType);

                ripple::RPC::ContextGeneric<TxRequest> context {
                    app.journal("gRPCServer"),
                        request_, app_, loadType, app_.getOPs(), app_.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                std::pair<TxResponse,Status> result = ripple::doTxGrpc(context);

                //TODO: what happens if server was shutdown but we try to respond?
                responder_.Finish(result.first,result.second, this);
            }
        }
        catch(std::exception const & ex)
        {
            Status status{StatusCode::INTERNAL,ex.what()};
            responder_.FinishWithError(status,this);
        }

    }

    ripple::Resource::Charge getLoadType() override
    {
        return ripple::Resource::feeMediumBurdenRPC;
    }
   
      private:
    // What we get from the client.
    TxRequest request_;
    // What we send back to the client.
    TxResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<TxResponse> responder_;

  }; //TxCallData



  
  //create CallData object, add to list and set iterator data member
  template <class T>
  void makeAndPush(std::list<std::shared_ptr<Processor>>& data)
  {
    auto ptr = createCallDataAndListen<T>(&service_,cq_.get(),app_);
    data.push_front(ptr);
    ptr->setIter(data.begin());
  }


  // This can be run in multiple threads if needed.
  void HandleRpcs() {
    // Container for CallData instances
    std::list<std::shared_ptr<Processor>> requests;

    //create CallData object for each request type
    makeAndPush<AccountInfoCallData>(requests);
    makeAndPush<FeeCallData>(requests);
    makeAndPush<SubmitCallData>(requests);
    makeAndPush<TxCallData>(requests);
    makeAndPush<LedgerSequenceCallData>(requests);

    void* tag;  // uniquely identifies a request.
    bool ok;
    // Block waiting to read the next event from the completion queue. The
    // event is uniquely identified by its tag, which in this case is the
    // memory address of a CallData instance.
    // The return value of Next should always be checked. This return value
    // tells us whether there is any kind of event or cq_ is shutting down.
    while (cq_->Next(&tag,&ok)) {

      //if ok is false, this event was terminated as part of a shutdown sequence
      //need to abort any further processing
      if(!ok)
      {
        //abort first, then erase. Otherwise, erase can delete object
        static_cast<Processor*>(tag)->abort();
        requests.erase(static_cast<Processor*>(tag)->getIter());
      }
      else
      {
          auto ptr = static_cast<Processor*>(tag);
          if(ptr.status_ == CallStatus::LISTEN)
          {
              //ptr is now processing a request, so create a new CallData object
              //to handle additional requests
              auto cloned = ptr->clone();
              requests.push_front(cloned);
              //set iterator as data member for later lookup
              cloned->setIter(requests.begin());
          }
          else
          {
              requests.erase(static_cast<Processor*>(tag)->getIter());
          }
          ptr->proceed();
      }
    }
  }

  std::unique_ptr<ServerCompletionQueue> cq_;
  XRPLedgerAPI::AsyncService service_;
  std::unique_ptr<Server> server_;
  ripple::Application& app_;
}; //GRPCServerImpl

class GRPCServer
{
    public:
    GRPCServer(ripple::Application& app) : impl_(app) {};

    void run()
    {
        threads_.emplace_back([this]()
                {
                this->impl_.Run();
                });
    }
    ~GRPCServer()
    {
        std::cout << "shutting down grpc server main" << std::endl;
        impl_.shutdown();
        threads_[0].join();
    }

    private:
    GRPCServerImpl impl_;
    std::vector<std::thread> threads_;
};
#endif
