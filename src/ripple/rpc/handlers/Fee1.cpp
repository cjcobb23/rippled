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

    io::xpring::FeeResponse doFeeGrpc(RPC::ContextGeneric<io::xpring::GetFeeRequest>& context)
    {
        io::xpring::FeeResponse reply;
        Application& app = context.app;

        auto const view = app.openLedger().current();
        if(!view)
        {
            BOOST_ASSERT(false);
            return reply;
        }


        auto const metrics = app.getTxQ().getMetrics(*view);


        reply.set_current_ledger_size(metrics.txInLedger);
        reply.set_current_queue_size(metrics.txCount);
        reply.set_expected_ledger_size(metrics.txPerLedger);
        reply.set_ledger_current_index(view->info().seq);
        reply.set_max_queue_size(*metrics.txQMaxSize);

        io::xpring::FeeLevels& levels = *reply.mutable_levels();
        levels.set_median_level(metrics.medFeeLevel);
        levels.set_minimum_level(metrics.minProcessingFeeLevel);
        levels.set_open_ledger_level(metrics.openLedgerFeeLevel);
        levels.set_reference_level(metrics.referenceFeeLevel);

        io::xpring::Fee& drops = *reply.mutable_drops();
        auto const baseFee = view->fees().base;
        drops.set_base_fee(mulDiv(metrics.referenceFeeLevel, baseFee,metrics.referenceFeeLevel).second);
        drops.set_minimum_fee(mulDiv(metrics.minProcessingFeeLevel,baseFee,metrics.referenceFeeLevel).second);
        drops.set_median_fee(mulDiv(metrics.medFeeLevel, baseFee, metrics.referenceFeeLevel).second);
     auto escalatedFee = mulDiv(
        metrics.openLedgerFeeLevel, baseFee,
            metrics.referenceFeeLevel).second;
    if (mulDiv(escalatedFee, metrics.referenceFeeLevel,
            baseFee).second < metrics.openLedgerFeeLevel)
        ++escalatedFee;       
    drops.set_open_ledger_fee(escalatedFee);



        return reply;

    }
} // ripple
