//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2015 Ripple Labs Inc.

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

#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/rpc/Context.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/Feature.h>


#include <ripple/basics/mulDiv.h>

#include <ripple/rpc/GRPCHandlers.h>

#include <ripple/app/misc/TxQ.h>

namespace ripple
{
    Json::Value doFee(RPC::Context& context)
    {
        auto result = context.app.getTxQ().doRPC(context.app);
        if (result.type() == Json::objectValue)
            return result;
        assert(false);
        RPC::inject_error(rpcINTERNAL, context.params);
        return context.params;
    }

    io::xpring::Fee doFeeGrpc(RPC::ContextGeneric<io::xpring::GetFeeRequest>& context)
    {
        io::xpring::Fee reply;
        Application& app = context.app;

        auto const view = app.openLedger().current();
        if(!view)
        {
            BOOST_ASSERT(false);
            reply.mutable_amount()->set_drops("-1");
            return reply;
        }


        auto const metrics = app.getTxQ().getMetrics(*view);

        auto const baseFee = view->fees().base;

        auto escalatedFee = mulDiv(
                metrics.openLedgerFeeLevel, baseFee,
                metrics.referenceFeeLevel).second;
        if (mulDiv(escalatedFee, metrics.referenceFeeLevel,
                    baseFee).second < metrics.openLedgerFeeLevel)
            ++escalatedFee;

        reply.mutable_amount()->set_drops(to_string(escalatedFee));
        return reply;

    }
} // ripple
