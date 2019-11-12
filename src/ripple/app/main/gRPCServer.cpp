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

#include <ripple/app/main/gRPCServer.h>

namespace ripple {

template <class Request,class Response>
GRPCServerImpl::CallData<Request,Response>::CallData(
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


template <class Request,class Response>
void GRPCServerImpl::CallData<Request,Response>::process()
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
        BOOST_ASSERT(false);
    }

}

template <class Request,class Response>
void GRPCServerImpl::CallData<Request,Response>::process(std::shared_ptr<JobQueue::Coro> coro)
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

  void GRPCServerImpl::HandleRpcs() {
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

  //create a CallData instance for each RPC
  //When adding a new RPC method, add it here
  //First argument is the grpc codegen'd method to call to start listening for
  //a given request type. Follows the pattern of Request<RPC Name>
  //Second argument is the handler.
  //Third argument is the necessary condition
  //Fourth argument is the charge
  void GRPCServerImpl::setup()
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

} //namespace ripple
