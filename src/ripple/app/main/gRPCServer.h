
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

    std::shared_ptr<Processor> Proceed() override
    {
      if (status_ == LISTEN) {
          std::cout << "processing" << std::endl;
          status_ = PROCESS;
        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
          auto ptr = createCallDataAndListen<D>(service_,cq_,app_);
          process();
          return ptr;
      } else {
          std::cout << "finishing" << std::endl;
          GPR_ASSERT(status_ == PROCESS);
          status_ = FINISH;
          finish();
          return std::shared_ptr<Processor>();
      }
    }

    virtual void makeListener() = 0;
    virtual void process() = 0;
    virtual void finish() {}

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

    void process() override
    {

        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
        auto this_s = std::static_pointer_cast<AccountInfoCallData>(shared_from_this());
        //TODO abstract the logic of posting to the JobQueue
        app_.getJobQueue().postCoro(ripple::JobType::jtCLIENT, "GRPC-Client",[this_s](std::shared_ptr<ripple::JobQueue::Coro> coro)
                {
                std::lock_guard<std::mutex> lock(this_s->mut_);
                if(this_s->aborted_)
                    return;//Do nothing if the call has been aborted due to server shutdown


                ripple::Resource::Charge loadType =
                    ripple::Resource::feeReferenceRPC;
                auto role = ripple::Role::USER;
                std::string peer = this_s->ctx_.peer();
                std::cout << "peer is " << peer << std::endl;

                std::size_t first = peer.find_first_of(":");
                std::size_t last = peer.find_last_of(":");

                if(first != last)
                {
                    peer = peer.substr(first+1);
                }

                std::cout << "peer now is " << peer << std::endl;
                boost::optional<beast::IP::Endpoint> endpoint = beast::IP::Endpoint::from_string_checked(peer);
                if(endpoint)
                    std::cout << "endpoint good" << std::endl;
                else
                    std::cout << "endpoint bad" << std::endl;


                ripple::Application& app = this_s->app_;
                auto usage = app.getResourceManager().newInboundEndpoint(endpoint.get());
                if(usage.disconnect())
                {
                    Status status{StatusCode::RESOURCE_EXHAUSTED, "usage balance exceeds threshhold"};
                    this_s->responder_.FinishWithError(status, this_s.get());
                    //TODO also return warning?
                }
                else
                {
                    usage.charge(loadType);


                    ripple::RPC::ContextGeneric<GetAccountInfoRequest> context {
                        app.journal("Server"),
                        this_s->request_, app, loadType, app.getOPs(), app.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                    std::pair<AccountInfo,Status> result = ripple::doAccountInfoGrpc(context);

                    // And we are done! Let the gRPC runtime know we've finished, using the
                    // memory address of this instance as the uniquely identifying tag for
                    // the event.
                    //TODO: what happens if server was shutdown but we try to respond?
                    this_s->responder_.Finish(result.first, result.second, this_s.get());
                }
                });

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
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    FeeCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
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
        service_->RequestGetFee(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    } 

    void process() override
    {

        app_.getJobQueue().postCoro(ripple::JobType::jtCLIENT, "GRPC-Client",[this](std::shared_ptr<ripple::JobQueue::Coro> coro)
                {


                ripple::Resource::Charge loadType =
                    ripple::Resource::feeReferenceRPC;
                auto role = ripple::Role::USER;
                std::string peer = ctx_.peer();
                std::cout << "peer is " << peer << std::endl;

                std::size_t first = peer.find_first_of(":");
                std::size_t last = peer.find_last_of(":");

                if(first != last)
                {
                    //TODO: should we use ip and port or just ip?
                    peer = peer.substr(first+1,last-first);
                }

                std::cout << "peer now is " << peer << std::endl;
                boost::optional<beast::IP::Endpoint> endpoint = beast::IP::Endpoint::from_string_checked(peer);
                if(endpoint)
                    std::cout << "endpoint good" << std::endl;
                else
                    std::cout << "endpoint bad" << std::endl;


                auto usage = app_.getResourceManager().newInboundEndpoint(endpoint.get());
                std::cout << "usage balance = " << usage.balance() << std::endl;
                //TODO: will this ever return true?
                if(usage.disconnect())
                {
                    //TODO return some error code
                    //TODO also return warning?
                    Status status{StatusCode::RESOURCE_EXHAUSTED, "usage balance exceeds threshhold"};
                    this->responder_.Finish(this->reply_,status, this);
                }
                else
                {
                    usage.charge(loadType);
                    ripple::RPC::ContextGeneric<GetFeeRequest> context {app_.journal("Server"),
                        request_, app_, loadType, app_.getOPs(), app_.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                    this->reply_ = ripple::doFeeGrpc(context);

                this->responder_.Finish(this->reply_, Status::OK, this);
                }

                // And we are done! Let the gRPC runtime know we've finished, using the
                // memory address of this instance as the uniquely identifying tag for
                // the event.
                });

    } 

   private:

    // What we get from the client.
    GetFeeRequest request_;
    // What we send back to the client.
    Fee reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<Fee> responder_;
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

        app_.getJobQueue().postCoro(ripple::JobType::jtCLIENT, "GRPC-Client",[this](std::shared_ptr<ripple::JobQueue::Coro> coro)
                {

                ripple::Resource::Charge loadType =
                    ripple::Resource::feeReferenceRPC;
                auto role = ripple::Role::USER;
                std::string peer = getEndpoint(ctx_.peer());

                boost::optional<beast::IP::Endpoint> endpoint = beast::IP::Endpoint::from_string_checked(peer);

                auto usage = app_.getResourceManager().newInboundEndpoint(endpoint.get());
                usage.charge(loadType);
                //TODO check usage
                ripple::RPC::ContextGeneric<SubmitSignedTransactionRequest> context {app_.journal("Server"),
                    request_, app_, loadType, app_.getOPs(), app_.getLedgerMaster(),
                    usage, role, coro, ripple::InfoSub::pointer()};               

                    auto res = ripple::doSubmitGrpc(context);
                    this->responder_.Finish(res.first,res.second,this);



                });
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

    void process() override
    {

        app_.getJobQueue().postCoro(ripple::JobType::jtCLIENT, "GRPC-Client",[this](std::shared_ptr<ripple::JobQueue::Coro> coro)
                {

                ripple::Resource::Charge loadType =
                    ripple::Resource::feeReferenceRPC;
                auto role = ripple::Role::USER;
                std::string peer = getEndpoint(ctx_.peer());

                boost::optional<beast::IP::Endpoint> endpoint = beast::IP::Endpoint::from_string_checked(peer);

                auto usage = app_.getResourceManager().newInboundEndpoint(endpoint.get());
                usage.charge(loadType);
                //TODO check usage
                ripple::RPC::ContextGeneric<TxRequest> context {app_.journal("Server"),
                    request_, app_, loadType, app_.getOPs(), app_.getLedgerMaster(),
                    usage, role, coro, ripple::InfoSub::pointer()};               

                    auto res = ripple::doTxGrpc(context);
                    this->responder_.Finish(res.first,res.second,this);



                });
    }
   
      private:
    // What we get from the client.
    TxRequest request_;
    // What we send back to the client.
    TxResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<TxResponse> responder_;

  }; //TxCallData

  
  class LedgerSequenceCallData : public CallData<LedgerSequenceCallData>
  {
    public:
    LedgerSequenceCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : CallData(service,cq,app), responder_(&ctx_)
    {
    }

    void makeListener() override
    {

        service_->RequestLedgerSequence(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    }

    void process() override
    {

        app_.getJobQueue().postCoro(ripple::JobType::jtCLIENT, "GRPC-Client",[this](std::shared_ptr<ripple::JobQueue::Coro> coro)
                {

                ripple::Resource::Charge loadType =
                    ripple::Resource::feeReferenceRPC;
                auto role = ripple::Role::USER;
                std::string peer = getEndpoint(ctx_.peer());

                boost::optional<beast::IP::Endpoint> endpoint = beast::IP::Endpoint::from_string_checked(peer);

                auto usage = app_.getResourceManager().newInboundEndpoint(endpoint.get());
                usage.charge(loadType);
                //TODO check usage
                ripple::RPC::ContextGeneric<LedgerSequenceRequest> context {app_.journal("Server"),
                    request_, app_, loadType, app_.getOPs(), app_.getLedgerMaster(),
                    usage, role, coro, ripple::InfoSub::pointer()};               

                    auto res = ripple::doLedgerSequenceGrpc(context);
                    this->responder_.Finish(res.first,res.second,this);



                });
    }
   
      private:
    // What we get from the client.
    LedgerSequenceRequest request_;
    // What we send back to the client.
    LedgerSequenceResponse reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<LedgerSequenceResponse> responder_;

  }; //LedgerSequenceCallData


  template <class T>
  void makeAndPush(std::list<std::shared_ptr<Processor>>& data)
  {
    auto ptr = createCallDataAndListen<T>(&service_,cq_.get(),app_);
    data.push_front(ptr);
    ptr->setIter(data.begin());
  }


  // This can be run in multiple threads if needed.
  void HandleRpcs() {
    // Spawn a new CallData instance to serve new clients.
    std::list<std::shared_ptr<Processor>> data;
    makeAndPush<AccountInfoCallData>(data);
    makeAndPush<FeeCallData>(data);
    makeAndPush<SubmitCallData>(data);
    makeAndPush<TxCallData>(data);
    makeAndPush<LedgerSequenceCallData>(data);

    void* tag;  // uniquely identifies a request.
    bool ok;
    std::unordered_map<void*,bool> deleted;
    while (cq_->Next(&tag,&ok)) {
      // Block waiting to read the next event from the completion queue. The
      // event is uniquely identified by its tag, which in this case is the
      // memory address of a CallData instance.
      // The return value of Next should always be checked. This return value
      // tells us whether there is any kind of event or cq_ is shutting down.
      if(!ok)
      {
          std::cout << "not ok. tag is " << tag << std::endl;
          if(deleted.find(tag) != deleted.end())
          {
        //abort first, then erase. Otherwise, erase may delete object
        static_cast<Processor*>(tag)->abort();
        data.erase(static_cast<Processor*>(tag)->getIter());
        deleted[tag] = true;
          }
      }
      else
      {
      std::cout << "Got tag" << std::endl;
      auto new_data = static_cast<Processor*>(tag)->Proceed();
      if(new_data)
      {
          data.push_front(new_data);
          new_data->setIter(data.begin());
      }
      else //Delete this object
      {
        data.erase(static_cast<Processor*>(tag)->getIter());
      }
      }
    }
  }

  std::unique_ptr<ServerCompletionQueue> cq_;
  XRPLedgerAPI::AsyncService service_;
  std::unique_ptr<Server> server_;
//  ripple::JobQueue& jobQueue_;
  ripple::Application& app_;
//
//  beast::Journal j_;
//  NetworkOps& netops_;
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
