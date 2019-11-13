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


#include "rpc/v1/xrp_ledger.pb.h"
#include <ripple/rpc/Context.h>

#include <grpcpp/grpcpp.h>

#ifndef RIPPLE_RPC_GRPCHANDLER_H_INCLUDED
#define RIPPLE_RPC_GRPCHANDLER_H_INCLUDED

namespace ripple {

struct Context;

std::pair<io::xpring::AccountInfo, grpc::Status>
doAccountInfoGrpc(RPC::ContextGeneric<io::xpring::GetAccountInfoRequest>& context);

std::pair<io::xpring::FeeResponse,grpc::Status>
doFeeGrpc(RPC::ContextGeneric<io::xpring::GetFeeRequest>& context);

std::pair<io::xpring::SubmitSignedTransactionResponse, grpc::Status>
doSubmitGrpc(RPC::ContextGeneric<io::xpring::SubmitSignedTransactionRequest>& context);

std::pair<io::xpring::TxResponse, grpc::Status>
doTxGrpc(RPC::ContextGeneric<io::xpring::TxRequest>& context);

} // ripple


#endif
