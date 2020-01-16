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
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/GRPCHandlers.h>

namespace ripple {

// {
//   transaction: <hex>
// }

static
bool
isHexTxID (std::string const& txid)
{
    if (txid.size () != 64)
        return false;

    auto const ret = std::find_if (txid.begin (), txid.end (),
        [](std::string::value_type c)
        {
            return !std::isxdigit (static_cast<unsigned char>(c));
        });

    return (ret == txid.end ());
}

static
bool
isValidated(LedgerMaster& ledgerMaster, std::uint32_t seq, uint256 const& hash)
{
    if (!ledgerMaster.haveLedger (seq))
        return false;

    if (seq > ledgerMaster.getValidatedLedger ()->info().seq)
        return false;

    return ledgerMaster.getHashBySeq (seq) == hash;
}

bool
getMetaHex (Ledger const& ledger,
    uint256 const& transID, std::string& hex)
{
    SHAMapTreeNode::TNType type;
    auto const item =
        ledger.txMap().peekItem (transID, type);

    if (!item)
        return false;

    if (type != SHAMapTreeNode::tnTRANSACTION_MD)
        return false;

    SerialIter it (item->slice());
    it.getVL (); // skip transaction
    hex = strHex (makeSlice(it.getVL ()));
    return true;
}

//TODO should some of these members be references?
struct TxResult
{
    Transaction::pointer txn;

    std::variant<std::shared_ptr<TxMeta>, Blob> meta;

    std::optional<STAmount> deliveredAmount;

    bool validated = false;

    uint256 hash;

    std::optional<bool> searchedAll;
};

//TODO change to refs if possible
struct TxArgs
{
    uint256 hash;

    bool binary = false;

    //TODO: change to pair
    std::optional<uint32_t> minLedger;

    std::optional<uint32_t> maxLedger;
};

std::pair<TxResult, error_code_i>
doTxHelp(TxArgs& args, RPC::Context& context)
{
    TxResult result;

    uint256 hash = args.hash;

    // hash is included in the response
    result.hash = hash;


    ClosedInterval<uint32_t> range;

    auto rangeProvided = args.minLedger && args.maxLedger;

    if (rangeProvided)
    {
        constexpr uint16_t MAX_RANGE = 1000;

        if (*args.maxLedger < *args.minLedger)
            return {result, rpcINVALID_LGR_RANGE};

        if (*args.maxLedger - *args.minLedger > MAX_RANGE)
            return {result, rpcEXCESSIVE_LGR_RANGE};

        range = ClosedInterval<uint32_t> (*args.minLedger, *args.maxLedger);
    }

    std::shared_ptr<Transaction> txn;
    auto ec {rpcSUCCESS};

    if (rangeProvided)
    {
        boost::variant<std::shared_ptr<Transaction>, bool> v =
            context.app.getMasterTransaction().fetch(
                hash, range, ec);

        if (v.which () == 1)
        {
            result.searchedAll = boost::get<bool> (v);
            return {result, rpcTXN_NOT_FOUND};
        }
        else
        {
            txn = boost::get<std::shared_ptr<Transaction>> (v);
        }
    }
    else
    {
        txn = context.app.getMasterTransaction().fetch(hash, ec);
    }

    if (ec == rpcDB_DESERIALIZATION)
    {
        return {result, ec};
    }
    if (!txn)
    {
        return {result, rpcTXN_NOT_FOUND};
    }

    // populate transaction data
    result.txn = txn;
    if(txn->getLedger() == 0)
    {
        return {result, rpcSUCCESS};
    }

    std::shared_ptr<Ledger const> ledger =
        context.ledgerMaster.getLedgerBySeq(txn->getLedger());
    // get meta data
    if (ledger)
    {
        bool ok = false;
        if (args.binary)
        {
            SHAMapTreeNode::TNType type;
            auto const item = ledger->txMap().peekItem(txn->getID(), type);

            if (item && type == SHAMapTreeNode::tnTRANSACTION_MD)
            {
                ok = true;
                SerialIter it(item->slice());
                it.skip(it.getVLDataLength());  // skip transaction
                Blob blob = it.getVL();
                result.meta = blob;
            }
        }
        else
        {
            auto rawMeta = ledger->txRead(txn->getID()).second;
            if (rawMeta)
            {
                ok = true;
                result.meta = std::make_shared<TxMeta>(
                    txn->getID(), ledger->seq(), *rawMeta);

                result.deliveredAmount = RPC::getDeliveredAmount(
                    context,
                    txn,
                    *(std::get<std::shared_ptr<TxMeta>>(result.meta)));
            }
        }
        if (ok)
        {
            result.validated = isValidated(
                context.ledgerMaster, ledger->info().seq, ledger->info().hash);
        }
    }

    return {result, rpcSUCCESS};
}

//TODO is this function actually helpful? Created it to abstract the response
//populating logic, but maybe this is not really helpful
void
populateResponse(
    TxArgs& args,
    std::pair<TxResult, error_code_i> & res,
    auto& fillTxn,
    auto& fillErr,
    auto& fillErrSearch,
    auto& fillMeta,
    auto& fillMetaBn,
    auto& fillDelivered,
    auto& fillValidated)
{
    //handle errors
    if (res.second != rpcSUCCESS)
    {
        if (res.second == rpcTXN_NOT_FOUND &&
            res.first.searchedAll.has_value())
        {
            fillErrSearch();
        }
        else
        {
            fillErr();
        }
    }
    //no errors
    else
    {
        fillTxn();

        //fill binary metadata
        if (args.binary && res.first.meta.index() == 1)
        {
            fillMetaBn();
        }
        //fill meta data
        else if (res.first.meta.index() == 0)
        {
            // check that meta is not nullptr
            if (auto& meta = std::get<std::shared_ptr<TxMeta>>(res.first.meta))
            {
                fillMeta(meta);
                if (res.first.deliveredAmount)
                {
                    fillDelivered();
                }
            }
        }
        fillValidated();
    }
}


Json::Value
doTxJson(RPC::JsonContext& context)
{
    // Deserialize and validate JSON arguments

    if (!context.params.isMember(jss::transaction))
        return rpcError(rpcINVALID_PARAMS);

    std::string txHash = context.params[jss::transaction].asString();
    if (!isHexTxID(txHash))
        return rpcError(rpcNOT_IMPL);

    TxArgs args;
    args.hash = from_hex_text<uint256>(txHash);

    args.binary = context.params.isMember(jss::binary) &&
        context.params[jss::binary].asBool();

    if (context.params.isMember(jss::min_ledger) &&
        context.params.isMember(jss::max_ledger))
    {
        try
        {
            args.minLedger = context.params[jss::min_ledger].asUInt();
            args.maxLedger = context.params[jss::max_ledger].asUInt();
        }
        catch (...)
        {
            // One of the calls to `asUInt ()` failed.
            return rpcError(rpcINVALID_LGR_RANGE);
        }
    }

    // Get data
    std::pair<TxResult, error_code_i> res = doTxHelp(args, context);

    Json::Value ret;
    auto fillTxn = [&args, &res, &ret]() {
        ret = res.first.txn->getJson(JsonOptions::include_date, args.binary);
    };

    auto fillErr = [&args,&res,&ret]() {

        ret = rpcError(res.second);
    };

    auto fillErrSearch = [&args,&res,&ret]() {

        auto jvResult = Json::Value(Json::objectValue);
        jvResult[jss::searched_all] = *res.first.searchedAll;

        ret = rpcError(res.second, jvResult);
    };

    auto fillMetaBn = [&args,&res,&ret]() {

        ret[jss::meta] = strHex(makeSlice(std::get<Blob>(res.first.meta)));
    };


    auto fillMeta = [&args,&res,&ret](auto& meta) {

        ret[jss::meta] = meta->getJson(JsonOptions::none);
    };

    auto fillDelivered = [&args,&res,&ret]() {

        ret[jss::meta][jss::delivered_amount] =
            res.first.deliveredAmount->getJson(
                    JsonOptions::include_date);
    };

    auto fillValidated = [&args,&res,&ret]() {

        ret[jss::validated] = res.first.validated;

    };

    populateResponse(
        args,
        res,
        fillTxn,
        fillErr,
        fillErrSearch,
        fillMeta,
        fillMetaBn,
        fillDelivered,
        fillValidated);

    return ret;
}

std::pair<rpc::v1::GetTransactionResponse, grpc::Status>
doTxGrpc(RPC::GRPCContext<rpc::v1::GetTransactionRequest>& context)
{
    // return values
    rpc::v1::GetTransactionResponse response;
    grpc::Status status = grpc::Status::OK;

    // input
    rpc::v1::GetTransactionRequest& request = context.params;

    TxArgs args;

    std::string const& hashBytes = request.hash();
    args.hash = uint256::fromVoid(hashBytes.data());
    if (args.hash.size() != hashBytes.size())
    {
        grpc::Status errorStatus{
            grpc::StatusCode::INVALID_ARGUMENT, "ledger hash malformed"};
        return {response, errorStatus};
    }


    args.binary = request.binary();

    if(request.ledger_range().ledger_index_min() != 0
            && request.ledger_range().ledger_index_max() != 0)
    {
        args.minLedger = request.ledger_range().ledger_index_min();
        args.maxLedger = request.ledger_range().ledger_index_max();
    }

    std::pair<TxResult, error_code_i> res = doTxHelp(args, context);

    auto fillErr = [&args,&res,&response,&status] ()
    {
        auto errorInfo = RPC::get_error_info(res.second);
        grpc::Status errorStatus{grpc::StatusCode::INTERNAL,
            errorInfo.message.c_str()};
        status = errorStatus;
    };

    auto fillErrSearched = [&args,&res,&response,&status] ()
    {

        grpc::Status errorStatus{grpc::StatusCode::NOT_FOUND,
            "txn not found. searched_all = " + *res.first.searchedAll};
        status = errorStatus;

    };

    auto fillTxn = [&args,&res,&request,&response] ()
    {
        std::shared_ptr<STTx const> stTxn = res.first.txn->getSTransaction();
        if(args.binary)
        {
            Serializer s = stTxn->getSerializer();
            response.set_transaction_binary(s.data(), s.size());
        }
        else
        {
            RPC::populateTransaction(*response.mutable_transaction(), stTxn);
        }

        response.set_ledger_index(res.first.txn->getLedger());
        response.set_hash(request.hash());
    };


    auto fillMeta = [&args,&res,&response,&status] (auto& meta)
    {
        RPC::populateMeta(*response.mutable_meta(), meta);
    };

    auto fillMetaBn = [&args,&res,&response,&status] ()
    {
        Slice slice = makeSlice(std::get<Blob>(res.first.meta));
        response.set_meta_binary(slice.data(), slice.size());
    };

    auto fillDelivered = [&args,&res,&response,&status] ()
    {

        RPC::populateAmount(*response.mutable_meta()->mutable_delivered_amount(),
                *res.first.deliveredAmount);
    };

    auto fillValidated = [&args,&res,&response,&status]()
    {
        response.set_validated(res.first.validated);
    };

    populateResponse(
        args,
        res,
        fillTxn,
        fillErr,
        fillErrSearched,
        fillMeta,
        fillMetaBn,
        fillDelivered,
        fillValidated);

    return {response, status};
}

}  // namespace ripple
