
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
      std::cout << "Server listening on " << server_address << std::endl;

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
              Resource::Charge load_type)
          : 
              service_(service),
              cq_(cq),
              status_(PROCESSING),
              app_(app),
              iter_(boost::none),
              aborted_(false),
              responder_(&this->ctx_),
              bind_listener_(bind_listener),
              handler_(handler),
              required_condition_(required_condition),
              load_type_(load_type)
      {
          //Bind a listener. When a request is received, "this" will be returned from CompletionQueue::Next
          bind_listener_(*this->service_,&this->ctx_,&request_,&responder_,this->cq_,this->cq_,this);
      }
        
      virtual ~CallData() {}

      void process() override
      {
          if (status_ == PROCESSING) {
              std::shared_ptr<CallData<Request,Response>> this_s = this->shared_from_this();
              app_.getJobQueue().postCoro(JobType::jtRPC, "gRPC-Client",
                      [this_s](std::shared_ptr<JobQueue::Coro> coro)
                      {
                          std::lock_guard<std::mutex> lock(this_s->mut_);

                          //Do nothing if the call has been aborted due to server shutdown
                          if(this_s->aborted_)
                              return;

                          this_s->process(coro);
                      });
          }
          else
          {
              assert(false);
          }
      }

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
              assert(false);

          return iter_.get();
      }

      std::shared_ptr<Processor> clone() override
      {
          return std::static_pointer_cast<Processor>(
                  std::make_shared<CallData<Request,Response>>(
                      this->service_, this->cq_,this->app_,bind_listener_,handler_, required_condition_, load_type_));
      }


      private:

      void process(std::shared_ptr<JobQueue::Coro> coro)
      {
          try
          {
              auto usage = this->getUsage();
              if(usage.disconnect())
              {
                  Status status{StatusCode::RESOURCE_EXHAUSTED,
                      "usage balance exceeds threshhold"};
                  responder_.FinishWithError(status, this);
              }
              else
              {

                  auto loadType = this->getLoadType();
                  usage.charge(loadType);
                  auto role = this->getRole();
                  Application& app = this->app_;

                  RPC::ContextGeneric<Request> context {
                      app.journal("gRPCServer"),
                          request_, app, loadType, app.getOPs(), app.getLedgerMaster(),
                          usage, role, coro, InfoSub::pointer()};


                  //Make sure we can currently handle the rpc
                  error_code_i condition_met_res = 
                      RPC::conditionMet(required_condition_, context);

                  if(condition_met_res != rpcSUCCESS)
                  {
                    RPC::ErrorInfo error_info = RPC::get_error_info(condition_met_res);
                    Status status{StatusCode::INTERNAL,error_info.message.c_str()};
                    responder_.FinishWithError(status,this);
                  }
                  else
                  {
                      std::pair<Response,Status> result = handler_(context);
                      //TODO: what happens if server was shutdown but we try to respond?
                      responder_.Finish(result.first, result.second, this);
                  }
              }
          }
          catch(std::exception const & ex)
          {
              Status status{StatusCode::INTERNAL,ex.what()};
              responder_.FinishWithError(status,this);
          }
          status_ = FINISH;
      }

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

  //create a CallData instance for each RPC
  //When adding a new RPC method, add it here
  //First argument is the grpc codegen'd method to call to start listening for
  //a given request type. Follows the pattern of Request<RPC Name>
  //Second argument is the handler.
  //Third argument is the necessary condition
  //Fourth argument is the charge
  void setup()
  {
      makeAndPush<GetFeeRequest,FeeResponse>(
              &XRPLedgerAPI::AsyncService::RequestGetFee,
              doFeeGrpc,
              RPC::NEEDS_CURRENT_LEDGER,
              Resource::feeReferenceRPC
              );

      makeAndPush<GetAccountInfoRequest,AccountInfo>(
              &XRPLedgerAPI::AsyncService::RequestGetAccountInfo,
              doAccountInfoGrpc,
              RPC::NO_CONDITION,
              Resource::feeReferenceRPC);

      makeAndPush<TxRequest,TxResponse>(
              &XRPLedgerAPI::AsyncService::RequestTx,
              doTxGrpc,
              RPC::NEEDS_NETWORK_CONNECTION,
              Resource::feeReferenceRPC);

      makeAndPush<SubmitSignedTransactionRequest,SubmitSignedTransactionResponse>(
              &XRPLedgerAPI::AsyncService::RequestSubmitSignedTransaction,
              doSubmitGrpc,
              RPC::NEEDS_CURRENT_LEDGER,
              Resource::feeMediumBurdenRPC);
  };



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
  void makeAndPush(BindListener<Request,Response> bl, Handler<Request,Response> handler, RPC::Condition condition, Resource::Charge load_type)
  {
      auto ptr = makeCallData(bl,handler,condition,load_type);
      requests_.push_front(ptr);
      ptr->set_iter(requests_.begin());
  }

  // This can be run in multiple threads if needed.
  void HandleRpcs() {
      setup();
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
              requests_.erase(static_cast<Processor*>(tag)->get_iter());
          }
          else
          {
              auto ptr = static_cast<Processor*>(tag);
              if(!ptr->isFinished())
              {
                  //ptr is now processing a request, so create a new CallData object
                  //to handle additional requests
                  auto cloned = ptr->clone();
                  requests_.push_front(cloned);
                  //set iterator as data member for later lookup
                  cloned->set_iter(requests_.begin());
                  ptr->process();
              }
              else
              {
                  //rpc is finished, delete CallData object
                  requests_.erase(static_cast<Processor*>(tag)->get_iter());
              }
          }
      }
  }

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
