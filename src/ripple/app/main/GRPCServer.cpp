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

#include <ripple/app/main/GRPCServer.h>

namespace ripple {

template <class Request, class Response>
GRPCServerImpl::CallData<Request, Response>::CallData(
              rpc::v1::XRPLedgerAPIService::AsyncService& service,
              grpc::ServerCompletionQueue& cq,
              Application& app,
              BindListener<Request, Response> bind_listener,
              Handler<Request, Response> handler,
              RPC::Condition required_condition,
              Resource::Charge load_type)
          : 
              service_(service),
              cq_(cq),
              status_(PROCESSING),
              app_(app),
              iter_(boost::none),
              aborted_(false),
              responder_(&ctx_),
              bind_listener_(bind_listener),
              handler_(handler),
              required_condition_(required_condition),
              load_type_(load_type)
{
      //Bind a listener. When a request is received, "this" will be returned
      //from CompletionQueue::Next
      bind_listener_(
              service_, &ctx_, &request_, &responder_, &cq_, &cq_, this);
}

template<class Request, class Response>
std::shared_ptr<Processor> GRPCServerImpl::CallData<Request, Response>::clone()
{
  return std::static_pointer_cast<Processor>(
          std::make_shared<CallData<Request, Response>>(
              service_,
              cq_,
              app_,
              bind_listener_,
              handler_,
              required_condition_,
              load_type_));
}

template <class Request, class Response>
void GRPCServerImpl::CallData<Request, Response>::process()
{
    if (status_ == PROCESSING) {
        std::shared_ptr<CallData<Request, Response>> this_s =
            this->shared_from_this();
        app_.getJobQueue().postCoro(JobType::jtRPC, "gRPC-Client",
                [this_s](std::shared_ptr<JobQueue::Coro> coro)
                {
                    std::lock_guard<std::mutex> lock(this_s->mut_);

                    //Do nothing if call has been aborted due to server shutdown
                    if(this_s->aborted_)
                        return;

                    this_s->process(coro);
                    this_s->status_ = FINISH;
                });
    }
    else
    {
        BOOST_ASSERT(false);
    }
}

template <class Request, class Response>
void GRPCServerImpl::CallData<Request, Response>::process(
        std::shared_ptr<JobQueue::Coro> coro)
{
    try
    {
        auto usage = getUsage();
        if(usage.disconnect())
        {
            grpc::Status status{grpc::StatusCode::RESOURCE_EXHAUSTED,
                "usage balance exceeds threshhold"};
            responder_.FinishWithError(status, this);
        }
        else
        {

            auto loadType = getLoadType();
            usage.charge(loadType);
            auto role = getRole();

            RPC::ContextGeneric<Request> context {
                app_.journal("gRPCServer"), request_, app_, loadType,
                    app_.getOPs(), app_.getLedgerMaster(), usage, role, coro,
                    InfoSub::pointer()};

            //Make sure we can currently handle the rpc
            error_code_i condition_met_res = 
                RPC::conditionMet(required_condition_, context);

            if(condition_met_res != rpcSUCCESS)
            {
                RPC::ErrorInfo error_info =
                    RPC::get_error_info(condition_met_res);
                grpc::Status status{grpc::StatusCode::INTERNAL,
                    error_info.message.c_str()};
                responder_.FinishWithError(status,this);
            }
            else
            {
                std::pair<Response,grpc::Status> result = handler_(context);
                responder_.Finish(result.first, result.second, this);
            }
        }
    }
    catch(std::exception const & ex)
    {
        grpc::Status status{grpc::StatusCode::INTERNAL,ex.what()};
        responder_.FinishWithError(status,this);
    }
}




void GRPCServerImpl::handleRpcs() {
    void* tag;  // uniquely identifies a request.
    bool ok;
    // Block waiting to read the next event from the completion queue. The
    // event is uniquely identified by its tag, which in this case is the
    // memory address of a CallData instance.
    // The return value of Next should always be checked. This return value
    // tells us whether there is any kind of event or cq_ is shutting down.
    while (cq_->Next(&tag,&ok)) {

        //if ok is false, event was terminated as part of a shutdown sequence
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
                //ptr is now processing a request, so create a new CallData
                //object to handle additional requests
                auto cloned = ptr->clone();
                requests_.push_front(cloned);
                //set iterator as data member for later lookup
                cloned->set_iter(requests_.begin());
                //process the request
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

//create a CallData instance for each RPC
void GRPCServerImpl::setupListeners()
{
   /*
    * When adding a new RPC method, add it here
    * First argument is the grpc codegen'd method to call to start listening for
    * It is always of the form :
    * rpc::v1::XRPLedgerAPIService::AsyncService::Request[RPC NAME]
    * Second argument is the handler, defined in rpc/GRPCHandlers.h
    * Third argument is the necessary condition
    * Fourth argument is the charge
    */
    makeAndPush<rpc::v1::GetFeeRequest, rpc::v1::GetFeeResponse>(
            &rpc::v1::XRPLedgerAPIService::AsyncService::RequestGetFee,
            doFeeGrpc,
            RPC::NEEDS_CURRENT_LEDGER,
            Resource::feeReferenceRPC
            );

    makeAndPush<rpc::v1::GetAccountInfoRequest, rpc::v1::GetAccountInfoResponse>(
            &rpc::v1::XRPLedgerAPIService::AsyncService::RequestGetAccountInfo,
            doAccountInfoGrpc,
            RPC::NO_CONDITION,
            Resource::feeReferenceRPC);

    makeAndPush<rpc::v1::TxRequest, rpc::v1::TxResponse>(
            &rpc::v1::XRPLedgerAPIService::AsyncService::RequestTx,
            doTxGrpc,
            RPC::NEEDS_NETWORK_CONNECTION,
            Resource::feeReferenceRPC);

    makeAndPush<rpc::v1::SubmitTransactionRequest, rpc::v1::SubmitTransactionResponse>(
            &rpc::v1::XRPLedgerAPIService::AsyncService::RequestSubmitTransaction,
            doSubmitGrpc,
            RPC::NEEDS_CURRENT_LEDGER,
            Resource::feeMediumBurdenRPC);
};

void GRPCServerImpl::start() {
    grpc::ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address_, grpc::InsecureServerCredentials());
    // Register "service_" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *asynchronous* service.
    builder.RegisterService(&service_);
    // Get hold of the completion queue used for the asynchronous communication
    // with the gRPC runtime.
    cq_ = builder.AddCompletionQueue();
    // Finally assemble the server.
    server_ = builder.BuildAndStart();
    //create necessary listeners
    setupListeners();
}

GRPCServerImpl::GRPCServerImpl(Application& app) :
    app_(app)
{

    //if present, get endpoint from config
    if(app_.config().exists("port_grpc"))
    {
        Section section = app_.config().section("port_grpc");

        //get the default values of ip and port
        std::size_t colon_pos = server_address_.find(':');
        std::string ip_str = server_address_.substr(0,colon_pos);
        std::string port_str = server_address_.substr(colon_pos+1);

        std::pair<std::string,bool> ip_pair = section.find("ip");
        if(ip_pair.second)
        {
            ip_str = ip_pair.first;
        }

        std::pair<std::string,bool> port_pair = section.find("port");
        if(port_pair.second)
        {
            port_str = port_pair.first;
        }

        server_address_ = ip_str + ":" + port_str;
    }
}

} //namespace ripple
