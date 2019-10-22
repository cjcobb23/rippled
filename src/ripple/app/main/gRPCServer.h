
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
using io::xpring::GetAccountInfoRequest;
using io::xpring::AccountInfo;
using io::xpring::XRPLedgerAPI;
using io::xpring::XRPAmount;
using io::xpring::GetFeeRequest;
using io::xpring::Fee;



class GRPCServerImpl final {
 public:
  ~GRPCServerImpl() {
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
  class CallData
  {
  public:
      
    CallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : service_(service), cq_(cq), status_(CREATE), app_(app)
    {
    }

    virtual ~CallData() {}

    void Proceed() {
      if (status_ == CREATE) {
          std::cout << "Proceed::Create - " << this << std::endl;
        // Make this instance progress to the PROCESS state.
        status_ = PROCESS;
        doCreate();
      } else if (status_ == PROCESS) {
        status_ = FINISH;
        doProcess();
      } else {

          GPR_ASSERT(status_ == FINISH);
          doFinish();
      }
    }

    virtual void doCreate() = 0;
    virtual void doProcess() = 0;
    virtual void doFinish() = 0;

  protected:
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
    enum CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;  // The current serving state.

    ripple::Application& app_;
  }; //CallData
  

  class AccountInfoCallData : public CallData {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    AccountInfoCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : CallData(service,cq,app), responder_(&ctx_)
    {
        Proceed();
    }



    void doCreate() override 
    {
        // As part of the initial CREATE state, we *request* that the system
        // start processing GetAccountInfo requests. In this request, "this" acts are
        // the tag uniquely identifying the request (so that different CallData
        // instances can serve different requests concurrently), in this case
        // the memory address of this CallData instance.
        service_->RequestGetAccountInfo(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    } 

    void doProcess() override
    {

        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
        new AccountInfoCallData(service_, cq_, app_);
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
                    peer = peer.substr(first+1);
                }

                std::cout << "peer now is " << peer << std::endl;
                boost::optional<beast::IP::Endpoint> endpoint = beast::IP::Endpoint::from_string_checked(peer);
                if(endpoint)
                    std::cout << "endpoint good" << std::endl;
                else
                    std::cout << "endpoint bad" << std::endl;


                auto usage = app_.getResourceManager().newInboundEndpoint(endpoint.get());
                if(usage.disconnect())
                {

                    //TODO return some error code
                    //TODO also return warning?
                }
                else
                {
                    usage.charge(loadType);


                    ripple::RPC::ContextGeneric<GetAccountInfoRequest> context {
                        app_.journal("Server"),
                        request_, app_, loadType, app_.getOPs(), app_.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                    this->reply_ = ripple::doAccountInfo(context);
                    // And we are done! Let the gRPC runtime know we've finished, using the
                    // memory address of this instance as the uniquely identifying tag for
                    // the event.
                }
                this->status_ = FINISH;
                this->responder_.Finish(this->reply_, Status::OK, this);
                });

    } 

    void doFinish() override
    {
        GPR_ASSERT(status_ == FINISH);
        // Once in the FINISH state, deallocate ourselves (CallData).
        delete this;
    }


   private:

    // What we get from the client.
    GetAccountInfoRequest request_;
    // What we send back to the client.
    AccountInfo reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<AccountInfo> responder_;
  }; //AccountInfoCallData

  class FeeCallData : public CallData {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    FeeCallData(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app)
        : CallData(service,cq,app), responder_(&ctx_)
    {
        Proceed();
    }



    void doCreate() override 
    {
        // As part of the initial CREATE state, we *request* that the system
        // start processing GetAccountInfo requests. In this request, "this" acts are
        // the tag uniquely identifying the request (so that different CallData
        // instances can serve different requests concurrently), in this case
        // the memory address of this CallData instance.
        service_->RequestGetFee(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
    } 

    void doProcess() override
    {

        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
        new FeeCallData(service_, cq_, app_);
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
                }
                else
                {
                    usage.charge(loadType);
                    ripple::RPC::ContextGeneric<GetFeeRequest> context {app_.journal("Server"),
                        request_, app_, loadType, app_.getOPs(), app_.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                    this->reply_ = ripple::doFee(context);
                }

                // And we are done! Let the gRPC runtime know we've finished, using the
                // memory address of this instance as the uniquely identifying tag for
                // the event.
                this->status_ = FINISH;
                this->responder_.Finish(this->reply_, Status::OK, this);
                });

    } 

    void doFinish() override
    {
        GPR_ASSERT(status_ == FINISH);
        // Once in the FINISH state, deallocate ourselves (CallData).
        delete this;
    }


   private:

    // What we get from the client.
    GetFeeRequest request_;
    // What we send back to the client.
    Fee reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<Fee> responder_;
  }; //AccountInfoCallData

  // This can be run in multiple threads if needed.
  void HandleRpcs() {
    std::cout << "entered handle rpcs" << std::endl;
    // Spawn a new CallData instance to serve new clients.
    new AccountInfoCallData(&service_, cq_.get(), app_);
    new FeeCallData(&service_, cq_.get(), app_);

    std::cout << "created call data" << std::endl;
    void* tag;  // uniquely identifies a request.
    bool ok;
    while (true) {
      // Block waiting to read the next event from the completion queue. The
      // event is uniquely identified by its tag, which in this case is the
      // memory address of a CallData instance.
      // The return value of Next should always be checked. This return value
      // tells us whether there is any kind of event or cq_ is shutting down.
        std::cout << "polling completion queue" << std::endl;
      GPR_ASSERT(cq_->Next(&tag, &ok));
      GPR_ASSERT(ok);
      std::cout << "got something from completion queue: " << tag << std::endl;
      static_cast<CallData*>(tag)->Proceed();
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

    private:
    GRPCServerImpl impl_;
    std::vector<std::thread> threads_;
};
#endif
