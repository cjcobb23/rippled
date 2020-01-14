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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/json/json_value.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/protocol/UintTypes.h>
#include <ripple/resource/Fees.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/Role.h>

namespace ripple {

// {
//   account: account,
//   ledger_index_min: ledger_index  // optional, defaults to earliest
//   ledger_index_max: ledger_index, // optional, defaults to latest
//   binary: boolean,                // optional, defaults to false
//   forward: boolean,               // optional, defaults to false
//   limit: integer,                 // optional
//   marker: opaque                  // optional, resume previous query
// }
Json::Value doAccountTx (RPC::JsonContext& context)
{
    auto& params = context.params;

    int limit = params.isMember (jss::limit) ?
            params[jss::limit].asUInt () : -1;
    bool bBinary = params.isMember (jss::binary) && params[jss::binary].asBool ();
    bool bForward = params.isMember (jss::forward) && params[jss::forward].asBool ();
    std::uint32_t   uLedgerMin;
    std::uint32_t   uLedgerMax;
    std::uint32_t   uValidatedMin;
    std::uint32_t   uValidatedMax;
    bool bValidated = context.ledgerMaster.getValidatedRange (
        uValidatedMin, uValidatedMax);

    if (!bValidated)
    {
        // Don't have a validated ledger range.
        return rpcError (rpcLGR_IDXS_INVALID);
    }

    if (!params.isMember (jss::account))
        return rpcError (rpcINVALID_PARAMS);

    auto const account = parseBase58<AccountID>(
        params[jss::account].asString());
    if (! account)
        return rpcError (rpcACT_MALFORMED);

    context.loadType = Resource::feeMediumBurdenRPC;

    if (params.isMember (jss::ledger_index_min) ||
        params.isMember (jss::ledger_index_max))
    {
        std::int64_t iLedgerMin  = params.isMember (jss::ledger_index_min)
                ? params[jss::ledger_index_min].asInt () : -1;
        std::int64_t iLedgerMax  = params.isMember (jss::ledger_index_max)
                ? params[jss::ledger_index_max].asInt () : -1;

        uLedgerMin  = iLedgerMin == -1 ? uValidatedMin :
            ((iLedgerMin >= uValidatedMin) ? iLedgerMin : uValidatedMin);
        uLedgerMax  = iLedgerMax == -1 ? uValidatedMax :
            ((iLedgerMax <= uValidatedMax) ? iLedgerMax : uValidatedMax);

        if (uLedgerMax < uLedgerMin)
            return rpcError (rpcLGR_IDXS_INVALID);
    }
    else if(params.isMember (jss::ledger_hash) ||
            params.isMember (jss::ledger_index))
    {
        std::shared_ptr<ReadView const> ledger;
        auto ret = RPC::lookupLedger (ledger, context);

        if (! ledger)
            return ret;

        std::cout << "ledger index" << std::endl;
        std::cout << "validated = " << ret[jss::validated].asBool()
            << "ledger info seq = " << ledger->info().seq
            << "uValidatedMax = " << uValidatedMax
            << "uValidatedMin = " << uValidatedMin
            << std::endl;



        if (! ret[jss::validated].asBool() ||
            (ledger->info().seq > uValidatedMax) ||
            (ledger->info().seq < uValidatedMin))
        {
            return rpcError (rpcLGR_NOT_VALIDATED);
        }

        uLedgerMin = uLedgerMax = ledger->info().seq;
    }
    else
    {
        uLedgerMin = uValidatedMin;
        uLedgerMax = uValidatedMax;
    }

    Json::Value resumeToken;

    if (params.isMember(jss::marker))
         resumeToken = params[jss::marker];

#ifndef DEBUG

    try
    {
#endif
        Json::Value ret (Json::objectValue);

        ret[jss::account] = context.app.accountIDCache().toBase58(*account);
        Json::Value& jvTxns = (ret[jss::transactions] = Json::arrayValue);

        if (bBinary)
        {
            auto txns = context.netOps.getTxsAccountB (
                *account, uLedgerMin, uLedgerMax, bForward, resumeToken, limit,
                isUnlimited (context.role));

            for (auto& it: txns)
            {
                Json::Value& jvObj = jvTxns.append (Json::objectValue);

                jvObj[jss::tx_blob] = std::get<0> (it);
                jvObj[jss::meta] = std::get<1> (it);

                std::uint32_t uLedgerIndex = std::get<2> (it);

                jvObj[jss::ledger_index] = uLedgerIndex;
                jvObj[jss::validated] = bValidated &&
                    uValidatedMin <= uLedgerIndex &&
                    uValidatedMax >= uLedgerIndex;
            }
        }
        else
        {
            auto txns = context.netOps.getTxsAccount (
                *account, uLedgerMin, uLedgerMax, bForward, resumeToken, limit,
                isUnlimited (context.role));

            for (auto const& [txn, txMeta]: txns)
            {
                Json::Value& jvObj = jvTxns.append (Json::objectValue);

                if (txn)
                    jvObj[jss::tx] =
                        txn->getJson (JsonOptions::include_date);

                if (txMeta)
                {
                    auto metaJ = txMeta->getJson (JsonOptions::include_date);
                    insertDeliveredAmount (metaJ, context, txn, *txMeta);
                    jvObj[jss::meta] = std::move(metaJ);

                    std::uint32_t uLedgerIndex = txMeta->getLgrSeq ();

                    jvObj[jss::validated] = bValidated &&
                        uValidatedMin <= uLedgerIndex &&
                        uValidatedMax >= uLedgerIndex;
                }
            }
        }

        //Add information about the original query
        ret[jss::ledger_index_min] = uLedgerMin;
        ret[jss::ledger_index_max] = uLedgerMax;
        if (params.isMember (jss::limit))
            ret[jss::limit]        = limit;
        if (resumeToken)
            ret[jss::marker] = resumeToken;

        return ret;
#ifndef DEBUG
    }
    catch (std::exception const&)
    {
        return rpcError (rpcINTERNAL);
    }

#endif
}




//Ledger sequence, Account sequence
//TODO this type needs to be defined somewhere else,
//to be used by NetworkOps and AccountTxPaging and here
using Marker = std::pair<uint32_t, uint32_t>;
using LedgerRange = std::pair<int, int>;
using LedgerSequence = uint32_t;
using LedgerHash = uint256;
using LedgerShortcut = std::string;

struct AccountTxArgs
{
    AccountID account;

    //Defaults to empty LedgerShortcut (empty string), which specifies the
    //current ledger
    std::variant<LedgerRange,LedgerShortcut,LedgerSequence,LedgerHash> ledger;

    bool binary = false;

    bool forward = false;

    uint32_t limit = 0;

    std::optional<Marker> marker;

};

struct AccountTxResult
{
    using AccountTx = std::pair<std::shared_ptr<Transaction>, TxMeta::pointer>;
    //bool indicates whether data is validated
    std::vector<std::pair<AccountTx,bool>> transactions;

    std::vector<std::pair<std::tuple<std::string, std::string, uint32_t>,bool>> transactionsBn;

    LedgerRange ledgerRange;

    uint32_t limit;

    std::optional<Marker> marker;

    bool validated;

    AccountID account;
};



std::pair<AccountTxResult, RPC::Status>
doAccountTxHelp(RPC::Context& context, AccountTxArgs& args)
{
    AccountTxResult result;

    std::uint32_t uValidatedMin;
    std::uint32_t uValidatedMax;
    bool bValidated =
        context.ledgerMaster.getValidatedRange(uValidatedMin, uValidatedMax);

    if (!bValidated)
    {
        // Don't have a validated ledger range.
        return {result, rpcLGR_IDXS_INVALID};
    }

    std::uint32_t uLedgerMin = uValidatedMin;
    std::uint32_t uLedgerMax = uValidatedMax;

    context.loadType = Resource::feeMediumBurdenRPC;

    if (std::holds_alternative<LedgerRange>(args.ledger))
    {

        auto& ledgerRange = std::get<LedgerRange>(args.ledger);
        if(ledgerRange.first >= 0 && ledgerRange.first > uValidatedMin)
        {
            uLedgerMin = ledgerRange.first;
        }
        if(ledgerRange.second >= 0 && ledgerRange.second < uValidatedMax)
        {
            uLedgerMax = ledgerRange.second;
        }
        if (uLedgerMax < uLedgerMin)
            return {result, rpcLGR_IDXS_INVALID};
    }
    else
    {
        std::shared_ptr<ReadView const> ledger;
        RPC::Status status{RPC::Status::OK};
        if (std::holds_alternative<LedgerSequence>(args.ledger))
        {
            status = getLedger(
                ledger, std::get<LedgerSequence>(args.ledger), context);
        }
        else if (std::holds_alternative<LedgerHash>(args.ledger))
        {
            status =
                getLedger(ledger, std::get<LedgerHash>(args.ledger), context);
        }
        else
        {
            std::cout << "Ledger shortcut " << std::endl;
            assert(std::holds_alternative<LedgerShortcut>(args.ledger));
            status = getLedger(
                ledger, std::get<LedgerShortcut>(args.ledger), context);
        }
        if (!ledger)
        {
            return {result, status};
        }

        bool validated =
            RPC::isValidated(context.ledgerMaster, *ledger, context.app);

        std::cout << "validated = " << validated
            << "ledger info seq = " << ledger->info().seq
            << "uValidatedMax = " << uValidatedMax
            << "uValidatedMin = " << uValidatedMin
            << std::endl;

        if (!validated || ledger->info().seq > uValidatedMax ||
            ledger->info().seq < uValidatedMin)
        {
            return {result, rpcLGR_NOT_VALIDATED};
        }
        uLedgerMin = uLedgerMax = ledger->info().seq;
    }

        if (!args.marker)
        {
            args.marker = {0, 0};
        }

    if (args.binary)
    {
        // TODO

        auto txns = context.netOps.getTxsAccountB(
            args.account,
            uLedgerMin,
            uLedgerMax,
            args.forward,
            *args.marker,
            args.limit,
            isUnlimited(context.role));

        for (auto& it : txns)
        {
            std::uint32_t uLedgerIndex = std::get<2>(it);

            bool validated = bValidated && uValidatedMin <= uLedgerIndex &&
                uValidatedMax >= uLedgerIndex;

            result.transactionsBn.emplace_back(
                std::make_tuple(
                    std::get<0>(it), std::get<1>(it), std::get<2>(it)),
                validated);
        }
    }
    else
    {

        auto txns = context.netOps.getTxsAccount(
            args.account,
            uLedgerMin,
            uLedgerMax,
            args.forward,
            *args.marker,
            args.limit,
            isUnlimited(context.role));

        for (auto const& [txn, txMeta] : txns)
        {
            if (!txMeta->hasDeliveredAmount())
            {
                std::optional<STAmount> amount =
                    getDeliveredAmount(context, txn, *txMeta);
                if (amount)
                {
                    txMeta->setDeliveredAmount(*amount);
                }
            }

            std::uint32_t uLedgerIndex = txMeta->getLgrSeq();
            bool validated = bValidated && uValidatedMin <= uLedgerIndex &&
                uValidatedMax >= uLedgerIndex;
            result.transactions.emplace_back(std::make_pair(txn, txMeta), validated);
        }
    }


    result.ledgerRange = std::make_pair(uLedgerMin,uLedgerMax);
    result.limit = args.limit;
    result.marker = args.marker;
    result.account = args.account;

    return {result, rpcSUCCESS};
}

std::pair<rpc::v1::GetAccountTransactionHistoryResponse, grpc::Status>
doAccountTxGrpc(
    RPC::GRPCContext<rpc::v1::GetAccountTransactionHistoryRequest>& context)
{
    // return values
    rpc::v1::GetTransactionResponse result;
    grpc::Status status = grpc::Status::OK;

    return {{}, {grpc::StatusCode::UNIMPLEMENTED, "Unimplemented"}};
}


Json::Value doAccountTxJson (RPC::JsonContext& context)
{
    //doAccountTx(context);

    auto& params = context.params;
    AccountTxArgs args;
    Json::Value response;

    args.limit = params.isMember (jss::limit) ?
        params[jss::limit].asUInt () : 0;
    args.binary = params.isMember (jss::binary) && params[jss::binary].asBool ();
    args.forward = params.isMember (jss::forward) && params[jss::forward].asBool ();


    if (!params.isMember (jss::account))
        return rpcError (rpcINVALID_PARAMS);

    auto const account = parseBase58<AccountID>(
            params[jss::account].asString());
    if (! account)
        return rpcError (rpcACT_MALFORMED);

    args.account = *account;

    if (params.isMember(jss::ledger_index_min) ||
            params.isMember(jss::ledger_index_max))
    {
        int min = params.isMember(jss::ledger_index_min)
            ? params[jss::ledger_index_min].asInt()
            : -1;
        int max = params.isMember(jss::ledger_index_max)
            ? params[jss::ledger_index_max].asInt()
            : -1;

        args.ledger = std::make_pair(min, max);
    }
    else if(params.isMember(jss::ledger_hash))
    {

        auto& hashValue = params[jss::ledger_hash];
        if (! hashValue.isString ())
        {
            RPC::Status status{rpcINVALID_PARAMS, "ledgerHashNotString"};
            status.inject(response);
            return response;
        }
        args.ledger = LedgerHash{};
        auto& hash = std::get<LedgerHash>(args.ledger);
        if (! hash.SetHex (hashValue.asString ()))
        {
            RPC::Status status{rpcINVALID_PARAMS, "ledgerHashMalformed"};
            status.inject(response);
            return response;
        }
    }
    else if(params.isMember(jss::ledger_index))
    {
        if(params[jss::ledger_index].isNumeric())
        {
            args.ledger = params[jss::ledger_index].asInt();
        } else
        {
            std::cout << "ledger index string" << std::endl;
            args.ledger = params[jss::ledger_index].asString();

        }
    }
    else
    {
        args.ledger = std::make_pair(-1,-1);
    }

    if (params.isMember(jss::marker))
    {
        auto& token = params[jss::marker];
        if (!token.isMember(jss::ledger) || !token.isMember(jss::seq) ||
                !token[jss::ledger].isUInt() || !token[jss::seq].isUInt())
        {
            return rpcError(
                    rpcINVALID_PARAMS,
                    "invalid marker. Provide ledger index via ledger field, and "
                    "account sequence number via seq field");
        }
        args.marker = std::make_pair(
                token[jss::ledger].asUInt(), token[jss::seq].asUInt());
    }

    auto res = doAccountTxHelp(context, args);

    if(res.second.toErrorCode() != rpcSUCCESS)
    {
        res.second.inject(response);
        return response;
    }


    Json::Value& jvTxns = (response[jss::transactions] = Json::arrayValue);
    for (auto const& [txn, validated] : res.first.transactions)
    {
        Json::Value& jvObj = jvTxns.append (Json::objectValue);

        auto const & [txnBasic, txnMeta] = txn;

        if (txnBasic)
            jvObj[jss::tx] =
                txnBasic->getJson (JsonOptions::include_date);

        if (txnMeta)
        {
            auto metaJ = txnMeta->getJson (JsonOptions::include_date);
            jvObj[jss::meta] = std::move(metaJ);
            jvObj[jss::validated] = validated;
        }
    }

    if(res.first.marker)
    {
        response[jss::marker] = Json::objectValue;
        response[jss::marker][jss::ledger] = res.first.marker->first;
        response[jss::marker][jss::seq] = res.first.marker->second;
    }

    return response;
}

} // ripple
