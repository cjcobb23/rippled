//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/json/json_value.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/impl/TransactionSign.h>
#include <ripple/rpc/Role.h>

#include <ripple/rpc/GRPCHandlers.h>

namespace ripple {

Json::Value doServerInfo (RPC::Context& context)
{
    Json::Value ret (Json::objectValue);

    ret[jss::info] = context.netOps.getServerInfo (
        true, context.role == Role::ADMIN,
        context.params.isMember(jss::counters) &&
            context.params[jss::counters].asBool());

    return ret;
}

std::pair<io::xpring::LedgerSequenceResponse, grpc::Status> doLedgerSequenceGrpc(RPC::ContextGeneric<io::xpring::LedgerSequenceRequest>& context)
{
    io::xpring::LedgerSequenceResponse result;
    grpc::Status status = grpc::Status::OK;
    bool valid = false;
    auto lpClosed = context.ledgerMaster.getValidatedLedger ();

    if (lpClosed)
        valid = true;
    else
        lpClosed = context.ledgerMaster.getClosedLedger ();

    if (lpClosed)
    {
        std::uint64_t seq = lpClosed->info().seq;

        if (valid)
            result.set_validated(seq);
        else
            result.set_closed(seq);
    }

    return {result,status};

}

} // ripple
