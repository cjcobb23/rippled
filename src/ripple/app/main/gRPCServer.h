
#ifndef RIPPLE_CORE_GRPCSERVER_H_INCLUDED
#define RIPPLE_CORE_GRPCSERVER_H_INCLUDED

#include <grpcpp/grpcpp.h>

#include "helloworld.grpc.pb.h"

#include <ripple/core/JobQueue.h>


using grpc::Server;
using grpc::ServerAsyncResponseWriter;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using grpc::Status;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;



class GRPCServerImpl final {
 public:
  ~GRPCServerImpl() {
    server_->Shutdown();
    // Always shutdown the completion queue after the server.
    cq_->Shutdown();
  }

  GRPCServerImpl(ripple::JobQueue& jq) : jobQueue_(jq) {}

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
  class CallData {
   public:
    // Take in the "service" instance (in this case representing an asynchronous
    // server) and the completion queue "cq" used for asynchronous communication
    // with the gRPC runtime.
    CallData(Greeter::AsyncService* service, ServerCompletionQueue* cq, ripple::JobQueue& jq)
        : service_(service), cq_(cq), responder_(&ctx_),status_(CREATE), jobQueue_(jq) {
      // Invoke the serving logic right away.
      Proceed();
    }

    void Proceed() {
      if (status_ == CREATE) {
          std::cout << "Proceed::Create - " << this << std::endl;
        // Make this instance progress to the PROCESS state.
        status_ = PROCESS;

        // As part of the initial CREATE state, we *request* that the system
        // start processing SayHello requests. In this request, "this" acts are
        // the tag uniquely identifying the request (so that different CallData
        // instances can serve different requests concurrently), in this case
        // the memory address of this CallData instance.
        std::cout << "requesting" << std::endl;
        service_->RequestSayHello(&ctx_, &request_, &responder_, cq_, cq_,
                                  this);
        //service_->RequestSayGoodbye(&ctx_,&request_, &responder2_, cq_, cq_, this);
        std::cout << "requested" << std::endl;
      } else if (status_ == PROCESS) {

          std::cout << "Proceed::PROCESS - " << this << std::endl;
        // Spawn a new CallData instance to serve new clients while we process
        // the one for this CallData. The instance will deallocate itself as
        // part of its FINISH state.
          std::cout << "making new call data" << std::endl;
        new CallData(service_, cq_, jobQueue_);
        std::cout << "made new call data" << std::endl;


        jobQueue_.postCoro(ripple::JobType::jtCLIENT, "GRPC-Client",[this](std::shared_ptr<ripple::JobQueue::Coro> coro)
                {

                // The actual processing.
                std::string prefix("Helloooooooo from job queue");
                this->reply_.set_message(prefix + request_.name());
                std::cout << "made reply" << std::endl;

                // And we are done! Let the gRPC runtime know we've finished, using the
                // memory address of this instance as the uniquely identifying tag for
                // the event.
                this->status_ = FINISH;
                //GoodbyeReply reply;
                //reply.set_message("foo");
                //responder_.finishe
                this->responder_.Finish(this->reply_, Status::OK, this);

                    std::cout << "IN JOB QUEUE !!!!!!!*******" << std::endl;
                });

        std::cout << "responded" << std::endl;
      } else {

          std::cout << "Proceed::FINISH - " << this << std::endl;
          std::cout << "finished" << std::endl;
        GPR_ASSERT(status_ == FINISH);
        // Once in the FINISH state, deallocate ourselves (CallData).
        delete this;
      }
    }

   private:
    // The means of communication with the gRPC runtime for an asynchronous
    // server.
    Greeter::AsyncService* service_;
    // The producer-consumer queue where for asynchronous server notifications.
    ServerCompletionQueue* cq_;
    // Context for the rpc, allowing to tweak aspects of it such as the use
    // of compression, authentication, as well as to send metadata back to the
    // client.
    ServerContext ctx_;

    // What we get from the client.
    HelloRequest request_;
    // What we send back to the client.
    HelloReply reply_;

    // The means to get back to the client.
    ServerAsyncResponseWriter<HelloReply> responder_;




    // Let's implement a tiny state machine with the following states.
    enum CallStatus { CREATE, PROCESS, FINISH };
    CallStatus status_;  // The current serving state.

    ripple::JobQueue& jobQueue_;
  };

  // This can be run in multiple threads if needed.
  void HandleRpcs() {
    std::cout << "entered handle rpcs" << std::endl;
    // Spawn a new CallData instance to serve new clients.
    new CallData(&service_, cq_.get(), jobQueue_);

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
  Greeter::AsyncService service_;
  std::unique_ptr<Server> server_;
  ripple::JobQueue& jobQueue_;
};

class GRPCServer
{
    public:
    GRPCServer(ripple::JobQueue& jq) : impl_(jq) {};

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
