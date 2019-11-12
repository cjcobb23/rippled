
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



class Processor
{
    public:
    virtual void Proceed() = 0;
    virtual ~Processor() {}

    virtual void set_iter(std::list<std::shared_ptr<Processor>>::iterator const& it) = 0;
    virtual std::list<std::shared_ptr<Processor>>::iterator get_iter() = 0;
    virtual void abort() = 0;

    virtual std::shared_ptr<Processor> clone() = 0;
    virtual bool isProcessing() = 0;
};


//Typedefs for functions passed to CallData
template <class Request, class Response>
using MakeListener = std::function<void(
        XRPLedgerAPI::AsyncService&,
        ServerContext*,
        Request*,
        ServerAsyncResponseWriter<Response>*,
        CompletionQueue*,
        ServerCompletionQueue*,
        void*)>;

template <class Request, class Response>
using Process = std::function<std::pair<Response,Status>(ripple::RPC::ContextGeneric<Request>&)>;


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

    bool isProcessing()
    {
    
        return status_ == CallStatus::LISTEN;
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

    virtual ripple::Resource::Consumer getUsage()
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

    void set_iter(std::list<std::shared_ptr<Processor>>::iterator const & it) override
    {
        iter_ = it;
    }

    std::list<std::shared_ptr<Processor>>::iterator get_iter() override
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


  template <class Request,class Response>
  class CallDataGen : public CallData<CallDataGen<Request,Response>>
  {


   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CallDataGen(XRPLedgerAPI::AsyncService* service, ServerCompletionQueue* cq, ripple::Application& app,
            MakeListener<Request,Response> make_listener,
            Process<Request,Response> process)
        : CallData<CallDataGen<Request,Response>>(service,cq,app), responder_(&this->ctx_), make_listener_(make_listener), process_(process)
    {
        make_listener_(*this->service_,&this->ctx_,&request_,&responder_,this->cq_,this->cq_,this);
    }

    std::shared_ptr<Processor> clone() override
    {
        return std::static_pointer_cast<Processor>(std::make_shared<CallDataGen<Request,Response>>(this->service_, this->cq_,this->app_,make_listener_,process_));
    }


    void makeListener() override 
    {
        // As part of the initial LISTEN state, we *request* that the system
        // start processing GetAccountInfo requests. In this request, "this" acts are
        // the tag uniquely identifying the request (so that different CallData
        // instances can serve different requests concurrently), in this case
        // the memory address of this CallData instance.
        make_listener_(*this->service_,&this->ctx_,&request_,&responder_,this->cq_,this->cq_,this);
    } 

    void process(std::shared_ptr<ripple::JobQueue::Coro> coro) override
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
                ripple::Application& app = this->app_;

                ripple::RPC::ContextGeneric<Request> context {
                    app.journal("gRPCServer"),
                        request_, app, loadType, app.getOPs(), app.getLedgerMaster(),
                        usage, role, coro, ripple::InfoSub::pointer()};

                std::pair<Response,Status> result = process_(context);

                //TODO: what happens if server was shutdown but we try to respond?
                responder_.Finish(result.first, result.second, this);
            }
        } catch(std::exception const & ex)
        {
            Status status{StatusCode::INTERNAL,ex.what()};
            responder_.FinishWithError(status,this);
        }
    }

   private:

    // What we get from the client.
    Request request_;
    // What we send back to the client.
    Response reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<Response> responder_;

    MakeListener<Request,Response> make_listener_;

    Process<Request,Response> process_;


  }; //CallDataGen


 
  void setup()
  {
      makeAndPush<GetFeeRequest,FeeResponse>(
              &XRPLedgerAPI::AsyncService::RequestGetFee,
              ripple::doFeeGrpc);

      makeAndPush<GetAccountInfoRequest,AccountInfo>(
              &XRPLedgerAPI::AsyncService::RequestGetAccountInfo,
              ripple::doAccountInfoGrpc);

      makeAndPush<TxRequest,TxResponse>(
              &XRPLedgerAPI::AsyncService::RequestTx,
              ripple::doTxGrpc);

      makeAndPush<SubmitSignedTransactionRequest,SubmitSignedTransactionResponse>(
              &XRPLedgerAPI::AsyncService::RequestSubmitSignedTransaction,
              ripple::doSubmitGrpc);
  };

  template <class Request, class Response>
  std::shared_ptr<Processor> makeCallData(MakeListener<Request,Response> ml, Process<Request,Response> p)
  {
    auto ptr = std::make_shared<CallDataGen<Request,Response>>(&service_,cq_.get(),app_,ml,p);
    return std::static_pointer_cast<Processor>(ptr);
  }

  template <class Request,class Response>
  void makeAndPush(MakeListener<Request,Response> ml, Process<Request,Response> p)
  {
    auto ptr = makeCallData(ml,p);
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
              if(ptr->isProcessing())
              {
                  //ptr is now processing a request, so create a new CallData object
                  //to handle additional requests
                  auto cloned = ptr->clone();
                  requests_.push_front(cloned);
                  //set iterator as data member for later lookup
                  cloned->set_iter(requests_.begin());
                  ptr->Proceed();
              }
              else
              {
                  //perform any cleanup, if necessary
                  ptr->Proceed();
                  requests_.erase(static_cast<Processor*>(tag)->get_iter());
              }
          }
      }
  }

  std::list<std::shared_ptr<Processor>> requests_;

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
